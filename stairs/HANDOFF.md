# Handoff — Ragamuffin Hall stair lights

`stairs/stairs.ino` (firmware) + `stairs/app/` (React phone page → `stairs/page.h`). Stairs only; the rose-window lights are `window/` next to it. Flasher button: **Hall stairs**; CI `stairs.yml` publishes to `.../Ragamuffin-Flasher/stairs/`.

## The board (Nov 27 2026)
- ESP32-S3 DevKit, name `stairs`, on the hall WiFi **Starscraper** at **192.168.1.250**, `http://hall-stairs.local`. 5 V 5 A supply.
- **GPIO 4:** 1700 px. **GPIO 5:** 2200 px (stairs + landing), level 60 %.
- Supply limit 4200 mA, 60 mA per pixel → brightness is held down by the limit at these counts.

## Network
- Own WiFi **"Stairs"**, open (owner's call), page at 192.168.4.1, captive. Android: tap "Stay connected".
- Hall WiFi set on the page or over USB serial; password in NVS only, **never in the repo**. Presets: STARLINK, ClarkShop.
- MQTT (optional, broker set on the page): `ragamuffin/hall/stairs/{set,state,status}`; `set` takes `c=rrggbb&b=0-100&m=0-10&s=1-100&test=0|1`.
- USB serial: `wifi<TAB>ssid<TAB>pass`, `scan`, `c<hex>`, `b<pct>`, `t`, `n<pixels>`, `?`.
- Update from a computer on the hall WiFi (keeps settings): `curl -F "f=@stairs.bin" http://hall-stairs.local/update`. A USB flasher full erase loses WiFi, pixel counts and name.

## Pixels and power
- Data: GPIO 4 and GPIO 5 (0 px = off), through a 74AHCT125 + 330 Ω. Colour order is a setting (GRB default).
- ~30 µs per pixel: 1700 px ≈ 20 fps, 2200 ≈ 15 fps. Max 4000 per pin.
- Pixel count = chips, not LEDs. A count shorter than the strip leaves the rest frozen.
- 5 V WS2812-class ≈ 60 mA per pixel at full white; set **supply limit mA** ≈ 85 % of the supply.

## Settings (NVS namespace `stairs`)
`name nA nB ord trim fade lim pma r g b bri test mode spd ssid wpass mhost mport muser mpass`. Old boards may still hold `rings`/`sync` from the window firmware; they're ignored.

## Modes (speed 50 = 1×)
0 solid · 1 pulse + colour · 2 breathe · 3 spin · 4 ripple · 5 glass · 6 candle · 7 christmas · 8 candy cane · 9 snowfall · 10 twinkle. Test (all effects, 8 s each) is separate. Patterns run along GPIO 4; GPIO 5 repeats them.

## Code shape
- Core 1 = `loop()` → `ledLoop()` (LEDs only). Core 0 = `netTask` (WiFi, web, MQTT, NVS saves, OTA).
- **Flash safety:** FastLED's RMT interrupt isn't cache-safe. LEDs start only after WiFi is up (`netReady`), every NVS write goes through `FlashHold`, `WiFi.persistent(false)`, OTA holds the LEDs. Keep it that way.
- Arduino prototype scan: keep the page in `page.h`; define functions after the types they use.

## Unverified
- This stairs-only build hasn't been compiled or run yet (cut down from window build 8500d5f on Nov 27 2026).
