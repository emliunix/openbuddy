# Formal Semantics of OpenCode Primitives

## Date: 2026-05-21

---

## 1. Effect.Effect<A, E, R> — Graded Monad

### Algebraic Structure

`Effect.Effect<A, E, R>` is a **graded monad** (indexed monad) with three type parameters:

- **A** (Success): The type of values produced on success
- **E** (Error): The type of errors that can occur  
- **R** (Requirements): The set of capabilities/dependencies required

In category-theoretic terms, `Effect` is an endofunctor on the category of TypeScript types with the following structure:

```
Functor:     map:    (A → B) → Effect<A, E, R> → Effect<B, E, R>
Applicative: of:     A → Effect<A, never, never>
             ap:     Effect<A → B, E, R> → Effect<A, E, R> → Effect<B, E, R>
Monad:       flatMap: Effect<A, E, R> → (A → Effect<B, E, R>) → Effect<B, E, R>
```

The grading over `E` and `R` means this is not just a plain monad but a **monad in the category of graded types**.

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `Effect.succeed(a)` | `A → Effect<A, never, never>` | Creates a pure computation that immediately returns `a` with no effects |
| `Effect.fail(e)` | `E → Effect<never, E, never>` | Creates a computation that immediately fails with error `e` |
| `Effect.flatMap(ma, f)` | `Effect<A, E, R> → (A → Effect<B, E, R>) → Effect<B, E, R>` | **Sequencing**: Runs `ma`, extracts value `a`, then runs `f(a)` |
| `Effect.promise(f)` | `(() → Promise<A>) → Effect<A, unknown, never>` | **Async boundary**: Suspends computation, delegates to JS Promise runtime, resumes on resolution |
| `Effect.sync(f)` | `(() → A) → Effect<A, never, never>` | **Sync boundary**: Wraps synchronous side effect as an Effect value |
| `Effect.gen(g)` | `(() → Generator<Effect<any, E, R>, A>) → Effect<A, E, R>` | **Do-notation**: Desugars to nested `flatMap` calls via generator protocol |

### Effect.gen Desugaring

```typescript
// Source (do-notation)
Effect.gen(function* () {
  const x = yield* ma
  const y = yield* f(x)
  return g(x, y)
})

// Desugars to (monadic bind chain)
ma.pipe(
  Effect.flatMap(x => f(x).pipe(
    Effect.flatMap(y => Effect.succeed(g(x, y)))
  ))
)
```

Operationally:
1. `Effect.gen` creates a **generator object** that maintains a continuation stack
2. `yield*` is a **reify** operation: suspends the generator, passes the yielded Effect to the runtime
3. The Effect runtime resolves the yielded Effect, then **resumes** the generator with the result
4. If the yielded Effect fails, the generator is aborted and the error propagates

This is **not** async/await — it's **monadic do-notation** implemented via JavaScript generators.

---

## 2. Context.Service + Layer — Algebraic Effects

### Algebraic Structure

This implements the **handler pattern** for algebraic effects (Plotkin & Pretnar):

```
Effect Signature (Context.Service): Declares operations a computation can perform
Effect Handler (Layer.effect):       Provides semantics for those operations
Effect Perform (yield*):             Invokes an operation
```

In type-theoretic terms:

```
Service: Type → Type  (a dependent type — indexed by interface)
Layer:   Type → Type  (a provider of services)
```

`Context.Service<Service, Interface>()` creates a **typed capability token**.
`yield* Service` is an **effect operation** that requires that capability.
`Layer.effect(Service, ...)` is an **effect handler** that provides the capability.

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `Context.Service<S, I>()` | `Tag<S, I>` | Creates a unique capability token indexed by interface `I` |
| `yield* Service` | `Effect<I, never, S>` | **Perform**: Look up capability `S` in current context, return its interface `I` |
| `Layer.effect(S, eff)` | `Layer<S, E, R>` | **Handler**: Provides service `S` by running effect `eff` |
| `layerA.pipe(Layer.provide(layerB))` | `Layer<S, E, R>` | **Composition**: Layer `A` with dependencies satisfied by layer `B` |

### Dependency Resolution

```
Program: Effect<A, E, Service1 | Service2>
Layer1:  Layer<Service1, E1, R1>
Layer2:  Layer<Service2, E2, R2>

Result:  Effect<A, E | E1 | E2, R1 | R2>
```

Operationally, `Layer.provide` is **effect handler composition**: each layer handles some capabilities, and the remaining capabilities are threaded through.

---

## 3. InstanceState — Memoized State Monad

