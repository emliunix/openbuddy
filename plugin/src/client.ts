import * as net from "net"
import type { Event } from "@opencode-ai/sdk"
import type { Logger } from "./logger.js"
import { AppLogger } from "./logger.js"
import {
    type Heartbeat,
    type TurnEvent,
    type BuddyMessage,
    encodeNdjson,
} from "./protocol.js"

const BUDDY_HOST = "127.0.0.1"
const BUDDY_PORT = 7887
const HEARTBEAT_INTERVAL_MS = 10000
const RECONNECT_DELAY_MS = 5000
const COMPLETED_PULSE_MS = 2000

interface PluginContextLike {
    client: {
        app: {
            log: (req: {
                body: {
                    service: string
                    level: "debug" | "info" | "warn" | "error"
                    message: string
                    extra: Record<string, unknown>
                }
            }) => Promise<unknown>
        }
    }
}

export class BuddyClient {
    private socket: net.Socket | null = null
    private ctx: PluginContextLike | null = null
    private logger: Logger = new AppLogger()
    private heartbeatTimer: NodeJS.Timeout | null = null
    private reconnectTimer: NodeJS.Timeout | null = null
    private completedTimer: NodeJS.Timeout | null = null
    private buffer = ""
    private connected = false

    private sessionStatuses = new Map<string, "idle" | "busy" | "retry">()
    private pendingPermissions = new Map<string, { hint: string; tool: string }>()

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

    async connect(ctx: PluginContextLike): Promise<void> {
        this.ctx = ctx
        ;(this.logger as AppLogger).setContext(ctx)
        this.logger.info("BuddyClient initializing", {
            host: BUDDY_HOST,
            port: BUDDY_PORT,
            heartbeatIntervalMs: HEARTBEAT_INTERVAL_MS,
        })
        await this.doConnect()
    }

    private async doConnect(): Promise<void> {
        if (this.socket) {
            this.logger.debug("Connection already in progress or established")
            return
        }

        try {
            this.logger.info("Attempting TCP connection to Buddy", {
                host: BUDDY_HOST,
                port: BUDDY_PORT,
            })

            this.socket = net.createConnection({
                host: BUDDY_HOST,
                port: BUDDY_PORT,
            })

            this.socket.on("connect", () => {
                this.connected = true
                this.logger.info("TCP connection established with Buddy")
                this.startHeartbeat()
                this.sendConnectInfo()
            })

            this.socket.on("data", (data) => {
                const chunk = data.toString("utf-8")
                this.logger.debug("Received raw data from Buddy", {
                    bytes: data.length,
                    preview: chunk.slice(0, 200),
                })
                this.buffer += chunk
                this.processBuffer()
            })

            this.socket.on("error", (err: Error) => {
                this.logger.error("TCP socket error", {
                    error: err.message,
                })
            })

            this.socket.on("close", (hadError: boolean) => {
                const wasConnected = this.connected
                this.connected = false
                this.socket = null
                this.stopHeartbeat()

                this.logger.warn("TCP connection closed", {
                    hadError,
                    wasConnected,
                })

                this.scheduleReconnect()
            })
        } catch (err) {
            this.logger.error("Failed to create TCP connection", {
                error: (err as Error).message,
            })
            this.scheduleReconnect()
        }
    }

    private processBuffer(): void {
        const lines = this.buffer.split("\n")
        this.buffer = lines.pop() || ""

        for (const line of lines) {
            if (!line.trim()) continue
            try {
                const msg: BuddyMessage = JSON.parse(line)
                this.logger.debug("Parsed Buddy message", { cmd: (msg as any).cmd })
                this.handleBuddyMessage(msg)
            } catch (err) {
                this.logger.error("Failed to parse NDJSON line from Buddy", {
                    line: line.slice(0, 500),
                    error: (err as Error).message,
                })
            }
        }
    }

