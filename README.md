# OpenBuddy

Desktop buddy for OpenCode. Buddy is the TCP server; OpenCode plugin connects.

## Goal

Port [claude-desktop-buddy](claude-desktop-buddy/) — an M5StickC firmware desktop companion for Claude — to a native desktop SDL2 app for use with OpenCode.

The upstream project is the **source of truth**. Behavior, state machine, personas, protocol semantics, and rendering all derive from it.

## Derivation Policy

**The upstream is authoritative.** Any deviation from upstream behavior requires:

1. Review of the upstream source to confirm the intended behavior.
2. Careful consideration of whether the deviation is genuinely necessary (e.g., platform difference, single-session vs multi-session environment).
3. Explicit documentation in `docs/PROTOCOL.md` or `docs/ARCHITECTURE.md` alongside the relevant spec — not in code comments alone.

Undocumented deviations are defects.

## Well-Known Locations

| Path | Purpose |
|---|---|
| `start-buddy.sh` | Launches buddy; stdout/stderr piped to `buddy.log` |
| `buddy.log` | Buddy runtime log (written by `start-buddy.sh`) |
| `~/.local/share/opencode/log/` | OpenCode logs, including plugin logs |

## Structure

```
buddy/      C++20 SDL2 app: TCP server + renderer + input
plugin/     OpenCode plugin (connects to buddy)
characters/ Example packs
docs/       Protocol specs, architecture, design decisions
analysis/   Research notes and upstream explorations
changes/    Change plans and implementation notes
```

## Conventions

- Python: snake_case, 4 spaces, 100 cols
- C++: snake_case, 4 spaces, 100 cols, C++20
- Files: snake_case
- JSON keys: snake_case, alpha-sorted, no trailing commas
- Commits: conventional commits format
- Line endings: LF

## Coding Style

all defensive code except message parsing is invalid. All compatibility code is invalid. The input and processing should both be deterministic and represented as types.

## Docs

- `docs/SDL_GUIDELINES.md` — Mandatory reading before touching renderer/window code**
- `docs/PROTOCOL.md` — TCP wire format
- `docs/ARCHITECTURE.md` — Components, data flow, state machine
- `docs/counters.md` — Counter derivations (tokens, mood, energy, nap)
- `docs/menu.md` — Help page: upstream menu behavior and desktop adaptation (key-reference popover)
- `docs/OPENCODE_HOOKS.md` — Hook coverage research
- `TODO.md` — Backlog and done items
- `analysis/screen_behavior_facts.md` — **Screen rendering: corrected fact-based analysis** (official upstream uses 135×240, not 240×240)
- `analysis/screen_behavior.md` — **DEPRECATED** — based on incorrectly modified local copy (W=240 instead of W=135)
- `analysis/sdl_flickering_rca.md` — SDL flickering investigation (why we have the guidelines)

## References

- `claude-desktop-buddy/REFERENCE.md` — Original BLE protocol
