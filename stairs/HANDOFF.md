# Handoff — hall stair lights

Folder `stairs/`, firmware `stairs.ino`. Board button "Stair lights" on the flasher. Started Nov 24 2026.

## The job
- The Ragamuffin Hall stair: two flights (about 10 + 12 steps) meeting at a landing. The plan doc is "Green room stair lighting plan" (Claude Docs).
- **The owner's call: no sensors. The only adjustments are brightness and colour.** Don't add sweeps, motion or modes unless he asks.
- Test mode (every colour + busy effects, 8 s each) is for checking a new strip, nothing more.

## Hardware (Nov 24 2026)
- **ESP32-S3 DevKit** on the bench now (Super Mini planned), strip data on **GPIO 4**. USB commands work on both DevKit ports. A second run can go on **GPIO 5**: the hub sits at the landing and the flights run away from it.
- **Strip:** addressable LED neon flex, "thousands of lights". Chip, voltage and W/m not confirmed yet. Also has a reel with a red/green/white JST-SM pigtail (red V+, green data, white GND by convention).
- **Hall WiFi:** STARLINK (page default + preset). ClarkShop is the shop preset. Passwords only in NVS.
- **Supply on the bench:** 5 V 5 A. The default supply limit (4200 mA) is for it.
- Super Mini pins: 1–13, 43, 44 on the header; no 16/17. GPIO 48 is the onboard RGB LED.

## Settings (page → Strip)
| setting | default | note |
|---|---|---|
| pixels on GPIO 4 / GPIO 5 | 300 / 0 | chips, not LEDs (WS2811 neon is often 3 LEDs per chip); change restarts |
| colour order | GRB | RGB / BRG common on WS2811 and neon |
| GPIO 5 level % | 100 | evens out the dark-stained upper flight (plan: lower flight at about 67 %) |
| fade ms | 600 | colour/brightness changes fade |
| supply limit mA | 4200 | 0 = none |
| mA per pixel, full white | 60 | W/m ÷ V ÷ pixels per m × 1000 |

## Unverified on hardware
1. FastLED on core 3.2.0 driving GPIO 4 on the Super Mini; real colour order of the neon reel.
2. Frame time with thousands of pixels (about 30 µs per pixel) while WiFi runs.
3. Install latest + `/update` end to end; `hall-stairs.local` on Android.