    private handleBuddyMessage(msg: BuddyMessage): void {
        if (msg.cmd === "ping") {
            this.logger.debug("Received ping from Buddy")
            return
        }

        if (msg.cmd === "permission") {
            const perm = msg as { cmd: "permission"; decision: string; id: string }
            this.logger.info("Received permission decision from Buddy", {
                decision: perm.decision,
                id: perm.id,
            })
            this.pendingPermissions.delete(perm.id)
            this.sendHeartbeat()
            return
        }

        if (msg.cmd === "status") {
            this.logger.debug("Received status request from Buddy")
            this.sendStatus()
            return
        }

        if (msg.cmd === "name") {
            const nameCmd = msg as { cmd: "name"; name: string }
            this.sendAck("name", true)
            this.logger.info("Buddy name acknowledged", { name: nameCmd.name })
            return
        }

        if (msg.cmd === "owner") {
            const ownerCmd = msg as { cmd: "owner"; name: string }
            this.sendAck("owner", true)
            this.logger.info("Buddy owner acknowledged", { name: ownerCmd.name })
            return
        }

        if (msg.cmd === "unpair") {
            this.sendAck("unpair", true)
            this.logger.info("Buddy unpair acknowledged")
            return
        }

        if (msg.cmd === "char_begin" || msg.cmd === "file" || msg.cmd === "chunk" || msg.cmd === "file_end" || msg.cmd === "char_end") {
            this.logger.debug("Received folder push command", { cmd: msg.cmd })
            return
        }

        this.logger.warn("Received unknown Buddy command", { cmd: (msg as any).cmd })
    }

    private send(obj: unknown): boolean {
        if (!this.socket || this.socket.destroyed) {
            this.logger.debug("Cannot send, socket not available")
            return false
        }
        const data = encodeNdjson(obj)
        this.socket.write(data)
        this.logger.debug("Sent to Buddy", { type: (obj as any).evt || (obj as any).cmd || "heartbeat" })
        return true
    }

    private sendConnectInfo(): void {
        const now = new Date()
        const timeMsg = {
            time: [
                Math.floor(now.getTime() / 1000),
                -now.getTimezoneOffset() * 60,
            ],
        }
        this.send(timeMsg)
        this.logger.info("Sent connect info (time sync)")
    }

    private sendStatus(): void {
        const ok = this.send({
            ack: "status",
            data: {
                name: "OpenBuddy",
                sec: true,
                stats: { appr: 0, deny: 0, lvl: 1, nap: 0, vel: 0 },
                sys: { heap: 0, up: 0 },
            },
            ok: true,
        })
        if (ok) {
            this.logger.debug("Sent status response")
        }
    }

    private sendAck(cmd: string, ok: boolean): void {
        this.send({ ack: cmd, ok })
    }

    private startHeartbeat(): void {
        this.stopHeartbeat()
        this.logger.info("Starting heartbeat", { intervalMs: HEARTBEAT_INTERVAL_MS })
        this.heartbeatTimer = setInterval(() => {
            this.sendHeartbeat()
        }, HEARTBEAT_INTERVAL_MS)
        this.sendHeartbeat()
    }

    private stopHeartbeat(): void {
        if (this.heartbeatTimer) {
            clearInterval(this.heartbeatTimer)
            this.heartbeatTimer = null
            this.logger.debug("Heartbeat stopped")
        }
    }

    private scheduleReconnect(): void {
        if (this.reconnectTimer) {
            this.logger.debug("Reconnect already scheduled")
            return
        }
        this.logger.info("Scheduling reconnect", { delayMs: RECONNECT_DELAY_MS })
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null
            this.logger.info("Attempting reconnect")
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
        return { id, ...perm }
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

        const ok = this.send(heartbeat)
        if (ok) {
            this.logger.debug("Heartbeat sent", {
                running,
                waiting,
                total: this.state.total,
                completed: this.state.completed,
                entriesCount: heartbeat.entries?.length || 0,
            })
        }
    }

