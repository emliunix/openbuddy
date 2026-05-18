# How SDL Events Work

## The Event Queue

SDL collects input from the OS into an **internal queue**. Your program pulls events from this queue and decides what to do.

```
OS (keyboard/mouse/window events)
    ↓
SDL_PumpEvents()  // SDL gathers them internally
    ↓
SDL Event Queue   // stored here
    ↓
SDL_PollEvent()   // your code reads them
```

## Two Ways to Read Events

### 1. SDL_PollEvent() — Non-blocking (for games/animations)

Returns immediately. If no events, returns 0. Use this when you have continuous animation.

```cpp
while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {  // drain ALL pending events
        if (e.type == SDL_QUIT) running = false;
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
    }
    
    // update and render happen EVERY frame, regardless of events
    update();
    render();
    SDL_RenderPresent(renderer);
}
```

**Critical**: The inner `while` loop drains the entire queue. If you only call `SDL_PollEvent` once per frame, events accumulate and input feels laggy.

### 2. SDL_WaitEvent() — Blocking (for UI tools, editors)

Pauses your program until an event arrives. CPU usage drops to 0%.

```cpp
while (running) {
    SDL_Event e;
    SDL_WaitEvent(&e);  // blocks here until something happens
    
    if (e.type == SDL_QUIT) running = false;
    // process event...
    
    render();  // only renders when something happens
}
```

**Don't use for animations** — if nothing happens, your program freezes.

## Event vs State

SDL provides two ways to know what the user is doing:

| Approach | Use for | Example |
|----------|---------|---------|
| **Events** (`SDL_KEYDOWN`) | One-shot actions | Menu toggle, jump, fire |
| **State** (`SDL_GetKeyboardState`) | Continuous actions | Movement, holding a key |

### Event-based (one-shot)

```cpp
while (SDL_PollEvent(&e)) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE) {
        jump();  // happens once per key press
    }
}
```

### State-based (continuous)

```cpp
const Uint8* keys = SDL_GetKeyboardState(nullptr);
if (keys[SDL_SCANCODE_A]) {
    player.x -= 1;  // happens every frame while A is held
}
```

## Our OpenBuddy Pattern

We use **PollEvent** because the pet animates continuously (idle, breathing, etc.):

```cpp
while (running) {
    // 1. Process ALL input events
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running = false;
        if (e.type == SDL_KEYDOWN) {
            state_handle_key(e.key.keysym.sym, true);
        }
    }
    
    // 2. Update logic (animation timers, state transitions)
    state_update();
    
    // 3. Render frame
    SDL_RenderClear(renderer);
    state_render(renderer);
    SDL_RenderPresent(renderer);
}
```

## Common Mistakes

**Mistake 1**: Only polling once per frame
```cpp
// WRONG - misses events if multiple happen per frame
if (SDL_PollEvent(&e)) { ... }  // only gets 1 event
```

**Mistake 2**: Rendering inside event loop
```cpp
// WRONG - renders once per event, not once per frame
while (SDL_PollEvent(&e)) {
    if (e.type == SDL_KEYDOWN) move_player();
    render();  // don't do this
}
```

**Mistake 3**: Using WaitEvent for games
```cpp
// WRONG - animation stops when user stops typing
while (SDL_WaitEvent(&e)) {  // blocks forever if no input
    render();
}
```

## Threading Rule

**Event functions must be called from the thread that called `SDL_Init(SDL_INIT_VIDEO)`**.

This is why we don't put `SDL_PollEvent` in a separate thread — it must stay in the main thread alongside rendering.

## References

- `docs/SDL_GUIDELINES.md` — Rendering rules
- SDL2 Wiki: https://wiki.libsdl.org/SDL2/SDL_PollEvent
