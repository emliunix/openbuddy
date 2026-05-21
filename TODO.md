# OpenBuddy Todo

## Todo

- [x] **Interrupted triggers celebrate** — Ctrl-C: `session.error` → `erroredSessions` suppresses celebrate. Retry-exhausted: `oldStatus === "retry"` → no celebrate (retry→idle means failure). Ctrl-D: indistinguishable from natural completion, accepted as-is.
- [ ] **Error tracking for interrupted session.status lost** — `session.error` handling was removed in refactor; `erroredSessions` no longer populated, so interrupted sessions celebrate incorrectly again. Need to restore `session.error` → `erroredSessions.add()` logic.
- [ ] **Tool activity tracking removed** — `tool.execute.before` trigger removed (correctly, since it's a trigger not an event). Need to track tool calls via `message.part.updated` events or restore command formatting if entries should show recent commands.
- [ ] **Permission approval resets info/pet view** — #BUG when a permission is approved/declined, the display is reset from info/pet view back to normal, should keep the previous state
- [ ] **Shake support with mouse** — detect rapid mouse movement over the window as a shake gesture
- [ ] **Persist pet stats** — save mood, energy, fed, level to `state.json` on exit; restore on launch

## Done

- [x] **Celebration is a single fast flash** — `base_state` transitioning to `P_CELEBRATE` now latches `anim_until = now + 3000` so it outlives the `net.completed` flag being cleared by the next heartbeat
- [x] **Evaluate OpenCode hook coverage** — opencode plugin built and wired with hooks
- [x] **Species field on info page** — `buddy_species_name()` shown on DISP_INFO; was incorrectly showing `pet_name()` (tamagotchi name) labeled as species
- [x] **Interrupted session still celebrates** — `erroredSessions.delete` consolidated to `session.status: idle` only (single point of check+clear); validated against log: `session.error` always precedes `session.status: idle`, no celebration on abort
- [x] **Persist species selection** — `species_idx` saved to `state.json` on exit, restored on load via `buddy_set_species_idx()`
- [x] **Prepare for GitHub** — `.gitignore` created; excludes `buddy/build/`, `.cache/`, logs, upstream reference copies, plugin build artifacts, `.DS_Store`
- [x] **Pet name from plugin** — `BUDDY_PET_NAME` env var overrides persisted pet name at startup; owner name set via wire `OwnerCmd` (upstream behaviour preserved)
- [x] **Info page: remove page-switch hint** — replaced `F1:screen F2:page Esc:menu` with actual desktop key hints
- [x] **Key bindings** — `w`/`s` species cycle; `A`/`D` info/pet display mode; removed debug `A`/`Z` state cycle and `D` demo mode
- [x] **Do not prevent system sleep** — `SDL_EnableScreenSaver()` called in `init_sdl()` immediately after renderer creation
- [x] Token usage info — `session.next.step.ended` → `tokens.output` accumulated into `state.tokens` / `state.tokensToday`; sent in heartbeat
- [x] Permission approval — `permission.asked` bus event → FIFO queue → buddy Y/N → `postSessionIdPermissionsPermissionId`
- [x] Build clean; all 11 tests pass
- [x] All 18 species compile and link
- [x] Tool call transcript entries — `tool.execute.after` with `args.command`/`args.filePath`, case-insensitive matching
- [x] `Heartbeat.completed` wire field restored
- [x] `select_persona` P_CELEBRATE restored
- [x] Interruption detection (`session.error` + `MessageAbortedError` → `erroredSessions`)
- [x] Debug transport logging (`[transport] recv/send` at DEBUG level)
- [x] `running >= 1` for P_BUSY deviation documented
- [x] Always render on timer tick
- [x] Entry word-wrap — ported `wrapInto()` from upstream; `lines[8][92]`; 3 display rows at 8px in DISP_NORMAL HUD overlay
- [x] Init stats counters to 0 on startup — match upstream (`stats_init()` no longer forces `level=1`)
- [x] Plugin raw event logs — `logger.info("EVENT", ...)` on all `onEvent` callbacks
- [x] Help popover — `H` toggles, `ESC` closes (conventional, not shown); 4-row key table (W/S, A/D, Y/N, H); replaces navigable menu
- [x] Buddy SDL skeleton: 240x240 logical, always-on-top, HiDPI scale detection
- [x] SDL renderer with TFT_eSPI-compatible API
- [x] Port all 18 ASCII species with auto-conversion script
- [x] Buddy state machine with 7 states
- [x] Stats/settings system (simplified for desktop)
- [x] Input handling: keyboard mapped to buttons
- [x] Fix SDL flickering (root cause: early return in buddy_tick vs per-frame screen clear)
- [x] TCP transport (decided over BLE 2026-05-17)
- [x] Reuse claude-desktop-buddy JSON schema
- [x] Draft architecture and project structure
