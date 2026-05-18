# Upstream State Graph — claude-desktop-buddy/src/main.cpp

## States

```
P_SLEEP     (0)  — sleeping, only shows in clock/idle mode
P_IDLE      (1)  — awake but nothing happening
P_BUSY      (2)  — 3+ sessions running
P_ATTENTION (3)  — permission/question pending
P_CELEBRATE (4)  — recently completed OR level up
P_DIZZY     (5)  — shake detected
P_HEART     (6)  — fast approval (<5s)
```

## State Variables

- `baseState`: derived from session data every tick
- `activeState`: what the pet actually displays; may be overridden by one-shot
- `oneShotUntil`: millis() + duration; when expired, activeState reverts to baseState
- `wakeTransitionUntil`: millis() + 12000; prevents idle→sleep for 12s after screen wake

## Derive Function (every tick)

```
derive(tama):
  if !connected              → P_IDLE
  if sessionsWaiting > 0     → P_ATTENTION
  if recentlyCompleted       → P_CELEBRATE
  if sessionsRunning >= 3    → P_BUSY
  else                       → P_IDLE
```

**Note**: Disconnected returns P_IDLE, NOT P_SLEEP. Sleep is only a UI/clock-mode state.

## Tick Transitions

1. `statsPollLevelUp()` → `triggerOneShot(P_CELEBRATE, 3000)`
2. `baseState = derive(tama)`
3. If `baseState == P_IDLE && now < wakeTransitionUntil` → `baseState = P_SLEEP`
4. If `now >= oneShotUntil` → `activeState = baseState`

## One-Shot Triggers

| Trigger | State | Duration | Condition |
|---------|-------|----------|-----------|
| Level up | P_CELEBRATE | 3000ms | `statsPollLevelUp()` |
| Shake | P_DIZZY | 2000ms | IMU shake + not in one-shot |
| Fast approval | P_HEART | 2000ms | Approval took <5s |
| Completed session | P_CELEBRATE | — | From `derive()` via `recentlyCompleted` |

## Input Transitions

### Prompt Arrival (new promptId)
- `wake()`
- `responseSent = false`
- `beep(1200, 80)`
- `displayMode = DISP_NORMAL`
- Close menus
- `invalidate()`

### BtnA (approve)
- If in prompt: send permission cmd, `responseSent = true`
- `statsOnApproval(tookS)`
- `beep(2400, 60)`
- If `tookS < 5`: `triggerOneShot(P_HEART, 2000)`

### BtnB (deny)
- If in prompt: send permission cmd (deny), `responseSent = true`
- `statsOnDenial()`

## Clock Mode (idle + USB + valid RTC + no sessions)

Directly sets `activeState` (not `baseState`) based on hour:

| Hour | State | Probability |
|------|-------|-------------|
| 1-6 | P_SLEEP | 100% |
| Weekend | P_HEART | 1/6 chance |
| Weekend | P_SLEEP | 5/6 chance |
| <9 (weekday) | P_IDLE | 1/4 chance |
| <9 (weekday) | P_SLEEP | 3/4 chance |
| 12 | P_HEART | 1/3 chance |
| 12 | P_IDLE | 2/3 chance |
| Friday ≥15 | P_CELEBRATE | 1/3 chance |
| Friday ≥15 | P_IDLE | 2/3 chance |
| ≥22 or 0 | P_DIZZY | 1/3 chance |
| ≥22 or 0 | P_SLEEP | 2/3 chance |
| Else | P_SLEEP | 1/5 chance |
| Else | P_IDLE | 4/5 chance |

## Sleep vs Idle

- **P_SLEEP**: Only shown when clocking OR during wakeTransitionUntil
- **P_IDLE**: Default when connected but nothing happening
- **Disconnected**: `derive()` returns P_IDLE, not P_SLEEP

## Key Differences from Our Code

1. We currently return P_SLEEP when disconnected; upstream returns P_IDLE
2. We don't have `wakeTransitionUntil` logic
3. We don't trigger P_CELEBRATE on level-up
4. We don't have shake detection → P_DIZZY
5. We don't have fast-approval → P_HEART
6. We don't have clock mode time-based states
