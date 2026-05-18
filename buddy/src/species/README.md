# Species

Species files are ported from `claude-desktop-buddy/src/buddies/` with minimal edits.

## Adding a New Species

1. Copy any existing `.cpp` file in this directory.
2. Rename the namespace and species struct.
3. Edit the 7 ASCII art state functions.
4. Register the species in `species_table.cpp`.

No third-party dependencies. Each file only uses:
- `buddy_renderer.h` helpers (`buddy_print_sprite`, `buddy_set_cursor`, etc.)
- Standard RGB565 color constants

## Port Notes

Changes from original:
- Remove `#include <M5StickCPlus.h>` and `extern TFT_eSprite spr;`
- Change `buddyPrintSprite` to `buddy_print_sprite`, etc. (snake_case)
- Remove `extern const Species CAT_SPECIES;` declarations (now in species_table.cpp)

Everything else — art arrays, animation sequences, particle effects — stays identical.
