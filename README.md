# Ragamuffin Flasher

One-tap firmware installs for Ragamuffin Hall's ESP32-S3 boards, from a phone over USB-C.

**https://jobot101.github.io/ragamuffin-flasher/**

| Button | Board | Built in |
|---|---|---|
| 🦾 Arm brain | Muffin, Waveshare ESP32-S3-AUDIO-Board | [jobot101/xarm](https://github.com/jobot101/xarm) |
| 👁️ Camera | Hiwonder ESP32-S3 vision module | [jobot101/xarm](https://github.com/jobot101/xarm) |
| 🔔 Solenoid | solenoid tester: any ESP32-S3 + one MOSFET module | `solenoid/` here |
| 🎐 Hall chimes | the bell machine: any ESP32-S3 + DIN MIDI + 4 MCP23017 solenoid drivers | `chime/` here |

## Using it

- **Android:** open the page in Chrome, pick the board, plug it in, tap Flash.
- **Computer:** Chrome or Edge.
- **iPhone / iPad:** no USB in any browser. Do the first install from Android or a computer, then update over WiFi.

If Flash can't find the board: hold BOOT, tap RESET, release BOOT, then try again.

After the first install, each board updates itself from its own page with **Install latest**:

| Board | Own WiFi (password `ragamuffin`) | On the hall WiFi |
|---|---|---|
| Solenoid | "Solenoid" → 192.168.4.1 | solenoid.local |
| Hall chimes | "Chimes" → 192.168.4.1 | hall-chimes.local |

Keep mobile data on, and open the page in Chrome (not the WiFi sign-in pop-up).

## How it builds

Every push to `main` builds only what changed. Each workflow publishes to its own folder of the `gh-pages` branch:

- `solenoid/**` → `.github/workflows/solenoid.yml` → `sol/`
- `chime/**` → `.github/workflows/chime.yml` → `chime/`
- `flasher/**` → `.github/workflows/flasher.yml` → the page itself

Firmware is compiled with arduino-cli and esp32 core 3.2.0 into a 4 MB image that runs on any S3. The page reads the arm and camera files from `https://jobot101.github.io/xarm/`.

**Nothing secret goes in this repo:** it and its Pages site are public. WiFi passwords and MQTT logins are typed on each board's page and kept in its flash.
