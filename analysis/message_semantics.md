# Message Semantics

Extracted from upstream `claude-desktop-buddy/src/data.h` and `xfer.h`.

All messages are newline-delimited JSON. A line is only processed if it starts with `{`.

---

## Heartbeat

```json
{
  "total": 3,
  "running": 1,
  "waiting": 1,
  "completed": false,
  "msg": "approve: Bash",
  "entries": ["10:42 git push", "10:41 yarn test", "10:39 reading file..."],
  "tokens": 184502,
  "tokens_today": 31200,
  "prompt": {"id": "req_abc123", "tool": "Bash", "hint": "rm -rf /tmp/foo"}
}
```

- `total`, `running`, `waiting`, `tokens_today`: fall back to previous value if the key is absent.
- `completed`: defaults to `false` if absent. Drives `P_CELEBRATE` persona state.
- `tokens`: only update token stats if the key is present **and** the value is a `uint32_t`. Ignore otherwise.
- `msg`: copy up to 23 chars (24-byte buffer including null).
- `entries`: array of up to 8 strings, each up to 91 chars. Replace all lines. Bump `lineGen` if line count changes or the last entry differs from `msg`.
- `prompt`: if the key is absent or null, clear `promptId`, `promptTool`, `promptHint`. If present, copy all three fields.

Receiving any heartbeat counts as a live connection (`_lastLiveMs = now`).

---

## Time Sync

```json
{"time": [1775731234, -25200]}
```

- Value must be a two-element array: `[epoch_sec, tz_offset_sec]`.
- Local time = `epoch_sec + tz_offset_sec`. Apply to RTC.
- Counts as a live connection (`_lastLiveMs = now`).
- Any other shape is ignored.

---

## Connection State

- Connected: any valid message received within the last 30 s.
- Disconnected: zero out `total`, `running`, `waiting`, `completed`; set `msg` to `"No Claude connected"`.
- `P_SLEEP` is never the disconnected state; disconnected maps to `P_IDLE`.

---

## Persona Selection

In priority order:

1. `!connected` → `P_IDLE`
2. `waiting > 0` → `P_ATTENTION`
3. `completed` → `P_CELEBRATE`
4. `running >= 3` → `P_BUSY`
5. otherwise → `P_IDLE`

---

## Permission Decision (outbound)

Sent by us in response to a prompt:

```json
{"cmd": "permission", "id": "<prompt.id>", "decision": "once"}
{"cmd": "permission", "id": "<prompt.id>", "decision": "deny"}
```

- `id` must echo `prompt.id` exactly.
- Send exactly one response per prompt. Guard with a `response_sent` flag.

---

## Commands (inbound)

All commands have a `"cmd"` key. Each must be acked:

```json
{"ack": "<cmd>", "ok": true, "n": 0}
```

`n` is 0 unless specified otherwise.

### `owner`

```json
{"cmd": "owner", "name": "Felix"}
```

Store owner name. Ack `ok: true` if `name` present, `ok: false` if absent.

### `name`

```json
{"cmd": "name", "name": "Clawd"}
```

Store pet name. Ack `ok: true` if `name` present, `ok: false` if absent.

### `species`

```json
{"cmd": "species", "idx": 2}
```

Store species index. If `idx == 0xFF` and GIF assets are available, use GIF mode; otherwise use buddy sprite mode.

### `unpair`

```json
{"cmd": "unpair"}
```

Clear BLE bonds (no-op on desktop). Ack `ok: true`.

### `status`

```json
{"cmd": "status"}
```

Respond with current state. Ack shape:

```json
{"ack": "status", "ok": true, "n": 0, "data": {
  "name": "<pet>", "owner": "<owner>",
  "stats": {"appr": 0, "deny": 0}
}}
```

---

## Folder Push Protocol

Used to install a character pack. Commands are processed in sequence; only `file`, `chunk`, `file_end`, `char_end`, and `permission` are accepted while a transfer is active.

```
{"cmd": "char_begin", "name": "bufo", "total": 184320}
{"cmd": "file",       "path": "manifest.json", "size": 412}
{"cmd": "chunk",      "d": "<base64>"}
...
{"cmd": "file_end"}
...
{"cmd": "char_end"}
```

- `char_begin`: check available space (`total + 4096` bytes headroom). Wipe existing character directory. Ack `ok: false` with `"error"` string if insufficient space.
- `file`: open named file for writing. Ack `ok: false` if `path` absent.
- `chunk`: base64-decode `d` and append to open file. Ack includes `"n": <bytes_written_so_far>`.
- `file_end`: close file. Ack `ok: true` if bytes written match `size` (or `size` was 0).
- `char_end`: finalize character. Ack `ok: true` on success.

Every command acks with `{"ack": "<cmd>", "ok": true/false, "n": <n>}`.
