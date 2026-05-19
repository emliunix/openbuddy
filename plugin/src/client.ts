import * as net from "net"
import type { Event } from "@opencode-ai/sdk"
import type { Plugin } from "@opencode-ai/plugin"
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

type LogFn = (req: {
    body: {
        service: string
        level: "debug" | "info" | "warn" | "error"
        message: string
        extra?: Record<string, unknown>
    }
}) => Promise<unknown>

export class BuddyClient {
    private socket: net.Socket | null = null
    private log: LogFn | null = null
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

    async connect(ctx: Parameters<Plugin>[0]): Promise<void> {
        this.log = ctx.client.app.log.bind(ctx.client.app)
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
            const perm = msg as { cmd: "permission"; decision: string; id: string }
            this.info("permission decision", { decision: perm.decision, id: perm.id })
            this.pendingPermissions.delete(perm.id)
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

        this.warn("unknown buddy command", { cmd: (msg as any).cmd })
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

        this.send(heartbeat)
        this.debug("heartbeat sent", { running, waiting, total: this.state.total, completed: this.state.completed })
    }

    onEvent(event: Event): void {
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

                this.debug("session status", { sessionID, status: status.type, running: this.getRunningCount() })
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
                this.sendHeartbeat()
                break
            }

            case "session.created": {
                this.state.total += 1
                this.debug("session created", { total: this.state.total })
                this.sendHeartbeat()
                break
            }

            case "session.deleted": {
                const sessionID = (event.properties as any).sessionID as string
                this.sessionStatuses.delete(sessionID)
                this.pendingPermissions.delete(sessionID)
                this.state.total = Math.max(0, this.state.total - 1)
                this.sendHeartbeat()
                break
            }

            case "session.updated": {
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
                    this.send(turnEvent)
                }
                break
            }

            case "todo.updated": {
                this.sendHeartbeat()
                break
            }

            case "permission.updated":
            case "permission.replied":
            case "file.edited":
            case "command.executed":
                break

            default: {
                this.debug("unhandled event", { type: (event as any).type })
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
        this.info("permission asked", { id: input.id, tool: input.tool })
        this.sendHeartbeat()
    }

    onToolExecuteBefore(input: { tool: string; sessionID: string; callID: string }): void {
        const entry = `${new Date().toLocaleTimeString()} ${input.tool}`
        this.state.entries.push(entry)
        if (this.state.entries.length > 20) {
            this.state.entries.shift()
        }
        this.sendHeartbeat()
    }

    onToolExecuteAfter(input: { tool: string; sessionID: string; callID: string }): void {
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
