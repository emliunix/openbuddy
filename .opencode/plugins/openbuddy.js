// OpenBuddy Plugin - Bundled
import * as net from "net"

// ===== protocol =====
function encodeNdjson(obj) {
    return JSON.stringify(obj) + "\n";
}

// ===== logger =====
const LOG_LEVEL_PRIORITY = {
    debug: 0,
    info: 1,
    warn: 2,
    error: 3,
};
function getConfiguredLogLevel() {
    const env = process.env.OPENBUDDY_LOG_LEVEL;
    if (env && env in LOG_LEVEL_PRIORITY) {
        return env;
    }
    return "info";
}
class AppLogger {
    ctx = null;
    minLevel = getConfiguredLogLevel();
    buffer = [];
    bufferLimit = 100;
    consoleFallback = true;
    setContext(ctx) {
        this.ctx = ctx;
        this.flushBuffer();
    }
    setMinLevel(level) {
        this.minLevel = level;
    }
    setConsoleFallback(enabled) {
        this.consoleFallback = enabled;
    }
    shouldLog(level) {
        return LOG_LEVEL_PRIORITY[level] >= LOG_LEVEL_PRIORITY[this.minLevel];
    }
    async sendLog(level, message, extra) {
        const entry = {
            service: "openbuddy",
            level,
            message,
            extra: {
                ...extra,
                _timestamp: new Date().toISOString(),
            },
        };
        if (this.ctx) {
            try {
                await this.ctx.client.app.log({ body: entry });
            }
            catch {
                if (this.consoleFallback) {
                    console.error("[OpenBuddy] Failed to send log, falling back to console");
                    this.consoleLog(level, message, extra);
                }
            }
        }
        else {
            this.buffer.push({ level, message, extra, timestamp: new Date() });
            if (this.buffer.length > this.bufferLimit) {
                this.buffer.shift();
            }
            if (this.consoleFallback) {
                this.consoleLog(level, message, extra);
            }
        }
    }
    consoleLog(level, message, extra) {
        const prefix = `[OpenBuddy:${level.toUpperCase()}]`;
        if (Object.keys(extra).length > 0) {
            console.log(prefix, message, extra);
        }
        else {
            console.log(prefix, message);
        }
    }
    flushBuffer() {
        const buffered = this.buffer;
        this.buffer = [];
        for (const entry of buffered) {
            this.sendLog(entry.level, entry.message, entry.extra);
        }
    }
    debug(message, extra) {
        if (!this.shouldLog("debug"))
            return;
        this.sendLog("debug", message, extra || {});
    }
    info(message, extra) {
        if (!this.shouldLog("info"))
            return;
        this.sendLog("info", message, extra || {});
    }
    warn(message, extra) {
        if (!this.shouldLog("warn"))
            return;
        this.sendLog("warn", message, extra || {});
    }
    error(message, extra) {
        if (!this.shouldLog("error"))
            return;
        this.sendLog("error", message, extra || {});
    }
}

