# ⚠️ DEPRECATED — Superseded by `screen_behavior_facts.md`

**This analysis is WRONG.** It was based on a locally-modified copy where `W` was changed from 135 to 240. The official upstream code uses `W = 135, H = 240` matching the M5StickC Plus display exactly. See `screen_behavior_facts.md` for the corrected, source-verified analysis.

---

# Upstream Screen Behavior Analysis (DEPRECATED)

## Hardware

- **Device**: M5StickC Plus (ESP32 + ST7789v2 display controller)
- **Physical panel**: 135 × 240 pixels (portrait orientation)
- **Controller RAM**: 240 × 320 pixels (larger than visible panel)
- **Library**: TFT_eSPI via M5StickCPlus Arduino core

## Confirmed: Display Width is 135px

TFT_eSPI is configured with:

```cpp
#define TFT_WIDTH  135
#define TFT_HEIGHT 240
#define CGRAM_OFFSET      // enables hardware offset handling
```

With `CGRAM_OFFSET`, portrait mode uses hardware offsets:
- `colstart = 52` (controller column offset)
- `rowstart = 40` (controller row offset)

These offsets **center** the 135×240 panel within the controller's 240×320 RAM.

**`M5.Lcd.width()` returns 135 at runtime.** The library clips all drawing to these bounds.

## Upstream Draws to 240×240, But Only 135×240 is Visible

The upstream code creates a 240×240 sprite and draws assuming 240px width:

```cpp
const int W = 240, H = 240;
spr.createSprite(W, H);
```

Then `spr.pushSprite(0, 0)` copies the sprite to the LCD, **clipping to the 135×240 display bounds**. Only x=0..134 is visible. The rightmost ~105px is off-screen.

**The code never queries display width.** It hardcodes `W = 240` for all layout.

## Right-Aligned Elements Are OFF-SCREEN on Hardware

```cpp
spr.setCursor(W - 60, H - 10);   // x=180 — "sp:%u" species counter → OFF-SCREEN
spr.setCursor(W - 28, y);         // x=212 — page counter "n/N" → OFF-SCREEN
spr.setCursor(W - 24, H - LH - 2); // x=216 — scroll indicator "-N" → OFF-SCREEN
spr.setCursor(W - 60, H - 12);    // x=180 — "B: deny" → OFF-SCREEN
```

**Visible width = 135px.** Right margin starts at x=135, so anything at x=135+ is clipped by `pushSprite()`.

## Menu Panel Clipping

```cpp
int mw = 210, mh = 60;
int mx = (W - mw) / 2;  // mx = 15
// panel spans x=15..225, but only x=15..134 is visible
// right half of menu (x=135..225, 90px wide) is clipped
```

The menu renders with its **right half missing** on the physical device.

## Prompt Panel Clipping

```cpp
int pw = 220, ph = 60, px = (240 - pw) / 2, py = 80;
// panel spans x=10..230, but only x=10..134 is visible
```

The approval panel's right edge is clipped.

## Pet Position

```cpp
const int BUDDY_X_CENTER = 120;
```

The pet is centered at x=120. With scale=2 and ~60px width, it spans x=90..150. **The right edge (x=135..150) is clipped.**

## Why Does This Happen?

The author wrote all layout for 240×240 (`const int W = 240`) but the physical display is 135×240. The code was likely:
1. Developed/tested on a simulator with 240×240
2. Or the author simply didn't account for the narrower display
3. Or intended for a future M5StickC variant with 240×240 panel

The ST7789 controller supports 240×240, and some displays use the full area. The M5StickC Plus specifically uses a 135×240 panel, but the code wasn't adapted.

## Setup Sequence

```cpp
void setup() {
    M5.begin();                  // init hardware, configure display (135×240)
    M5.Lcd.setRotation(0);       // 0° portrait
    spr.createSprite(W, H);      // allocate 240×240 off-screen buffer
    // ...
    spr.pushSprite(0, 0);        // copies 240×240 → clips to 135×240 visible window
}
```

## Rendering Architecture

### Primary Path: Sprite → LCD

