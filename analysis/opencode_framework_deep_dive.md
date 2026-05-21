# OpenCode Framework Deep Dive: Effect, Trigger, and Event Architecture

## Date: 2026-05-21

## Executive Summary

OpenCode's plugin system sits on top of **Effect** — a universal computational framework that is fundamentally different from async/await. Within this framework, there are **two separate plugin interaction models** that are often confused:

1. **Bus Events** (`event` hook): Fire-and-forget notifications
2. **Triggers** (`trigger()` + named hooks): Bidirectional interception/mutation points

Understanding these as distinct architectural patterns is essential for building correct plugins.

---

## Tier 1: Effect as Universal Computation Framework

### What Effect Actually Is

`Effect.Effect<A, E, R>` is not a Promise. It is a **typed, lazy, composable description of a computation**:

- **`A`** — Success value type
- **`E`** — Error type (typed failures, not exceptions)
- **`R`** — Required dependencies (context/services)

This is a **monadic** computation model, not imperative async/await. Every Effect is a value that describes what to do; it does not execute until run by the Effect runtime.

### Core Patterns in OpenCode

#### 1. Sequential Composition (Monadic Bind)

```typescript
Effect.gen(function* () {
  const bus = yield* Bus.Service        // Bind: extract service from context
  const config = yield* Config.Service  // Bind: sequential dependency
  
  // compose more effects...
  yield* bus.publish(Event, props)      // Bind: run effect for side effects
})
```

`yield*` is **not** `await`. It is monadic bind (`flatMap`) that:
- Extracts the value from the inner Effect
- Composes the dependency requirements (`R`)
- Propagates error types (`E`)
- Maintains execution context (fiber, scope, tracing)

#### 2. Service Definition and Dependency Injection

```typescript
// Define a typed service identifier
export class Service extends Context.Service<Service, Interface>()("@opencode/Bus") {}

// Provide the service implementation via Layer
export const layer = Layer.effect(
  Service,
  Effect.gen(function* () {
    // ... implementation
    return Service.of({ publish, subscribe, subscribeAll })
  })
)
```

Services are:
- **Typed**: `Interface` defines the contract
- **Injectable**: `yield* Bus.Service` resolves at runtime
- **Composable**: Layers combine via `Layer.merge`, `Layer.provide`
- **Scoped**: `InstanceState` provides per-directory lifecycle

#### 3. Named and Traced Effects

```typescript
const trigger = Effect.fn("Plugin.trigger")(function* <Name extends TriggerName>(...) {
  // ...
})
```

`Effect.fn("Name")`:
- Names the effect for tracing/debugging
- Enables fiber inspection (`Fiber.getCurrent()`)
- Provides stack traces through Effect runtime
- Is NOT just a function decorator — it integrates with the framework

#### 4. Resource Management via Scopes

```typescript
yield* bus.subscribeAll().pipe(
  Stream.runForEach((input) => Effect.sync(() => { ... })),
  Effect.forkScoped,  // Fork into current scope — auto-cleanup on disposal
)
```

`Effect.forkScoped` creates a background fiber that:
- Runs independently of the parent
- Is **interruptible** when the scope closes
- Automatically cleans up resources (PubSub subscriptions, file handles, etc.)
- Is managed by the Effect runtime, not the JS event loop

#### 5. Bridging to Real World

```typescript
// Bridge async code into Effect
yield* Effect.promise(async () => fn(input, output))

// Bridge sync side effects
yield* Effect.sync(() => { sideEffect() })

// Bridge callbacks
yield* Effect.callback<A, E>((resume) => {
  asyncOperation().then(result => resume(Effect.succeed(result)))
})
```

These are **boundary crossings** — they lift real-world operations into the Effect computational context.

### Why This Matters

Effect is a **framework-level abstraction**, not a library:
- The runtime controls execution (fibers, scheduling, interruption)
- Types track effects, errors, and dependencies
- Composition is mathematical (monadic), not imperative
- Resource management is automatic (scopes, finalizers)

Writing OpenCode plugins without understanding this leads to:
- Thinking `yield*` is `await` → misunderstanding sequencing
- Thinking services are singletons → misunderstanding `InstanceState`
- Thinking effects execute immediately → misunderstanding laziness

---

## Tier 2: The Plugin System Architecture

### How Plugins Are Loaded

From `packages/opencode/src/plugin/index.ts:110-255`:

```typescript
export const layer = Layer.effect(
  Service,
  Effect.gen(function* () {
    const state = yield* InstanceState.make<State>(
      Effect.fn("Plugin.state")(function* (ctx) {
        const hooks: Hooks[] = []
        
        // 1. Load built-in plugins
        for (const plugin of INTERNAL_PLUGINS) {
          const init = yield* Effect.tryPromise({
            try: () => plugin(input),
            catch: (err) => { /* ... */ }
          })
          if (init._tag === "Some") hooks.push(init.value)
        }
        
        // 2. Load external plugins
        const loaded = yield* Effect.promise(() => PluginLoader.loadExternal(...))
        for (const load of loaded) {
          yield* Effect.tryPromise({
            try: () => applyPlugin(load, input, hooks),
            catch: (err) => { /* ... */ }
          })
        }
        
        // 3. Subscribe to bus events
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
        
        return { hooks }
      })
    )
    
    // ... define trigger, list, init
  })
)
```

Key observations:
1. Plugin loading is an **Effect** — sequenced, traced, scoped
2. Hooks are collected into a flat array: `Hooks[]`
3. Bus subscription is forked into scope — auto-cleanup on instance disposal
4. `InstanceState` ensures **per-directory** plugin isolation

### The Two Dispatch Mechanisms

The plugin system exposes **two separate dispatch mechanisms** to plugins:

#### Mechanism A: Bus Event Subscription

