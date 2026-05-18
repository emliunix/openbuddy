# permission-approval

## Problem
Plugin received `permission.asked` hook events and forwarded them to buddy via heartbeat, but never relayed buddy's decisions back to opencode. Permissions stuck in `pending` state forever.

## Root cause
`handleBuddyMessage` permission case was a no-op (logged only).

## Fix
Use the v1 plugin hook promise pipe pattern:

```
permission.ask hook → set output.status="ask", return pending Promise
opencode blocks on Promise (plugin/index.ts:271)
buddy replies → resolve Promise with output.status="allow"/"deny"
opencode reads output.status → proceeds
```

No SDK imports, no `ctx.client` usage. Pure pipe.

## Changes

### `.opencode/plugins/openbuddy.js`

1. **+1 field**: `deferreds: Map<permissionID, {output, resolve, sessionID}>`
2. **`onPermissionAsk(input, output)`**: returns `Promise` stored in deferreds (was void, returns immediately)
3. **`handleBuddyMessage`**: permission case resolves deferred; wire `"once"` → `"allow"`, `"deny"` stays. Removed dead handlers for `status`/`name`/`owner`/`unpair`/`char_*` — these are plugin→buddy direction, buddy never sends them.
4. **Hook registration**: `return client.onPermissionAsk(input, output)` (was fire-and-forget without output)
5. **Cleanup**: `disconnect()`, `session.status idle`, `session.idle`, `session.deleted` — all reject stale deferreds with `"deny"`
6. **Bug fix**: prompt `tool` was `input.tool` (undefined in v1 Permission type) → now `input.title`; prompt `hint` was `input.hint` (undefined) → now `input.pattern`

## Verification

- Tail `buddy.log` during permission flow — buddy sends `{"cmd":"permission","decision":"once","id":"..."}`
- Plugin log should show `"Permission decision relayed to opencode"`
- `waiting` should drop to 0 in subsequent heartbeats
- Buddy approval/denial stats increment via `DISP_INFO` panel

## Related

- `docs/PROTOCOL.md` § Buddy → Daemon — permission command
- `docs/OPENCODE_HOOKS.md` § 4 — permission decision flow
- `docs/counters.md` § 3 — approval/denial counters
