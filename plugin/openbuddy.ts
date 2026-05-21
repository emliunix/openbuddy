import * as net from "net"
import type {
    EventSessionStatus,
    EventSessionError,
    EventSessionCreated,
    EventSessionDeleted,
    EventSessionUpdated,
    EventSessionNextStepEnded,
    EventMessageUpdated,
    EventMessagePartUpdated,
    EventTodoUpdated,
    EventPermissionAsked,
    EventPermissionReplied,
    EventFileEdited,
    EventCommandExecuted,
} from "@opencode-ai/sdk/v2"

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
    private ctx: unknown = null
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

    async connect(ctx: unknown): Promise<void> {
        this.ctx = ctx
        const c = ctx as { client: { app: { log: LogFn } } }
        this.log = c.client.app.log.bind(c.client.app)
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
            this.info("permission decision", { decision: perm.decision, id: perm.id })
            const pp = this.pendingPermissions.get(perm.id)
            if (pp) {
                this.pendingPermissions.delete(perm.id)
                const response = perm.decision === "deny" ? "reject" : "once"
                const c = this.ctx as { client: { app: { permissions: { respond: (req: { path: { id: string; permissionID: string }; body: { response: string } }) => Promise<unknown> } } } }
                c.client.app.permissions.respond({
                    path: { id: pp.sessionID, permissionID: pp.permissionID },
                    body: { response },
                }).then(() => {
                    this.info("permission relayed", { sessionID: pp.sessionID, permissionID: pp.permissionID, decision: perm.decision })
                }).catch((err: unknown) => {
                    const msg = err instanceof Error ? err.message : String(err)
                    this.error("permission relay failed", { error: msg })
                })
            } else {
                this.warn("permission decision for unknown id", { id: perm.id })
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

        this.send(heartbeat)
        if (this.state.completed) {
            this.state.completed = false
        }
        this.debug("heartbeat sent", { running: this.running, waiting: this.waiting, total: this.state.total, completed: this.state.completed })
    }

    // ------------------------------------------------------------------
    // Event handlers — each maps 1:1 to an OpenCode event type
    // ------------------------------------------------------------------

    handleSessionStatus({ event }: { event: EventSessionStatus }): void {
        const { sessionID, status } = event.properties
        const s = this.ensureSession(sessionID)

        if (status.type === "busy" || status.type === "retry") {
            if (!s.running) {
                s.running = true
                this.info("Session running (busy)", { sessionID })
                this.sendHeartbeat()
                return
            }
        } else if (status.type === "idle") {
            const errored = this.erroredSessions.has(sessionID)
            this.erroredSessions.delete(sessionID)
            this.clearPendingForSession(sessionID)
            s.running = false
            s.waiting = 0
            if (!errored) {
                this.state.completed = true
            }
            this.sendHeartbeat()
            return
        }

        this.debug("session status", { sessionID, status: status.type, running: this.running })
        this.sendHeartbeat()
    }

    handleSessionError({ event }: { event: EventSessionError }): void {
        const { sessionID, error } = event.properties
        if (!sessionID || !error) return
        this.erroredSessions.add(sessionID)
        this.debug("session error", { sessionID, error: error.name })
    }

    handleSessionCreated({ event }: { event: EventSessionCreated }): void {
        const sessionID = event.properties.info.id
        this.state.total += 1
        this.ensureSession(sessionID)
        this.debug("session created", { sessionID, total: this.state.total })
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
        this.sendHeartbeat()
    }

    handleSessionUpdated(_input: { event: EventSessionUpdated }): void {
        this.sendHeartbeat()
    }

    handleSessionNextStepEnded({ event }: { event: EventSessionNextStepEnded }): void {
        const stepTokens = event.properties.tokens.output
        if (stepTokens > 0) {
            this.state.tokens += stepTokens
            this.state.tokensToday += stepTokens
            this.debug("step tokens accumulated", { stepTokens, total: this.state.tokens })
        }
    }

    handleMessageUpdated(_input: { event: EventMessageUpdated }): void {
        // SDK v2 AssistantMessage has no `content` field.
        // Text arrives via message.part.updated instead.
    }

    handleMessagePartUpdated({ event }: { event: EventMessagePartUpdated }): void {
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
        this.info("permission asked", { id, tool: permission, sessionID })
        this.sendHeartbeat()
    }

    handlePermissionReplied({ event }: { event: EventPermissionReplied }): void {
        const { sessionID } = event.properties
        this.debug("permission.replied", { sessionID })
        const sr = sessionID ? this.sessionStates.get(sessionID) : null
        if (sr && sr.waiting > 0) {
            sr.waiting -= 1
        }
        // Remove first entry from queue (FIFO for external replies)
        const firstKey = this.pendingPermissions.keys().next().value as string | undefined
        if (firstKey) this.pendingPermissions.delete(firstKey)
        this.sendHeartbeat()
    }

    handleFileEdited(_input: { event: EventFileEdited }): void {
        // No-op
    }

    handleCommandExecuted(_input: { event: EventCommandExecuted }): void {
        // No-op
    }

    private extractTextFromContent(content: unknown): string | undefined {
        if (typeof content === "string") return content
        if (Array.isArray(content)) {
            for (const part of content) {
                if (part && typeof part === "object" && "type" in part && part.type === "text" && "text" in part && typeof part.text === "string") {
                    return part.text
                }
            }
        }
        return undefined
    }

    // ------------------------------------------------------------------
    // Named hooks
    // ------------------------------------------------------------------

    handleToolExecuteBefore(input: ToolExecuteInput, _output: unknown): void {
        const s = this.ensureSession(input.sessionID)
        s.running = true
        const entry = `${new Date().toLocaleTimeString()} ${input.tool}`
        this.state.entries.push(entry)
        if (this.state.entries.length > 20) {
            this.state.entries.shift()
        }
        this.sendHeartbeat()
    }

    handleToolExecuteAfter(_input: ToolExecuteInput, _output: unknown): void {
        this.sendHeartbeat()
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

let buddyClient: BuddyClient | null = null

function getBuddyClient(): BuddyClient {
    if (!buddyClient) {
        buddyClient = new BuddyClient()
    }
    return buddyClient
}

export const OpenBuddyPlugin = async (ctx: unknown) => {
    const client = getBuddyClient()
    await client.connect(ctx)

    return {
        "session.status": client.handleSessionStatus.bind(client),
        "session.error": client.handleSessionError.bind(client),
        "session.created": client.handleSessionCreated.bind(client),
        "session.deleted": client.handleSessionDeleted.bind(client),
        "session.updated": client.handleSessionUpdated.bind(client),
        "session.next.step.ended": client.handleSessionNextStepEnded.bind(client),
        "message.updated": client.handleMessageUpdated.bind(client),
        "message.part.updated": client.handleMessagePartUpdated.bind(client),
        "todo.updated": client.handleTodoUpdated.bind(client),
        "permission.asked": client.handlePermissionAsked.bind(client),
        "permission.replied": client.handlePermissionReplied.bind(client),
        "file.edited": client.handleFileEdited.bind(client),
        "command.executed": client.handleCommandExecuted.bind(client),
        "tool.execute.before": client.handleToolExecuteBefore.bind(client),
        "tool.execute.after": client.handleToolExecuteAfter.bind(client),
    }
}
