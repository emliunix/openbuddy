# Upstream Character/GIF System Architecture

## Overview

The character system provides animated GIF pets as an alternative to ASCII species. It supports two modes:

1. **GIF mode** — AnimatedGIF library decodes GIF frames to the sprite
2. **Text mode** — Simple text frames rendered at size 2 (fallback for minimal characters)

## Filesystem Layout

```
/characters/
  <name>/
    manifest.json
    sleep.gif
    idle_1.gif
    idle_2.gif
    busy.gif
    attention.gif
    celebrate.gif
    dizzy.gif
    heart.gif
```

## Manifest Format (`manifest.json`)

```json
{
  "name": "Bufo",
  "colors": {
    "body": "#C2A6",
    "bg": "#0000",
    "text": "#FFFF",
    "textDim": "#8410",
    "ink": "#0000"
  },
  "states": {
    "sleep": ["sleep.gif"],
    "idle": ["idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

States can be:
- Single string: one GIF file
- Array of strings: multiple variants that rotate

## Color Palette

```cpp
struct Palette {
  uint16_t body,    // primary character color
           bg,      // background (sprite clear color)
           text,    // bright text
           textDim, // dim text
           ink;     // drawing color
};
```

Colors are 16-bit RGB565 parsed from hex strings in manifest.

## GIF Mode Details

### Frame Decoding

Uses `AnimatedGIF` library with LittleFS callbacks:
- `gifOpenCb` — opens file from LittleFS
- `gifReadCb` / `gifSeekCb` — file I/O
- `gifDrawCb` — renders scanline to sprite

### Transparency Handling

Transparent pixels in GIF get the character's `bg` color (not true transparency):
```cpp
put(x, y, (hasT && idx == t) ? pal.bg : pal16[idx]);
```

This means each frame fully repaints its region — no ghosting, but no compositing either.

### Placement

```cpp
// Normal mode: centered in upper 140px
int outW = gifW;
int outH = gifH;
gifX = (spr.width() - outW) / 2;
gifY = (140 - outH) / 2;

// Peek mode: half scale, pinned to info panel top (y=70)
int outW = gifW / 2;
int outH = gifH / 2;
gifX = (spr.width() - outW) / 2;
gifY = (PEEK_TOP - outH) / 2;
```

### Animation Timing

- Per-frame delay from GIF metadata
- **Variant dwell**: 5000ms per GIF variant (multi-variant states only)
- **Inter-variant pause**: 800ms between GIFs
- Single-GIF states: freeze on last frame (don't loop to avoid blocking)
- Multi-variant states: loop same GIF until dwell elapses, then rotate

### State Rotation

```cpp
stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
```

Each state maintains an index into its variant array. On dwell expiry, advance to next variant.

## Text Mode Details

For lightweight characters without GIF assets:

```json
{
  "mode": "text",
  "states": {
    "idle": {
      "delay": 200,
      "frames": ["(^-^)", "(o-o)", "(^-^)"]
    }
  }
}
```

- Frames rendered at text size 2 (12px glyphs)
- Centered horizontally
- Only clears a band around text (not full sprite) to preserve overlays

## Lifecycle

```cpp
characterInit("bufo");      // load manifest, parse colors, cache paths
characterSetState(P_IDLE);  // open GIF for idle state
characterTick();            // decode next frame if timing elapsed
characterRenderTo(&M5.Lcd); // one-shot render to arbitrary target
characterInvalidate();      // close + reopen current state (mode switch)
characterClose();           // unload everything
```

## Peek Mode

When displaying info/pet screens, the character renders at half scale:
- `characterSetPeek(true)` — enables 2:1 nearest-neighbor downscale
- Renders in header strip (y=0 to 70)
- Used by info pages and landscape clock

## Comparison: ASCII Buddy vs GIF Character

| Feature | ASCII Buddy | GIF Character |
|---------|------------|---------------|
| Source | Compiled C++ code | Filesystem GIFs + manifest |
| Assets | None (code-defined) | GIF files + JSON manifest |
| Animation | Code-driven tick counts | GIF frame delays |
| Variants | One per species | Multiple per state |
| Scale | 1x or 2x text size | Native resolution, peek=0.5x |
| Transparency | None (full redraw) | Transparent→bg color |
| Installation | Built-in | File transfer from desktop |
| Fallback | Always available | Falls back to ASCII if missing |

## For SDL Port

GIF mode is **out of scope** for initial SDL port:
1. No LittleFS equivalent
2. No AnimatedGIF library for desktop
3. File transfer protocol (`char_begin`/`chunk`/`char_end`) adds complexity
4. ASCII species already provide full functionality

**Recommendation**: Support ASCII species only. If GIF support is desired later:
- Use SDL_image or similar for GIF decoding
- Load from regular filesystem (`~/.config/openbuddy/characters/`)
- Reuse same manifest format
- Implement same variant rotation logic

## Key Code Locations

- `character.h:30` — public interface
- `character.cpp:140-245` — `characterInit()` manifest parsing
- `character.cpp:295-331` — `characterSetState()` GIF opening
- `character.cpp:333-401` — `characterTick()` animation loop
- `character.cpp:102-136` — `gifDrawCb()` pixel rendering
