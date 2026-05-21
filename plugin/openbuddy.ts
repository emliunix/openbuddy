import * as net from "net"
import type {
    Event,
    EventSessionStatus,
    EventSessionError,
    EventSessionCreated,
    EventSessionDeleted,
    EventSessionNextStepEnded,
    EventMessageUpdated,
    EventMessagePartUpdated,
    EventTodoUpdated,
    EventPermissionAsked,
    EventPermissionReplied,
    EventFileEdited,
    EventCommandExecuted,
} from "@opencode-ai/sdk/v2"

import type { PluginInput } from "@opencode-ai/plugin"

// ---------------------------------------------------------------------------
// Protocol types
// ---------------------------------------------------------------------------

interface Heartbeat {
    completed?: boolean
    entries?: string[]
    msg?: string
    prompt?: {
        hint: string
        id: string
        tool: string
    }
    running?: number
    tokens?: number
    tokens_today?: number
    total?: number
    waiting?: number
}

interface TurnEvent {
    content: Array<{ text: string; type: string }>
    evt: "turn"
    role: string
}

interface BuddyCommand {
    cmd: string
    [key: string]: unknown
}

interface BuddyPing {
    cmd: "ping"
}

interface BuddyPermission {
    cmd: "permission"
    decision: "once" | "deny" | "allow"
    id: string
}

type BuddyMessage = BuddyCommand | BuddyPing | BuddyPermission

function encodeNdjson(obj: unknown): string {
    return JSON.stringify(obj) + "\n"
}

// ---------------------------------------------------------------------------
// Plugin hook input types
// ---------------------------------------------------------------------------

interface ToolExecuteInput {
    tool: string
    sessionID: string
    callID: string
}

// ---------------------------------------------------------------------------
// BuddyClient
// ---------------------------------------------------------------------------

const BUDDY_HOST = "127.0.0.1"
const BUDDY_PORT = 7887
const HEARTBEAT_INTERVAL_MS = 10000
const RECONNECT_DELAY_MS = 5000


type LogFn = (req: {
    body: {
        service: string
        level: "debug" | "info" | "warn" | "error"
        message: string
        extra?: Record<string, unknown>
    }
}) => Promise<unknown>

class BuddyClient {
    private socket: net.Socket | null = null
    private log: LogFn | null = null
    private ctx: PluginInput | null = null
    private heartbeatTimer: NodeJS.Timeout | null = null
    private reconnectTimer: NodeJS.Timeout | null = null
    private buffer = ""
    private connected = false

    // Per-session state: {running, waiting}. Aggregates derived on heartbeat.
    private sessionStates = new Map<string, { running: boolean; waiting: number }>()
    private erroredSessions = new Set<string>()
    // permissionID -> metadata (for prompt display and API relay)
    private pendingPermissions = new Map<string, { hint: string; tool: string; sessionID: string; permissionID: string }>()

    private state: {
        entries: string[]
        total: number
        tokens: number
        tokensToday: number
        currentMsg?: string
        completed: boolean
    } = {
        entries: [],
        total: 0,
        tokens: 0,
        tokensToday: 0,
        completed: false,
    }

    constructor(ctx: PluginInput) {
        this.ctx = ctx
        this.log = ctx.client.app.log.bind(ctx.client.app)
    }

    async connect(): Promise<void> {
        await this.doConnect()
    }

    private info(message: string, extra?: Record<string, unknown>): void {
        this.log?.({ body: { service: "openbuddy", level: "info", message, extra } })
    }

    private warn(message: string, extra?: Record<string, unknown>): void {
        this.log?.({ body: { service: "openbuddy", level: "warn", message, extra } })
    }

    private error(message: string, extra?: Record<string, unknown>): void {
        this.log?.({ body: { service: "openbuddy", level: "error", message, extra } })
    }

    private debug(message: string, extra?: Record<string, unknown>): void {
        this.log?.({ body: { service: "openbuddy", level: "debug", message, extra } })
    }

    // ------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------

