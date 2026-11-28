# Handoff — Ragamuffin Hall pixel lights (window + stairs)

One firmware, `window/window.ino` (+ `window/page.h`), runs **every addressable-pixel board in the hall**. Each board gets a **board name** on its own page; that name is its identity on the network. The flasher button is **Hall window**; CI (`window.yml`) publishes to `window/` and also copies to `stairs/`.

## The boards (Nov 27 2026)
| board | name | where | status |
|---|---|---|---|
| ESP32-S3 DevKit (the first chip, flashed as "window") | `stairs` (being renamed from `window`) | the hall stair, ~8 steps lit under the tread nosings | running on the hall WiFi **Starscraper**, last seen at **192.168.1.250**; 5 V 5 A supply |
| (next chip) | `window` (default) | LED ring around the rose window: feed spoke + 3 rings | chip moved to the stairs; a new chip gets flashed with **Hall window** and keeps the default name |

Rename over the network: `curl "http://<ip>/cfg?name=stairs&rings="` (restarts). Update over the network from a computer on Starscraper:
```
curl -sL https://jobot101.github.io/Ragamuffin-Flasher/window/window.bin -o /tmp/w.bin && curl -F "f=@/tmp/w.bin" http://<ip>/update
```
Or the board page's **Install latest** (phone with mobile data). **The flasher does a full erase** — WiFi login, pixel counts, rings and name are lost; re-set them after a USB flash. Install latest / curl keep everything.

## Network, per board (name = `<name>`)
- Own WiFi **"<Name>"**, **open (no password — owner's call)**, page at 192.168.4.1, captive. Android: tap "Stay connected" when it says no internet.
- Hall WiFi: the owner uses **Starscraper** (STARLINK isn't heard at the board; the page also has STARLINK and ClarkShop preset buttons). Password typed on the page or over USB serial; NVS only, **never in the repo**.
- `http://hall-<name>.local` / its IP. MQTT (optional, broker set on the page): `ragamuffin/hall/<name>/{set,state,status}`; `set` takes `c=rrggbb&b=0-100&m=0-12&s=1-100&test=0|1`.
- USB serial (native port and a DevKit's UART port): `wifi<TAB>ssid<TAB>pass`, `scan`, `c<hex>`, `b<pct>`, `t`, `n<pixels>`, `?`.

## Pixels and hardware
- **Data:** GPIO 4 (output 1), GPIO 5 (output 2, 0 px = off). Through a 74AHCT125 (5 V) + 330 Ω at the board; BIN to GND on WS2815. Super Mini has no GPIO 16/17; GPIO 48 is the onboard LED.
- **Strip:** addressable neon flex + JST-SM pigtail strips (red V+, green data, white GND). Voltage/chip still unconfirmed — the colour order is a setting (GRB default).
- **Pixels per pin:** ~30 µs per pixel → 1,000 px ≈ 33 fps, 2,000 ≈ 16 fps, 4,000 ≈ 8 fps (firmware cap `MAX_PIX` 4000 per pin). The owner is planning **~5,000 pixels** on the window: split **2,500 on GPIO 4 + 2,500 on GPIO 5**.
- **Power:** a 5 V WS2812-class pixel ≈ 60 mA full white. 5,000 px ≈ 300 A full white, ~100 A single colours at 100%. The 5 V 5 A supply is only fine for small runs (the stairs work on it). For the window: 2× 5 V 60 A (LRS-350-5), injection every 150–200 px with 14 AWG, 15 A fuse per feed, all grounds tied; set **supply limit mA** ≈ 85% of installed. If the reel is 12 V: ~⅓ the current, one 12 V 50 A, injection every 300–500 px.
- **Pixel count** setting = chips, not LEDs. A count shorter than the strip leaves the rest frozen at the last colour (that was the "pattern stops" bug).

## Page → Strip settings (NVS namespace `stairs`, kept so moved boards keep settings)
| key | setting | default |
|---|---|---|
| `name` | board name → hall-<name>.local, WiFi "<Name>", MQTT topic; change restarts | `window` |
| `nA` / `nB` | pixels on GPIO 4 / GPIO 5; change restarts | 300 / 0 |
| `rings` | ring lengths on GPIO 4 in wiring order, `r` = runs backwards; leftover = one more ring | "" |
| `sync` | Test draws one 256-step virtual ring and lays it on every ring by angle | on |
| `ord` | colour order (GRB RGB BRG RBG GBR BGR) | GRB |
| `trim` | GPIO 5 level % | 100 |
| `fade` | ms for colour/brightness changes | 600 |
| `lim` | supply limit mA (brightness is cut to stay under it), 0 = none | 4200 |
| `pma` | mA per pixel at full white | 60 |
| `r g b bri test mode spd` | the look: colour, brightness %, test on/off, mode, speed | warm, 40, test on, 0, 50 |
| `ssid wpass mhost mport muser mpass` | hall WiFi + MQTT | — |

**Show rings** lights each ring red/yellow/blue/green with a white start pixel for 20 s, for dialling in `rings`.

## Modes (page → Mode, speed 50 = 1×)
0 solid · 1 pulse + colour (~6 s breath, hue round the wheel ~60 s — the owner's ask) · 2 breathe (chosen colour) · 3 spin · 4 ripple (ring to ring) · 5 glass (the four glass colours swapping) · 6 candle · 7 christmas · 8 candy cane · 9 snowfall · 10 twinkle · **11 ragamuffin · 12 ragamuffin turning**. Test (all effects, 8 s each) is separate. Picking a colour drops to solid unless the mode uses it (0, 2, 4).

**Ragamuffin = the logo on the rings** (brand/BRAND.md): outermost ring red/blue dashes, next in the 16 petals (yellow, green, blue, red ×4, dark gap), next in the hub's five wedges (yellow, green, blue, red, blue), the feed spoke warm white. Needs `rings` set; angle 0 = each ring's first pixel (a rotation setting to put petal 1 at 12 o'clock is an open idea). The owner wants the window to look like the Ragamuffin Hall logo — that's the direction for new window modes.

## Code shape
- Core 1 = `loop()` → `ledLoop()` (LEDs only). Core 0 = `netTask` (WiFi, web, MQTT, NVS saves, OTA).
- **Flash safety (fixed a boot crash loop):** FastLED's RMT interrupt isn't cache-safe. LEDs start only after WiFi is up (`netReady`), every NVS write goes through `FlashHold` (waits out the frame), `WiFi.persistent(false)`, OTA holds the LEDs. Keep it that way.
- Colour order is applied in software (`wire()`); FastLED is set to RGB.
- Arduino prototype scan: keep the page in `page.h`, and define functions after the types they use (a function before `struct Look` broke the build once).

## Original stair plan (reference only)
Claude Docs "Green room stair lighting plan": two flights (~10 + 12 steps) at a landing, strips under the nosings. Owner's calls then: no sensors; brightness and colour only — later widened to the modes above.

## Unverified on hardware
- The modes, rings/sync, Ragamuffin modes and board-name rename have built in CI but not been seen running.
- Real pixel counts and ring lengths of the window; the reel's voltage and chip.
