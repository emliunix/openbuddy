# SDL2 Programming Guidelines — OpenBuddy

Mandatory reading before modifying the renderer or windowing code.

## 1. Framebuffer Model

SDL2 uses **double-buffering** but **does NOT persist content** between frames.

```
Frame N:   clear → draw → present → [buffer swap]
Frame N+1: clear → draw → present → [buffer swap]
```

After `SDL_RenderPresent()`, the next frame starts with undefined pixel contents.
You must redraw everything every frame.

## 2. Never Skip Full Redraws

Embedded optimizations that skip redraws when "nothing changed" are **harmful** here.

**Wrong** (ESP32 pattern):
```cpp
if (!ticked && state == last_state) return; // leaves black holes
```

**Right** (SDL pattern):
```cpp
// Always draw. SDL clears the buffer every frame.
buddy_tick(state);
```

## 3. No Persistent Offscreen Buffer Needed

The original ESP32 code uses `TFT_eSprite` (a persistent offscreen buffer).
On desktop SDL, the renderer's backbuffer serves the same purpose.

**Wrong**:
```cpp
SDL_Texture* tex = SDL_CreateTexture(..., SDL_TEXTUREACCESS_TARGET, ...);
// draw to texture, then copy to screen
```

**Right**:
```cpp
// Draw directly to the default target
SDL_RenderClear(renderer);
// draw everything
SDL_RenderPresent(renderer);
```

Texture-based offscreen rendering is only needed if you want to cache static content
(e.g., a background image) and reuse it across frames.

## 4. Clear Before Draw, Present After

Every frame must follow this exact sequence:

```cpp
SDL_RenderClear(renderer);      // 1. clear
render_everything(renderer);    // 2. draw all content
SDL_RenderPresent(renderer);    // 3. swap buffers
```

Never call `SDL_RenderPresent()` more than once per frame.
Never draw after `SDL_RenderPresent()` without another clear.

## 5. Alpha Blending

Keep alpha blending off unless you genuinely need transparency:

```cpp
SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
```

Enabling blending with a black background (0,0,0,0) can cause artifacts
if you accidentally draw with alpha < 255.

## 6. Coordinate System

Use `SDL_RenderSetLogicalSize()` to maintain a fixed logical resolution
regardless of window size or DPI:

```cpp
SDL_RenderSetLogicalSize(renderer, 135, 240);
// all drawing uses 135x240 coordinates (matches upstream M5StickC Plus)
```

This also handles HiDPI scaling automatically.

## 7. VSync

Do NOT use `SDL_RENDERER_PRESENTVSYNC`. On high-refresh-rate displays (120Hz+), vsync burns CPU by presenting every frame.

Instead, use an event-driven loop with `SDL_WaitEvent()`:

```cpp
// Block until event arrives; SDL internally sleeps efficiently
SDL_Event e;
if (SDL_WaitEvent(&e)) {
    // ...handle event...
    dirty = true;
}

// Only render when something changed
if (dirty) {
    SDL_RenderClear(renderer);
    render_everything(renderer);
    SDL_RenderPresent(renderer);
    dirty = false;
}
```

This yields CPU when idle and renders on demand.

## 8. Window Flags

For an always-on-top utility window:

```cpp
SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_ALLOW_HIGHDPI
```

Use `SDL_WINDOW_HIDDEN` during initialization to avoid a white flash,
then `SDL_ShowWindow()` once the first frame is ready.

## 9. Font Rendering

Our bitmap font renders each glyph as 5x7 rectangles. Skip background pixels:

```cpp
if (!pixel_on) continue; // don't draw background-colored rects
```

This reduces draw calls by ~70%.

## 10. Debug Tips

If you see flickering:

1. Check for early-return optimizations in draw code
2. Ensure `SDL_RenderClear()` is called exactly once per frame
3. Verify no draw calls happen after `SDL_RenderPresent()`
4. Temporarily set `SDL_RENDERER_SOFTWARE` to rule out GPU driver bugs
5. Use `SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255)` for the clear
to visually confirm which regions are being redrawn

## References

- `analysis/sdl_flickering_rca.md` — Full investigation timeline
- SDL2 wiki: https://wiki.libsdl.org/SDL2/APIByCategory
