# OpenCode Hooks Manifest

How each wire protocol message maps to OpenCode APIs, events, and state.

## Plugin Architecture

The OpenCode plugin is a **TUI plugin** that connects to the Buddy TCP server and translates between OpenCode's internal event bus and the Buddy wire protocol.

```
OpenCode Core → Event Bus → TUI Plugin → TCP Client → Buddy TCP Server
```

**Plugin entrypoint**: `@opencode-ai/plugin/tui`  
**API docs**: `packages/opencode/specs/tui-plugins.md`

---

## Status Tracking Architecture

The Buddy protocol has a **heartbeat-centric** status model. The plugin maintains session state internally and derives wire protocol fields.

### Session State Machine

```
session.created  →  session.status:"busy"  →  session.status:"idle"  →  session.deleted
                        │                         │
                        │                         └─ completed=true (celebrate)
                        │
                        └─ running += 1 (if busy/retry)
```

**Key insight**: `running` counts **busy/retry sessions**, not individual tool executions. This matches the upstream desktop app where multiple Claude sessions can be active simultaneously.

### State Map

| Wire Field | Source | Update Trigger |
|------------|--------|----------------|
| `running` | Count of sessions with `status.type === "busy" \| "retry"` | `session.status` |
| `waiting` | Count of pending permission requests | `permission.asked` / `permission` cmd |
| `total` | Session counter (created++ / deleted--) | `session.created` / `session.deleted` |
| `completed` | `true` for 2s after session goes idle from busy | `session.status` → idle |
| `msg` | Current activity summary | Derived from running/waiting |
| `entries` | Tool name + timestamp | `tool.execute.before` |
| `prompt` | Pending permission info | `permission.asked` |

---

## Wire Protocol → Hook Mapping

### 1. Heartbeat

**Message**: `{"total": 3, "running": 1, "waiting": 1, "msg": "...", "entries": [...], "tokens": N, "tokens_today": N, "completed": true/false, "prompt": {...}}`

**How to build it:**

| Field | Hook | Implementation |
|-------|------|----------------|
| `total` | `session.created` / `session.deleted` | Increment/decrement counter |
| `running` | `session.status` | Count sessions with `status.type === "busy"` or `"retry"` |
| `waiting` | `permission.asked` | Count pending permissions in Map |
| `completed` | `session.status` → idle | Set `true` when session transitions busy→idle, auto-clear after 2s |
| `msg` | Derived | Priority: explicit msg > permission prompt > running count |
| `entries` | `tool.execute.before` | `${time} ${toolName}` format |
| `tokens` | Not available | Omitted (upstream tracks internally) |
| `tokens_today` | Not available | Omitted |
| `prompt` | `permission.asked` | Map to `{id, tool, hint}` |

**Implementation:**
```typescript
private sessionStatuses = new Map<string, "idle" | "busy" | "retry">()
private pendingPermissions = new Map<string, {hint: string, tool: string}>()

// running = count of busy/retry sessions
private getRunningCount(): number {
    let count = 0
    for (const status of this.sessionStatuses.values()) {
        if (status === "busy" || status === "retry") count++
    }
    return count
}
```

---

### 2. Turn Event

**Message**: `{"evt": "turn", "role": "assistant", "content": [...]}`

**How to build it:**

| Field | Hook | Implementation |
|-------|------|----------------|
| `evt` | Hardcoded | `"turn"` |
| `role` | `message.part.updated` | `"assistant"` |
| `content` | `message.part.updated` | Text parts from assistant messages |

**Note**: Upstream device code currently ignores TurnEvents (falls through to heartbeat parsing). They are documented in REFERENCE.md but not rendered. The plugin sends them anyway to comply with the protocol spec.

**Implementation:**
```typescript
case "message.part.updated": {
    const part = event.properties.part
    if (part?.type === "text" && part?.text) {
        this.send({ evt: "turn", role: "assistant", content: [{text: part.text.slice(0, 4096), type: "text"}] })
    }
}
```

---

### 3. Time Sync

**Message**: `{"time": [epoch_sec, tz_offset_sec]}`

Sent once on TCP connect. Buddy updates its internal clock (used for display timeouts).

---

### 4. Permission Decision (Buddy → Plugin)

**Message**: `{"cmd": "permission", "id": "req_abc123", "decision": "once"}`

**Flow**: uses the v1 plugin hook promise-pipe pattern.

```
1. opencode calls permission.ask hook (input, output)
2. plugin sets output.status = "ask", stores {output,resolve,sessionID} in deferreds map
3. plugin sends heartbeat with prompt/waiting to buddy, returns pending Promise
4. opencode blocks on Promise (plugin/index.ts:271 — Effect.promise awaits hook)
5. buddy shows approval UI → user presses Y/N
6. buddy → plugin TCP: {"cmd":"permission","decision":"once","id":"..."}
7. plugin resolves Promise: output.status = "allow" (or "deny")
8. opencode resumes, reads output.status → proceeds or rejects
```

**Field mapping**:

| Wire field | V1 SDK Permission field | Notes |
|------------|------------------------|-------|
| `id` | `input.id` | — |
| `tool` | `input.title` | `title` is human-readable name (e.g. "Bash") |
| `hint` | `input.pattern` | First pattern if array, else pattern string |
| `decision` | mapped to `output.status` | `"once"` → `"allow"`, `"deny"` → `"deny"` |

