import * as net from "net"

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
// OpenCode event types (deterministic — absent fields are invalid)
// ---------------------------------------------------------------------------

interface SessionStatusEvent {
    type: "session.status"
    properties: {
        sessionID: string
        status: { type: "idle" | "busy" | "retry" }
    }
}

interface SessionErrorEvent {
    type: "session.error"
    properties: {
        sessionID: string
        error: { name: string }
    }
}

interface SessionIdleEvent {
    type: "session.idle"
    properties: {
        sessionID: string
    }
}

interface SessionCreatedEvent {
    type: "session.created"
    properties: {
        info: { id: string }
    }
}

interface SessionDeletedEvent {
    type: "session.deleted"
    properties: {
        info?: { id: string }
        sessionID?: string
    }
}

interface SessionUpdatedEvent {
    type: "session.updated"
    properties: Record<string, unknown>
}

interface SessionNextStepEndedEvent {
    type: "session.next.step.ended"
    properties: {
        tokens: { output: number }
    }
}

interface MessageUpdatedEvent {
    type: "message.updated"
    properties: {
        info: {
            role: string
            content: unknown
        }
    }
}

interface MessagePartUpdatedEvent {
    type: "message.part.updated"
    properties: {
        part: {
            type: string
            text: string
        }
    }
}

interface TodoUpdatedEvent {
    type: "todo.updated"
    properties: Record<string, unknown>
}

interface PermissionAskedEvent {
    type: "permission.asked"
    properties: {
        id: string
        sessionID: string
        permission: string
        patterns: Array<string>
    }
}

interface PermissionRepliedEvent {
    type: "permission.replied"
    properties: {
        id: string
    }
}

interface PermissionUpdatedEvent {
    type: "permission.updated"
    properties: Record<string, unknown>
}

interface FileEditedEvent {
    type: "file.edited"
    properties: Record<string, unknown>
}

interface CommandExecutedEvent {
    type: "command.executed"
    properties: Record<string, unknown>
}