    private async doConnect(): Promise<void> {
        if (this.socket) return

        this.socket = net.createConnection({
            host: BUDDY_HOST,
            port: BUDDY_PORT,
        })

        this.socket.on("connect", () => {
            this.connected = true
            this.info("connected to buddy")
            this.startHeartbeat()
            this.sendConnectInfo()
        })

        this.socket.on("data", (data) => {
            this.buffer += data.toString("utf-8")
            this.processBuffer()
        })

        this.socket.on("error", (err: Error) => {
            this.error("socket error", { error: err.message })
        })

        this.socket.on("close", (hadError: boolean) => {
            const wasConnected = this.connected
            this.connected = false
            this.socket = null
            this.stopHeartbeat()
            if (wasConnected) {
                this.info("disconnected from buddy", { hadError })
            }
            this.scheduleReconnect()
        })
    }

    private processBuffer(): void {
        const lines = this.buffer.split("\n")
        this.buffer = lines.pop() || ""

        for (const line of lines) {
            if (!line.trim()) continue
            try {
                const msg: BuddyMessage = JSON.parse(line)
                this.handleBuddyMessage(msg)
            } catch (err) {
                this.error("failed to parse buddy message", {
                    line: line.slice(0, 200),
                    error: (err as Error).message,
                })
            }
        }
    }

    private handleBuddyMessage(msg: BuddyMessage): void {
        if (msg.cmd === "ping") return

        if (msg.cmd === "permission") {
            const perm = msg as BuddyPermission
            const pp = this.pendingPermissions.get(perm.id)
            if (pp) {
                this.pendingPermissions.delete(perm.id)
                const response = perm.decision === "deny" ? "reject" : "once"
                this.debug("buddy→permission", { id: perm.id, decision: perm.decision, sessionID: pp.sessionID, remainingQueue: this.pendingPermissions.size })
                this.ctx?.client?.postSessionIdPermissionsPermissionId({
                    path: { id: pp.sessionID, permissionID: pp.permissionID },
                    body: { response },
                }).then(() => {
                    this.info("permission→relayed", { sessionID: pp.sessionID, permissionID: pp.permissionID, decision: perm.decision })
                }).catch((err: any) => {
                    this.error("permission relay failed", { error: err?.message || String(err) })
                })
            } else {
                this.warn("buddy→permission unknown id", { id: perm.id })
            }
            this.sendHeartbeat()
            return
        }

        if (msg.cmd === "status") {
            this.sendStatus()
            return
        }

        if (msg.cmd === "name") {
            this.sendAck("name", true)
            return
        }

        if (msg.cmd === "owner") {
            this.sendAck("owner", true)
            return
        }

        if (msg.cmd === "unpair") {
            this.sendAck("unpair", true)
            return
        }

        if (msg.cmd === "char_begin" || msg.cmd === "file" || msg.cmd === "chunk" || msg.cmd === "file_end" || msg.cmd === "char_end") {
            return
        }

        this.warn("unknown buddy command", { cmd: msg.cmd })
    }

    private send(obj: unknown): boolean {
        if (!this.socket || this.socket.destroyed) return false
        this.socket.write(encodeNdjson(obj))
        return true
    }

    private sendConnectInfo(): void {
        const now = new Date()
        this.send({
            time: [
                Math.floor(now.getTime() / 1000),
                -now.getTimezoneOffset() * 60,
            ],
        })
    }

    private sendStatus(): void {
        this.send({
            ack: "status",
            data: {
                name: "OpenBuddy",
                sec: true,
                stats: { appr: 0, deny: 0, lvl: 1, nap: 0, vel: 0 },
                sys: { heap: 0, up: 0 },
            },
            ok: true,
        })
    }

    private sendAck(cmd: string, ok: boolean): void {
        this.send({ ack: cmd, ok })
    }

    private startHeartbeat(): void {
        this.stopHeartbeat()
        this.heartbeatTimer = setInterval(() => {
            this.sendHeartbeat()
        }, HEARTBEAT_INTERVAL_MS)
        this.sendHeartbeat()
    }

