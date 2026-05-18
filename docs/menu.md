# Menu / Help Page

Upstream menu behavior and desktop adaptation. Follows the derivation policy in `README.md`: upstream is authoritative; deviations are documented here with reasoning.

---

## Upstream menu

Opened by holding button A (`main.cpp:1071`). Six items (`main.cpp:137`):

| # | Label | Action |
|---|-------|--------|
| 0 | settings | Open settings sub-panel (`settingsOpen = true`) |
| 1 | turn off | `M5.Axp.PowerOff()` — hardware power-off |
| 2 | help | `displayMode = DISP_INFO`, `infoPage = INFO_PG_BUTTONS` |
| 3 | about | `displayMode = DISP_INFO`, `infoPage = INFO_PG_CREDITS` |
| 4 | demo | Toggle `dataDemo()` on/off (auto-cycles persona states) |
| 5 | close | `menuOpen = false` |

Navigation: button B cycles items (`menuSel = (menuSel + 1) % MENU_N`); button A confirms (`menuConfirm()`). A hint row shows button labels at the bottom of the panel (`drawMenuHints`).

### Settings sub-panel

Opened from menu item 0. Eight settings items (`main.cpp:258–281`):

| # | Label | Value |
|---|-------|-------|
| 0 | brightness | 1–4 |
| 1 | sound | on/off |
| 2 | bt | on/off (Bluetooth) |
| 3 | wifi | on/off |
| 4 | led | on/off |
| 5 | hud | on/off |
| 6 | clock | auto/port/land (clock orientation) |
| 7 | species | N/total (species index) |

---

## Desktop adaptation

> **Derivation note**: upstream has a 6-item navigable menu opened by holding button A. Desktop replaces this entirely with a non-interactive help popover on `H`. The upstream menu items (settings, turn off, help, about, demo, close) are all hardware-specific or debug-only — none map meaningfully to desktop. The navigable item list adds no value when all meaningful actions are already on direct keys. A key-reference overlay serves the actual user need (discoverability) without the navigable chrome.

**Decision**: replace the upstream navigable menu with a simple help popover. Key `H` toggles it; `ESC` also closes it when open. No item selection — display only.

Content: all active key bindings:

| Key | Action |
|-----|--------|
| `W` | Next species |
| `S` | Previous species |
| `A` | Toggle info page |
| `D` | Toggle pet page |
| `Y` | Approve prompt |
| `N` | Deny prompt |
| `H` | Toggle help |