### Algebraic Structure

`InstanceState<A, E, R>` is a **scoped memoization wrapper** around the State monad pattern:

```
State Monad: get: Effect<S, never, R>
             put: S → Effect<void, never, R>
             modify: (S → S) → Effect<void, never, R>
```

But `InstanceState` adds:
- **Keying**: State is indexed by directory/instance
- **Memoization**: `init` runs once per key, then cached
- **Lifecycle**: Cleanup via `Scope` and finalizers

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `InstanceState.make(init)` | `(Ctx → Effect<A, E, R>) → Effect<InstanceState<A, E, R>, never, R>` | Creates a memoized state container keyed by directory |
| `InstanceState.get(state)` | `Effect<A, E, R>` | **Memoized read**: If current directory in cache, return cached value; else run `init`, cache, return |
| `InstanceState.invalidate(state)` | `Effect<void, never, R>` | **Evict**: Remove current directory from cache |

### Lifecycle Management

```
make(init):
  cache ← empty_map
  disposer ← register_cleanup(directory → invalidate(cache, directory))
  add_finalizer(disposer)
  return { cache }

get(state, directory):
  if directory in state.cache:
    return state.cache[directory]
  else:
    value ← run(init(ctx_for_directory))
    state.cache[directory] ← value
    return value
```

The `Scope` ensures that when an instance is disposed:
1. All finalizers run (reverse order)
2. All fibers in scope are interrupted
3. Cached state is invalidated

---

## 4. Effect.forkScoped — Concurrent Regions

### Algebraic Structure

This implements **parallel composition** with **cancellation regions**:

```
Fork:   Effect<A, E, R> → Effect<Fiber<A, E>, never, R | Scope>
Join:   Fiber<A, E> → Effect<A, E, R>
Cancel: Fiber<A, E> → Effect<void, never, R>
```

A **Fiber** is a lightweight thread of execution with its own:
- Call stack (continuation chain)
- Interruption status
- Scope (resource lifetime)

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `Effect.forkScoped(eff)` | `Effect<A, E, R | Scope> → Effect<Fiber<A, E>, never, R | Scope>` | **Fork**: Create new fiber running `eff` in current scope; fiber interrupted when scope closes |
| `Fiber.join(fiber)` | `Effect<A, E, R>` | **Join**: Block until fiber completes, return its result |
| `Fiber.interrupt(fiber)` | `Effect<void, never, R>` | **Cancel**: Send interruption signal to fiber |

### Scope Lifecycle

```
Scope.open():
  scope ← { fibers: [], finalizers: [] }
  return scope

Scope.fork(scope, effect):
  fiber ← create_fiber(effect)
  scope.fibers.push(fiber)
  return fiber

Scope.close(scope):
  // Reverse order: last opened, first closed
  for finalizer in reverse(scope.finalizers):
    run(finalizer)
  for fiber in scope.fibers:
    interrupt(fiber)
```

This is a **structured concurrency** model (similar to Kotlin's coroutines or Java's Project Loom).

---

## 5. Bus (PubSub) — Asynchronous Message Passing

### Algebraic Structure

The Bus implements **typed message passing** via Publisher-Subscriber pattern:

```
PubSub<T>:
  publish: T → Effect<void, never, never>
  subscribe: Effect<Subscription<T>, never, never>
  
Stream<T>:
  fromPubSub: PubSub<T> → Stream<T>
  runForEach: Stream<T> → (T → Effect<void, E, R>) → Effect<void, E, R>
```

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `PubSub.publish(pubsub, msg)` | `PubSub<T> → T → Effect<void, never, never>` | **Broadcast**: Enqueue `msg` in all subscriber queues |
| `PubSub.subscribe(pubsub)` | `Effect<Subscription<T>, never, never>` | **Subscribe**: Create new queue, register as subscriber |
| `Stream.fromPubSub(ps)` | `Stream<T>` | **Lift**: Convert PubSub to pull-based Stream |
| `Stream.runForEach(s, f)` | `Stream<T> → (T → Effect<void, E, R>) → Effect<void, E, R>` | **Consume**: For each message, run effectful handler `f` |

### Bus Event Flow

```
publish(event_def, properties):
  payload ← { id, type: event_def.type, properties }
  
  // Typed subscribers
  if event_def.type in state.typed:
    for queue in state.typed[event_def.type]:
      enqueue(queue, payload)
  
  // Wildcard subscribers (includes plugin event hook)
  for queue in state.wildcard:
    enqueue(queue, payload)

subscribeAllCallback(callback):
  scope ← Scope.make()
  subscription ← PubSub.subscribe(state.wildcard)
  fork(Stream.runForEach(stream, msg => callback(msg)))
  return () => Scope.close(scope)  // Unsubscribe function
```

