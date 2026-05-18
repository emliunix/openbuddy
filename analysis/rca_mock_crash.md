# RCA: MockTransport Segfault on `--mock` Startup

**Date**: 2026-05-17
**Exit code**: 139 (SIGSEGV)
**Component**: `main.cpp` event loop
**Introduced in**: SDL event bridge refactor

## Symptom

```bash
$ ./buddy --mock
# ...runs for ~3 seconds...
# silent crash, exit code 139
```

GDB backtrace (reconstructed from log analysis):

```
#0  std::variant<...>::index() const
#1  process_transport_msg(DaemonMsg const&)
#2  main() event loop
```

## Root Cause

**SDL event type collision between timer and transport messages.**

### Before (broken)

```cpp
// Registered ONE event
g_event_transport_msg = SDL_RegisterEvents(1);  // returns SDL_USEREVENT (0x8000)

// Timer pushes SDL_USEREVENT
static Uint32 timer_callback(Uint32, void*) {
    SDL_Event e;
    e.type = SDL_USEREVENT;  // SAME VALUE as g_event_transport_msg!
    SDL_PushEvent(&e);
    return 50;
}

// Event loop handles BOTH with same branch
if (e.type == SDL_USEREVENT) {           // catches timer events
    process_timer(now);
}
if (e.type == g_event_transport_msg) {   // ALSO catches timer events!
    std::unique_ptr<DaemonMsg> msg(static_cast<DaemonMsg*>(e.user.data1));
    // e.user.data1 is UNINITIALIZED for timer events
    process_transport_msg(*msg);  // SEGFAULT: garbage pointer dereference
}
```

### Why it worked for AsioTransport but crashed with MockTransport

- **AsioTransport**: Messages arrive slowly (every 2s heartbeats). Timer fires 20×/sec. Race condition rarely hits the exact same SDL event queue slot.
- **MockTransport**: Sends 2 messages immediately on start (TimeSync + OwnerCmd), then heartbeat every 2s. The rapid-fire initial messages + timer ticks guaranteed collision within the first 3 seconds.

### The garbage data

Timer events have `e.user.data1 = 0x0000000055100960` (or similar uninitialized stack value). Casting to `DaemonMsg*` and calling `msg->index()` dereferences invalid memory.

## Fix

Register **two distinct** SDL user events:

```cpp
Uint32 events = SDL_RegisterEvents(2);
g_event_timer = events;              // 0x8000
g_event_transport_msg = events + 1;  // 0x8001

// Timer uses its own event
e.type = g_event_timer;

// Transport uses its own event
e.type = g_event_transport_msg;
```

Now timer events and transport messages are handled by separate, non-overlapping `if` branches.

## Prevention

1. **Never use `SDL_USEREVENT` directly** — always register custom events with `SDL_RegisterEvents()`
2. **Document event type allocation** — comment which index maps to which semantic event
3. **Assert non-collision** — `SDL_assert(g_event_timer != g_event_transport_msg)`

## Timeline

| Time | Event |
|------|-------|
| 0ms | MockTransport thread starts, pushes TimeSync + OwnerCmd |
| 50ms | Timer fires, pushes `SDL_USEREVENT` (0x8000) |
| 50ms | Main thread processes timer event, **also** enters transport branch due to type collision |
| 50ms | `e.user.data1` contains garbage, cast to `DaemonMsg*`, `index()` → SIGSEGV |

## Verification

```bash
# Before fix: crashes within 5 seconds
$ timeout 5 ./buddy --mock
Process exited early (crashed?)
Exit code: 139

# After fix: stable for 10+ seconds
$ timeout 10 ./buddy --mock
SUCCESS: still running after 10s
```

## Files Changed

- `buddy/src/main.cpp`: Register 2 events, use distinct event types for timer and transport