    private stopHeartbeat(): void {
        if (this.heartbeatTimer) {
            clearInterval(this.heartbeatTimer)
            this.heartbeatTimer = null
        }
    }

    private scheduleReconnect(): void {
        if (this.reconnectTimer) return
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null
            this.doConnect()
        }, RECONNECT_DELAY_MS)
    }

    private get running(): number {
        let count = 0
        for (const s of this.sessionStates.values()) {
            if (s.running) count++
        }
        return count
    }

    private get waiting(): number {
        let count = 0
        for (const s of this.sessionStates.values()) {
            count += s.waiting
        }
        return count
    }

    private ensureSession(id: string): { running: boolean; waiting: number } {
        if (!this.sessionStates.has(id)) {
            this.sessionStates.set(id, { running: false, waiting: 0 })
        }
        return this.sessionStates.get(id)!
    }

    private clearPendingForSession(sessionID: string): void {
        for (const [id, pp] of this.pendingPermissions) {
            if (pp.sessionID === sessionID) {
                this.pendingPermissions.delete(id)
            }
        }
    }

    private getCurrentPrompt(): { hint: string; id: string; tool: string } | undefined {
        const entries = Array.from(this.pendingPermissions.entries())
        if (entries.length === 0) return undefined
        const [id, perm] = entries[0]
        return { id, hint: perm.hint, tool: perm.tool }
    }

    private sendHeartbeat(): void {
        const prompt = this.getCurrentPrompt()

        const heartbeat: Heartbeat = {
            completed: this.state.completed,
            entries: this.state.entries.slice(-5),
            running: this.running,
            tokens: this.state.tokens,
            tokens_today: this.state.tokensToday,
            total: this.state.total,
            waiting: this.waiting,
        }

        if (this.state.currentMsg) {
            heartbeat.msg = this.state.currentMsg
        } else if (prompt) {
            heartbeat.msg = `approve: ${prompt.tool}`
        } else if (this.running > 0) {
            heartbeat.msg = `${this.running} session${this.running > 1 ? "s" : ""} running`
        }

        if (prompt) {
            heartbeat.prompt = prompt
        }

        this.debug("heartbeat", { running: heartbeat.running, waiting: heartbeat.waiting, total: heartbeat.total, completed: heartbeat.completed })
        this.send(heartbeat)
        if (this.state.completed) {
            this.state.completed = false
        }
    }

    // ------------------------------------------------------------------
    // Event dispatcher — single entrypoint for all bus events
    // ------------------------------------------------------------------

    handleEvent(event: Event): void {
        switch (event.type) {
            case "session.status":
                this.handleSessionStatus({ event: event as EventSessionStatus })
                break
            case "session.error":
                this.handleSessionError({ event: event as EventSessionError })
                break
            case "session.created":
                this.handleSessionCreated({ event: event as EventSessionCreated })
                break
            case "session.deleted":
                this.handleSessionDeleted({ event: event as EventSessionDeleted })
                break
            case "session.next.step.ended":
                this.handleSessionNextStepEnded({ event: event as EventSessionNextStepEnded })
                break
            case "message.updated":
                this.handleMessageUpdated({ event: event as EventMessageUpdated })
                break
            case "message.part.updated":
                this.handleMessagePartUpdated({ event: event as EventMessagePartUpdated })
                break
            case "todo.updated":
                this.handleTodoUpdated({ event: event as EventTodoUpdated })
                break
            case "permission.asked":
                this.handlePermissionAsked({ event: event as EventPermissionAsked })
                break
            case "permission.replied":
                this.handlePermissionReplied({ event: event as EventPermissionReplied })
                break
            case "file.edited":
                this.handleFileEdited({ event: event as EventFileEdited })
                break
            case "command.executed":
                this.handleCommandExecuted({ event: event as EventCommandExecuted })
                break
            default:
                this.debug("unhandled event type", { type: (event as any).type })
        }
    }

    // ------------------------------------------------------------------
    // Event handlers — each maps 1:1 to an OpenCode event type
    // ------------------------------------------------------------------

    handleSessionStatus({ event }: { event: EventSessionStatus }): void {
        const { sessionID, status } = event.properties
        const s = this.ensureSession(sessionID)
        const wasRunning = s.running

        if (status.type === "busy" || status.type === "retry") {
            if (!s.running) {
                s.running = true
                this.info("session→running", { sessionID, status: status.type, wasRunning, nowRunning: true, totalRunning: this.running })
                this.sendHeartbeat()
                return
            }
            this.debug("session still running", { sessionID, status: status.type, running: this.running })
            this.sendHeartbeat()
            return
        }

        if (status.type === "idle") {
            if (!wasRunning) {
                this.debug("session→idle duplicate ignored", { sessionID, wasRunning, erroredSessions: this.erroredSessions.has(sessionID) })
                return
            }
            const errored = this.erroredSessions.has(sessionID)
            this.erroredSessions.delete(sessionID)
            this.clearPendingForSession(sessionID)
            s.running = false
            s.waiting = 0
            const willComplete = !errored
            if (willComplete) {
                this.state.completed = true
            }
            this.info("session→idle", { sessionID, wasRunning, errored, willComplete, totalRunning: this.running })
            this.sendHeartbeat()
            return
        }

        this.debug("session status other", { sessionID, status: (status as any).type, running: this.running })
        this.sendHeartbeat()
    }

    handleSessionError({ event }: { event: EventSessionError }): void {
        const { sessionID, error } = event.properties
        if (!sessionID || !error) {
            this.debug("session.error ignored", { hasSessionID: !!sessionID, hasError: !!error })
            return
        }
        this.erroredSessions.add(sessionID)
        this.info("session→errored", { sessionID, errorName: error.name, errorMessage: (error as any).message })
    }

    handleSessionCreated({ event }: { event: EventSessionCreated }): void {
        const sessionID = event.properties.info.id
        this.state.total += 1
        this.ensureSession(sessionID)
        this.info("session→created", { sessionID, total: this.state.total })
        this.sendHeartbeat()
    }

    handleSessionDeleted({ event }: { event: EventSessionDeleted }): void {
        const sessionID = event.properties.info?.id ?? event.properties.sessionID
        if (!sessionID) {
            this.warn("session.deleted missing sessionID")
            return
        }
        this.sessionStates.delete(sessionID)
        this.clearPendingForSession(sessionID)
        this.state.total = Math.max(0, this.state.total - 1)
        this.info("session→deleted", { sessionID, total: this.state.total })
        this.sendHeartbeat()
    }

    handleSessionNextStepEnded({ event }: { event: EventSessionNextStepEnded }): void {
        const stepTokens = event.properties.tokens.output
        if (stepTokens > 0) {
            const oldTokens = this.state.tokens
            this.state.tokens += stepTokens
            this.state.tokensToday += stepTokens
            this.debug("tokens→added", { stepTokens, oldTotal: oldTokens, newTotal: this.state.tokens })
        }
    }

    handleMessageUpdated(_input: { event: EventMessageUpdated }): void {
        this.debug("handleMessageUpdated", { type: _input.event.type })
    }

    handleMessagePartUpdated({ event }: { event: EventMessagePartUpdated }): void {
        this.debug("handleMessagePartUpdated", { type: event.type })
        const { part } = event.properties
        if (part.type === "text") {
            const turnEvent: TurnEvent = {
                content: [{ text: part.text.slice(0, 4096), type: "text" }],
                evt: "turn",
                role: "assistant",
            }
            this.send(turnEvent)
        }
    }

    handleTodoUpdated(_input: { event: EventTodoUpdated }): void {
        this.debug("handleTodoUpdated", { type: _input.event.type })
        this.sendHeartbeat()
    }

    handlePermissionAsked({ event }: { event: EventPermissionAsked }): void {
        const { id, sessionID, permission, patterns } = event.properties
        const s = this.ensureSession(sessionID)
        s.waiting += 1
        this.pendingPermissions.set(id, {
            hint: patterns[0] || permission,
            tool: permission,
            sessionID,
            permissionID: id,
        })
        this.info("permission→asked", { id, tool: permission, sessionID, queueSize: this.pendingPermissions.size, sessionWaiting: s.waiting })
        this.sendHeartbeat()
    }

    handlePermissionReplied({ event }: { event: EventPermissionReplied }): void {
        const { sessionID, requestID, reply } = event.properties
        const sr = sessionID ? this.sessionStates.get(sessionID) : null
        if (sr && sr.waiting > 0) {
            sr.waiting -= 1
        }
        const firstKey = this.pendingPermissions.keys().next().value as string | undefined
        if (firstKey) this.pendingPermissions.delete(firstKey)
        this.info("permission→replied", { sessionID, requestID, reply, queueSize: this.pendingPermissions.size })
        this.sendHeartbeat()
    }

    handleFileEdited(_input: { event: EventFileEdited }): void {
        this.debug("handleFileEdited", { type: _input.event.type })
    }

    handleCommandExecuted(_input: { event: EventCommandExecuted }): void {
        this.debug("handleCommandExecuted", { type: _input.event.type })
    }

    // ------------------------------------------------------------------
    // Named hooks
    // ------------------------------------------------------------------

    handleToolExecuteBefore(input: ToolExecuteInput, output: { args?: unknown }): void {
        const cmd = this.formatToolCommand(input.tool, output.args)
        const entry = `${new Date().toLocaleTimeString()} ${cmd}`
        this.state.entries.push(entry)
        if (this.state.entries.length > 20) {
            this.state.entries.shift()
        }
        this.debug("tool→log", { tool: input.tool, sessionID: input.sessionID, entry, entriesCount: this.state.entries.length })
        this.sendHeartbeat()
    }

    private formatToolCommand(tool: string, args: unknown): string {
        if (!args || typeof args !== "object") return tool
        const a = args as Record<string, unknown>
        switch (tool) {
            case "bash":
                return a.command ? `bash: ${String(a.command).slice(0, 40)}` : tool
            case "read":
                return a.filePath ? `read: ${String(a.filePath).replace(/.*\//, "").slice(0, 40)}` : tool
            case "grep":
                return a.pattern ? `grep: ${String(a.pattern).slice(0, 40)}` : tool
            case "edit":
                return a.filePath ? `edit: ${String(a.filePath).replace(/.*\//, "").slice(0, 40)}` : tool
            case "write":
                return a.filePath ? `write: ${String(a.filePath).replace(/.*\//, "").slice(0, 40)}` : tool
            case "glob":
                return a.pattern ? `glob: ${String(a.pattern).slice(0, 40)}` : tool
            default:
                return tool
        }
    }

    disconnect(): void {
        this.info("disconnecting")
        this.stopHeartbeat()
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer)
            this.reconnectTimer = null
        }
        if (this.socket) {
            this.socket.destroy()
            this.socket = null
        }
        this.connected = false
    }
}

// ---------------------------------------------------------------------------
// Plugin entrypoint
// ---------------------------------------------------------------------------

declare global {
    // eslint-disable-next-line no-var
    var __openbuddy_active_client: BuddyClient | undefined
}

export const OpenBuddyPlugin = async (ctx: PluginInput) => {
    // Disconnect old instance if exists (prevents stale heartbeats after reload)
    const oldClient = globalThis.__openbuddy_active_client
    if (oldClient) {
        oldClient.disconnect()
        globalThis.__openbuddy_active_client = undefined
    }

    const client = new BuddyClient(ctx)
    globalThis.__openbuddy_active_client = client
    await client.connect()

    return {
        event: async ({ event }: { event: Event }) => {
            return await client.handleEvent(event)
        },

        "tool.execute.before": async (input: ToolExecuteInput, output: { args?: unknown }) => {
            client.handleToolExecuteBefore(input, output)
        },
    }
}