The Bus provides **backpressure** via bounded queues and **typed filtering** via separate PubSubs per event type.

---

## 6. Plugin.trigger — Monadic Fold with Mutation

### Algebraic Structure

`trigger` is a **monadic left fold** (foldlM) over a collection of plugins with imperative mutation:

```
trigger: Name → Input → Output → Effect<Output, never, State>
trigger(name, input, output) =
  foldlM(plugins, output, (out, hook) =>
    if hook[name] exists:
      hook[name](input, out)  // mutates out in-place
      return out
    else:
      return out
  )
```

But because plugins mutate `output` in-place and return `Promise<void>`, this is not a pure fold. It's:

```
foldlM_plugins: [Hook] → Name → Input → Output → Effect<Output, never, State>
foldlM_plugins(hooks, name, input, output):
  for hook in hooks:
    if hook[name]:
      yield* Effect.promise(() => hook[name](input, output))
  return output
```

### Operational Semantics

| Primitive | Type | Operational Semantics |
|-----------|------|----------------------|
| `trigger(name, input, output)` | `Name → Input → Output → Effect<Output, never, PluginState>` | **Fold**: Iterate all plugins; for each implementing `name`, run `hook(input, output)` as async effect; return (mutated) `output` |

### Key Properties

1. **Sequential**: Plugins are called in registration order
2. **Mutating**: Each plugin can modify `output` in-place
3. **Effectful**: Each plugin call is wrapped in `Effect.promise`
4. **Non-terminating on error**: `Effect.promise` does not specify error handling; errors likely crash the fiber
5. **Type-preserving**: Input and Output types are fixed by the trigger definition

### Signature Pattern

All trigger hooks share the signature:

```
(input: Input, output: Output) => Promise<void>
```

This is **continuation-passing style** (CPS) with a mutable accumulator:
- `input` is read-only context
- `output` is the mutable continuation/accumulator
- `void` return means all communication is through mutation

This is essentially an **imperative callback pattern** wrapped in the Effect monad for composability.

---

## 7. The Two Plugin Interaction Models — Formal Comparison

### Model A: Bus Events (Observer Pattern)

```
┌─────────────────────────────────────────┐
│  Application                            │
│  ┌─────────┐      ┌───────────────┐    │
│  │ Publish │──────▶│ Wildcard PubSub│    │
│  └─────────┘      └───────┬───────┘    │
│                           │             │
│                           ▼             │
│                     ┌─────────────┐     │
│                     │ Stream.run  │     │
│                     │ ForEach     │     │
│                     └──────┬──────┘     │
│                            │            │
│                     ┌──────▼──────┐     │
│                     │ void hook   │     │
│                     │ ["event"]?. │     │
│                     │ ({event})   │     │
│                     └─────────────┘     │
└─────────────────────────────────────────┘
```

**Formal type:**
```
EventHandler: ({ event: Event }) → Effect<void, never, never>
```

**Semantics:**
- **Producer**: Application publishes events to PubSub
- **Consumer**: Plugin subscribes via `event` hook
- **Pattern**: Observer / Publish-Subscribe
- **Coupling**: Loose (event bus decouples producer and consumer)
- **Time**: Asynchronous (consumer may lag behind producer)

### Model B: Triggers (Interceptor Pattern)

```
┌─────────────────────────────────────────┐
│  Application                            │
│  ┌─────────────┐   ┌──────────────┐    │
│  │ Integration │   │ trigger(name,│    │
│  │ Point       │──▶│ input,       │    │
│  │ (app code)  │   │ output)      │    │
│  └─────────────┘   └──────┬───────┘    │
│                           │             │
│              ┌────────────┼────────┐    │
│              ▼            ▼        ▼    │
│         ┌────────┐  ┌────────┐         │
│         │ hook[  │  │ hook[  │  ...    │
│         │ name]  │  │ name]  │         │
│         └───┬────┘  └───┬────┘         │
│             │           │               │
│             ▼           ▼               │
│         mutate        mutate            │
│         output        output            │
│                                         │
│  return output  ◀───────────────────────┘
```

**Formal type:**
```
TriggerHook: (input: Input, output: Output) → Effect<void, never, never>
Trigger: Name → Input → Output → Effect<Output, never, PluginState>
```

