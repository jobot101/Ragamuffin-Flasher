# CLAUDE.md — ragamuffin-flasher

The phone flasher for Ragamuffin Hall's ESP32-S3 boards, plus the Hall bell-machine firmware. It was moved out of `jobot101/xarm` on Nov 2 2026. Muffin (the arm) and its camera are still built in xarm; this page only reads their files. README.md covers what the owner sees.

## The owner works from an Android phone

GitHub Actions is the build machine. Ship a change by pushing to `main`, then reading the Actions log. Don't expect a local Arduino toolchain.

## Layout

- **`flasher/`:** a Vite page using esptool-js. The transport depends on the browser:
  - Android Chrome → WebUSB, through `web-serial-polyfill` for 303A (native USB) or esptool-js's WebUSBSerialPort for CH340.
  - Desktop → Web Serial.
  - Keep baud at 115200.
  - `BOARDS` in `main.js`: `arm`/`cam` read from `https://jobot101.github.io/xarm/` (CORS `*`); `sol`/`chime` read from `./sol/` and `./chime/`.
  - Every board except `arm` gets a full erase and a write of all 4 parts.
- **`solenoid/`:** a one-MOSFET solenoid tester. See `solenoid/HANDOFF.md` (hardware, wiring, settings, what's still unverified).
- **`chime/`:** the bell machine (`chime.ino`).
  - **Inputs:** DIN MIDI on GPIO 18, plus contact-closure triggers.
  - **Outputs:** 4× Adafruit MCP23017 solenoid drivers at 0x20–0x23, on I2C SDA 8 / SCL 9. They drive 25 tubes from MIDI note 48. At most 4 coils are on at once (24 V supply limit).
  - **Core 1** runs MIDI and coil timing only. **Core 0** runs WiFi, the web page, MQTT and updates. Page and MQTT notes reach core 1 through `netToChimes`. Never drive a coil from core 0.
  - **Network**, the same as the solenoid:
    - its own AP "Chimes" / ragamuffin at 192.168.4.1, always on, with a captive DNS;
    - a paced join to the hall WiFi (ClarkShop preset), reachable at hall-chimes.local;
    - an MQTT broker set on the page (blank = off), under topics `ragamuffin/hall/chimes/{note,mute,status,played}`;
    - **Install latest** posts to `/update`, and the coils stay muted while it writes.
  - **NVS namespace `chime`:** `ssid wpass mhost mport muser mpass`.

## Build / publish

| Path | Workflow | Publishes to gh-pages |
|---|---|---|
| `solenoid/**` | `solenoid.yml` | `sol/` (`keep_files`) |
| `chime/**` | `chime.yml` | `chime/` (`keep_files`) |
| `flasher/**` | `flasher.yml` | the page root (`keep_files`) |

- **Firmware builds:** arduino-cli with esp32 core 3.2.0, FQBN `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=disabled,FlashSize=4M,PartitionScheme=default,FlashMode=dio`.
- **Versions:** CI writes `<dir>/version.h` with `FW_VERSION` = the short SHA (gitignored). That's the same value as `manifest.json` `version`, which is how a board's page knows it's up to date.
- **Every publish job shares the `gh-pages` concurrency group.** Never use `force_orphan` here: it would wipe the other folders.

## Rules

- **Nothing secret in the repo or on Pages** (both public): no WiFi passwords, MQTT logins or OTA passwords. They're typed on the board's page and kept in NVS.
- **Keep xarm's paths:** `https://jobot101.github.io/xarm/fw/` and `/cam/` belong to xarm. Muffin's auto-update and the old flasher at `/xarm/` still use them.
- **Keep it plain:** the Ragamuffin Hall voice, oak/brass, and plain words on the page.