1. Draw everything to `spr` (240×240 buffer)
2. `spr.pushSprite(0, 0)` — copies sprite to LCD, **clipping to 135×240 bounds**
3. Result: left 135px visible, right 105px discarded

### Secondary Path: Direct LCD (Landscape Clock Only)

```cpp
M5.Lcd.setRotation(clockOrient);  // rotate to 1 or 3 (landscape)
M5.Lcd.fillScreen(p.bg);          // clear directly
M5.Lcd.drawString(hm, 170, 42);   // draw clock
// ...
M5.Lcd.setRotation(0);            // restore portrait
```

Landscape clock mode bypasses the sprite and draws directly. In landscape, the display becomes 240×135 (swapped dimensions), showing the full controller RAM width. This is the only mode where the full 240px width is visible.

## Coordinate System

All coordinates are **absolute pixels** relative to the top-left of the current drawing surface (sprite or LCD).

No relative positioning, layout engine, or containers exist. Each element hardcodes its own `(x, y)` based on `W = 240`.

### Alignment Patterns

**Left-aligned** (most common, always visible):
```cpp
spr.setCursor(4, H - 10);           // x=4
spr.setCursor(mx + 6, my + 8);      // x=21 (panel-left + padding)
spr.setCursor(4, H - AREA + 2);     // x=4
```

**Right-aligned** (OFF-SCREEN on 135px display):
```cpp
spr.setCursor(W - 60, H - 10);      // x=180 → clipped
spr.setCursor(W - 28, y);           // x=212 → clipped
spr.setCursor(W - 24, H - LH - 2);  // x=216 → clipped
spr.setCursor(W - 60, H - 12);      // x=180 → clipped
```

**Center-aligned** (clock only, partially visible):
```cpp
spr.setTextDatum(MC_DATUM);
spr.drawString(hm, CX, 140);        // x=120 — text centered, but right half clipped if text >30px wide
```

## What IS Visible on the Physical Panel

| Region | X Range | Content |
|--------|---------|---------|
| Left margin | 0–4 | Padding |
| Pet area | 90–134 | Buddy left portion (center at 120, right edge clipped) |
| Status msg | 4–134 | "connected", "2 sessions running" |
| Prompt tool | 4–134 | Tool name (left-aligned, fits) |
| Prompt hint | 4–134 | Hint text (left-aligned, fits) |
| "A: approve" | 4–60 | Approval button (visible) |
| "B: deny" | 180–240 | **OFF-SCREEN** |
| Species counter | 180–240 | **OFF-SCREEN** |
| Page counter | 212–240 | **OFF-SCREEN** |
| Menu right half | 135–225 | **OFF-SCREEN** |
| HUD scroll indicator | 216–240 | **OFF-SCREEN** |

## Layout is Hardcoded for 240px, NOT Adaptive

- No `M5.Lcd.width()` queries anywhere
- No responsive breakpoints
- No alternative layouts for narrow displays
- `const int W = 240` is used uniformly
- Right-aligned elements assume 240px width
- Menu width (210px) exceeds display width (135px)

## SDL Port Decision: Stick to 240×240

Since upstream layout is **hardcoded for 240px and not adaptive**, using 135×240 in SDL would require **redesigning the entire layout** (moving right-aligned elements, shrinking menus, repositioning the pet). This is unnecessary work and diverges from upstream.

**Decision:** Use 240×240 logical resolution in SDL.

This gives us:
- Exact upstream layout parity (no redesign needed)
- All elements visible (unlike hardware where right side is clipped)
- Simple scaling via `SDL_RenderSetLogicalSize(240, 240)`
- Bug-for-bug compatible with upstream source

**Do NOT use 135×240** unless we want to fork the layout.

## If Hardware-Accurate Mode is Desired Later

To emulate the exact 135×240 hardware look:
- Add a viewport that clips rendering to 135×240
- Or scale the 240×240 frame down to 135×240
- But note: this hides UI elements that upstream placed at x=180+
- Better approach: fix the layout to fit 135px, then render at 135×240

## Recommendation

Render the full 240×240 as the upstream code draws it. The 135px clipping is a hardware limitation, not an intentional design choice. On desktop SDL, show the full 240×240 so all UI elements are visible.
