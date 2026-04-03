# Axia Livewire+ Integration

Decouples Audio (AES67/RTP) from Logic (LWRP). PipeWire handles audio transport; Quadrature manages GPIO over TCP.

## Network Addressing

| Source Name           | Livewire Ch | Multicast IP    | Studio |
| --------------------- | ----------- | --------------- | ------ |
| Quadrature_Studio_A_0 | 101         | `239.192.0.101` | A      |
| Quadrature_Studio_A_1 | 102         | `239.192.0.102` | A      |
| Quadrature_Studio_A_2 | 103         | `239.192.0.103` | A      |
| Quadrature_Studio_A_3 | 104         | `239.192.0.104` | A      |
| Quadrature_Studio_B_0 | 201         | `239.192.0.201` | B      |
| Quadrature_Studio_B_1 | 202         | `239.192.0.202` | B      |

**Format:** 48kHz, Stereo, 24-bit (L24) | **Port:** UDP 5004

## PipeWire AES67 Setup

```bash
pactl load-module module-null-sink \
    sink_name=Livewire_Tx_101 \
    sink_properties=device.description="Livewire_Ch_101" \
    format=s24be rate=48000 channels=2 channel_map=front-left,front-right

pactl load-module module-rtp-send \
    source=Livewire_Tx_101.monitor destination_ip=239.192.0.101 \
    port=5004 mtu=1280 sap_address=239.255.255.255
```

## Axia Console Config

**Sources → New Profile:** Name=`Quadrature_Studio_A_0`, Source ID=`101`, IP=`239.192.0.101`, GPIO=Enabled

______________________________________________________________________

## LWRP Protocol

### Overview

LWRP (Livewire Routing Protocol) is the TCP control plane for all Axia Livewire+ devices.
Specification: Maciej Szlapka, Telos Systems Corp., version 2.0.1 (2003-12-11, revised 2004-08+).

| Property         | Value                                              |
| ---------------- | -------------------------------------------------- |
| Transport        | TCP, port **93** (hardcoded, not configurable)     |
| Encoding         | 7-bit ASCII, human-readable                        |
| Line termination | `\r\n` (CRLF); server also accepts bare `\n`       |
| Command case     | UPPERCASE                                          |
| Framing          | None — no binary framing, no length prefix, no CRC |
| Multi-line resp  | Wrapped in `BEGIN` / `END` delimiters              |
| Telnet-testable  | Yes — any raw TCP/telnet client works              |

### Connection Lifecycle

**On connect:** The server sends **no banner**. The TCP handshake completes silently. The client must speak first.

**Session sequence:**

