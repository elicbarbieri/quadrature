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

## LWRP Protocol

TCP port 93, ASCII `\r\n` terminated.

| Command                  | Dir   | Description                                     |
| ------------------------ | ----- | ----------------------------------------------- |
| `ADD GPO <ch>`           | TX    | Subscribe to channel GPIO                       |
| `GPO <ch> <pin> <state>` | RX/TX | GPIO state (pin 1=ON, pin 2=PREVIEW, state 0/1) |

## API

```c
typedef struct axia_gpio_t axia_gpio_t;

typedef enum {
    AXIA_PIN_ON = 1,       // Fader on/off state
    AXIA_PIN_PREVIEW = 2,  // Preview/cue state
} axia_pin_t;

typedef enum {
    AXIA_STATE_LOW = 0,
    AXIA_STATE_HIGH = 1,
} axia_state_t;

// Callback for incoming GPIO state changes
typedef void (*axia_gpio_callback_t)(axia_pin_t pin, axia_state_t state, void *user_data);

// Lifecycle
quadrature_result_t axia_gpio_create(const char *console_ip, uint16_t channel_id, axia_gpio_t **out);
void axia_gpio_destroy(axia_gpio_t *gpio);

// Set callback for console → app GPIO events
void axia_gpio_set_callback(axia_gpio_t *gpio, axia_gpio_callback_t cb, void *user_data);

// Start/stop listener thread (auto-reconnects)
quadrature_result_t axia_gpio_start(axia_gpio_t *gpio);
quadrature_result_t axia_gpio_stop(axia_gpio_t *gpio);

// Send GPIO state to console (app → console)
quadrature_result_t axia_gpio_set(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state);
```

## Operations

| Operation  | Direction     | Trigger                                             |
| ---------- | ------------- | --------------------------------------------------- |
| Fader ON   | Console → App | Console fader raised → `engine_play()`              |
| Fader OFF  | Console → App | Console fader lowered → `engine_stop()`             |
| Queue Play | App → Console | Play pressed in queue mode → set `AXIA_PIN_ON` high |
| Preview On | Bidirectional | Toggle preview/cue monitoring                       |

## Multi-Studio

**Dedicated:** Single `axia_gpio_t` per studio.

**Floating:** Multiple instances; any console can trigger playback.
