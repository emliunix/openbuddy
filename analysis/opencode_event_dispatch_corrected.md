# OpenCode Plugin Event Dispatching — Corrected Analysis

## Date: 2026-05-21

## Executive Summary

OpenCode's plugin system has **two separate dispatch mechanisms** that are often confused. This document clarifies their correct relationship and operational semantics based on source code analysis.

## Architectural Layers (Correct Order)

```
┌─────────────────────────────────────────────────────────────┐
│  LAYER 3: Plugin Interaction                                 │
│  ┌─────────────────┐  ┌──────────────────────────────────┐  │
│  │ Bus Events      │  │ Triggers                         │  │
│  │ (automatic)     │  │ (manual)                         │  │
│  │ Observer pattern│  │ Interceptor pattern              │  │
│  └────────┬────────┘  └──────────────┬───────────────────┘  │
├───────────┼──────────────────────────┼──────────────────────┤
│  LAYER 2: Bus (Pub/Sub)                                    │
│  - publish / subscribe / subscribeAll                      │
│  - Wildcard and typed PubSubs                              │
│  - Built on Effect                                         │
├────────────────────────────────────────────────────────────┤
│  LAYER 1: Effect (Foundation)                              │
│  - Effect.Effect<A, E, R> graded monad                     │
│  - yield* is monadic bind (not await)                      │
│  - Fibers, scopes, structured concurrency                  │
└────────────────────────────────────────────────────────────┘
```

## Layer 1: Effect — The Computational Foundation

Effect is the universal async/IO abstraction in OpenCode. It is **not** Promise-based async/await.

### Core Properties

- `Effect.Effect<A, E, R>` is a **graded monad** with three type parameters:
  - `A`: Success value type
  - `E`: Error type (typed failures)
  - `R`: Required dependencies (context/services)

- `yield*` is **monadic bind** (`flatMap`), not `await`:
  ```typescript
  // This is do-notation, not imperative await
  Effect.gen(function* () {
    const bus = yield* Bus.Service      // Bind: extract service
    yield* bus.publish(Event, props)    // Bind: run effect
  })
  ```

- Effects are **lazy descriptions** that only execute when interpreted by the Effect runtime

### Why This Matters

All plugin operations — loading, event dispatch, trigger execution — happen within the Effect framework:

```typescript
// Plugin loading is an Effect
export const layer = Layer.effect(
  Service,
  Effect.gen(function* () {
    const bus = yield* Bus.Service
    // ...
  })
)

// Bus subscription is an Effect
yield* bus.subscribeAll().pipe(
  Stream.runForEach((input) => Effect.sync(() => { ... })),
  Effect.forkScoped,
)

// Trigger execution is an Effect
const trigger = Effect.fn("Plugin.trigger")(function* (name, input, output) {
  // ...
})
```

## Layer 2: Bus — Pub/Sub Messaging

The Bus is an Effect service providing typed publish/subscribe operations.

### Key Operations

```typescript
export interface Interface {
  readonly publish: <D extends BusEvent.Definition>(
    def: D,
    properties: BusProperties<D>,
    options?: { id?: string },
  ) => Effect.Effect<void>
  
  readonly subscribeAll: () => Stream.Stream<Payload>
  
  readonly subscribe: <D extends BusEvent.Definition>(
    def: D
  ) => Stream.Stream<Payload<D>>
}
```

### Event Flow

```
session.status.set() 
  → bus.publish(Event.Status, { sessionID, status })
    → PubSub.publish(wildcard, payload)     // all subscribers
    → PubSub.publish(typed, payload)        // typed subscribers only
      → GlobalBus.emit("event", { ... })    // external emit
```

### Important: Bus Events are NOT Triggers

Bus events (`session.status`, `permission.asked`, etc.) are **published** by application code and **received** by subscribers. They are NOT triggers.

## Layer 3: Two Plugin Mechanisms

### Mechanism A: Bus Events (Observer Pattern)

**How it works:**

