# Protocol

TCP localhost:7887. NDJSON framing.

**Upstream wire spec**: `claude-desktop-buddy/REFERENCE.md` — authoritative definition of all fields, framing, and semantics. This doc describes our TCP adaptation; deviations from the upstream spec are noted explicitly.

## Connection Model

**One TCP connection, one opencode instance.**

The plugin establishes a single persistent TCP connection to buddy per opencode process. All sessions within that process are aggregated into instance-level heartbeat fields (`running`, `waiting`, `total`) before sending.

Buddy accepts only one connection at a time. If a second connection arrives while one is already active, buddy closes the new socket immediately and resumes listening. The existing connection is not affected.

> **Future**: multi-instance support (switchable windows per connected instance) is out of scope for now.

## Daemon -> Buddy

### Heartbeat

```json
{
    "completed": false,
    "entries": ["10:42 git push"],
    "msg": "approve: Bash",
    "prompt": {"hint": "rm -rf /tmp/foo", "id": "req_abc123", "tool": "Bash"},
    "running": 1,
    "tokens": 184502,
    "tokens_today": 31200,
    "total": 3,
    "waiting": 1
}
```

Sent on change and every 10s. No message for 30s = dead.

| Field | Spec semantics (REFERENCE.md) | OpenBuddy adaptation |
|---|---|---|
| `total` | Count of all open sessions | Count of open sessions within this instance |
| `running` | Sessions actively generating | Count of sessions with agentic loop active |
| `waiting` | Sessions blocked on permission | Count of sessions with an unresolved permission prompt |
| `completed` | One-shot `true` for one heartbeat after a turn finishes, then `false` | Same; set on `session.idle` |
| `tokens` | Cumulative output tokens since app start | Same |
| `tokens_today` | Output tokens since local midnight | Same |
| `msg` | One-line summary | Same |
| `entries` | Recent transcript lines, newest first | Same |
| `prompt` | Present when permission decision needed | Same; one pending prompt surfaced at a time |

> **Derivation note — `total`/`running`/`waiting`**: the upstream spec defines these as concurrent session counts (Claude desktop supports multiple open sessions). OpenCode supports multiple sessions per process too — the plugin maintains per-session state and aggregates into instance-level counts. `total` is the count of open sessions. `running` is the count of sessions whose agentic loop is active (set on first `tool.execute.before` of a turn, cleared on `session.idle`). `waiting` is the count of sessions with an unresolved permission prompt. These are semantically identical to upstream — the adaptation is only in how the plugin tracks them (opencode events vs BLE). The `running > 0` / `waiting > 0` thresholds in `select_persona` are unchanged from upstream.
>
 > **Note on `completed`**: not listed in `REFERENCE.md` but present in the reference implementation. `data.h` parses it from the wire (`out->recentlyCompleted = doc["completed"] | false`) and `derive()` in `main.cpp` uses it to select `P_CELEBRATE` in the persona priority chain (between `ATTENTION` and `BUSY`). It is a real wire field sent by the Claude desktop app, omitted from the public spec. For compatibility with other unofficial implementations that may not send it, the firmware defaults it to `false` when the key is absent — we follow the same convention and always include it, defaulting to `false`.

### Turn Event

```json
{"content": [{"text": "...", "type": "text"}], "evt": "turn", "role": "assistant"}
```

Dropped if > 4KB.

### Connect One-Shots

```json
{"time": [1775731234, -25200]}
{"cmd": "owner", "name": "Felix"}
```

### Commands (require ack)

| Command | Ack |
|---------|-----|
| `{"cmd":"status"}` | Status response |
| `{"cmd":"name","name":"Clawd"}` | `{"ack":"name","ok":true}` |
| `{"cmd":"owner","name":"Felix"}` | `{"ack":"owner","ok":true}` |
| `{"cmd":"unpair"}` | `{"ack":"unpair","ok":true}` |

Status response:

```json
{
    "ack": "status",
    "data": {"bat": {"mA": -120, "mV": 4012, "pct": 87, "usb": true}, "name": "Clawd", "sec": true, "stats": {"appr": 42, "deny": 3, "lvl": 5, "nap": 12, "vel": 8}, "sys": {"heap": 84200, "up": 8412}},
    "ok": true
}
```

### Folder Push

```
{"cmd":"char_begin","name":"bufo","total":184320}
{"cmd":"file","path":"manifest.json","size":412}
{"cmd":"chunk","d":"<base64>"} ...
{"cmd":"file_end"}
...repeat...
{"cmd":"char_end"}
```

## Buddy -> Daemon

### Permission

```json
{"cmd":"permission","decision":"once","id":"req_abc123"}
{"cmd":"permission","decision":"deny","id":"req_abc123"}
```

### Ping

```json
{"cmd":"ping"}
```

If silent for 15s.

## Version

1.0.0-tcp
