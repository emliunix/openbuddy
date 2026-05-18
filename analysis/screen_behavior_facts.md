# Screen Behavior Analysis — Source-Verified Facts

> **Critical finding:** Local copy had been modified from `W=135` to `W=240`. Official upstream uses `W=135, H=240` matching the M5StickC Plus 135×240 display. All coordinates below use the official upstream values.

---

## 1. Display Controller Coordinate System (TFT)

### fact: TFT configured as 135×240 portrait
**Source:** `upstream/M5StickC-Plus/src/utility/In_eSPI_Setup.h:54-57`
```cpp
#define TFT_WIDTH  135
#define TFT_HEIGHT 240
```

### fact: Constructor initializes _width=135, _height=240
**Source:** `upstream/TFT_eSPI/TFT_eSPI.cpp:439-442`
```cpp
TFT_eSPI::TFT_eSPI(int16_t w, int16_t h) {
  _init_width  = _width  = w;  // = 135
  _init_height = _height = h;  // = 240
  resetViewport();
}
```

### fact: Rotation 0 sets colstart=52, rowstart=40
**Source:** `upstream/M5StickC-Plus/src/utility/ST7789_Rotation.h:8-14`
```cpp
case 0:  // Portrait
#ifdef CGRAM_OFFSET
    if (_init_width == 135) {
        colstart = 52;
        rowstart = 40;
    }
```

### fact: Viewport is [0,0) to [135,240)
**Source:** `upstream/TFT_eSPI/TFT_eSPI.cpp:270-283`
```cpp
void TFT_eSPI::resetViewport(void) {
  _vpX = 0;
  _vpY = 0;
  _vpW = width();   // = 135
  _vpH = height();  // = 240
  _xDatum = 0;
  _yDatum = 0;
}
```

**TFT coordinate system:** x ∈ [0, 135), y ∈ [0, 240)

---

## 2. Sprite Coordinate System

### fact: Sprite dimensions match display: 135×240
**Source:** `claude-desktop-buddy/src/main.cpp:23,954`
```cpp
const int W = 135, H = 240;
// ...
spr.createSprite(W, H);  // 135×240
```

### fact: Sprite uses same coordinate origin as TFT
**Source:** `upstream/TFT_eSPI/Extensions/Sprite.cpp:51-59`
```cpp
void* TFT_eSprite::createSprite(int16_t w, int16_t h, uint8_t frames) {
  _iwidth  = _dwidth  = _bitwidth = w;   // = 135
  _iheight = _dheight = h;               // = 240
  setViewport(0, 0, _dwidth, _dheight);  // [0,0) to [135,240)
  setPivot(_iwidth/2, _iheight/2);
}
```

**Sprite coordinate system:** x ∈ [0, 135), y ∈ [0, 240)

---

## 3. Sprite-to-TFT Transfer

### fact: pushSprite passes sprite dimensions directly
**Source:** `upstream/M5StickC-Plus/src/utility/Sprite.cpp:456-464`
```cpp
void TFT_eSprite::pushSprite(int32_t x, int32_t y) {
    if (!_created) return;
    if (_bpp == 16)
        _tft->pushImage(x, y, _iwidth, _iheight, _img);  // _iwidth=135, _iheight=240
}
```

### fact: pushImage clips to viewport via PI_CLIP
**Source:** `upstream/TFT_eSPI/TFT_eSPI.cpp:41-59`
```cpp
#define PI_CLIP
  // ... clips to [_vpX, _vpY) to [_vpW, _vpH)
```

### fact: Applied to pushImage(0, 0, 135, 240, _img)
With viewport [0,0) to [135,240):
- `x = 0`, `y = 0`, `w = 135`, `h = 240`
- `(x + dw) > _vpW`? `(0 + 135) > 135`? **No** (equal, not greater)
- Final: `dw = 135`, `dh = 240` — **entire sprite fits, nothing clipped**

### fact: setWindow adds colstart/rowstart for physical panel
**Source:** `upstream/TFT_eSPI/TFT_eSPI.cpp:3410-3414`
```cpp
#ifdef CGRAM_OFFSET
    x0+=colstart;   // +52
    x1+=colstart;   // +52
    y0+=rowstart;   // +40
    y1+=rowstart;   // +40
#endif
```

**Result:** `setWindow(0, 0, 134, 239)` → controller receives CASET(52,186), RASET(40,279), mapping to the centered 135×240 physical panel.

---

## 4. Coordinate Alignment: Perfect Match