type Event =
    | SessionStatusEvent
    | SessionErrorEvent
    | SessionIdleEvent
    | SessionCreatedEvent
    | SessionDeletedEvent
    | SessionUpdatedEvent
    | SessionNextStepEndedEvent
    | MessageUpdatedEvent
    | MessagePartUpdatedEvent
    | TodoUpdatedEvent
    | PermissionAskedEvent
    | PermissionRepliedEvent
    | PermissionUpdatedEvent
    | FileEditedEvent
    | CommandExecutedEvent

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
const COMPLETED_PULSE_MS = 2000

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
    private completedTimer: NodeJS.Timeout | null = null
    private buffer = ""
    private connected = false

    private sessionStatuses = new Map<string, "idle" | "busy" | "retry">()
    private erroredSessions = new Set<string>()
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

    private getRunningCount(): number {
        let count = 0
        for (const status of this.sessionStatuses.values()) {
            if (status === "busy" || status === "retry") count++
        }
        return count
    }

    private getCurrentPrompt(): { hint: string; id: string; tool: string } | undefined {
        const entries = Array.from(this.pendingPermissions.entries())
        if (entries.length === 0) return undefined
        const [id, perm] = entries[0]
        return { id, hint: perm.hint, tool: perm.tool }
    }

    private setCompleted(): void {
        this.state.completed = true
        if (this.completedTimer) clearTimeout(this.completedTimer)
        this.completedTimer = setTimeout(() => {
            this.state.completed = false
        }, COMPLETED_PULSE_MS)
    }

    private sendHeartbeat(): void {
        const running = this.getRunningCount()
        const waiting = this.pendingPermissions.size
        const prompt = this.getCurrentPrompt()

        const heartbeat: Heartbeat = {
            entries: this.state.entries.slice(-5),
            running,
            total: this.state.total,
            waiting,
        }

        if (this.state.completed) {
            heartbeat.completed = true
        }

        if (this.state.currentMsg) {
            heartbeat.msg = this.state.currentMsg
        } else if (prompt) {
            heartbeat.msg = `approve: ${prompt.tool}`
        } else if (running > 0) {
            heartbeat.msg = `${running} session${running > 1 ? "s" : ""} running`
        }

        if (prompt) {
            heartbeat.prompt = prompt
        }

        this.send(heartbeat)
        this.debug("heartbeat sent", { running, waiting, total: this.state.total, completed: this.state.completed })
    }

    // ------------------------------------------------------------------
    // Event handlers — each maps 1:1 to an OpenCode event type
    // ------------------------------------------------------------------

    handleSessionStatus(event: SessionStatusEvent["properties"]): void {
        const { sessionID, status } = event
        const oldStatus = this.sessionStatuses.get(sessionID)
        this.sessionStatuses.set(sessionID, status.type)

        if (status.type === "busy" && oldStatus !== "busy") {
            this.state.currentMsg = "working..."
        } else if (status.type === "idle") {
            this.sessionStatuses.delete(sessionID)
            this.state.currentMsg = undefined
            const errored = this.erroredSessions.has(sessionID)
            this.erroredSessions.delete(sessionID)
            if (!errored && oldStatus === "busy") {
                this.setCompleted()
            }
        }

        this.debug("session status", { sessionID, status: status.type, running: this.getRunningCount() })
        this.sendHeartbeat()
    }

    handleSessionError(event: SessionErrorEvent["properties"]): void {
        const { sessionID, error } = event
        this.erroredSessions.add(sessionID)
        this.debug("session error", { sessionID, error: error.name })
    }

    handleSessionIdle(event: SessionIdleEvent["properties"]): void {
        const { sessionID } = event
        const wasBusy = this.sessionStatuses.get(sessionID) === "busy"
        this.sessionStatuses.delete(sessionID)
        this.pendingPermissions.delete(sessionID)
        this.state.currentMsg = undefined
        const errored = this.erroredSessions.has(sessionID)
        this.erroredSessions.delete(sessionID)
        if (!errored && wasBusy) {
            this.setCompleted()
        }
        this.sendHeartbeat()
    }

    handleSessionCreated(_event: SessionCreatedEvent["properties"]): void {
        this.state.total += 1
        this.debug("session created", { total: this.state.total })
        this.sendHeartbeat()
    }

    handleSessionDeleted(event: SessionDeletedEvent["properties"]): void {
        const sessionID = event.info?.id ?? event.sessionID
        if (!sessionID) {
            this.warn("session.deleted missing sessionID")
            return
        }
        this.sessionStatuses.delete(sessionID)
        this.pendingPermissions.delete(sessionID)
        this.state.total = Math.max(0, this.state.total - 1)
        this.sendHeartbeat()
    }

    handleSessionUpdated(_event: SessionUpdatedEvent["properties"]): void {
        this.sendHeartbeat()
    }

    handleSessionNextStepEnded(event: SessionNextStepEndedEvent["properties"]): void {
        const stepTokens = event.tokens.output
        if (stepTokens > 0) {
            this.state.tokens += stepTokens
            this.state.tokensToday += stepTokens
            this.debug("step tokens accumulated", { stepTokens, total: this.state.tokens })
        }
    }

    handleMessageUpdated(event: MessageUpdatedEvent["properties"]): void {
        const { info } = event
        if (info.role === "assistant") {
            const text = this.extractTextFromContent(info.content)
            if (text) {
                this.state.entries.push(text.slice(0, 80))
                if (this.state.entries.length > 20) {
                    this.state.entries.shift()
                }
            }
        }
    }

    handleMessagePartUpdated(event: MessagePartUpdatedEvent["properties"]): void {
        const { part } = event
        if (part.type === "text") {
            const turnEvent: TurnEvent = {
                content: [{ text: part.text.slice(0, 4096), type: "text" }],
                evt: "turn",
                role: "assistant",
            }
            this.send(turnEvent)
        }
    }

    handleTodoUpdated(_event: TodoUpdatedEvent["properties"]): void {
        this.sendHeartbeat()
    }

    handlePermissionAsked(event: PermissionAskedEvent["properties"]): void {
        const { id, sessionID, permission, patterns } = event
        this.pendingPermissions.set(id, {
            hint: patterns[0] || permission,
            tool: permission,
            sessionID,
            permissionID: id,
        })
        this.info("permission asked", { id, tool: permission, sessionID })
        this.sendHeartbeat()
    }

    handlePermissionReplied(event: PermissionRepliedEvent["properties"]): void {
        const { id } = event
        if (this.pendingPermissions.has(id)) {
            this.pendingPermissions.delete(id)
        } else {
            this.warn("permission.replied for unknown id", { id })
        }
        this.sendHeartbeat()
    }

    handlePermissionUpdated(_event: PermissionUpdatedEvent["properties"]): void {
        // No-op
    }

    handleFileEdited(_event: FileEditedEvent["properties"]): void {
        // No-op
    }

    handleCommandExecuted(_event: CommandExecutedEvent["properties"]): void {
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

    handleToolExecuteBefore(input: ToolExecuteInput): void {
        if (!this.sessionStatuses.has(input.sessionID)) {
            this.sessionStatuses.set(input.sessionID, "busy")
        }
        const entry = `${new Date().toLocaleTimeString()} ${input.tool}`
        this.state.entries.push(entry)
        if (this.state.entries.length > 20) {
            this.state.entries.shift()
        }
        this.sendHeartbeat()
    }

    handleToolExecuteAfter(_input: ToolExecuteInput): void {
        this.sendHeartbeat()
    }

    disconnect(): void {
        this.info("disconnecting")
        this.stopHeartbeat()
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer)
            this.reconnectTimer = null
        }
        if (this.completedTimer) {
            clearTimeout(this.completedTimer)
            this.completedTimer = null
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
        "session.status": async ({ event }: { event: SessionStatusEvent }) => {
            client.handleSessionStatus(event.properties)
        },
        "session.error": async ({ event }: { event: SessionErrorEvent }) => {
            client.handleSessionError(event.properties)
        },
        "session.idle": async ({ event }: { event: SessionIdleEvent }) => {
            client.handleSessionIdle(event.properties)
        },
        "session.created": async ({ event }: { event: SessionCreatedEvent }) => {
            client.handleSessionCreated(event.properties)
        },
        "session.deleted": async ({ event }: { event: SessionDeletedEvent }) => {
            client.handleSessionDeleted(event.properties)
        },
        "session.updated": async ({ event }: { event: SessionUpdatedEvent }) => {
            client.handleSessionUpdated(event.properties)
        },
        "session.next.step.ended": async ({ event }: { event: SessionNextStepEndedEvent }) => {
            client.handleSessionNextStepEnded(event.properties)
        },
        "message.updated": async ({ event }: { event: MessageUpdatedEvent }) => {
            client.handleMessageUpdated(event.properties)
        },
        "message.part.updated": async ({ event }: { event: MessagePartUpdatedEvent }) => {
            client.handleMessagePartUpdated(event.properties)
        },
        "todo.updated": async ({ event }: { event: TodoUpdatedEvent }) => {
            client.handleTodoUpdated(event.properties)
        },
        "permission.asked": async ({ event }: { event: PermissionAskedEvent }) => {
            client.handlePermissionAsked(event.properties)
        },
        "permission.replied": async ({ event }: { event: PermissionRepliedEvent }) => {
            client.handlePermissionReplied(event.properties)
        },
        "permission.updated": async ({ event }: { event: PermissionUpdatedEvent }) => {
            client.handlePermissionUpdated(event.properties)
        },
        "file.edited": async ({ event }: { event: FileEditedEvent }) => {
            client.handleFileEdited(event.properties)
        },
        "command.executed": async ({ event }: { event: CommandExecutedEvent }) => {
            client.handleCommandExecuted(event.properties)
        },
        "tool.execute.before": async (input: ToolExecuteInput, _output: unknown) => {
            client.handleToolExecuteBefore(input)
        },
        "tool.execute.after": async (input: ToolExecuteInput, _output: unknown) => {
            client.handleToolExecuteAfter(input)
        },
    }
}
