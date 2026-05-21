# OpenBuddy Plugin

Single-file TypeScript plugin for OpenCode. Loaded directly by OpenCode at runtime.

## MUST READ: OpenCode Plugin Architecture

Before reading any OpenCode source code or modifying this plugin, read:

- **`analysis/opencode_event_dispatch_corrected.md`** — Corrected analysis of OpenCode event dispatching
- **`analysis/opencode_formal_semantics.md`** — Formal semantics of Effect, triggers, and event dispatching

This document explains:
- Why `Effect` is a **graded monad** (not async/await)
- Why `yield*` is **monadic bind** (not `await`)
- The **two separate plugin interaction models**: bus events (`event` hook) vs triggers (`trigger()` + named hooks)
- Why returning `"session.status"` as a named hook **does not** handle bus events
- The formal difference between **Observer pattern** (fire-and-forget) and **Interceptor pattern** (bidirectional mutation)

Without understanding this, you will conflate the two dispatch mechanisms and write broken plugins.

## Key Takeaway: The Handler Path

There are **two separate code paths** in `packages/opencode/src/plugin/index.ts`. Understanding both is critical:

### Path A: Bus Events (line 250)

```typescript
void hook["event"]?.({ event: input as any })
```

This is the **only** bus event dispatch. It ONLY calls `hook["event"]`. It never looks at `hook["session.status"]`, `hook["permission.asked"]`, or any other key. Period.

### Path B: Triggers (line 269)

```typescript
const fn = hook[name] as any
```

This is inside `Plugin.trigger()`. It DOES dynamic lookup by name. But it is ONLY called at explicit integration points in the application code:

- `session/prompt.ts`: `"tool.execute.before"`, `"tool.execute.after"`, `"chat.message"`
- `session/llm.ts`: `"chat.params"`, `"chat.headers"`
- `permission/index.ts`: `"permission.ask"` ← Note: this is the **trigger**, not the event
- `tool/registry.ts`: `"tool.definition"`

**OpenCode NEVER calls `Plugin.trigger("session.status", ...)` or `Plugin.trigger("permission.asked", ...)` anywhere in the codebase.**

### Empirical Proof

Debug plugin logging shows:

**`event` hook IS called for `session.status`:**
```
service=debug-backtrace type=session.status stack=Error
    at event (/Users/emliunix/Documents/openbuddy/.opencode/plugins/debug-backtrace.ts:12:28)
    at <anonymous> (/$bunfs/root/chunk-cbnmr6eh.js:571:12941)  <-- Plugin.trigger() NOT called
    at ~effect/Effect/evaluate (/$bunfs/root/chunk-svzhkhfd.js:25:4492)
    ...
```

**`"session.status"` named handler: NEVER called** — zero occurrences in the log.

### The Trap

When you return `{ "session.status": handler }`:

```typescript
// It IS stored in hooks[]
hooks.push(await plugin(input))  // hooks[0]["session.status"] exists

// But Path A NEVER accesses it
for (const hook of hooks) {
  void hook["event"]?.({ event })  // Only "event" is called
}

// And Path B is never invoked with "session.status"
// No code calls: plugin.trigger("session.status", ...)
```

**The `hook[name]` dynamic lookup at line 269 is real, but it is ONLY used for triggers, not for bus events.**

### Correct Pattern for Bus Events

```typescript
// Single event hook with internal dispatch by event.type
return {
  event: async ({ event }) => {
    switch (event.type) {
      case "session.status":
        // handle session status
        break
      case "permission.asked":
        // handle permission asked
        break
      // ... etc
    }
  }
}
```

## Development

The `package.json` in this directory exists **only for type-checking** during development. It installs `@opencode-ai/sdk` and `@opencode-ai/plugin` as devDependencies so TypeScript can resolve the SDK event types.

OpenCode loads `openbuddy.ts` directly — no build step, no bundling.

### Type check

```sh
cd plugin
npm install
npx tsc --noEmit
```

### Install plugin

Project-local:
```sh
mkdir -p .opencode/plugins
cp plugin/openbuddy.ts .opencode/plugins/
```

Global:
```sh
mkdir -p ~/.config/opencode/plugins
cp plugin/openbuddy.ts ~/.config/opencode/plugins/
```