```typescript
// In plugin loading:
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
- **Fire-and-forget**: `void hook["event"]?.(...)` — return value discarded
- **Unidirectional**: App publishes → Plugin receives
- **Notification pattern**: "Something happened"
- **Single hook**: Only `event` is called; no dynamic dispatch by type
- **Automatic**: All bus events flow here; no application code needed

**Bus Event Types** (from `@opencode-ai/sdk`):
- `session.status`, `session.created`, `session.deleted`
- `permission.asked`, `permission.replied`
- `message.updated`, `message.part.updated`
- `todo.updated`, `file.edited`, `command.executed`

#### Mechanism B: Trigger System

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

**Properties:**
- **Bidirectional mutation**: `(input, output)` — plugins mutate output in-place
- **Request-response**: App calls → Plugin mutates → App uses modified output
- **Interception pattern**: "About to do X, want to modify?"
- **Named hooks**: Dynamic lookup `hook[name]` by string
- **Manual**: Application code calls `trigger()` at specific integration points

**Trigger Names** (from `Hooks` interface):
- `tool.execute.before`, `tool.execute.after`
- `permission.ask`
- `shell.env`
- `chat.message`, `chat.params`, `chat.headers`
- `command.execute.before`
- `experimental.chat.system.transform`

### Critical Distinction

These are **NOT** two ways to do the same thing. They are **completely separate architectural patterns**:

| Aspect | Bus Events (`event`) | Triggers (`trigger`) |
|--------|----------------------|----------------------|
| **Direction** | Unidirectional (app → plugin) | Bidirectional (app ↔ plugin) |
| **Pattern** | Notification | Interception |
| **Data flow** | Input only | Input + mutable Output |
| **Plugin action** | React to event | Modify behavior |
| **Return value** | Ignored (`void`) | Mutated output returned |
| **Signature** | `(input: { event }) => Promise<void>` | `(input, output) => Promise<void>` |
| **Dispatch** | Automatic (all events) | Manual (at integration points) |
| **Naming** | Event types (`*.asked`) | Hook names (`*.ask`) |

### The Naming Trap

The most confusing aspect is the naming overlap:

- **`permission.ask`** (trigger hook): Plugin intercepts permission request and can modify decision
- **`permission.asked`** (bus event): Notification that permission was requested

These are **NOT** the same thing:
- `permission.ask` is an interception point in the permission flow
- `permission.asked` is a notification that the flow reached a certain state

Similarly:
- `tool.execute.before` (trigger): Intercept before tool runs
- `command.executed` (event): Notification that command finished

---

## Tier 3: Reconsidering Event Dispatching

### The User's Mistake (And Why It Happened)

The user removed the `event` hook and returned named handlers like:

```typescript
return {
  "session.status": handler,
  "permission.asked": handler,
  // ...
}
```

This doesn't work because:
1. **Bus events only dispatch through `event`** — the framework has `hook["event"]` hardcoded
2. **Named handlers are for triggers** — they expect `(input, output)` signature, not `({ event })`
3. **Triggers and events are separate namespaces** — returning `"session.status"` creates a trigger hook, not an event handler

The mistake stems from conflating the two dispatch mechanisms.

### The Correct Mental Model

When building on OpenCode's plugin system, think in **three layers**:

```
┌─────────────────────────────────────────────────────────────┐
│  LAYER 3: Your Plugin Logic                                  │
│  - Handle bus events via `event` hook                       │
│  - Intercept behavior via named trigger hooks               │
│  - NEVER conflate the two                                   │
├─────────────────────────────────────────────────────────────┤
│  LAYER 2: OpenCode Plugin Framework                          │
│  - Effect runtime (fibers, scopes, tracing)                 │
│  - Service layer (Layer, Context, InstanceState)            │
│  - Two dispatch mechanisms (bus vs trigger)                 │
├─────────────────────────────────────────────────────────────┤
│  LAYER 1: Effect Computational Model                         │
│  - Monadic composition (gen, yield*, pipe)                  │
│  - Typed errors and dependencies                            │
│  - Lazy evaluation, runtime-controlled execution            │
└─────────────────────────────────────────────────────────────┘
```

### For the OpenBuddy Plugin

Given this architecture, the correct approach is:

```typescript
export default async function openbuddyPlugin(input: PluginInput) {
  const client = new BuddyClient(input)
  
  return {
    // LAYER 3a: Bus event handling (fire-and-forget)
    event: async ({ event }: { event: Event }) => {
      switch (event.type) {
        case "session.status":
          return client.handleSessionStatus(event)
        case "permission.asked":
          return client.handlePermissionAsked(event)
        // ... etc
      }
    },
    
    // LAYER 3b: Trigger interception (bidirectional mutation)
    // Only implement triggers that make sense for Buddy
    "tool.execute.before": async (input, output) => {
      // Can inspect/modify tool args before execution
    },
    "tool.execute.after": async (input, output) => {
      // Can inspect/modify tool results after execution
    },
  }
}
```

### Key Rules

1. **Bus events → `event` hook ONLY**: There is no automatic dispatch by event type. The framework iterates `hook["event"]` for every bus event.

2. **Triggers → named hooks ONLY**: These are called manually by the app via `Plugin.trigger()`. They are not events.

3. **Signatures are DIFFERENT**:
   - Event handler: `({ event: Event }) => Promise<void>`
   - Trigger hook: `(input: T, output: U) => Promise<void>` (mutates output)

4. **Do not add named handlers for bus events**: `"session.status"` as a named hook is meaningless to the event dispatcher. It only works as a trigger name if the app calls `trigger("session.status", ...)`, which it doesn't.

5. **Effect is the foundation**: All plugin loading, bus subscription, and trigger execution happens within the Effect framework. Understanding Effect is prerequisite to understanding the plugin system.

---

## Appendix: Effect Type Signatures in OpenCode

### Effect.fn

```typescript
const myEffect = Effect.fn("Name")(function* (arg: T) {
  const dep = yield* Dependency.Service
  const result = yield* someOtherEffect(arg)
  return result
})
// Type: (arg: T) => Effect.Effect<Result, Error, Dependency>
```

### Effect.gen

```typescript
Effect.gen(function* () {
  const a = yield* effectA  // Bind: unwrap Effect<A>
  const b = yield* effectB  // Bind: unwrap Effect<B>
  return { a, b }           // Return: Effect<{a, b}>
})
```

### Layer.effect

```typescript
const layer = Layer.effect(
  Service,                                    // What to provide
  Effect.gen(function* () {
    const dep = yield* OtherService           // Dependency
    return Service.of({ method: ... })        // Implementation
  })
)
// Type: Layer.Layer<Service, Error, OtherService>
```

### InstanceState

```typescript
const state = yield* InstanceState.make<State>(
  Effect.fn("State.init")(function* (ctx) {
    // Runs once per directory
    // Scoped to instance lifetime
    return { ... }
  })
)
// Type: InstanceState<State, Error, Dependencies>
```

---

## References

- `packages/opencode/src/plugin/index.ts` — Plugin loading, trigger definition, bus subscription
- `packages/opencode/src/bus/index.ts` — Bus publish/subscribe implementation
- `packages/plugin/src/index.ts` — Hooks interface definition
- `packages/opencode/src/effect/instance-state.ts` — Per-instance state management
- `packages/opencode/src/effect/bridge.ts` — Effect-to-JS callback bridging
- `packages/opencode/specs/effect/migration.md` — Effect migration patterns
- `packages/opencode/test/plugin/trigger.test.ts` — Trigger behavior tests
- [Effect Documentation](https://effect.website/)
