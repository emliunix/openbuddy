# Architecture

## Pet

### `PersonaVariant` — state index

Seven states, fixed indices. The index is used directly to look up the render function in `Species.states[]`.

| Index | Name | Meaning |
|---|---|---|
| 0 | `SLEEP` | Late-night / early-morning idle personality (time-of-day rule, see below) |
| 1 | `IDLE` | Connected, no active work |
| 2 | `BUSY` | One or more tool calls are executing |
| 3 | `ATTENTION` | Waiting for user approval of a prompt |
| 4 | `CELEBRATE` | Session just completed or level-up fired |
| 5 | `DIZZY` | Shake gesture detected |
| 6 | `HEART` | Prompt approved quickly (response time < 5 s) |

`SLEEP` is never returned by `select_persona`. It is applied by the time-of-day rule in `apply_tick` when `base_state` is `IDLE`.

### Time-of-day idle personality

When `base_state` is `IDLE` and no anim timer is running, `active_state` is biased by wall-clock hour:

| Hour window | `active_state` |
|---|---|
| 01:00–06:59 | `SLEEP` |
| 07:00–08:59 | `SLEEP` most of the time, brief `IDLE` flash periodically |
| 12:00–12:59 | `IDLE` most of the time, brief `HEART` flash periodically |
| 22:00–00:59 | `SLEEP` most of the time, brief `DIZZY` flash periodically |
| All other hours | `IDLE` |

This runs on every `ANIM_TICK` when `base_state == IDLE` and `anim_until == 0`.

### `Species`

Each species is a named set of seven render functions, one per state.

```cpp
struct Species {
    const char* name;
    uint16_t    body_color;   // RGB565; used as default sprite color
    StateFn     states[7];    // indexed by PersonaVariant
};

typedef void (*StateFn)(uint32_t t);
```

`t` is a monotonic tick counter incremented by `ANIM_TICK` (every 200 ms). Each `StateFn` uses `t` to select the current animation frame, driving looping sequences without any external frame state.

### Animation model

Each state function owns its frame sequence as a local `SEQ[]` array. The current frame is:

```
beat = (t / speed) % len(SEQ)
```

`speed` is a per-state divisor (typically 3–5 ticks per pose). `SEQ[beat]` indexes into a local poses array. The function calls `buddy_print_sprite` with the selected pose and optionally overlays particles (Z drifts, confetti, orbiting stars, rising hearts) using additional `t`-modulo expressions.

### Canvas

All rendering targets a fixed 135×64 px character grid.

| Constant | Value | Meaning |
|---|---|---|
| `BUDDY_X_CENTER` | 67 | Horizontal center for sprite alignment |
| `BUDDY_Y_BASE` | 30 | Top of sprite body |
| `BUDDY_Y_OVERLAY` | 6 | Top of overlay particle area |
| `BUDDY_CHAR_W` | 6 | Character cell width in pixels |
| `BUDDY_CHAR_H` | 8 | Character cell height in pixels |

Scale factor is 1 in peek mode, 2 in normal mode. All coordinates are pre-scale; `buddy_print_sprite` applies the scale.

---

## State Objects

### `NetworkState`

Mirrors upstream `TamaState`. Updated exclusively by `apply_message`.

```
running:            u32         -- count of sessions with agentic loop active
waiting:            u32         -- count of sessions with an unresolved permission prompt
total:              u32         -- count of open sessions
```

> **Derivation note**: the upstream wire spec (REFERENCE.md) defines `running`/`waiting`/`total` as concurrent session counts — semantically identical to what we send. The adaptation is only in how the plugin tracks them: opencode fires `tool.execute.before`/`session.idle`/`permission.ask` events per session; the plugin maintains per-session state and aggregates into instance-level counts before each heartbeat. `total` is the count of open sessions tracked by `session.created`/`session.deleted`. `running` is the count of sessions whose agentic loop is active (set on first `tool.execute.before` of a turn, cleared on `session.idle`). `waiting` is the count of sessions with an unresolved permission prompt. The `waiting > 0` threshold is unchanged from upstream. The `running >= 1` threshold is a deliberate deviation: upstream uses `>= 3` because Claude Desktop fans out multiple background sessions and 1–2 running is normal noise. opencode is a single-user CLI; one active session means the user's agentic loop is running, so `>= 1` is the correct threshold.

`connected` is derived: it is `true` when `now - last_live_ms < 30000`. It is not written directly on message receipt; it is recomputed each tick.

### `PersonaState`

```
base_state:   PersonaVariant   -- output of select_persona(); updated each ANIM_TICK
active_state: PersonaVariant   -- what the renderer draws; may be overridden by anim timer or time-of-day rule
anim_until:   u32              -- deadline shared by CELEBRATE_ANIM, HEART_ANIM, DIZZY_ANIM; 0 = none active
```

