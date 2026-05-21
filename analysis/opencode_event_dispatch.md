# OpenCode Plugin Event Dispatching — Exploration

## Date: 2026-05-21

## Summary

OpenCode dispatches bus events to plugins through **two separate mechanisms**:

1. **Bus event stream** (`subscribeAll`) → generic `event` hook only
2. **Named triggers** (`trigger()`) → specific named hooks (`"permission.ask"`, `"tool.execute.before"`, etc.)

Bus events (session.status, permission.asked, etc.) do **NOT** go through named hooks. They go through the single `event` hook.

## Evidence

### 1. Bus Event Subscription (the only path for events)

**File:** `packages/opencode/src/plugin/index.ts:246-255`

```typescript
// Subscribe to bus events, fiber interrupted when scope closes
yield* bus.subscribeAll().pipe(
  Stream.runForEach((input) =>
    Effect.sync(() => {
      for (const hook of hooks) {
        void hook["event"]?.({ event: input as any })
      }
    }),
  ),
  Effect.forkScoped,
)
```

**Key observation:** The code iterates all loaded plugins and calls `hook["event"]` with the bus payload. There is no dynamic lookup by `input.type` or any other field. The `input` here is the raw bus event (e.g., `{ type: "session.status", properties: {...} }`).

### 2. Trigger System (for named hooks)

**File:** `packages/opencode/src/plugin/index.ts:261-274`

```typescript
const trigger = Effect.fn("Plugin.trigger")(function* <
  Name extends TriggerName,
  Input = Parameters<Required<Hooks>[Name]>[0],
  Output = Parameters<Required<Hooks>[Name]>[1],
>(name: Name, input: Input, output: Output) {
  if (!name) return output
  const s = yield* InstanceState.get(state)
  for (const hook of s.hooks) {
    const fn = hook[name] as any
    if (!fn) continue
    yield* Effect.promise(async () => fn(input, output))
  }
  return output
})
```

**Key observation:** `trigger()` looks up `hook[name]` dynamically. This is how named hooks like `"permission.ask"` and `"tool.execute.before"` are called. But `trigger()` is called by the application at specific integration points — NOT in response to bus events.

### 3. TriggerName Type Definition

**File:** `packages/opencode/src/plugin/index.ts:38-41`

```typescript
type TriggerName = {
  [K in keyof Hooks]-?: NonNullable<Hooks[K]> extends (input: any, output: any) => Promise<void> ? K : never
}[keyof Hooks]
```

This extracts only hook names that match the `(input, output) => Promise<void>` signature. From the `Hooks` interface, this includes:
- `"permission.ask"`
- `"tool.execute.before"`
- `"tool.execute.after"`
- `"command.execute.before"`
- etc.

It does **NOT** include `"event"` because `event` has a different signature: `(input: { event: Event }) => Promise<void>` (single parameter, no output).

### 4. Hooks Interface

**File:** `packages/plugin/src/index.ts:222-333`

```typescript
export interface Hooks {
  event?: (input: { event: Event }) => Promise<void>
  // ... other hooks
  "permission.ask"?: (input: Permission, output: { status: "ask" | "deny" | "allow" }) => Promise<void>
  "tool.execute.before"?: (input: { tool: string; sessionID: string; callID: string }, output: { args: any }) => Promise<void>
  "tool.execute.after"?: (input: { tool: string; sessionID: string; callID: string; args: any }, output: { title: string; output: string; metadata: any }) => Promise<void>
  // ...
}
```

**Key observation:** `event` is a separate hook from all named hooks. The `Event` type is the union of all bus event types (session.status, permission.asked, etc.).

### 5. Bus Architecture

**File:** `packages/opencode/src/bus/index.ts:87-108`

```typescript
function publish<D extends BusEvent.Definition>(def: D, properties: BusProperties<D>, options?: { id?: string }) {
  return Effect.gen(function* () {
    const s = yield* InstanceState.get(state)
    const payload: Payload = { id: options?.id ?? createID(), type: def.type, properties }
    
    const ps = s.typed.get(def.type)
    if (ps) yield* PubSub.publish(ps, payload)
    yield* PubSub.publish(s.wildcard, payload)  // <-- this is what subscribeAll listens to
    
    // ... global bus emit
  })
}
```

All bus events go through the wildcard pubsub. `subscribeAll()` listens to this wildcard. There is no per-event-type dispatch to plugins.

## Important Distinction: Hooks vs Bus Events

OpenCode has **two separate namespaces** that look similar but are completely different:

### 1. Plugin Hooks (called via `trigger()`)

These are named hooks that plugins implement:
- `"tool.execute.before"`
- `"tool.execute.after"`
- `"permission.ask"` ← Note: this is the **hook**, not the event
- `"shell.env"`
- `"chat.message"`, etc.

Called via `Plugin.trigger(name, input, output)` at specific integration points.

### 2. Bus Events (received via `event` hook)

