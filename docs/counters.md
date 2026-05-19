# Counters

All numeric counters tracked by OpenBuddy — their wire source, derivation logic, and relevant source locations. Follows the derivation policy in `README.md`: upstream is authoritative; deviations are documented here with reasoning.

---

## 1. Session activity counters

Sent by the plugin every heartbeat and stored in `NetworkState` (`session.h`). They drive persona selection.

| Counter | Wire field | Description |
|---------|-----------|-------------|
| `running` | `running` | Sessions with agentic loop active |
| `waiting` | `waiting` | Sessions with unresolved permission prompt |
| `total` | `total` | Open session count |
| `completed` | `completed` | One-shot flag: last turn finished |
| `tokens` | `tokens` | Cumulative output tokens, instance lifetime |
| `tokens_today` | `tokens_today` | Output tokens today |

Upstream: `tokens_today` is reset at midnight by the plugin. `running >= 3` drives `P_BUSY` because Claude Desktop fans out multiple background sessions and 1–2 running is normal noise.

> **Derivation note**: `tokens_today` — desktop has no account-level token data and no midnight-reset mechanism. `tokens_today` is duped from `tokens` (instance-lifetime accumulation). Acceptable because the distinction is meaningless without an account API.
>
> **Derivation note**: `running >= 1` drives `P_BUSY` instead of upstream `>= 3`. opencode is a single-user CLI; one active session means the user's agentic loop is running, so `>= 1` is the correct threshold.

---

## 2. Token accumulation (buddy-side)

Upstream logic: plugin sends `tokens` = cumulative total since the plugin started. Buddy tracks the last seen value and computes the delta per heartbeat. Only the delta is added to the buddy's lifetime counter via `stats_on_bridge_tokens(delta)` (`stats.cpp:81–86`). Every 50 000 tokens = level up (`TOKENS_PER_LEVEL`, `stats.cpp:4`).

The buddy also implements a `_tokensSynced` first-sight latch (`stats.h:76–88`):
- On the very first heartbeat after device boot, the buddy records the plugin's cumulative total as a baseline.
- No tokens are added on that first packet.
- This prevents a device restart from re-crediting the entire plugin session.
- If the plugin restarts (value drops), the buddy resyncs to the new lower baseline without adding a negative delta.

`tokens_today` is sent as an absolute value and stored directly in `NetworkState` (no delta logic). It is display-only and not persisted.

`tokens` (the lifetime cumulative counter) is persisted to `~/.config/openbuddy/state.json` under `stats.tokens` and restored on load. This preserves level and fed progress across buddy restarts.

This delta+latch scheme relies on the device being bound to a single Claude Code app where `tokens` is monotonically non-decreasing (only reset on app restart). A multi-instance setup where different apps send independent cumulative totals would corrupt the delta tracking.

---

## 3. Permission counters

Upstream logic: `stats().approvals` and `stats().denials` are incremented on key press (`session.cpp:236`, `session.cpp:246`). `stats_on_approval(seconds)` also records response time into the velocity ring buffer used for mood calculation.

Desktop matches upstream. `sendStatus()` in the plugin hardcodes `appr: 0, deny: 0` in the status response — the buddy reads its own internal `stats()` struct, not the wire value, so this is cosmetic only.

---

## 4. Pet counters (DISP_PET panel)

### Level

Upstream logic: `stats().tokens / TOKENS_PER_LEVEL`. `stats_on_bridge_tokens()` checks for level crossing and sets `_level_up_pending`; `stats_poll_level_up()` (`stats.cpp:67`) returns true once per crossing. Displayed at `session_renderer.cpp:111`, `session_renderer.cpp:175`.

Desktop matches upstream.

### Fed (0–10 pip bar)

Upstream logic: `stats_fed_progress()` → `(tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10)`. Progress within current level; one pip per 5 000 tokens. Displayed at `session_renderer.cpp:150–157`.

`tokens` is persisted to `~/.config/openbuddy/state.json` and restored on load, so fed progress survives restarts.

Desktop matches upstream.

### Mood (0–4 hearts)

Upstream logic (`stats.cpp:41–57`):
1. Base tier from median of last 8 approval response-time samples (velocity ring buffer `stats().velocity[8]`). Each sample is the number of seconds the user took to press Y after a permission prompt appeared. The median of the ring is mapped to a tier:
   - No data → tier 2
   - `< 15s` → 4, `< 30s` → 3, `< 60s` → 2, `< 120s` → 1, else → 0
2. Ratio adjustment when `approvals + denials >= 3`:
   - `denials > approvals` → −2; `denials*2 > approvals` → −1

`stats_on_approval(seconds_to_respond)` populates velocity ring — called from `session.cpp:236`. Displayed at `session_renderer.cpp:141–146`. Mood is stuck at tier 2 until the first approval populates the velocity ring — this matches upstream behavior on a fresh boot.