### `UiState`

Keyboard and display input. Updated exclusively by `apply_key`.

```
display_mode:    DisplayMode    -- NORMAL | PET | INFO
menu_open:       bool
menu_sel:        u8
species_cycle:   i8             -- +1/−1 set by key, consumed by renderer each frame
```

### `AppState`

Top-level owner. Passed by pointer to all handlers.

```
net:     NetworkState
persona: PersonaState
ui:      UiState
```

---

## Timers

Named timers referenced throughout the event handlers below.

| Timer | Description |
|---|---|
| `ANIM_TICK` | Global. Fires every 200 ms. Created once at program start, never cancelled. |
| `CONN_TIMEOUT` | Fires 30 s after the last live message. Replaced on every live message. |
| `CELEBRATE_ANIM` | Fires 3000 ms after creation. |
| `HEART_ANIM` | Fires 2000 ms after creation. |
| `DIZZY_ANIM` | Fires 2000 ms after creation. |
| `NAP_IDLE` | Inline staleness check inside `ANIM_TICK`. Tracks `_idle_since_ms`; fires after 10 min continuous `P_IDLE`. |

---

## Events

### Startup

1. Create `ANIM_TICK`

---

### Network events (`apply_message`)

#### Heartbeat received

1. `net.last_live_ms = now` — replace `CONN_TIMEOUT`
2. Update `net.running`, `net.waiting`, `net.total`, `net.tokens_today` with fallback (keep previous value if key absent)
3. If `tokens` key present: `net.tokens_bridge = value`; call `stats_on_bridge_tokens(value)`
4. `net.msg = value[:23]` if key present
5. Replace `net.lines`, `net.n_lines`; bump `net.line_gen` if count changed or last line ≠ msg
6. If `prompt` key present and `prompt_id` changed: `net.prompt_id = id`, `net.prompt_tool = tool`, `net.prompt_hint = hint`, `net.response_sent = false`, `net.prompt_arrived_ms = now`; force `ui.display_mode = NORMAL`, close all menus
7. If `prompt` key absent: clear `net.prompt_id`, `net.prompt_tool`, `net.prompt_hint`

#### Time sync received

1. `net.last_live_ms = now` — replace `CONN_TIMEOUT`

#### Owner command received

1. Write owner name to persistence

#### Transport disconnected

1. `net.last_live_ms = 0` — `connected` becomes false on next tick
2. Clear `net.prompt_id`, `net.prompt_tool`, `net.prompt_hint`

---

### Timer events (`apply_tick`)

#### `ANIM_TICK` fires

1. Recompute `net.connected = (now - net.last_live_ms < 30000)`
2. `persona.base_state = select_persona(net)`
3. If `persona.anim_until != 0` and `now >= persona.anim_until`: `persona.active_state = persona.base_state`, `persona.anim_until = 0`
4. If `stats_poll_level_up()`: `persona.active_state = CELEBRATE`, `persona.anim_until = now + 3000`
5. If `persona.anim_until == 0`:
   - if `persona.base_state == IDLE`: apply time-of-day rule → `persona.active_state`
   - otherwise: `persona.active_state = persona.base_state`
6. Advance renderer frame counter for `persona.active_state`