1. TCP connect
1. Optionally authenticate: `LOGIN <password>` (see [Authentication](#authentication))
1. Query device capabilities: `VER`
1. Subscribe to GPIO events: `ADD GPI` / `ADD GPO`
1. Receive asynchronous push notifications until disconnect

**On disconnect:** All subscriptions are torn down automatically. There is no graceful QUIT command — simply close the TCP socket. On reconnect, all subscriptions must be re-issued from scratch. `ADD GPI` / `ADD GPO` will immediately push the current state of all ports, providing re-sync.

**Connection limits:** Axia hardware has an undocumented per-device limit on concurrent connections. Exceeding it causes undefined misbehaviour. Maintain a **single persistent connection per device**.

### Keep-Alive

LWRP has **no application-level keepalive or heartbeat**. Dead links must be detected via:

- OS-level `SO_KEEPALIVE` on the socket
- Failed `send()` returning `EPIPE` / `ECONNRESET`
- Periodic polling with `VER` (device responds immediately if alive)

Quadrature detects link death via `recv()` returning 0 or error, then reconnects with exponential backoff.

### Authentication

```
LOGIN <password>\r\n
```

- No response on success — the TCP connection itself carries the authenticated state
- `ERROR` response on failure
- If no password is configured on the device, omit `LOGIN` entirely; all access is granted
- Read-only commands (`VER`, `ADD GPI`, `ADD GPO`) do not require login on most devices
- Local loopback connections (127.0.0.1) are implicitly granted full access

**Security note:** Credentials are transmitted in plaintext over TCP. LWRP has no TLS support. Restrict Axia devices to trusted LAN segments.

### Device Info Commands

```
VER
```

Returns device capabilities. Issue this after connecting to discover port counts.

Example response:

```
VER LWRP:2.0.2 DEVN:"xNode" NSRC:8 NDST:8 NGPI:8 NGPO:8
```

| Field         | Description                           |
| ------------- | ------------------------------------- |
| `LWRP:<ver>`  | Protocol version                      |
| `DEVN:<name>` | Device name (quoted string)           |
| `NSRC:<n>`    | Number of audio sources               |
| `NDST:<n>`    | Number of audio destinations          |
| `NGPI:<n>`    | Number of GPI ports (each has 5 pins) |
| `NGPO:<n>`    | Number of GPO ports (each has 5 pins) |

### GPIO Architecture

Each Livewire device exposes GPI (input) and GPO (output) **ports**, each containing exactly **5 pins**.

- **GPI** — Console → App. Physical hardware button presses / fader state changes.
- **GPO** — App → Console. LED feedback written by the application back to the console surface.

Port numbers start at 1. Maximum port count comes from `VER` (`NGPI`, `NGPO`).

### GPIO Pin State Encoding

LWRP represents the full state of a port as a **5-character ASCII string** (one character per pin, position 1–5 left to right):

| Character | Logic level | Stability                     |
| --------- | ----------- | ----------------------------- |
| `H`       | High (off)  | **Transitioning** (momentary) |
| `h`       | High (off)  | Stable                        |
| `L`       | Low (on)    | **Transitioning** (momentary) |
| `l`       | Low (on)    | Stable                        |
| `x`/`X`   | No change   | Used in set commands only     |

Uppercase = pin is mid-transition. Lowercase = pin has settled. A state change typically produces two sequential messages:

```
GPI 1 hhhhL     ← pin 5 going low, mid-transition
GPI 1 hhhhl     ← pin 5 now stable low
```

Quadrature's parser (`src/gpio/axia_protocol.c`) accepts both uppercase and lowercase as equivalent — only the direction matters, not the transition state.

### GPIO Commands

**Subscribe to port state changes:**

```
ADD GPI             ← subscribe to all GPI ports
ADD GPO             ← subscribe to all GPO ports
ADD GPI <n>         ← subscribe to specific port n
ADD GPI <n>-<m>     ← subscribe to port range n through m
```

On subscribe, the server immediately pushes the current state of all matching ports.

**Set GPO pin states (app → console):**

```
GPO <port> <5-char-state>
```

Examples:

```
GPO 1 lllll        ← all 5 pins on port 1 to low (active/LED on)
GPO 1 hhhhh        ← all 5 pins to high (inactive/LED off)
GPO 1 lxxxx        ← set only pin 1 to low, leave pins 2–5 unchanged
GPO 1 xxlxx        ← set pin 3 to low, leave others unchanged
```

**Unsubscribe:** No explicit unsubscribe command exists. Subscriptions terminate when the TCP connection closes.

### Response Format

```
<TYPE> [<number>] [KEY:VALUE ...]
```

Multi-line bulk queries use `BEGIN`/`END`:

```
BEGIN
DST 1 ADDR:239.192.8.52 NAME:"Studio A"
DST 2 ADDR:239.192.8.53 NAME:"Studio B"
END
```

Unsolicited async notifications (GPI, GPO) arrive as bare lines without `BEGIN`/`END`.

### Error Responses

```
ERROR <code> <description>
```

| Code   | Meaning                                        |
| ------ | ---------------------------------------------- |
| `1000` | Bad command (unrecognised command, blank line) |

### Latency

- GPIO state change notifications: **< 5ms** from physical pin change to TCP delivery (LAN)
- GPO commands (LED feedback): ~1ms round-trip on LAN
- No application-level keepalive; dead link detection relies on `recv()` failure or `SO_KEEPALIVE`

### Telnet Debugging Session

```bash
nc <console_ip> 93
```

```
(connect — server is silent, no banner)
LOGIN mypassword\r\n
(no response = success)

VER\r\n
→ VER LWRP:2.0.2 DEVN:"xNode" NSRC:8 NDST:8 NGPI:8 NGPO:8

ADD GPI\r\n
→ GPI 1 hhhhh
→ GPI 2 lhhhh
(then asynchronous on fader raise:)
→ GPI 1 Lhhhh     ← pin 1 transitioning to low
→ GPI 1 lhhhh     ← pin 1 stable low

ADD GPO\r\n
→ GPO 1 hhhhh

GPO 1 lxxxx\r\n   ← set port 1 pin 1 low (ON-AIR LED on)
→ GPO 1 Lhhhh
→ GPO 1 lhhhh

\r\n              ← blank line
→ ERROR 1000 bad command
```

______________________________________________________________________

## Quadrature Implementation

### Source Layout

| File                            | Purpose                                           |
| ------------------------------- | ------------------------------------------------- |
| `include/quadrature/gpio.h`     | Public API — types, enums, lifecycle functions    |
| `src/gpio/axia_gpio.c`          | TCP connection, listener thread, reconnect loop   |
| `src/gpio/axia_protocol.c`      | LWRP message parse/format helpers                 |
| `src/gpio/internal.h`           | Private struct, protocol constants                |
| `src/ui/settings/gpio_bridge.c` | UI ↔ GPIO bridge (engine play/stop, LED feedback) |
| `tests/unit/test_axia_gpio.c`   | Unit tests with mock TCP server                   |

### Protocol Constants (`src/gpio/internal.h`)

| Constant                     | Value  | Description                    |
| ---------------------------- | ------ | ------------------------------ |
| `LWRP_PORT`                  | `93`   | Default TCP port               |
| `LWRP_MAX_LINE`              | `512`  | Max bytes per protocol line    |
| `RECONNECT_INITIAL_DELAY_MS` | `100`  | First reconnect wait (100ms)   |
| `RECONNECT_MAX_DELAY_MS`     | `5000` | Max reconnect wait (5s)        |
| `RECONNECT_BACKOFF_FACTOR`   | `2`    | Exponential backoff multiplier |

### Channel ID Mapping

| Layer             | Range | Example                        |
| ----------------- | ----- | ------------------------------ |
| Internal (C enum) | 0–3   | `channel_id = 0`               |
| LWRP protocol     | 1–4   | `GPO 1 lxxxx` (pin 1 = ON-AIR) |

### Pin Assignments

| Enum constant      | LWRP pin | Console surface  | Direction     |
| ------------------ | -------- | ---------------- | ------------- |
| `AXIA_PIN_ON_AIR`  | 1        | Fader on/off LED | Bidirectional |
| `AXIA_PIN_PREVIEW` | 2        | Preview/PFL LED  | Bidirectional |

### Threading Model

```
GTK Main Thread              Listener Thread (per axia_gpio_t)
───────────────              ────────────────────────────────
axia_gpio_set()          →   send() GPO command (MSG_NOSIGNAL)
axia_gpio_stop()         →   atomic_store(running=false)
                             shutdown(sockfd) → wakes recv()

                             connect_to_console()  [2s timeout via select()]
                             authenticate()        [LOGIN <password> if set]
                             send_subscription()   [ADD GPI + ADD GPO]
                             recv() loop           [blocking]
                             → parse GPI line
                             → g_main_context_invoke() ──→ callback on GTK thread
                             exponential_backoff_sleep() [interruptible, 100ms chunks]
```

- Callbacks are **always delivered on the GTK main thread** via `g_main_context_invoke()`
- `running` and `connected` flags are `_Atomic bool` — lock-free
- Callback pointers protected by `GMutex callback_mutex`
- `axia_gpio_set()` is thread-safe (callable from any thread)

### Connection Sequence (Quadrature)

```
TCP connect (non-blocking, 2s timeout)
  └─ [if password set] LOGIN <password>\n  (no response = success)
       └─ ADD GPI\n
            └─ ADD GPO\n
                 └─ recv() loop
                      ├─ parse GPI line → dispatch_gpio_event() → GTK callback
                      └─ recv()=0 or error → disconnect → exponential backoff → reconnect
```

### API

```c
typedef struct axia_gpio_t axia_gpio_t;

typedef enum {
    AXIA_PIN_ON_AIR  = 1,   // Fader on/off state
    AXIA_PIN_PREVIEW = 2,   // Preview/PFL state
} axia_pin_t;

typedef enum {
    AXIA_STATE_LOW  = 0,    // Pin inactive (LED off)
    AXIA_STATE_HIGH = 1,    // Pin active (LED on)
} axia_state_t;

// Callback for console → app GPIO events (GTK main thread)
typedef void (*axia_gpio_callback_t)(int channel_id, axia_pin_t pin,
                                     axia_state_t state, void *user_data);

// Callback for connection status changes (GTK main thread)
typedef void (*axia_gpio_status_callback_t)(int channel_id, bool connected,
                                            void *user_data);

// address: "ip" or "ip:port" (default port 93)
// channel_id: 0–3 (internal); sent as 1–4 to console
// password: NULL if auth not required
quadrature_result_t axia_gpio_create(const char *address, int channel_id,
                                     const char *password,
                                     axia_gpio_t **out);
void axia_gpio_destroy(axia_gpio_t *gpio);

void axia_gpio_set_callback(axia_gpio_t *gpio, axia_gpio_callback_t cb, void *user_data);
void axia_gpio_set_status_callback(axia_gpio_t *gpio, axia_gpio_status_callback_t cb, void *user_data);

quadrature_result_t axia_gpio_start(axia_gpio_t *gpio);
quadrature_result_t axia_gpio_stop(axia_gpio_t *gpio);
bool axia_gpio_is_connected(const axia_gpio_t *gpio);

// Send GPIO state to console (LED feedback) — thread-safe
quadrature_result_t axia_gpio_set(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state);
```

______________________________________________________________________

## Operations

| Operation    | Direction     | Trigger                                                         |
| ------------ | ------------- | --------------------------------------------------------------- |
| Fader ON     | Console → App | Fader raised → `GPI <ch> lxxxx` → `engine_play()`               |
| Fader OFF    | Console → App | Fader lowered → `GPI <ch> hxxxx` → `engine_stop()`              |
| Queue Play   | App → Console | Play in queue mode → `GPO <ch> lxxxx` (pin 1 HIGH)              |
| Preview On   | Bidirectional | Toggle preview → `GPO <ch> xlxxx` (pin 2 HIGH)                  |
| LED Feedback | App → Console | Channel mode change → `gpio_bridge.c:on_channel_mode_changed()` |

______________________________________________________________________

## Multi-Studio

**Dedicated:** Single `axia_gpio_t` per studio. One LWRP connection per console.

**Floating:** Multiple `axia_gpio_t` instances on different consoles; any fader raise can trigger playback.

Each studio channel requires its own `axia_gpio_create()` call with the corresponding `channel_id` (0–3).
