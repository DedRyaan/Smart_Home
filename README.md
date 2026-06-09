# ESP32 Smart-Home Attachment

Local, free, no-subscription smart controller for **3 lights + 1 fan** (relays) and a
**Croma AC** (IR), with **Alexa voice on/off** and your **existing wall switches kept live** —
flipping a rocker inverts the current state regardless of who last changed it. Everything is
**self-hosted on the ESP32**; no router/PC/cloud to run.

## Features

- 3 lights + fan on/off via a 4-channel 5 V relay.
- AC on/off, **temperature**, and mode via captured IR codes.
- Alexa voice control through **local Philips-Hue emulation** (Espalexa) — no account/cloud.
- Manual rocker switches act as toggles (edge-detected) and stay in sync with Alexa/dashboard.
- Self-hosted responsive **web dashboard** for precise AC temperature/mode.
- State persisted to NVS; `smarthome.local` via mDNS; OTA updates.

## Hardware

| Function            | ESP32 GPIO |
|---------------------|-----------|
| Relay IN1–IN4 (L1/L2/L3/Fan) | 23, 22, 21, 19 |
| Wall switches (L1/L2/L3/Fan) | 13, 14, 27, 26 (`INPUT_PULLUP`, rocker → GND) |
| IR LED drive        | 4 → 330 Ω → 2N2222 base; LED in collector path, anode → 5 V via ~33–100 Ω |
| IR receiver (capture only) | 15 |

**Still need:** a VS1838B/TSOP1838 IR receiver (one-time, to capture Croma codes) and one
~33–100 Ω resistor for the IR-LED current limit.

> ⚠️ **Mains safety:** relays switch live 230 V. Rewire the wall rockers as **low-voltage
> inputs** (GPIO ↔ GND) so they no longer switch mains directly. Have a licensed electrician do
> the mains side; bench-test everything on USB power with no mains connected first.

## Build & flash

```bash
pio run -t upload          # build + flash firmware
pio device monitor         # serial @ 115200
```

On first boot connect to the **`SmartHome-Setup`** Wi-Fi AP and enter your Wi-Fi credentials
(captive portal). Then open `http://smarthome.local/`.

In the Alexa app: **Devices → +  → Add Device → Other → Discover**. Five devices appear:
`Light 1/2/3`, `Fan`, `AC`. If discovery fails on a Gen-3/4 Echo (Espalexa emulates the
legacy Hue v1 bridge, which newer Echos discover less reliably), check
`http://smarthome.local/espalexa` to confirm the devices are registered, then re-run Discover.

## Security

This is a LAN device that switches mains relays, so:

- **Set passwords.** Copy `secrets.example.h` → `secrets.h` and set `OTA_PASSWORD` and
  `AP_PASSWORD` (or edit the defaults in `config.h`). `secrets.h` is gitignored.
- **Put it on an IoT/guest VLAN** with no internet egress — the most effective mitigation,
  since the relay/AC HTTP API and the Hue/SSDP emulation are unauthenticated by design.

## Capturing Croma AC codes

1. Wire a VS1838B receiver to GPIO15 and flash `tools/ir_capture/ir_capture.ino`.
2. Point the remote and read the **`Protocol:`** line it prints.
   - **If it decodes to a known class** (Croma units are commonly `COOLIX` or `VOLTAS`),
     prefer the IRremoteESP8266 protocol class (e.g. `IRCoolixAC`) over raw frames — one
     stateful object handles temp/mode/fan, no 15-row table needed.
   - **If it reports `UNKNOWN`**, capture each command (Power OFF; Power ON at each temp
     16–30 °C in Cool) and paste the raw arrays into `include/ac_codes.h`
     (`AC_OFF`, `AC_COOL_FRAMES`).
3. Reflash the main firmware. Uncaptured frames are safely skipped (logged) until filled.

> Note: only **Cool** mode is wired to IR today; selecting Fan/Dry/Auto updates state but
> sends no distinct frame until per-mode tables (or a protocol class) are added.

## API

- `GET  /api/state` → `{relays[], acPower, acTemp, acMode}`
- `POST /api/relay?ch=<0-3>&state=<0|1>`
- `POST /api/ac?power=<0|1>&temp=<16-30>&mode=<0-3>`

## Layout

```
platformio.ini            build config + deps
src/main.cpp              firmware (relays, switches, Alexa, AC IR, web API)
include/config.h          pins & constants
include/dashboard.h       embedded web UI
include/ac_codes.h        captured Croma IR frames (fill after capture)
tools/ir_capture/         one-time IR capture sketch
```
.venv\Scripts\pio device monitor -e ir_blink -b 115200