    onEvent(event: Event): void {
        this.logger.debug("OpenCode event received", { type: event.type })

        switch (event.type) {
            case "session.status": {
                const { sessionID, status } = event.properties as { sessionID: string; status: { type: "idle" | "busy" | "retry" } }
                const oldStatus = this.sessionStatuses.get(sessionID)
                this.sessionStatuses.set(sessionID, status.type)

                if (status.type === "busy" && oldStatus !== "busy") {
                    this.state.currentMsg = "working..."
                } else if (status.type === "idle") {
                    this.sessionStatuses.delete(sessionID)
                    this.state.currentMsg = undefined
                    if (oldStatus === "busy" || oldStatus === "retry") {
                        this.setCompleted()
                    }
                }

                this.logger.info("Session status changed", {
                    sessionID,
                    status: status.type,
                    running: this.getRunningCount(),
                })
                this.sendHeartbeat()
                break
            }

            case "session.idle": {
                const { sessionID } = event.properties as { sessionID: string }
                const wasBusy = this.sessionStatuses.get(sessionID) === "busy" || this.sessionStatuses.get(sessionID) === "retry"
                this.sessionStatuses.delete(sessionID)
                this.pendingPermissions.delete(sessionID)
                this.state.currentMsg = undefined
                if (wasBusy) {
                    this.setCompleted()
                }
                this.logger.info("Session idle", { sessionID, running: this.getRunningCount() })
                this.sendHeartbeat()
                break
            }

            case "session.created": {
                this.state.total += 1
                this.logger.info("Session created", {
                    sessionID: (event.properties as any).sessionID,
                    totalSessions: this.state.total,
                })
                this.sendHeartbeat()
                break
            }

            case "session.deleted": {
                const sessionID = (event.properties as any).sessionID as string
                this.sessionStatuses.delete(sessionID)
                this.pendingPermissions.delete(sessionID)
                this.state.total = Math.max(0, this.state.total - 1)
                this.logger.info("Session deleted", { sessionID, totalSessions: this.state.total })
                this.sendHeartbeat()
                break
            }

            case "session.updated": {
                this.logger.debug("Session updated", { sessionID: (event.properties as any).info?.id })
                this.sendHeartbeat()
                break
            }

            case "message.updated": {
                const msg = (event.properties as any).info
                if (msg?.role === "assistant" && msg?.content) {
                    const text = this.extractTextFromContent(msg.content)
                    if (text) {
                        this.state.entries.push(text.slice(0, 80))
                        if (this.state.entries.length > 20) {
                            this.state.entries.shift()
                        }
                    }
                }
                this.logger.debug("Message updated", { sessionID: msg?.sessionID, messageID: msg?.id })
                break
            }

            case "message.part.updated": {
                const part = (event.properties as any).part
                if (part?.type === "text" && part?.text) {
                    const turnEvent: TurnEvent = {
                        content: [{ text: part.text.slice(0, 4096), type: "text" }],
                        evt: "turn",
                        role: "assistant",
                    }
                    this.logger.debug("Sending turn event to Buddy", {
                        partID: part.id,
                        textLength: part.text.length,
                    })
                    this.send(turnEvent)
                }
                break
            }

            case "todo.updated": {
                this.logger.debug("Todo updated")
                this.sendHeartbeat()
                break
            }

            case "permission.updated": {
                this.logger.debug("Permission updated")
                break
            }

            case "permission.replied": {
                this.logger.debug("Permission replied")
                break
            }

            case "file.edited": {
                this.logger.debug("File edited")
                break
            }

            case "command.executed": {
                this.logger.debug("Command executed")
                break
            }

            default: {
                this.logger.debug("Unhandled event type", { type: (event as any).type })
            }
        }
    }

    private extractTextFromContent(content: unknown): string | undefined {
        if (typeof content === "string") return content
        if (Array.isArray(content)) {
            for (const part of content) {
                if (part?.type === "text" && part?.text) {
                    return part.text
                }
            }
        }
        return undefined
    }

    onPermissionAsk(input: { id?: string; tool?: string; hint?: string }): void {
        if (!input.id) return
        this.pendingPermissions.set(input.id, {
            hint: input.hint || "",
            tool: input.tool || "unknown",
        })
        this.logger.info("Permission asked", {
            id: input.id,
            tool: input.tool,
            waiting: this.pendingPermissions.size,
        })
        this.sendHeartbeat()
    }

    onToolExecuteBefore(input: { tool: string; sessionID: string; callID: string }): void {
        const entry = `${new Date().toLocaleTimeString()} ${input.tool}`
        this.state.entries.push(entry)
        if (this.state.entries.length > 20) {
            this.state.entries.shift()
        }
        this.logger.info("Tool execute started", {
            tool: input.tool,
            sessionID: input.sessionID,
            callID: input.callID,
        })
        this.sendHeartbeat()
    }

    onToolExecuteAfter(input: { tool: string; sessionID: string; callID: string }): void {
        this.logger.info("Tool execute finished", {
            tool: input.tool,
            sessionID: input.sessionID,
            callID: input.callID,
        })
        this.sendHeartbeat()
    }

    disconnect(): void {
        this.logger.info("Disconnecting from Buddy")
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