**Stale deferred cleanup**: rejected with `"deny"` on `disconnect()`, `session.status idle`, `session.idle`, or `session.deleted`.

---

### 5. Status Command

**Message**: `{"cmd": "status"}` → `{"ack": "status", "ok": true, "data": {...}}`

Static response with Buddy metadata (name, security flag, dummy stats).

---

## Event Subscription Strategy

### Required Subscriptions

```typescript
// Session lifecycle — drives running/total/completed
api.event.on("session.status", handleSessionStatus)
api.event.on("session.idle", handleSessionIdle)
api.event.on("session.created", handleSessionCreated)
api.event.on("session.deleted", handleSessionDeleted)

// Permissions — drives waiting/prompt
api.event.on("permission.asked", handlePermissionAsked)

// Tool execution — drives entries (transcript)
api.event.on("tool.execute.before", handleToolExecuteBefore)
```

### Polling Strategy

| Data | Source | Notes |
|------|--------|-------|
| Session status | Event-driven | `session.status` is reliable |
| Tool names | Hook intercept | `tool.execute.before` provides tool name |
| Permission queue | Hook intercept | `permission.asked` provides request ID |
| Connection health | 10s keepalive | Heartbeat timer |

---

## Known Differences from Upstream Desktop App

### Busy Threshold

**Upstream desktop**: Claude desktop can have multiple concurrent sessions → `running >= 3` threshold makes sense (tool calls fan out across sessions).

**OpenBuddy**: Uses `running >= 1` — any tool call executing shows `P_BUSY`. This is intentional: not all models/integrations support parallel tool execution, so `>= 1` gives useful feedback for single-tool-at-a-time workflows.
- `P_BUSY` when any tool call is executing
- `P_ATTENTION` when permission pending
- `P_CELEBRATE` briefly when session completes
2. Lower threshold to `>= 1` for CLI (deviates from upstream)

---

## Data Availability Matrix

| Wire Field | Event | State | HTTP API | Available? |
|------------|-------|-------|----------|------------|
| `total` | `session.created/deleted` | — | — | ✅ Yes |
| `running` | `session.status` | `session.status()` | — | ✅ Yes |
| `waiting` | `permission.asked` | — | — | ✅ Yes |
| `completed` | `session.status` → idle | — | — | ✅ Derived |
| `msg` | Derived | — | — | ✅ Buildable |
| `entries` | `tool.execute.before` | — | — | ✅ Yes |
| `tokens` | — | — | — | ❌ Not available |
| `tokens_today` | — | — | — | ❌ Not available |
| `prompt.id` | `permission.asked` | — | — | ✅ Yes |
| `prompt.tool` | `permission.asked` | — | — | ✅ Yes |
| `prompt.hint` | `permission.asked` | — | — | ✅ Yes |
| `evt:turn` | `message.part.updated` | — | — | ✅ Sent (device ignores) |
| `owner` | — | — | — | ⚠️ Not directly |
| `time` | — | `Date.now()` | — | ✅ Yes |

---

## Plugin Lifecycle

```typescript
const tui: TuiPlugin = async (api, options, meta) => {
  // 1. Read config
  const buddyHost = options.host || "localhost"
  const buddyPort = options.port || 7887

  // 2. Connect to Buddy TCP server
  const client = new BuddyClient(buddyHost, buddyPort)
  await client.connect()

  // 3. Send connect one-shots
  client.sendTimeSync()

  // 4. Subscribe to OpenCode events
  const unsubStatus = api.event.on("session.status", (evt) => {
    client.onEvent(evt)
  })

  const unsubPermission = api.event.on("permission.asked", (evt) => {
    client.onPermissionAsk(evt.properties)
  })

  // 5. Read Buddy responses
  client.onMessage((msg) => {
    if (msg.cmd === "permission") {
      // Forward to OpenCode
    }
  })

  // 6. Cleanup on plugin deactivate
  api.lifecycle.onDispose(() => {
    unsubStatus()
    unsubPermission()
    client.disconnect()
  })
}
```

---

## Implementation Priority

### P0 (MVP) - DONE
1. ✅ `session.status` event → heartbeat `running`
2. ✅ `permission.asked` event → heartbeat `waiting`/`prompt`
3. ✅ Buddy `permission` cmd → forward decision
4. ✅ 10s keepalive heartbeat
5. ✅ Time sync on connect
6. ✅ `completed` flag → celebrate animation
7. ✅ `session.created/deleted` → `total` tracking

### P1
8. `entries` from tool execution with output content
9. `question.*` events → contribute to "waiting" count
10. `name`/`owner` command handling

### P2
11. Turn events (protocol-compliant but device ignores)
12. Token tracking (local plugin KV approximation)
13. Folder push protocol
14. `species` command handling

---

## Notes

- **TCP vs BLE**: Upstream uses BLE with NUS. We use TCP localhost:7887. Wire format (NDJSON) is identical.
- **Turn Events**: Sent per protocol spec. Current upstream device code silently ignores them (falls through to heartbeat parsing). Future device firmware may use them.
- **Busy Threshold**: OpenBuddy uses `running >= 1` (upstream uses `>= 3` targeting Claude desktop's parallel multi-session fanout). `running`/`waiting`/`total` are tool-call counters despite the upstream struct naming them `sessionsRunning` etc.
- **Reconnection**: Buddy is TCP server, plugin is client. Plugin auto-reconnects on disconnect with 5s delay.
