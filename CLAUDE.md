# CLAUDE.md — ragamuffin-flasher

The phone flasher for Ragamuffin Hall's ESP32-S3 boards, plus the Hall bell-machine firmware. It was moved out of `jobot101/xarm` on Nov 2 2026. Muffin (the arm) and its camera are still built in xarm; this page only reads their files. README.md covers what the owner sees.

## The owner works from an Android phone

GitHub Actions is the build machine. Ship a change by pushing to `main`, then reading the Actions log. Don't expect a local Arduino toolchain.

## Layout

- **`flasher/`:** a Vite page using esptool-js. The transport depends on the browser:
  - Android Chrome → WebUSB, through `web-serial-polyfill` for 303A (native USB) or esptool-js's WebUSBSerialPort for CH340.
  - Desktop → Web Serial.
  - Keep baud at 115200.
  - `BOARDS` in `main.js`: `arm`/`cam` read from `https://jobot101.github.io/xarm/` (CORS `*`); `sol`/`chime`/`chimebox` read from `./sol/`, `./chime/` and `./chimebox/`.
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
- **`chime_box/`:** the same bell machine on the original **ESP32-S3-BOX** (TT21100 touch, ILI9342C) and its dock. It's the hall chimes' display + control board.
  - **Pins:** drivers on PMOD1 SDA 41 / SCL 40, MIDI on GPIO 44, and triggers on driver 0x20 port B (B6 = solenoid tap, B7 = mute). The BOX's own pins are LCD 4/5/6/7/45/48, I2C 8/18 and touch INT 3: never use them for anything else.
  - **Screen:** LovyanGFX with an explicit S3-BOX config (copied from its autodetect). It shows 25 tube keys, Mute, Soft/Med/Hard and TAP. The UI task runs on core 0 and only queues notes.
  - **Solenoid tester output:** one MOSFET on a dock GPIO (default 9; set on the page, applied at boot). It strikes only, 3–30 ms, with at least 150 ms between taps. The BOOT button taps it too. Tube pulses are capped at 30 ms.
  - **Never put the plain `chime.bin` on a BOX:** it drives the screen's pins. Install latest only accepts a manifest with `"box": true`.
  - **NVS `chime` also holds** `spin sinv sms`. USB serial `wifi<TAB>ssid<TAB>pass` sets the hall WiFi.

## Build / publish

| Path | Workflow | Publishes to gh-pages |
|---|---|---|
| `solenoid/**` | `solenoid.yml` | `sol/` (`keep_files`) |
| `chime/**` | `chime.yml` | `chime/` (`keep_files`) |
| `chime_box/**` | `chimebox.yml` | `chimebox/` (`keep_files`); 16 MB, PSRAM opi, `app3M_fat9M_16MB` |
| `flasher/**` | `flasher.yml` | the page root (`keep_files`) |

- **Firmware builds:** arduino-cli with esp32 core 3.2.0, FQBN `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=disabled,FlashSize=4M,PartitionScheme=default,FlashMode=dio`.
- **Versions:** CI writes `<dir>/version.h` with `FW_VERSION` = the short SHA (gitignored). That's the same value as `manifest.json` `version`, which is how a board's page knows it's up to date.
- **Every publish job shares the `gh-pages` concurrency group.** Never use `force_orphan` here: it would wipe the other folders.

## Rules

- **Nothing secret in the repo or on Pages** (both public): no WiFi passwords, MQTT logins or OTA passwords. They're typed on the board's page and kept in NVS.
- **Keep xarm's paths:** `https://jobot101.github.io/xarm/fw/` and `/cam/` belong to xarm. Muffin's auto-update and the old flasher at `/xarm/` still use them.
- **Keep it plain:** the Ragamuffin Hall voice and plain words on the page.
- **Page design = the Ragamuffin Hall logo.** `brand/BRAND.md` is the spec and `brand/ragamuffin-hall-logo.png` the original; match it, don't reinterpret it.
  - White paper, black ink outlines, flat glass colours: red #c62b29, yellow #fdcd09, blue #60b6e5, green #8dad3e, ink #060709.
  - New Rocker for the wordmark and headings (a stand-in for the logo's face), Alegreya Sans for the UI. Both are self-hosted through @fontsource.
  - **`flasher/rose.js`** draws the window exactly. It's used by the page and by `brand/ragamuffin-hall-window.svg`; regenerate that file if rose.js changes.
  - While flashing, the petals go white and get coloured in as progress (`roseFill`). The progress bar uses the ring's red/blue dashes.
  - **Board colours:** arm = yellow, camera = blue, solenoid = green, chimes and Chimes BOX = red (`body[data-board]` → `--gem` fills the Flash button). A new board needs a colour from those four.
