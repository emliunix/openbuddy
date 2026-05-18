# Upstream Transport Architecture

## BLE Bridge (`ble_bridge.cpp`)

### Protocol

**Nordic UART Service (NUS)** — standard BLE serial profile:
- Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (client→device): `6e400002-b5a3-f393-e0a9-e50e24dcca9e` (WRITE)
- TX (device→client): `6e400003-b5a3-f393-e0a9-e50e24dcca9e` (NOTIFY)

### Security

- LE Secure Connections with MITM protection
- Passkey entry: device displays 6-digit code, user types on desktop
- Encrypted characteristics (read/write require encryption)
- Bonding persists in NVS (cleared via `bleClearBonds()`)

### Data Flow

```
Desktop (Web Bluetooth / noble)
    │
    ▼ WRITE
BLE RX characteristic
    │
    ▼ onWrite callback
rxPush() — ring buffer (2048 bytes)
    │
    ▼ dataPoll() in main loop
bleRead() — drains ring buffer byte-by-byte
    │
    ▼ _LineBuf assembles lines
_applyJson() — parses NDJSON, updates TamaState
```

### Outbound Flow

```
sendCmd() or _xAck()
    │
    ▼ bleWrite(data, len)
Chunk to MTU-3 bytes (macOS: ~182, default: 20)
    │
    ▼ notify() per chunk
BLE TX characteristic
    │
    ▼ NOTIFY
Desktop client
```

### Key Behaviors

1. **Auto-reconnect**: on disconnect, immediately restarts advertising
2. **MTU-aware**: chunks outbound data to negotiated MTU minus 3-byte ATT header
3. **Flow control**: ring buffer drops if full (2048 bytes); desktop should keep up
4. **Dual transport**: same data goes to both Serial and BLE (`sendCmd()` writes to both)

## USB Serial

- Standard `Serial` object (Arduino USB CDC)
- `_usbLine` buffer (1024 bytes) assembled in `dataPoll()`
- No framing beyond newline-delimited JSON
- Used for: debugging, wired connection, initial setup

## Message Framing

**NDJSON** — one JSON object per line, newline delimited:
```
{"running":1,"waiting":0,"total":1,"msg":"connected"}
{"cmd":"owner","name":"Felix"}
{"time":[1775731234,-25200]}
```

No length prefix, no binary framing. Simple line buffering.

## Connection Lifecycle

1. **Boot**: `bleInit()` starts advertising as "Claude-XXXX"
2. **Pairing**: Desktop scans → connects → passkey entry → bonding
3. **Connected**: Bidirectional NDJSON flow begins
4. **Disconnect**: Auto-restart advertising; session state goes idle after 30s timeout
5. **Unpair**: `bleClearBonds()` erases LTKs; next connection requires re-pairing

## Desktop Plugin Side (Inferred)

Not in this repo, but the protocol implies:
- Scans for BLE devices matching "Claude-*"
- Connects via Web Bluetooth or noble
- Sends heartbeat JSON every ~10s
- Receives permission responses via same channel
- Sends file transfers for GIF characters

## Our TCP Equivalent

| BLE Feature | TCP Equivalent |
|-------------|---------------|
| NUS service | TCP server (port 0 = auto) |
| BLE advertising | TCP listen socket |
| Pairing/bonding | None (localhost only) |
| Passkey | None |
| MTU chunking | TCP stream (no chunking needed) |
| Auto-reconnect | Accept next client automatically |
| Encrypted | Implicit (localhost loopback) |
| `bleWrite()` | `asio::async_write()` |
| `bleRead()` | `asio::async_read_until('\n')` |
| Ring buffer | Asio streambuf |

### Key Differences

1. **No discovery**: Desktop plugin connects to `127.0.0.1:<port>` instead of scanning BLE
2. **No pairing**: No passkey, no bonding, no encryption handshake
3. **Stream vs packets**: TCP is a stream; BLE is packet-based with MTU limits
4. **Single connection**: BLE allows one client; TCP server accepts one at a time (drops old)
5. **No MTU**: TCP handles segmentation; we write full JSON lines

## Message Types Summary

### Device → Desktop (BuddyMsg)

| Type | Trigger | Fields |
|------|---------|--------|
| Permission response | User presses A/B on prompt | `cmd:"permission", decision, id` |
| Status ack | Desktop sends `cmd:"status"` | `ack:"status", ok, data:{...}` |
| Ping | Health check | `cmd:"ping"` |

### Desktop → Device (DaemonMsg)

| Type | Purpose | Key Fields |
|------|---------|-----------|
| Heartbeat | State sync every ~10s | `running, waiting, total, msg, prompt, tokens` |
| Time sync | Set RTC from desktop | `time:[epoch, tz_offset]` |
| Owner cmd | Set owner name | `cmd:"owner", name` |
| Name cmd | Set pet name | `cmd:"name", name` |
| Species cmd | Set species index | `cmd:"species", idx` |
| Status cmd | Request full status | `cmd:"status"` |
| Unpair cmd | Clear BLE bonds | `cmd:"unpair"` |
| Permission cmd | Desktop asking approval | `cmd:"permission", id` |
| Char begin | Start GIF upload | `cmd:"char_begin", name, total` |
| File meta | File info for upload | `cmd:"file", path, size` |
| Chunk | Base64 data chunk | `cmd:"chunk", d` |
| File end | End of file | `cmd:"file_end"` |
| Char end | End of character | `cmd:"char_end"` |

## Transport State Machine

```
[IDLE] ──advertising──► [CONNECTING] ──client connects──► [CONNECTED]
   ▲                                                    │
   └───disconnect / 30s timeout / error─────────────────┘
```

In CONNECTED state:
- Heartbeats keep session alive (`last_live_ms` updated)
- Prompts arrive via heartbeat JSON
- User responses sent immediately
- 30s without heartbeat → disconnect → idle
