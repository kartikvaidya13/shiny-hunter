# shiny-hunter
Automated shiny hunter for Fire Red and Leaf Green using RNG manipulation.

Targets the Nintendo Switch and Switch 2 releases. An ESP32-S3 acts as a wired
controller and sends button presses at exact microsecond offsets, so the game
lands on a chosen RNG state instead of being soft-reset at random.

**Status:** early. The firmware is in bring-up. The host is not written yet.

## Firmware (`firmware/shiny_pad/`)

### Hardware

An ESP32-S3 devkit with two USB ports:

- **Native USB** port → console dock. This is the gamepad.
- **UART** port → PC. Used for upload and for the [serial protocol](#serial-protocol).
- Optional: a logic analyser on **GPIO4**. It goes high for each timed send.

### Build

1. Arduino IDE → Boards Manager → install **esp32** by Espressif Systems.
2. Open `firmware/shiny_pad/shiny_pad.ino`. The vendored library in `src/` is compiled automatically. Nothing else to install.
3. Set **Tools**:

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| USB Mode | **USB-OTG (TinyUSB)** |
| USB CDC On Boot | **Disabled** |
| Upload Mode | UART0 / Hardware CDC |
| Core Debug Level | None |
| Port | the devkit's UART port |

These settings are stored in the IDE, not in the repo. If USB Mode or CDC On
Boot is wrong, the build stops with an error that names the setting. Core Debug
Level must be None, otherwise core log output mixes into the protocol stream.

### Console

System Settings → Controllers and Sensors → **Pro Controller Wired
Communication** → On.

### Smoke test

Open Serial Monitor at **115200** with line ending **Newline**. Type `PING`.
You should get `PONG 0.1.0 1 64`. To press A for 100 ms:

```
EV 1 0 0004 15
EV 2 100000 0000 15
ARM 30000
FIRE
```

## Serial protocol

Protocol version **1**. UART at 115200 8N1. ASCII, one message per line,
ending in `\n` (`\r\n` also works). Fields are separated by spaces. Commands
are case-insensitive. Lines are at most 96 characters. Opening the port can
reset the board, so ignore anything before `BOOT`.

The host uploads a timeline of events, arms it, then fires it. Each event is a
full pad state, sent at `at_us` microseconds after `FIRE` arrives. Times are
absolute offsets, not durations, so error does not accumulate. A press is two
events: one down, one up.

```
IDLE --EV--> LOADING --ARM--> ARMED --FIRE--> RUNNING --(last event)--> IDLE
ABORT (or an ARM timeout) returns to IDLE from any state
```

| Command | Valid in | Reply |
| --- | --- | --- |
| `PING` | any | `PONG <fw_ver> <proto_ver> <free_slots>` |
| `EV <seq> <at_us> <buttons_hex> <dpad> [lx ly rx ry]` | IDLE, LOADING | `OK <seq> <free_slots>` |
| `ARM <timeout_ms>` | LOADING | `ARMED <events>` |
| `FIRE` | ARMED | `ANCHOR <device_us>`, then one `DONE` per event, then `END` |
| `ABORT` | any | `ABORTED` (queue cleared, neutral report sent) |

`EV` fields:

- `seq`: decimal 0–65535, must increase from one event to the next.
- `at_us`: must increase, at least 2000 µs after the previous event. The 2000 is provisional until the timing bench test (F3) measures how long a send blocks.
- `buttons_hex`: hex bitmask, max `3FFF`.
- `dpad`: 0–7, or 15 for centred.
- Sticks: decimal 0–255, all four or none. Default is 128 (centred).

A rejected `EV` leaves the queue unchanged. The queue holds 64 events.

Buttons: `0001` Y, `0002` B, `0004` A, `0008` X, `0010` L, `0020` R,
`0040` ZL, `0080` ZR, `0100` Minus, `0200` Plus, `0400` L stick,
`0800` R stick, `1000` Home, `2000` Capture. The bit order comes from the
vendored library. The console's mapping hasn't been checked yet.

D-pad: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left,
7 up-left, 15 centred.

If `FIRE` doesn't arrive within `timeout_ms`, the device replies
`ERR ARM_TIMEOUT -` then `ABORTED`. A *running* timeline is never aborted
because the host went quiet. Uploading it in advance is what makes host
lateness irrelevant. Only `ABORT` stops a run.

Messages the device sends on its own:

| Message | Meaning |
| --- | --- |
| `BOOT <fw_ver> <proto_ver>` | Firmware started. Seen mid-session, it means the board reset |
| `DONE <seq> <before_us> <error_us> <send_us>` | Event sent. `before_us` is device `micros()` just before the write. `error_us = before_us − (anchor + at_us)`. `send_us` is how long the write blocked |
| `END <events> <max_abs_err_us> <failed_sends>` | Timeline finished, neutral report sent, anything still held released |
| `ERR SEND <seq>` | The USB write failed (for example, nothing has enumerated the pad). `DONE` is still sent |

Device times are 32-bit `micros()`, which wraps about every 71.6 minutes.

Errors have the form `ERR <code> <detail>`. `detail` is one token, `-` if there
is nothing to add.

| Code | Detail | Cause |
| --- | --- | --- |
| `UNKNOWN_CMD` | command | not a command |
| `BAD_ARGS` | command | wrong number of fields |
| `BAD_VALUE` | field | unparseable or out of range |
| `SEQ_ORDER` / `TIME_ORDER` / `SPACING` | seq | ordering or spacing rule broken |
| `QUEUE_FULL` | seq | 64 events already loaded |
| `EMPTY` | `-` | `ARM` with no events |
| `BUSY` | state | `EV` or `ARM` while ARMED or RUNNING |
| `NOT_ARMED` | state | `FIRE` outside ARMED |
| `LINE_TOO_LONG` | `-` | line discarded |

## Credits

Gamepad descriptor from [esp32beans/switch_ESP32](https://github.com/esp32beans/switch_ESP32)
(MIT), vendored unmodified in `firmware/shiny_pad/src/switch_ESP32/`.

## License

MIT