| System | Width | Height | X Range | Y Range | Alignment |
|--------|-------|--------|---------|---------|-----------|
| TFT viewport | 135 | 240 | [0, 135) | [0, 240) | Origin (0,0) |
| Sprite | 135 | 240 | [0, 135) | [0, 240) | Origin (0,0) |
| Sprite→TFT | 135 cols | 240 rows | [0, 135) | [0, 240) | pushSprite(0,0) → perfect 1:1 |
| Upstream layout | 135 | 240 | [0, 135) | [0, 240) | `const int W = 135` |

**Conclusion:** No clipping. Sprite dimensions exactly match TFT viewport. One-to-one pixel transfer.

---

## 5. Upstream Drawing Coordinates (all within bounds)

### fact: Menu panel is 118px wide, centered at x=8
**Source:** `claude-desktop-buddy/src/main.cpp:252-255`
```cpp
int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
int mx = (W - mw) / 2, my = (H - mh) / 2;
// mx = (135 - 118) / 2 = 8
// Panel spans x=8 to x=126 (fits within 135)
```

### fact: Prompt panel uses W=135
**Source:** `claude-desktop-buddy/src/main.cpp:528-557`
```cpp
spr.fillRect(0, H - AREA, W, AREA, p.bg);     // fillRect(0, 162, 135, 78)
spr.drawFastHLine(0, H - AREA, W, p.textDim); // drawFastHLine(0, 162, 135)
spr.setCursor(4, H - AREA + 4);               // (4, 166) — "approve? Ns"
spr.setCursor(4, H - AREA + 14/18);           // (4, 176/180) — tool name
spr.setCursor(4, H - AREA + 34);              // (4, 196) — hint
spr.setCursor(4, H - 12);                     // (4, 228) — "A: approve"
spr.setCursor(W - 48, H - 12);                // (87, 228) — "B: deny"
```

**Constraint check:** "B: deny" at x=87. Text is 7 chars × 6px = 42px wide. Right edge = 87 + 42 = 129. **129 < 135 ✓**

### fact: Pet centered at BUDDY_X_CENTER = 67
**Source:** `claude-desktop-buddy/src/buddy.cpp:12`
```cpp
const int BUDDY_X_CENTER = 67;   // = 135/2
const int BUDDY_CANVAS_W = 135;
```

### fact: Pet lines trimmed and centered at scale=2
**Source:** `claude-desktop-buddy/src/buddy.cpp:42-53`
```cpp
void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  if (_scale > 1) { /* trim spaces */ }
  int w = len * BUDDY_CHAR_W * _scale;     // len * 6 * 2 = len * 12
  int x = BUDDY_X_CENTER - w / 2 + xOff * _scale;  // 67 - w/2
}
```

**Example:** Capybara REST line `" ( o    o ) "` (12 chars raw)
- Trim trailing: `" ( o    o )"` → 11 chars
- Trim leading: `"( o    o )"` → 10 chars
- Width: `10 * 12 = 120px`
- x: `67 - 60 = 7`
- Right edge: `7 + 120 = 127`. **127 < 135 ✓**

### fact: HUD transcript width = 21 chars
**Source:** `claude-desktop-buddy/src/main.cpp:569`
```cpp
const int SHOW = 3, LH = 8, WIDTH = 21;
```
- 21 chars × 6px = 126px wide. **126 < 135 ✓**

### fact: Info page header within bounds
**Source:** `claude-desktop-buddy/src/main.cpp:507-518`
```cpp
spr.setCursor(4, y); spr.print("Info");
spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
// "n/N" at x = 135-28 = 107. "1/6" is 3 chars = 18px. Right edge = 125. ✓
```

---

## 6. Constraint Validation Table

| Element | X Position | Width | Right Edge | < 135? | Source |
|---------|-----------|-------|-----------|--------|--------|
| Menu panel | 8 | 118 | 126 | ✓ | main.cpp:252 |
| "A: approve" | 4 | ~60 | 64 | ✓ | main.cpp:555 |
| "B: deny" | 87 | 42 | 129 | ✓ | main.cpp:557 |
| Pet (capybara) | 7 | 120 | 127 | ✓ | buddy.cpp:49 |
| HUD transcript | 4 | 126 | 130 | ✓ | main.cpp:569 |
| Info header "n/N" | 107 | 18 | 125 | ✓ | main.cpp:514 |
| drawPasskey digits | (135-108)/2=13 | 108 | 121 | ✓ | main.cpp:524 |

---

## 7. Two Display Modes (Pet Scale)

The upstream has two display modes that control pet scale via `buddySetPeek()`:

### fact: Normal mode — pet at 2× scale (full size)

**Trigger:** `displayMode == DISP_NORMAL` → `peek = false` → `_scale = 2`

**Source:** `claude-desktop-buddy/src/main.cpp:125-128`
```cpp
void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  buddySetPeek(peek);  // false → _scale = 2
}
```

**Pet vertical extent:**
**Source:** `claude-desktop-buddy/src/buddy.cpp:190-192`
```cpp
// Clear the whole render strip — at 2× the body reaches y≈126
spr.fillRect(0, 0, BUDDY_CANVAS_W,
             (BUDDY_Y_BASE + 5 * BUDDY_CHAR_H + 12) * _scale, BUDDY_BG);
```

- Pet occupies top ~126px
- Bottom area (y≈212-240) used by HUD / approval panel
- Pet is centered, large, and prominent

### fact: Peek mode — pet at 1× scale (scaled down)

**Trigger:** `displayMode == DISP_INFO` or `DISP_PET` → `peek = true` → `_scale = 1`

**Pet vertical extent:**
**Source:** `claude-desktop-buddy/src/buddy.cpp:190-192`
```cpp
// at 1× ≈82
```

- Pet occupies top ~82px (smaller, less prominent)
- `drawInfo()` clears from y=70 down: `fillRect(0, TOP, W, H-TOP, p.bg)` where `TOP=70`
- Info/pet stats text starts at y=72, covering bottom ~170px
- Pet top portion remains visible as background

### fact: Draw flow in loop()

**Source:** `claude-desktop-buddy/src/main.cpp:1191-1229`
```cpp
// 1. Draw pet first (at current scale)
buddyTick(activeState);   // draws pet to sprite

// 2. Then draw info/pet/HUD on top
if (displayMode == DISP_INFO) drawInfo();      // overlays info text
else if (displayMode == DISP_PET) drawPet();   // overlays pet stats
else if (settings().hud) drawHUD();            // overlays transcript/status

// 3. Menu overlays everything (if open)
if (menuOpen) drawMenu();

// 4. Push to display
spr.pushSprite(0, 0);
```

**Mode comparison:**

| Mode | _scale | Pet Height | Text Area | Use Case |
|------|--------|-----------|-----------|----------|
| **Normal** | 2 | ~126px | y=212-240 (HUD/approval) | Home screen, shows status/transcript |
| **Info** | 1 | ~82px | y=70-240 (info pages) | Read device info, buttons help |
| **Pet** | 1 | ~82px | y=70-240 (pet stats) | View pet stats, how-to guide |

---

## 8. Rendering Paths Summary

**Primary path (normal/portrait mode):**
1. Draw to 135×240 sprite (coordinates match display exactly)
2. `pushSprite(0,0)` → transfers all 135×240 pixels
3. No clipping, no offset, perfect 1:1 mapping

**Secondary path (landscape clock):**
1. `setRotation(1 or 3)` → swaps dimensions to 240×135
2. Draw directly to `M5.Lcd` (no sprite)
3. Full 240px width visible (for clock display)
4. `setRotation(0)` to restore portrait

---

## 8. Summary

| Property | Value | Source |
|----------|-------|--------|
| Display config | 135×240 | In_eSPI_Setup.h |
| Runtime width() | 135 | TFT_eSPI.cpp constructor |
| Runtime height() | 240 | TFT_eSPI.cpp constructor |
| Viewport default | [0,0) to [135,240) | resetViewport() |
| Sprite size | 135×240 | main.cpp `createSprite(W,H)` |
| pushSprite(0,0) | pushes 135×240 image | Sprite.cpp |
| Clipping | **None** — sprite matches viewport | PI_CLIP (dw=135) |
| Alignment | 1:1 pixel mapping | _vpX=0, _xDatum=0 |
| Upstream layout constant | W=135, H=240 | main.cpp:23 |
| BUDDY_X_CENTER | 67 (=135/2) | buddy.cpp:12 |
| Layout adaptive? | Yes — uses display width | W=135 matches display |

**Key insight:** The official upstream code was designed specifically for the 135×240 M5StickC Plus display. All coordinates, panel widths, and pet centering use `W=135`, ensuring perfect fit with no clipping. The `colstart=52, rowstart=40` hardware offset centers the 135×240 panel in the ST7789's 240×320 controller RAM, independent from the software coordinate system.

**Note on local copy:** A previous session incorrectly modified `W` from 135 to 240 and `BUDDY_X_CENTER` from 67 to 120, which would cause apparent "clipping" of right-side content. The official upstream does not have this issue.