`approvals` and `denials` counts are persisted to `~/.config/openbuddy/state.json` and restored on load. The velocity ring is **not** persisted — mood resets to tier 2 after every restart regardless of approval history. This matches upstream (M5 NVS also only persists counts, not the ring).

Desktop matches upstream.

### Energy (0–5 bar)

Upstream logic (`claude-desktop-buddy/src/stats.h:160–169`): starts 3/5 on boot; drains 1 tier per 2 hours awake; resets to 5/5 on nap end (`statsOnWake()`). Displayed at `session_renderer.cpp:159–169`.

Current implementation: `stats.cpp:59–61` hardcodes `return 3`.

> **Derivation note**: upstream energy drain relies on a wall-clock timer and nap detection via accelerometer (device face-down). Desktop has no accelerometer. Timer-based drain is implementable but the nap trigger is unresolved. Hardcoded 3 is a placeholder pending a decision on the desktop nap substitute.

### Napped (cumulative hh:mm)

Upstream logic: device face-down (accelerometer) accumulates seconds into `nap_seconds`; `statsOnWake()` also triggers energy refill. Displayed at `session_renderer.cpp:182–183`.

Current implementation: `stats().nap_seconds` field exists and is displayed but nothing increments it.

> **Derivation note**: desktop has no accelerometer. A substitute trigger is needed. See candidates below.

#### Upstream nap mechanics (reference)

- `faceDown()` (`main.cpp:92–94`): `az < -0.7 && |ax| < 0.4 && |ay| < 0.4` — device lying screen-down.
- Hysteresis: `faceDownFrames` counter; nap starts after 15 consecutive face-down frames, ends after 8 consecutive face-up frames (`main.cpp:1236–1250`).
- On nap start: `napping = true`, record `napStartMs`.
- On nap end: `statsOnNapEnd(seconds)` accumulates into `nap_seconds`; `statsOnWake()` sets `_lastNapEndMs` and `_energyAtNap = 5` (full energy refill).
- While napping: sprite render is skipped entirely — screen goes blank (`main.cpp:1188`).
- The pet shows `P_SLEEP` persona during nap because `select_persona` returns `P_IDLE` (not running/waiting/completed) and `time_of_day_idle` returns `P_SLEEP` when `napping` suppresses any other state. (Actually upstream sets `activeState = SLEEP` directly on nap start — `main.cpp:1244–1246`.)

#### Desktop nap candidates

The nap trigger must map to a state that is:
1. Unambiguously "the user has stepped away" — not just between tool calls.
2. Reversible — a clear wake event exists.
3. Preferably passive — no explicit user gesture required.

**Candidate A — `P_SLEEP` persona as nap proxy**

`P_SLEEP` is already shown by `time_of_day_idle` during night hours (01:00–07:00) and late evening (22:00–00:00). When `active_state == P_SLEEP` and `base_state == P_IDLE`, the pet is visually sleeping. This state is entered and exited automatically by the wall-clock hour. Nap accumulation could start when `active_state` transitions to `P_SLEEP` and stop on transition away.

- Entry: `time_of_day_idle` returns `P_SLEEP` (hour 1–7, 22–24/0).
- Exit: hour advances out of the sleep window, `time_of_day_idle` returns `P_IDLE` or `P_DIZZY`.
- Problem: `P_SLEEP` also fires for `P_DIZZY`/`P_HEART` blends — transitions are noisy every few seconds at boundary hours. Also couples nap to time-of-day, not actual user absence.

**Candidate B — inactivity idle timer**

Start a wall-clock idle timer when `base_state == P_IDLE` (no sessions running/waiting/completed). If idle continuously for N minutes (e.g., 10), transition into nap. Wake on any session activity (`running`, `waiting`, or `completed` going non-zero).

- Entry: `base_state == P_IDLE` for ≥ N minutes.
- Exit: any heartbeat with `running > 0`, `waiting > 0`, or `completed == true`.
- Clean mapping: "user has been away from opencode long enough" → nap. Wake is automatic on next agentic turn.
- Does not require `P_SLEEP` persona; nap and sleep are independent — nap can occur at any hour.
- This is the closest desktop analogue to face-down detection: both require sustained absence of activity.

**Candidate C — explicit key**

Dedicate a key (e.g., `N` when not in permission prompt) to toggle nap manually.

- Entry/exit: key press.
- Simple to implement; requires user awareness. Not passive — diverges from upstream feel.

**Recommendation**: Candidate B (inactivity idle timer). It is the only passive, unambiguous, reversible trigger available on desktop without hardware. Suggested threshold: 10 minutes of continuous `base_state == P_IDLE`. No decision finalised yet; `nap_seconds` remains at 0.

**Decision**: Candidate B — inactivity idle timer. Threshold: 10 minutes of continuous `base_state == P_IDLE`. Wake on any heartbeat with `running > 0`, `waiting > 0`, or `completed == true`. Not yet implemented; `nap_seconds` remains at 0.