// ===== client =====
const BUDDY_HOST = "127.0.0.1";
const BUDDY_PORT = 7887;
const HEARTBEAT_INTERVAL_MS = 10000;
const RECONNECT_DELAY_MS = 5000;
class BuddyClient {
    socket = null;
    ctx = null;
    logger = new AppLogger();
    heartbeatTimer = null;
    reconnectTimer = null;
    buffer = "";
    connected = false;
    // Per-session state: Map<sessionID, {running: bool, waiting: int}>
    // Aggregated counts are derived on each heartbeat send.
    sessionStates = new Map();
    erroredSessions = new Set();  // sessions interrupted by user abort
    pendingPermissions = new Map();  // permissionID -> { sessionID, permissionID }
    state = {
        entries: [],
        total: 0,           // count of open sessions (session.created / session.deleted)
        completed: false,   // one-shot: true for one heartbeat after session.idle
        tokens: 0,  // instance-lifetime accumulated output tokens; duped into tokens_today (no account-level data)
    };
    // Derived aggregates from sessionStates
    get running() {
        let n = 0;
        for (const s of this.sessionStates.values()) if (s.running) n++;
        return n;
    }
    get waiting() {
        let n = 0;
        for (const s of this.sessionStates.values()) n += s.waiting;
        return n;
    }
    _ensureSession(id) {
        if (!this.sessionStates.has(id)) {
            this.sessionStates.set(id, { running: false, waiting: 0 });
        }
        return this.sessionStates.get(id);
    }
    _permissionEntry(perm) {
        const description = perm.permission || "";
        return {
            sessionID: perm.sessionID,
            permissionID: perm.id,
            description,
            hint: perm.patterns?.[0] || description,
            patterns: perm.patterns || [],
            prompt: {
                hint: perm.patterns?.[0] || description,
                id: perm.id,
                tool: description,
            },
            msg: `approve: ${description}`,
        };
    }
    _updatePermissionPrompt() {
        if (this.pendingPermissions.size === 0) {
            this.state.currentPrompt = undefined;
            this.state.currentMsg = undefined;
            return;
        }
        const first = this.pendingPermissions.values().next().value;
        this.state.currentPrompt = first.prompt;
        this.state.currentMsg = first.msg;
    }
    _clearPendingForSession(sessionID) {
        for (const [id, pp] of this.pendingPermissions) {
            if (pp.sessionID === sessionID) this.pendingPermissions.delete(id);
        }
    }
    async connect(ctx) {
        this.ctx = ctx;
        this.logger.setContext(ctx);
        this.logger.info("BuddyClient initializing", {
            host: BUDDY_HOST,
            port: BUDDY_PORT,
            heartbeatIntervalMs: HEARTBEAT_INTERVAL_MS,
        });
        await this.doConnect();
    }
    async doConnect() {
        if (this.socket) {
            this.logger.debug("Connection already in progress or established");
            return;
        }
        try {
            this.logger.info("Attempting TCP connection to Buddy", {
                host: BUDDY_HOST,
                port: BUDDY_PORT,
            });
            this.socket = net.createConnection({
                host: BUDDY_HOST,
                port: BUDDY_PORT,
            });
            this.socket.on("connect", () => {
                this.connected = true;
                this.logger.info("TCP connection established with Buddy");
                this.startHeartbeat();
                this.sendConnectInfo();
            });
            this.socket.on("data", (data) => {
                const chunk = data.toString("utf-8");
                this.logger.debug("Received raw data from Buddy", {
                    bytes: data.length,
                    preview: chunk.slice(0, 200),
                });
                this.buffer += chunk;
                this.processBuffer();
            });
            this.socket.on("error", (err) => {
                this.logger.error("TCP socket error", {
                    error: err.message,
                });
            });
            this.socket.on("close", (hadError) => {
                const wasConnected = this.connected;
                this.connected = false;
                this.socket = null;
                this.stopHeartbeat();
                this.logger.warn("TCP connection closed", {
                    hadError,
                    wasConnected,
                });
                this.scheduleReconnect();
            });
        }
        catch (err) {
            this.logger.error("Failed to create TCP connection", {
                error: err.message,
            });
            this.scheduleReconnect();
        }
    }
    processBuffer() {
        const lines = this.buffer.split("\n");
        this.buffer = lines.pop() || "";
        for (const line of lines) {
            if (!line.trim())
                continue;
            try {
                const msg = JSON.parse(line);
                this.logger.debug("Parsed Buddy message", { cmd: msg.cmd });
                this.handleBuddyMessage(msg);
            }
            catch (err) {
                this.logger.error("Failed to parse NDJSON line from Buddy", {
                    line: line.slice(0, 500),
                    error: err.message,
                });
            }
        }
    }
    handleBuddyMessage(msg) {
        if (msg.cmd === "ping") {
            this.logger.debug("Received ping from Buddy");
            return;
        }
        if (msg.cmd === "permission") {
            const decision = msg.decision;
            const pp = this.pendingPermissions.get(msg.id);
            if (!pp) {
                this.logger.warn("Permission decision for unknown id", { decision, id: msg.id });
                return;
            }
            this.pendingPermissions.delete(msg.id);
            const response = decision === "deny" ? "reject" : "once";
            this.ctx.client.postSessionIdPermissionsPermissionId({
                path: { id: pp.sessionID, permissionID: pp.permissionID },
                body: { response },
            }).then(() => {
                const sr = this.sessionStates.get(pp.sessionID);
                if (sr && sr.waiting > 0) sr.waiting -= 1;
                this._updatePermissionPrompt();
                this.sendHeartbeat();
                this.logger.info("Permission decision relayed via API", {
                    sessionID: pp.sessionID,
                    permissionID: pp.permissionID,
                    decision,
                });
            }).catch((err) => {
                this.logger.error("Permission API reply failed", { error: err.message });
            });
            return;
        }
        this.logger.warn("Received unknown Buddy command", { cmd: msg.cmd });
    }
    send(obj) {
        if (!this.socket || this.socket.destroyed) {
            this.logger.debug("Cannot send, socket not available");
            return false;
        }
        const data = encodeNdjson(obj);
        this.socket.write(data);
        this.logger.debug("Sent to Buddy", { type: obj.evt || obj.cmd || "heartbeat" });
        return true;
    }
    sendConnectInfo() {
        const now = new Date();
        const timeMsg = {
            time: [
                Math.floor(now.getTime() / 1000),
                -now.getTimezoneOffset() * 60,
            ],
        };
        this.send(timeMsg);
        this.logger.info("Sent connect info (time sync)");
    }
    sendStatus() {
        const ok = this.send({
            ack: "status",
            data: {
                name: "OpenBuddy",
                sec: true,
                stats: { appr: 0, deny: 0, lvl: 1, nap: 0, vel: 0 },
                sys: { heap: 0, up: 0 },
            },
            ok: true,
        });
        if (ok) {
            this.logger.debug("Sent status response");
        }
    }
    sendAck(cmd, ok) {
        this.send({ ack: cmd, ok });
    }
    startHeartbeat() {
        this.stopHeartbeat();
        this.logger.info("Starting heartbeat", { intervalMs: HEARTBEAT_INTERVAL_MS });
        this.heartbeatTimer = setInterval(() => {
            this.sendHeartbeat();
        }, HEARTBEAT_INTERVAL_MS);
        this.sendHeartbeat();
    }
    stopHeartbeat() {
        if (this.heartbeatTimer) {
            clearInterval(this.heartbeatTimer);
            this.heartbeatTimer = null;
            this.logger.debug("Heartbeat stopped");
        }
    }
    scheduleReconnect() {
        if (this.reconnectTimer) {
            this.logger.debug("Reconnect already scheduled");
            return;
        }
        this.logger.info("Scheduling reconnect", { delayMs: RECONNECT_DELAY_MS });
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.logger.info("Attempting reconnect");
            this.doConnect();
        }, RECONNECT_DELAY_MS);
    }
    sendHeartbeat() {
        const heartbeat = {
            entries: this.state.entries.slice(-5),
            running: this.running,
            waiting: this.waiting,
            total: this.state.total,
            completed: this.state.completed,
            tokens: this.state.tokens,
            tokens_today: this.state.tokens,
        };
        if (this.state.currentMsg) {
            heartbeat.msg = this.state.currentMsg;
        }
        if (this.state.currentPrompt) {
            heartbeat.prompt = this.state.currentPrompt;
        }
        const ok = this.send(heartbeat);
        if (this.state.completed) this.state.completed = false;
        if (ok) {
            this.logger.debug("Heartbeat sent", {
                running: heartbeat.running,
                waiting: heartbeat.waiting,
                total: heartbeat.total,
                entriesCount: heartbeat.entries?.length || 0,
            });
        }
    }
    onEvent(event) {
        this.logger.debug("OpenCode event received", { type: event.type });
        this.logger.info("EVENT", { type: event.type, keys: Object.keys(event.properties || {}), sample: JSON.stringify(event.properties).slice(0, 500) });
        switch (event.type) {
            // session.status {type:"idle"} is the canonical handler; this is a safety net
            case "session.error": {
                const sessionID = event.properties.sessionID;
                const error = event.properties.error;
                if (sessionID && error?.name === "MessageAbortedError") {
                    this.erroredSessions.add(sessionID);
                    this.logger.info("Session aborted, skipping completed flag", { sessionID });
                }
                break;
            }
            case "session.idle": {
                this.logger.debug("Session idle event (deprecated)", {
                    sessionID: event.properties.sessionID,
                });
                break;
            }
            case "session.created": {
                const sessionID = event.properties.info.id;
                this.state.total += 1;
                this._ensureSession(sessionID);
                this.logger.info("Session created", {
                    sessionID,
                    total: this.state.total,
                });
                this.sendHeartbeat();
                break;
            }
            case "session.deleted": {
                const sessionID = event.properties.info?.id || event.properties.sessionID;
                this.state.total = Math.max(0, this.state.total - 1);
                this.sessionStates.delete(sessionID);
                this._clearPendingForSession(sessionID);
                this._updatePermissionPrompt();
                this.logger.info("Session deleted", {
                    sessionID,
                    total: this.state.total,
                });
                this.sendHeartbeat();
                break;
            }
            case "session.updated": {
                this.logger.debug("Session updated", { sessionID: event.properties.info.id });
                this.sendHeartbeat();
                break;
            }
            case "session.next.step.ended": {
                const stepTokens = event.properties?.tokens?.output ?? 0;
                if (stepTokens > 0) {
                    this.state.tokens += stepTokens;
                    this.logger.debug("Step tokens accumulated", { stepTokens, total: this.state.tokens });
                }
                break;
            }
            case "session.status": {
                const sessionID = event.properties.sessionID;
                const status = event.properties.status;
                this.logger.debug("Session status changed", { sessionID, status: status.type });
                const s = this._ensureSession(sessionID);
                if (status.type === "busy" || status.type === "retry") {
                    if (!s.running) {
                        s.running = true;
                        this.logger.info("Session running (busy)", { sessionID });
                        this.sendHeartbeat();
                    }
                } else if (status.type === "idle") {
                    if (!this.erroredSessions.has(sessionID)) {
                        this.state.completed = true;
                    }
                    this.erroredSessions.delete(sessionID);
                    this._clearPendingForSession(sessionID);
                    s.running = false;
                    s.waiting = 0;
                    this._updatePermissionPrompt();
                    this.sendHeartbeat();
                }
                break;
            }
            case "message.updated": {
                const msg = event.properties.info;
                if (msg.role === "assistant") {
                    this.logger.debug("Assistant message updated", {
                        sessionID: msg.sessionID,
                        messageID: msg.id,
                    });
                }
                break;
            }
            case "message.part.updated": {
                const part = event.properties.part;
                if (part.type === "text" && part.text) {
                    const turnEvent = {
                        content: [{ text: part.text.slice(0, 4096), type: "text" }],
                        evt: "turn",
                        role: "assistant",
                    };
                    this.logger.info("Sending turn event to Buddy", {
                        partID: part.id,
                        textLength: part.text.length,
                    });
                    this.send(turnEvent);
                }
                break;
            }
            case "todo.updated": {
                this.logger.debug("Todo updated");
                this.sendHeartbeat();
                break;
            }
            case "permission.updated": {
                this.logger.debug("Permission updated");
                break;
            }
            case "permission.asked": {
                const perm = event.properties;
                const s = this._ensureSession(perm.sessionID);
                s.waiting += 1;
                this.pendingPermissions.set(perm.id, this._permissionEntry(perm));
                this.logger.info("Permission asked (bus)", {
                    id: perm.id,
                    sessionID: perm.sessionID,
                    permission: perm.permission,
                    patterns: perm.patterns,
                });
                this._updatePermissionPrompt();
                this.sendHeartbeat();
                break;
            }
            case "permission.replied": {
                const sessionID = event.properties.sessionID;
                this.logger.debug("Permission replied", { sessionID });
                const sr = sessionID ? this.sessionStates.get(sessionID) : null;
                if (sr && sr.waiting > 0) {
                    sr.waiting -= 1;
                }
                // Remove first entry from queue (FIFO assumption for external replies)
                const firstKey = this.pendingPermissions.keys().next().value;
                if (firstKey) this.pendingPermissions.delete(firstKey);
                this._updatePermissionPrompt();
                this.sendHeartbeat();
                break;
            }
            case "file.edited": {
                this.logger.debug("File edited");
                break;
            }
            case "command.executed": {
                this.logger.debug("Command executed");
                break;
            }
            default: {
                this.logger.debug("Unhandled event type", { type: event.type });
            }
        }
    }
    onToolExecuteBefore(input) {
        const sessionID = input.sessionID || "__default__";
        const s = this._ensureSession(sessionID);
        s.running = true;
        this.logger.info("Tool execute started", {
            tool: input.tool,
            sessionID,
            callID: input.callID,
            running: this.running,
        });
        this.sendHeartbeat();
    }
    onToolExecuteAfter(input) {
        const sessionID = input.sessionID || "__default__";
        const s = this._ensureSession(sessionID);
        const tool = input.tool || "";
        const args = input.args || {};
        let summary;
        const t = tool.toLowerCase();
        if (t === "bash" && args.command) {
            summary = String(args.command).slice(0, 30).replace(/\n.*/s, "");
        } else if ((t === "edit" || t === "write" || t === "read") && args.filePath) {
            const base = String(args.filePath).replace(/.*\//, "");
            summary = `${tool} ${base}`;
        } else {
            summary = tool;
        }
        const now = new Date();
        const time = `${String(now.getHours()).padStart(2,'0')}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`;
        const entry = `${time} ${summary}`;
        this.state.entries.push(entry);
        this.logger.info("Tool execute finished", {
            tool: input.tool,
            sessionID,
            callID: input.callID,
            running: this.running,
        });
        this.sendHeartbeat();
    }
    disconnect() {
        this.logger.info("Disconnecting from Buddy");
        this.stopHeartbeat();
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        this.pendingPermissions.clear();
        this.state.currentPrompt = undefined;
        this.state.currentMsg = undefined;
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
        this.connected = false;
    }
}

// ===== index =====
let buddyClient = null;
function getBuddyClient() {
    if (!buddyClient) {
        buddyClient = new BuddyClient();
    }
    return buddyClient;
}
export const OpenBuddyPlugin = async (ctx) => {
    const client = getBuddyClient();
    await client.connect(ctx);
    return {
        event: async ({ event }) => {
            client.onEvent(event);
        },
        "tool.execute.before": async (input, output) => {
            client.onToolExecuteBefore(input);
        },
        "tool.execute.after": async (input, output) => {
            client.onToolExecuteAfter(input);
        },
    };
};

