import os
import re
import glob

REF_DIR = "claude-desktop-buddy/src/buddies"
OUT_DIR = "buddy/src/species"

os.makedirs(OUT_DIR, exist_ok=True)

for src_path in glob.glob(f"{REF_DIR}/*.cpp"):
    name = os.path.basename(src_path)
    with open(src_path, 'r') as f:
        content = f.read()

    # Remove M5StickCPlus includes and TFT_eSprite extern
    content = re.sub(r'#include\s+<M5StickCPlus\.h>\n', '', content)
    content = re.sub(r'extern\s+TFT_eSprite\s+spr;\n', '', content)

    # Change includes
    content = content.replace('#include "../buddy.h"', '#include "../species.h"')
    content = content.replace('#include "../buddy_common.h"', '#include "../species.h"')

    # Convert function names from camelCase to snake_case
    content = content.replace('buddyPrintSprite', 'buddy_print_sprite')
    content = content.replace('buddyPrintLine', 'buddy_print_line')
    content = content.replace('buddySetCursor', 'buddy_set_cursor')
    content = content.replace('buddySetColor', 'buddy_set_color')
    content = content.replace('buddyPrint', 'buddy_print')

    # Add header comment
    header = f"""// AUTO-PORTED from claude-desktop-buddy/src/buddies/{name}
// Minimal edits: function names converted to snake_case, M5 includes removed.
// See species/README.md for how to add new species.

"""
    content = header + content

    out_path = os.path.join(OUT_DIR, name)
    with open(out_path, 'w') as f:
        f.write(content)

    print(f"Converted {name}")

print("Done.")