Anim timer cancellation on `ATTENTION`: step 3 only expires timers naturally. Cancellation is eager — when `select_persona` returns `ATTENTION` at step 2, step 5 sets `persona.active_state = ATTENTION` immediately (since `anim_until` is still nonzero, step 3 does not fire, but step 5's else-branch overwrites `active_state` regardless of `anim_until`).

#### `CONN_TIMEOUT` fires

Superseded by `ANIM_TICK` recomputing `connected` from `last_live_ms` staleness. `CONN_TIMEOUT` is not a separate timer — staleness is checked inline every `ANIM_TICK`.

#### `NAP_IDLE` — inline inside `ANIM_TICK`

Not a discrete SDL timer. Tracked via two statics in `session.cpp`: `_idle_since_ms` and `_napping`.

**Creation**: when `persona.base_state == P_IDLE` and `_idle_since_ms == 0`, set `_idle_since_ms = now`.

**Cancellation**: when `persona.base_state != P_IDLE` and not napping, set `_idle_since_ms = 0`.

**Trigger**: when `now - _idle_since_ms >= 10 min` and not already napping:
1. `_napping = true`, `_nap_start_ms = now`, `_idle_since_ms = 0`

**Wake** (nap end): when `_napping` and `persona.base_state != P_IDLE`:
1. `stats_on_nap_end((now - _nap_start_ms) / 1000)`
2. `stats_on_wake(now)` — resets energy to 5/5, records wake timestamp
3. `_napping = false`

---

### Key events (`apply_key`)

#### Y key — approve prompt

Condition: `net.prompt_id` non-empty, `net.response_sent = false`, `ui.menu_open = false`

1. Send `PermissionCmd{decision="once", id=net.prompt_id}`
2. `net.response_sent = true`
3. Record approval in stats
4. If `now - net.prompt_arrived_ms < 5000`: `persona.active_state = HEART`, `persona.anim_until = now + 2000`

#### N key — deny prompt

Condition: `net.prompt_id` non-empty, `net.response_sent = false`, `ui.menu_open = false`

1. Send `PermissionCmd{decision="deny", id=net.prompt_id}`
2. `net.response_sent = true`
3. Record denial in stats

#### Display mode keys (I, P)

1. Toggle `ui.display_mode`

#### Menu keys (M, UP, DOWN, RETURN)

1. Mutate `ui.menu_open`, `ui.menu_sel`

#### Species cycle keys (S, X)

1. `ui.species_cycle = +1 or −1` (consumed by renderer)

---

## Pure Functions

### `select_persona(net: &NetworkState) → PersonaVariant`

Priority order (matches upstream `derive()`):

1. `!net.connected` → `IDLE`
2. `net.waiting > 0` → `ATTENTION`
3. `net.running >= 1` → `BUSY`
4. otherwise → `IDLE`

`SLEEP` is never returned by `select_persona`. It is applied by the time-of-day rule in `ANIM_TICK`.

---

## Info Screen (DISP_INFO)

Upstream `drawInfo()` has 3 sub-pages cycled with button B:

| Page | Content | Valid for desktop? |
|------|---------|--------------------|
| ABOUT (1/3) | "I watch your Claude desktop sessions. I sleep when nothing's happening…" — behavioral description of the physical device | **No.** Describes a hardware pet. The persona state already communicates behavior visually. |
| BUTTONS (2/3) | Physical button bindings: A=front/next screen/approve, B=right/next page/deny, hold A=menu, Power=screen off | **No.** Hardware-specific. Desktop uses keyboard: F1/F2 toggle screens and pages, Y/N for approval, Esc for menu. |
| CLAUDE (3/3) | Session stats (sessions total / running / waiting), LINK section (transport name, BLE encryption status, last message age, persona state) | **Partially.** Session counters are valid. BLE encryption status has no desktop equivalent (we use TCP). Last message age and persona state are valid. |

### Desktop adaptation

We consolidate DISP_INFO into a single stats overview page — no sub-pages. Layout (below pet sprite, TOP=70):

| Row | Content | Source |
|-----|---------|--------|
| TOP+2 | `Lv {level}  {persona}` | `stats().level`, `persona_name()` |
| +12 | `appr {n}  deny {n}` | `stats().approvals`, `stats().denials` |
| +12 | `tokens {n}` | `stats().tokens` |
| +12 | `run {n}  wait {n}` | `net.running`, `net.waiting` |
| +12 | `species {name}` | `pet_name()` |
| +12 | `pet #{idx}` | `buddy_species_idx()` |

Sub-pages are omitted because ABOUT and BUTTONS have no desktop equivalent. A keyboard help line is rendered at the bottom instead of a separate page.

> **Derivation note**: upstream shows 3 info pages; we collapse to 1. ABOUT and BUTTONS are hardware-device-specific content. CLAUDE's BLE encryption/link info is replaced with TCP connection status (implicit — connected state drives persona, no explicit link status line needed). This is a deliberate deviation from upstream; the alternative of keeping 3 pages with rewritten content adds no value for a desktop CLI companion.

## Approval Panel

When `net.prompt_id` is non-empty, the approval panel replaces all other HUD content. Layout (bottom 78 px of the window):

| Row | Content |
|---|---|
| +4 px | `"approve? Xs"` — elapsed seconds since `prompt_arrived_ms`; turns red after 10 s |
| +14 px | `net.prompt_tool` — large text if ≤10 chars, small if longer |
| +34 px | `net.prompt_hint[:21]` — dimmed |
| +42 px | `net.prompt_hint[21:42]` — dimmed, only if hint > 21 chars |
| bottom | `"Y: approve"` / `"N: deny"` — or `"sent..."` if `response_sent` |

---

## Module Boundaries

```
main.cpp            SDL event loop; owns AppState, ITransport, Renderer
apply_message.cpp   NetworkState mutations from wire messages
apply_tick.cpp      ANIM_TICK handler: connected recompute, select_persona, time-of-day, frame advance
apply_key.cpp       UiState + response mutations from keyboard
select_persona.cpp  Pure function; no side effects
render_session.cpp  Reads AppState as const; no mutations
transport/          AsioTransport, MockTransport; emit DaemonMsg, accept BuddyMsg
stats.cpp           Approval/denial/token counters; owned outside AppState
persistence.cpp     Load/save owner name, pet name, species index
```
