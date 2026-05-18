# SDL Flickering Investigation Timeline

## Problem Statement

The OpenBuddy SDL window exhibited severe flickering — the screen would flash between black and content rapidly, making the ASCII pet animations unwatchable.

## Environment

- macOS 15.x, Apple Silicon (M1/M2)
- SDL2 via Homebrew (`/opt/homebrew/include/SDL2`)
- 240x240 logical resolution, scaled 3x (720x720 window)
- Always-on-top window flag

---

## Attempt 1: Texture-based Offscreen Rendering

**Date**: 2026-05-17
**Hypothesis**: Flickering is caused by drawing directly to the screen without double buffering. Using an SDL texture as an offscreen render target (like the original `TFT_eSprite`) should eliminate tearing.

**Implementation**:
- Created `SDL_Texture` with `SDL_TEXTUREACCESS_TARGET`
- Set texture blend mode to `SDL_BLENDMODE_BLEND`
- All draw calls targeted the texture via `SDL_SetRenderTarget(ren_, tex_)`
- `push_sprite()` copied texture to screen via `SDL_RenderCopy()`

**Code**:
```cpp
tex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND);

// In every draw call:
SDL_SetRenderTarget(ren_, tex_);
// ... draw ...
SDL_SetRenderTarget(ren_, nullptr);

// Present:
SDL_RenderCopy(ren_, tex_, nullptr, &dst);
```

**Outcome**: **FAIL** — flickering persisted, possibly worse.

**Reasoning**: The texture target approach adds overhead but doesn't address the fundamental issue. The problem is not buffer swapping; it's that we're clearing the screen and re-rendering everything every frame, and the sprite renderer has an early-exit optimization that skips drawing when it thinks nothing changed.

---

## Attempt 2: Disable Alpha Blending on Texture

**Date**: 2026-05-17
**Hypothesis**: `SDL_BLENDMODE_BLEND` on the target texture causes compositing artifacts when the texture is copied to the screen, especially with black (0x0000) background.

**Implementation**:
- Changed `SDL_BLENDMODE_BLEND` to `SDL_BLENDMODE_NONE`

**Outcome**: **FAIL** — no visible improvement.

**Reasoning**: Blend mode affects how the texture pixels combine with the screen, but since we're doing a full-screen copy after clearing the screen, blend mode is irrelevant. The flickering is not a compositing issue.

---

## Attempt 3: Remove Texture Target Entirely — Direct Rendering

**Date**: 2026-05-17
**Hypothesis**: The texture target indirection is unnecessary on desktop. SDL2's default renderer already has a backbuffer. Drawing directly to the default target should work and is simpler.

**Implementation**:
- Removed `SDL_Texture` member entirely
- Removed all `SDL_SetRenderTarget()` calls from draw primitives
- `begin_frame()` and `push_sprite()` became no-ops
- Main loop clears screen, calls `state_render()`, calls `SDL_RenderPresent()`

**Code**:
```cpp
// Main loop
SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 255);
SDL_RenderClear(g_sdl_renderer);
state_render(g_renderer);
SDL_RenderPresent(g_sdl_renderer);
```

**Outcome**: **FAIL** — still flickering.

**Reasoning**: Direct rendering is correct in principle, but the buddy animation system has a frame-skip optimization from the ESP32 port that skips redraws when it thinks the state hasn't changed. On ESP32 this saved power; on desktop it causes incomplete redraws.

---

## Attempt 4: Optimize Character Rendering (Skip Background Pixels)

**Date**: 2026-05-17
**Hypothesis**: Drawing background-colored rectangles for every "off" pixel in the bitmap font is causing GPU fill-rate issues or synchronization problems.

**Implementation**:
- In `render_char()`: only draw rectangles for ON pixels, skip OFF pixels entirely
- This reduces draw calls from 35 rects/char to ~10 rects/char

**Code**:
```cpp
bool on = (f[col] >> row) & 1;
if (!on) continue;  // skip background pixels
// draw only foreground pixels
```

**Outcome**: **FAIL** — flickering unchanged.

**Reasoning**: The optimization is good for performance but not the root cause. The issue is logical, not fill-rate related.

---

## Attempt 5: Disable Buddy Animation Frame Skip (ROOT CAUSE FIX)

**Date**: 2026-05-17
**Hypothesis**: The flickering is caused by `buddy_tick()` returning early when `ticked == false` AND state hasn't changed. This optimization from the ESP32 port assumes a persistent framebuffer (like `TFT_eSprite`), but on desktop we clear the entire screen every frame via `SDL_RenderClear()`. When `buddy_tick()` skips drawing, that frame shows only the black background for the pet area.

**Implementation**:
- Removed the early-return guard in `buddy_tick()`:

**Before**:
```cpp
if (!ticked && persona_state == last_drawn_state
    && current_species_idx == last_drawn_species) {
    return;  // skip redraw
}
```

**After**:
```cpp
// Always redraw — desktop clears screen every frame
last_drawn_state = persona_state;
last_drawn_species = current_species_idx;
```

**Outcome**: **SUCCESS** — flickering eliminated. Smooth 60 FPS animation.

---

## Root Cause Analysis

The flickering was caused by a **mismatch between the rendering model assumptions**:

| Aspect | ESP32 (Original) | Desktop (SDL) |
|--------|-----------------|---------------|
| Framebuffer | Persistent sprite (`TFT_eSprite`) | Cleared every frame |
| Draw strategy | Incremental (only redraw changed regions) | Full redraw required |
| Optimization | Skip redraw if state unchanged | Skip redraw = black hole |
| Clear behavior | `spr.fillSprite()` only when needed | `SDL_RenderClear()` every frame |

The ESP32 `buddyTick()` function has a guard:
```cpp
if (!ticked && state == lastDrawnState) return;
```

This works on ESP32 because:
1. The sprite buffer persists between frames
2. Only the animation changes at 5 FPS (200ms ticks)
3. The 60Hz loop can safely skip redraws — old pixels remain visible

On desktop SDL:
1. `SDL_RenderClear()` zeros the entire backbuffer every frame
2. If `buddy_tick()` skips drawing, the pet area is black for that frame
3. The result is a 60Hz alternation between "full render" and "black screen" = flickering

The fix is to always redraw the pet, because the cost is negligible on desktop and the screen is cleared every frame anyway.

---

## Lessons Learned

1. **Don't port embedded optimizations blindly**. Frame-skip logic designed for a persistent sprite buffer is harmful when the backend clears every frame.
2. **Profile before optimizing**. The "optimization" was causing a visual bug. On modern desktop GPUs, drawing ~100 small rectangles is trivial.
3. **Understand the framebuffer model**. SDL2's default renderer is double-buffered but does NOT persist content between `SDL_RenderPresent()` calls. Each frame starts fresh.
4. **Texture-based offscreen rendering is unnecessary** for this use case. SDL2's backbuffer is sufficient.

---

## Final Architecture

```
Main Loop (60 Hz):
  SDL_RenderClear()          // black background
  state_render(renderer):    // draw everything directly
    buddy_tick(state)        // ALWAYS draw pet (no skip)
    draw_menu/info/pet       // overlays
  SDL_RenderPresent()        // swap buffers
```

No offscreen texture. No frame skip. Direct rendering to SDL's backbuffer.