These are events published on the internal bus:
- `"session.status"`
- `"session.created"`, `"session.deleted"`, etc.
- `"permission.asked"` ← Note: this is the **event**, with different payload than the hook
- `"permission.replied"`
- `"message.part.updated"`, etc.

Received only through the generic `event` hook.

### The Trap

`permission.ask` (hook) and `permission.asked` (event) are NOT the same:
- **Hook**: `permission.ask` receives `(Permission, output)` and lets plugins intercept/modify permission decisions
- **Event**: `permission.asked` receives `{ id, sessionID, permission, patterns }` as a bus notification

## Codebase Conventions

### Effect

OpenCode uses [Effect](https://effect.website/) as the universal async/IO convention throughout the codebase. All side-effecting operations (plugin loading, bus publishing, hook execution) are wrapped in `Effect` computations.

Key patterns:
- `Effect.gen(function* () { ... })` for sequential composition
- `yield*` for unwrapping effects
- `Effect.promise()` for bridging async code
- `Effect.forkScoped` for background fibers

Examples:
- Plugin loading: `packages/opencode/src/plugin/index.ts:154-160`
- Bus publishing: `packages/opencode/src/bus/index.ts:87-108`
- Hook execution: `packages/opencode/src/plugin/index.ts:261-274`

### trigger()

`Plugin.trigger()` is the universal convention for calling named hooks. It is defined once in `packages/opencode/src/plugin/index.ts:261-274` and used throughout the codebase.

Signature:
```typescript
const trigger = Effect.fn("Plugin.trigger")(function* <
  Name extends TriggerName,
  Input = Parameters<Required<Hooks>[Name]>[0],
  Output = Parameters<Required<Hooks>[Name]>[1],
>(name: Name, input: Input, output: Output) {
  if (!name) return output
  const s = yield* InstanceState.get(state)
  for (const hook of s.hooks) {
    const fn = hook[name] as any
    if (!fn) continue
    yield* Effect.promise(async () => fn(input, output))
  }
  return output
})
```

Key properties:
- Iterates all loaded plugins
- Looks up `hook[name]` dynamically
- Skips plugins that don't implement the hook
- Returns the output (possibly modified by plugins)
- Wraps execution in `Effect.promise()` for async plugins

### trigger() call sites

All named hooks triggered in the codebase:

| Hook name | Call site | Purpose |
|---|---|---|
| `"tool.execute.before"` | `session/prompt.ts:582`, `session/prompt.ts:623`, `session/prompt.ts:754` | Before tool execution |
| `"tool.execute.after"` | `session/prompt.ts:597`, `session/prompt.ts:641`, `session/prompt.ts:833` | After tool execution |
| `"shell.env"` | `session/prompt.ts:1014`, `session/processor.ts:595`, `tool/shell.ts:413` | Inject shell env vars |
| `"experimental.chat.system.transform"` | `session/llm.ts:118`, `agent/agent.ts:394` | Transform system prompt |
| `"experimental.chat.messages.transform"` | `session/prompt.ts:1810`, `session/compaction.ts:407` | Transform messages |
| `"tool.definition"` | `tool/registry.ts:336` | Modify tool definitions |
| `"chat.message"` | `session/prompt.ts:1472` | New message received |
| `"chat.params"` | `session/llm.ts:161` | Modify LLM params |
| `"chat.headers"` | `session/llm.ts:181` | Modify LLM headers |
| `"command.execute.before"` | `session/prompt.ts:1984` | Before command execution |
| `"experimental.session.compacting"` | `session/compaction.ts:400` | Before compaction |
| `"experimental.compaction.autocontinue"` | `session/compaction.ts:511` | Auto-continue after compaction |
| `"experimental.text.complete"` | `session/processor.ts:595` | Text completion |

## Official Documentation Confirmation

The [OpenCode Plugins documentation](https://opencode.ai/docs/plugins/#events) confirms this architecture:

> Plugins can subscribe to events... Here is a list of the different events available.

All events (session.status, permission.asked, etc.) are listed under the **Events** section, and the example shows:

```javascript
return {
  event: async ({ event }) => {
    if (event.type === "session.idle") {
      // handle event
    }
  },
}
```

The documentation does **NOT** list `"session.status"`, `"permission.asked"`, etc. as named hooks. They are only accessible through the `event` hook.

## Conclusion

**Bus events are dispatched ONLY through the generic `event` hook.** There is no automatic dispatch by event type to named handlers.

Named hooks like `"tool.execute.before"`, `"shell.env"`, etc. are triggered at specific integration points via `Plugin.trigger()` — they are NOT bus events.

Our plugin **must** implement the `event` hook to receive bus events. The named handlers we extracted can be called from a thin dispatcher within the `event` hook.

## References

- `packages/opencode/src/plugin/index.ts` — plugin loading and event dispatch
- `packages/opencode/src/bus/index.ts` — bus publish/subscribe
- `packages/plugin/src/index.ts` — Hooks interface definition
- `packages/sdk/js/src/v2/gen/types.gen.ts` — Event type definitions
