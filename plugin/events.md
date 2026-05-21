# Plugin Event Mapping

Each section is a **Buddy feature**. Under it are the **events** that feed that feature, and what to do when each arrives.

**Important**: the same event can appear in multiple features. A feature often needs **multiple events** to maintain its state. Read the whole section, not just one event.

---

# Session Lifecycle

Track which sessions exist, their run state, and whether they completed normally or were interrupted.

Two events work together for interruption detection: `session.error` marks sessions as errored, and `session.status` → `idle` checks that mark to decide whether to celebrate.

## `session.created`
- Increment `state.total`.
- Initialize `{ running: false, waiting: 0 }` in `sessionStates` Map.
- Send heartbeat.

## `session.status` + `session.error` (combined)

### On `session.status` with status busy / retry:
- Set `sessionStates[sessionID].running = true`.
- If transitioning from not-running: log and send heartbeat immediately.
- **Why only session.status sets running**: Triggers like `tool.execute.before` do NOT modify running state. The session's run state is authoritative from the bus.

### On `session.error`:
- Add `sessionID` to `erroredSessions` Set (ALL errors, not just MessageAbortedError).
- Log the error name for debugging.
- **Effect**: the next `session.status` → `idle` for this session will skip celebration (interruption detection).

### On `session.status` with status idle:
- Check `erroredSessions` Set. If `erroredSessions.has(sessionID)`: session was interrupted/aborted, do NOT set `state.completed`.
- Clear `erroredSessions` entry for this session.
- Set `sessionStates[sessionID].running = false`.
- Set `sessionStates[sessionID].waiting = 0`.
- Clear pending permissions for this session.
- If NOT errored: set `state.completed = true` (Buddy celebrates).
- Send heartbeat.

## `session.deleted`
- Remove `sessionID` from `sessionStates`.
- Decrement `state.total`.
- Clear pending permissions for this session.
- Send heartbeat.

## `session.updated`
- **Removed**. This event does not carry fields that affect heartbeat state. No-op.

## `session.idle`
- **Deprecated**. The canonical idle signal is `session.status` with `status.type === "idle"`. No-op.

---

# Permission Queue

FIFO queue of pending user approvals. Buddy shows Y/N for the head item.

## `permission.asked`
- Push `{ id, sessionID, tool, hint }` to `pendingPermissions` Map.
- Increment `sessionStates[sessionID].waiting`.
- Heartbeat will include `prompt` field (derived from head of queue) so Buddy renders the approval UI.
- Send heartbeat.

## `permission.replied`
- Decrement `sessionStates[sessionID].waiting`.
- Remove the first (FIFO) entry from `pendingPermissions`.
- **Note**: This does not look up by ID; it assumes FIFO ordering for external replies (OpenCode UI, other plugins).
- Heartbeat clears the `prompt` field; Buddy returns to normal view.
- Send heartbeat.

## Permission response (Buddy → OpenCode)
- When Buddy sends `{"cmd":"permission","id":"...","decision":"once|deny"}`
- Call `client.postSessionIdPermissionsPermissionId({ path: { id: sessionID, permissionID: id }, body: { response: "once" | "reject" } })`
- Decrement `sessionStates[sessionID].waiting`.
- Remove from `pendingPermissions`.
- Update prompt (heartbeat will reflect new head of queue).
- Send heartbeat.

---

# Token Counter

Cumulative output tokens since app start.

## `session.next.step.ended`
- Read `event.properties.tokens.output`.
- Add to `state.tokens` (lifetime).
- **Note**: `tokens_today` in heartbeat mirrors `tokens` (no midnight reset implemented).
- Send heartbeat so Buddy updates the stats display.

---

# Turn Streaming

Forward assistant text to Buddy as it arrives.

## `message.part.updated`
- If `part.type === "text"`: construct `TurnEvent` with `part.text` (truncated to 4096 chars).
- Send immediately to Buddy as one-shot event (NOT heartbeat).

## `message.updated`
- **No-op**. SDK v2 `AssistantMessage` has no `content` field. Text arrives via `message.part.updated`.

---

# Activity Log

Recent tool commands shown in Buddy HUD entries field.

## `tool.execute.before` (trigger — bidirectional, has `output.args`)

### Formatting logic
Each entry is formatted as:
```
<toLocaleTimeString()> <tool-specific-string>
```

If `output.args` is null or not an object, tool-specific string is just `input.tool`.

Otherwise:
- **`bash`**: `bash: ${args.command.slice(0,40)}`
- **`read`**: `read: ${basename(args.filePath).slice(0,40)}` (extracts filename, drops directory)
- **`grep`**: `grep: ${args.pattern.slice(0,40)}`
- **`edit`**: `edit: ${basename(args.filePath).slice(0,40)}` (extracts filename, drops directory)
- **`write`**: `write: ${basename(args.filePath).slice(0,40)}` (extracts filename, drops directory)
- **`glob`**: `glob: ${args.pattern.slice(0,40)}`
- **default**: `input.tool`

### Storage
- Push formatted entry to `state.entries`.
- Cap at 20 entries; drop oldest (`shift()`) when exceeded.
- Heartbeat sends `entries.slice(-5)` (last 5).

### Why trigger
Bus events do not carry tool arguments; only the trigger gives us `output.args` for pretty-printing.

### Does NOT set running
Session run state is managed exclusively by `session.status`.

## `tool.execute.after`
- **Not used**. The activity log entry is written in `tool.execute.before` with the formatted command. The after-trigger is redundant because `session.status` → idle signals completion.

---

# Completed Flag

One-shot celebration flag sent in heartbeat.

## Behavior
- Set to `true` when a session transitions from busy/retry to idle without error.
- Sent in the next heartbeat.
- **Must auto-clear after 2 seconds** (not immediately after sending) so Buddy has time to render the celebration animation.
- If another session completes during the 2-second window, the timer resets.

---

# Buddy Commands

Commands Buddy sends to the plugin.

## `ping`
- No-op. Buddy uses this to check connection liveness.

## `permission`
- See "Permission response (Buddy → OpenCode)" above.

## Unknown commands
- Log warning with command name. Do not crash.

---

# Plugin Return Shape

```typescript
return {
  // All bus events feed into one event hook
  event: async ({ event }) => {
    switch (event.type) {
      case "session.status":       /* session lifecycle */
      case "session.error":        /* session lifecycle — interruption detection */
      case "session.created":      /* session lifecycle */
      case "session.deleted":      /* session lifecycle */
      case "session.next.step.ended": /* token counter */
      case "message.part.updated": /* turn streaming */
      case "permission.asked":     /* permission queue */
      case "permission.replied":   /* permission queue */
      case "todo.updated":         /* placeholder */
      case "file.edited":          /* placeholder */
      case "command.executed":     /* placeholder */
    }
  },

  // Tool pretty-print needs the trigger (only place with output.args)
  "tool.execute.before": async (input, output) => {
    client.handleToolExecuteBefore(input, output)
  },
}
```

---

# See Also

- `docs/PROTOCOL.md` — Buddy wire format
- `docs/ARCHITECTURE.md` — Bus events vs triggers
- `plugin/README.md` — OpenCode plugin architecture