**Semantics:**
- **Producer**: Application calls `trigger()` at integration points
- **Consumer**: Plugin implements named hooks
- **Pattern**: Chain of Responsibility / Interceptor
- **Coupling**: Tight (application must know when to call trigger)
- **Time**: Synchronous (application blocks until all plugins run)

### The Fundamental Difference

| Dimension | Bus Events | Triggers |
|-----------|-----------|----------|
| **Category theory** | Co-product injection (sum type) | Product projection (tuple traversal) |
| **Process calculus** | Asynchronous message send | Synchronous call with mutable state |
| **Type theory** | `Event → void` (observer) | `(Input, Output) → void` (mutator) |
| **Algebra** | Monoid under concatenation | Semigroup under sequential composition |
| **Control flow** | Fire-and-forget | Request-response |
| **State** | Read-only (event payload) | Read-write (mutable output) |
| **Composition** | Fan-out (all subscribers get same event) | Chain (each plugin sees output of previous) |

---

## 8. Complete Type-theoretic Summary

### Effect System

```
Effect: Type → Type → Type → Type  (A, E, R)

unit:    A → Effect<A, ⊥, ⊤>                    [pure value]
bind:    Effect<A, E, R> → (A → Effect<B, E, R>) → Effect<B, E, R>  [sequencing]
map:     (A → B) → Effect<A, E, R> → Effect<B, E, R>             [functor]
fail:    E → Effect<⊥, E, ⊤>                    [error injection]
catch:   Effect<A, E, R> → (E → Effect<A, E', R>) → Effect<A, E', R> [error handling]
provide: Layer<R, E, R'> → Effect<A, E, R> → Effect<A, E, R'>     [dependency satisfaction]
```

### Plugin System

```
Plugin: PluginInput → Effect<Hooks, Error, never>

Hooks: {
  event?: Event → Effect<void, Error, never>                    [bus subscriber]
  [name: TriggerName]?: (Input, Output) → Effect<void, Error, never>  [trigger handler]
}

Bus.publish: EventDef → Properties → Effect<void, never, BusService>
Bus.subscribeAll: Effect<Stream<Payload>, never, BusService>

Plugin.trigger: Name → Input → Output → Effect<Output, never, PluginState>
  where trigger(name, input, output) =
    foldlM(hooks, output, (out, hook) =>
      hook[name] ? (hook[name](input, out) > out) : out
    )
```

### Layer System

```
Layer: Type → Type → Type → Type  (Service, Error, Requirements)

Layer.effect: Service → Effect<Implementation, Error, R> → Layer<Service, Error, R>
Layer.provide: Layer<S, E, R | S'> → Layer<S', E', R'> → Layer<S, E | E', R | R'>

Program: Effect<A, E, R>
Run: Layer<R, E', R'> → Effect<A, E | E', R'>
```

---

## 9. Why This Matters for Plugin Development

Understanding these formal structures prevents common mistakes:

1. **Thinking `yield*` is `await`**: It's monadic bind, not imperative await. The runtime controls execution, not the JS event loop.

2. **Thinking `event` and triggers are interchangeable**: They are completely different algebraic structures (observer vs interceptor). Bus events go through `event` only; triggers go through named hooks only.

3. **Thinking named handlers receive bus events**: Returning `"session.status"` from a plugin creates a trigger hook, not an event handler. The event dispatcher never calls it.

4. **Thinking triggers return values**: Triggers mutate `output` in-place. The return value (`Promise<void>`) is discarded. All communication is through mutation.

5. **Thinking services are singletons**: `InstanceState` provides per-directory memoization. Services are scoped, not global.

6. **Thinking Effect executes immediately**: Effects are lazy descriptions. They only run when the Effect runtime interprets them (via `runPromise`, `runSync`, etc.).

---

## References

- Plotkin, G. & Pretnar, M. (2013). "Handling Algebraic Effects." *Logical Methods in Computer Science*.
- Wadler, P. (1995). "Monads for Functional Programming." *Advanced Functional Programming*.
- Kiselyov, O. et al. (2013). "Extensible Effects: An Alternative to Monad Transformers." *Haskell Symposium*.
- `packages/opencode/src/plugin/index.ts` — Plugin.trigger definition
- `packages/opencode/src/bus/index.ts` — Bus PubSub implementation
- `packages/opencode/src/effect/instance-state.ts` — InstanceState semantics
- `packages/opencode/src/effect/bridge.ts` — Effect-to-JS bridging
- [Effect Documentation](https://effect.website/) — Practical implementation of these patterns
