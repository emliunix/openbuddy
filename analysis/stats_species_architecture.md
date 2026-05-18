# Upstream Persistence, Stats & Species Architecture

## Stats System (`stats.h`)

### Persistent Data (NVS-backed)

```cpp
struct Stats {
    uint32_t napSeconds;       // cumulative face-down time
    uint16_t approvals;
    uint16_t denials;
    uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
    uint8_t  velIdx;
    uint8_t  velCount;
    uint8_t  level;
    uint32_t tokens;          // cumulative output tokens
};
```

**Level system:** 50,000 tokens per level (`TOKENS_PER_LEVEL`). Level-ups trigger celebration animation.

**Velocity ring buffer:** Tracks how fast user approves prompts (8 samples). Median velocity determines mood tier:
- <15s = tier 4 (happy)
- <30s = tier 3
- <60s = tier 2
- <120s = tier 1
- ≥120s = tier 0 (sad)

**Mood calculation:** Base from velocity, then penalized by denial ratio:
- Deny rate >50%: -2 tiers
- Deny rate >33%: -1 tier

**Energy:** Starts at 3/5 on boot, refills to 5/5 after nap. Drains 1 tier per 2 hours awake.

**Fed progress:** `(tokens % 50000) / 5000` = 0–10 pips within current level.

### Settings (NVS-backed)

```cpp
struct Settings {
    bool sound;      // beep on events
    bool bt;         // BLE discoverable (preference only)
    bool wifi;       // placeholder
    bool led;        // pulse LED on attention
    bool hud;        // show transcript HUD
    uint8_t clockRot; // 0=auto, 1=portrait, 2=landscape
};
```

### Persistence Strategy

- Load once at boot (`statsLoad()`, `settingsLoad()`)
- Save only on significant events (approval, denial, nap end, level-up)
- NVS has ~100K write cycles; avoid timer-based saves
- Token deltas accumulate in RAM; persist only on level-up milestone

## Species System (`buddy.h`, `buddy_common.h`)

### Architecture

Each species is a standalone `.cpp` file defining:

```cpp
struct Species {
    const char* name;
    uint16_t bodyColor;
    StateFn states[7];  // indexed by PersonaState: sleep, idle, busy, attention, celebrate, dizzy, heart
};
```

### Shared Geometry

```cpp
BUDDY_X_CENTER = 120;   // horizontal center of pet area
BUDDY_Y_BASE   = 120;   // vertical baseline
BUDDY_CHAR_W   = 6;     // text width at size 1
BUDDY_CHAR_H   = 8;     // text height at size 1
BUDDY_Y_OVERLAY = 6;    // particle offset (hearts, Zzz)
```

### Render Helpers

- `buddyPrintLine()` — centered text at given Y
- `buddyPrintSprite()` — multi-line ASCII art block
- `buddySetCursor()` / `buddySetColor()` / `buddyPrint()` — ad-hoc drawing

### Species Table (18 total)

capybara, duck, goose, blob, cat, dragon, octopus, owl, penguin, turtle, snail, ghost, axolotl, cactus, robot, rabbit, mushroom, chonk

### Scale Modes

- **Normal:** scale=2 (12px glyphs), pet occupies ~120×100px
- **Peek:** scale=1 (6px glyphs), smaller for clock overlay
- Pet is centered horizontally via `BUDDY_X_CENTER`

## Character System (`character.h` — GIF mode)

Not examined in detail yet. Key facts:
- GIFs stored in LittleFS `/characters/<name>/`
- `characterInit()` scans filesystem
- `characterLoaded()` checks if GIF available
- `characterRenderTo()` draws to target surface
- Fallback to ASCII buddy mode if no GIF

## Persistence for SDL Port

| Upstream | SDL Equivalent |
|----------|---------------|
| NVS (`Preferences`) | JSON file (`~/.config/openbuddy/state.json`) |
| `statsLoad/Save()` | Read/write JSON on events |
| `settingsLoad/Save()` | Read/write JSON on changes |
| `petNameLoad/Set()` | JSON field |
| `ownerSet/Name()` | JSON field |
| `speciesIdxLoad/Save()` | JSON field |

## Key Behaviors to Port

1. **Approval velocity tracking** — ring buffer, median calculation
2. **Token delta handling** — bridge restart detection, level-up celebration
3. **Energy decay** — 1 tier per 2h awake, nap refill
4. **Mood calculation** — velocity base + denial penalty
5. **Settings persistence** — sound, led, hud toggles
6. **Pet/owner names** — set via `name`/`owner` commands
7. **Species selection** — cycle through 18 + optional GIF mode

## Unresolved Questions

1. How does the GIF character system work exactly? (frame parsing, animation)
2. What is the `lineGen` bumping logic for transcript scroll reset?
3. How does the BLE bridge handle reconnection vs fresh pairing?
4. What is the exact TFT_eSPI display configuration (240 vs 135 exposed)?