```typescript
// plugin/index.ts:246-255
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

**Properties:**
- **Automatic**: All bus events flow to plugins without manual integration points
- **Fire-and-forget**: `void hook["event"]?.(...)` — return value discarded
- **Unidirectional**: App publishes → Plugin receives (read-only)
- **Single hook**: Only `hook["event"]` is called; no dynamic dispatch by type
- **Signature**: `(input: { event: Event }) => Promise<void>`

**Event types include:**
- `session.status`, `session.idle`
- `permission.asked`, `permission.replied`
- `message.part.updated`
- `command.executed`
- `session.error`

### Mechanism B: Triggers (Interceptor Pattern)

**How it works:**

```typescript
// plugin/index.ts:261-274
const trigger = Effect.fn("Plugin.trigger")(function* <
  Name extends TriggerName,
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

**Properties:**
- **Manual**: Application calls `Plugin.trigger()` at specific integration points
- **Bidirectional**: `(input, output)` — plugins mutate `output` in-place
- **Request-response**: App calls → Plugin mutates → App uses modified output
- **Named hooks**: Dynamic lookup `hook[name]` by string key
- **Signature**: `(input: T, output: U) => Promise<void>` (mutates output)

**Trigger call sites:**

| Hook | Location | Purpose |
|------|----------|---------|
| `tool.execute.before` | `session/prompt.ts:582` | Inspect/modify tool args |
| `tool.execute.after` | `session/prompt.ts:597` | Inspect/modify tool results |
| `permission.ask` | `permission/index.ts:?` | Intercept permission decision |
| `shell.env` | `session/prompt.ts:1014` | Inject env vars |
| `chat.message` | `session/prompt.ts:1472` | New message received |
| `chat.params` | `session/llm.ts:161` | Modify LLM params |
| `chat.headers` | `session/llm.ts:181` | Modify LLM headers |
| `tool.definition` | `tool/registry.ts:336` | Modify tool definitions |
| `command.execute.before` | `session/prompt.ts:1984` | Before command execution |

## Critical Distinction: Why "session.status" as Named Hook is Wrong

### The Trap

Returning `"session.status"` as a named hook creates a **trigger interceptor**, not an event handler:

```typescript
// WRONG: This does NOT receive bus events
return {
  "session.status": async (input, output) => { ... }  // Never called for events
}

// CORRECT: Bus events only go through the event hook
return {
  event: async ({ event }) => {
    if (event.type === "session.status") {
      // Handle event here
    }
  }
}
```

### Why It Fails

1. **Bus events dispatch through `hook["event"]` only**: The framework has this hardcoded:
   ```typescript
   void hook["event"]?.({ event: input as any })
   ```
   There is no `hook[event.type]` lookup.

2. **Named handlers expect `(input, output)` signature**: Triggers mutate output. Bus events are read-only notifications.

3. **Triggers and events are separate namespaces**:
   - Event namespace: `session.status`, `permission.asked`, `message.part.updated`
   - Trigger namespace: `tool.execute.before`, `permission.ask`, `shell.env`

4. **Different operational semantics**:
   - Events: Observer pattern (fire-and-forget)
   - Triggers: Interceptor pattern (bidirectional mutation)

## Formal Comparison

| Dimension | Bus Events (`event` hook) | Triggers (`trigger()`) |
|-----------|---------------------------|------------------------|
| **Pattern** | Observer / Pub-Sub | Interceptor / Chain of Responsibility |
| **Direction** | Unidirectional (app → plugin) | Bidirectional (app ↔ plugin) |
| **Data flow** | Read-only event payload | Read input + mutable output |
| **Plugin action** | React to notification | Modify behavior/output |
| **Return value** | Ignored (`void`) | Mutated output returned |
| **Signature** | `({ event: Event }) => Promise<void>` | `(input: T, output: U) => Promise<void>` |
| **Dispatch** | Automatic (all events) | Manual (at integration points) |
| **Event examples** | `session.status`, `permission.asked` | N/A (these are not triggers) |
| **Trigger examples** | N/A | `tool.execute.before`, `permission.ask` |

## Source Code References

- `packages/opencode/src/plugin/index.ts:246-255` — Bus event subscription
- `packages/opencode/src/plugin/index.ts:261-274` — Trigger definition
- `packages/opencode/src/plugin/index.ts:38-41` — TriggerName type
- `packages/opencode/src/bus/index.ts:87-108` — Bus publish implementation
- `packages/plugin/src/index.ts:222-333` — Hooks interface definition

## Correct Mental Model for Plugin Development

```
┌─────────────────────────────────────────────────────────────┐
│  Plugin Implementation                                       │
│                                                              │
│  return {                                                    │
│    // Handle bus events (fire-and-forget notifications)     │
│    event: async ({ event }) => {                             │
│      switch (event.type) {                                   │
│        case "session.status": /* react */ break             │
│        case "permission.asked": /* react */ break           │
│      }                                                       │
│    },                                                        │
│                                                              │
│    // Intercept behavior (bidirectional mutation)           │
│    "tool.execute.before": async (input, output) => {        │
│      output.args = modifyArgs(input.tool, output.args)       │
│    },                                                        │
│                                                              │
│    "permission.ask": async (input, output) => {             │
│      output.status = "allow"  // auto-allow certain perms   │
│    }                                                         │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
```

## Rules

1. **Bus events → `event` hook ONLY**: No automatic dispatch by event type. The framework iterates `hook["event"]` for every bus event.

2. **Triggers → named hooks ONLY**: Called manually by the app via `Plugin.trigger()`. Not events.

3. **Signatures are DIFFERENT**: Event handler receives `{ event }`, trigger receives `(input, output)`.

4. **Do not add named handlers for bus events**: `"session.status"` as a named hook is meaningless to the event dispatcher.

5. **Effect is the foundation**: All operations happen within the Effect framework. Understand `yield*` as bind, not `await`.
