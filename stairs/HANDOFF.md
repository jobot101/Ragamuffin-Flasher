# Handoff — Ragamuffin Hall stair lights

`stairs/stairs.ino` (firmware) + `stairs/app/` (React phone page → `stairs/page.h`). Stairs only; the rose-window lights are `window/` next to it. Flasher button: **Hall stairs**; CI `stairs.yml` publishes to `.../Ragamuffin-Flasher/stairs/`.

## The board (Nov 27 2026)
- ESP32-S3 DevKit, name `stairs`, on the hall WiFi **Starscraper** at **192.168.1.250**, `http://hall-stairs.local`. 5 V 5 A supply.
- **GPIO 4:** 1700 px. **GPIO 5:** 2200 px (stairs + landing), level 60 %.
- Supplies (Nov 28 2026): upper flight (GPIO 4) on a 5 V 6 A, landing + lower flight (GPIO 5) on a 5 V 5 A, the chip on its own; all grounds tied, + kept separate. Limits: `lim` 5100, `limB` 4250 (85 %). The brightness slider is a share of what the supplies allow (100 % = the most they give), so it always does something.

## Network
- Own WiFi **"Stairs"**, open (owner's call), page at 192.168.4.1, captive. Android: tap "Stay connected".
- Hall WiFi set on the page or over USB serial; password in NVS only, **never in the repo**. Presets: STARLINK, ClarkShop.
- MQTT (optional, broker set on the page): `ragamuffin/hall/stairs/{set,state,status}`; `set` takes `c=rrggbb&b=0-100&m=0-14&s=1-100&test=0|1`.
- USB serial: `wifi<TAB>ssid<TAB>pass`, `scan`, `c<hex>`, `b<pct>`, `t`, `n<pixels>`, `?`.
- Update from a computer on the hall WiFi (keeps settings): `curl -F "f=@stairs.bin" http://hall-stairs.local/update`. A USB flasher full erase loses WiFi, pixel counts and name.

## Pixels and power
- Data: GPIO 4 and GPIO 5 (0 px = off), through a 74AHCT125 + 330 Ω. Colour order is a setting; the stairs strip is **GRB** (set Nov 27 2026 after the measure video showed red/green swapped).
- ~30 µs per pixel: 1700 px ≈ 20 fps, 2200 ≈ 15 fps. Max 4000 per pin.
- Pixel count = chips, not LEDs. A count shorter than the strip leaves the rest frozen.
- 5 V WS2812-class ≈ 60 mA per pixel at full white; set the **supply limits** ≈ 85 % of each supply (`lim` for GPIO 4, `limB` for GPIO 5 when it has its own).

## Step map — tuned on the stairs (Nov 28 2026, Runner look perfected by the owner)
These are the saved maps (also the firmware defaults). Format `start-end/startBorder/endBorder`; gaps between steps are dark loops.
- **GPIO 4:** `0-136/-1/15,137-309/15/25,310-467/13/22,468-631/27/-1,632-783/15/-1,784-939/15/17,940-1091,1092-1243,1244-1397,1398-1545,1546-1699`
- **GPIO 5:** `0-511/15/-1,512-675/22/22,681-833/17/17,851-1003/18/18,1009-1161/18/19,1183-1334/19/16,1339-1491/18/19,1492-1644/20/15,1659-1811/-1/20,1819-1971/20/15,1993-2135/15/14`
- Border colour magenta (`ff00ff`), default border 15 px, middle stripe fills, GPIO 5 level 100 %, colour order GRB.

### First measurement (Measure photos, ±5 px) — kept for reference
**GPIO 4 = the upper flight, 11 steps.** Pixel 0 is the top step's left end; the strip snakes down (left→right, then right→left, …).

| step (top = 1) | pixels | runs |
|---|---|---|
| 1 | 0–137 | L→R |
| 2 | 138–300 | R→L |
| 3 | 301–465 | L→R |
| 4 | 466–627 | R→L |
| 5 | 628–785 | L→R |
| 6 | 786–931 | R→L |
| 7 | 932–1096 | L→R |
| 8 | 1097–1244 | R→L |
| 9 | 1245–1395 | L→R |
| 10 | 1396–1545 | R→L |
| 11 | 1546–1699 | L→R |

**GPIO 5 = three landing walls along the ceiling line, then the lower flight, 10 steps.** Same snake on the steps.

| part | pixels | runs |
|---|---|---|
| landing wall 1 (pixel 0 at its left end, by the door frame) | 0–~175 | along the top |
| landing wall 2 | ~176–~338 | along the top |
| landing wall 3, to the door corner above the stairs | ~339–~519 | along the top |
| step 1 (top of the lower flight) | ~520–675 | R→L |
| 2 | 676–829 | L→R |
| 3 | 830–1000 | R→L |
| 4 | 1001–1166 | L→R |
| 5 | 1167–1313 | R→L |
| 6 | 1314–1493 | L→R |
| 7 | 1494–1644 | R→L |
| 8 | 1645–1814 | L→R |
| 9 | 1815–~1960 (left end too dim to read) | R→L |
| 10 (bottom) | ~1960–2199 (2100 seen near the right end; the last ~70 px and the end marker not seen) | L→R |

The far end of GPIO 5 is starved of power: by step 8 the blue ticks were gone and only red/green showed (voltage drop). Needs power injected at the bottom of the lower flight.

**Map format:** `start-end` per step (end included), top step first, optionally `/startBorder/endBorder` in pixels (e.g. `138-300/20/12`; -1 or missing = the global border width); a plain `start` runs to the next step. Pixels between one step's end and the next step's start stay dark (a loop of strip round a corner).

**Editing the map:** the Steps card and the Measure modes were taken off the page once the stairs were tuned (owner's call, Nov 28 2026). The firmware still has them: `/step?stA=…&stB=…&out=A|B&i=N` lights one step (`/step?done=1` ends), `/cfg?stA=…&stB=…` saves, `/set?m=13|14` shows the Measure ruler. Bring the card back from git history (commit 0fee6fb) if the strip is re-laid.

## Settings (NVS namespace `stairs`)
`name nA nB ord trim fade lim limB pma stA stB bcr bcg bcb bw mw nOn nFr nTo nLv nWk tz r g b bri test mode spd ssid wpass mhost mport muser mpass`. Old boards may still hold `rings`/`sync` from the window firmware; they're ignored.

## Modes (speed 50 = 1×)
**On the page (owner's pick, Nov 28 2026):** Runner (11) · Walk up (12) · Arrow (15: a big arrow climbing the stairs — point on one step, head widening over 4 steps below, 4-step shaft; borders lit; 300 ms a step at 1×) · Party (= the Test sequence, `test=1`) · Christmas (7) · Solid (0). The others below stay in the firmware but have no button.

0 solid · 1 pulse + colour · 2 breathe · 3 spin · 4 ripple · 5 glass · 6 candle · 7 christmas · 8 candy cane · 9 snowfall · 10 twinkle · 11 runner (every step: a border at both ends in the border colour, the chosen colour up the middle) · 12 walk up (the side borders stay lit; the middle stripes light step by step from the bottom of GPIO 5 up through the landing and GPIO 4 to the top, holds, clears the same way; speed scales it) · 13 measure GPIO 4 · 14 measure GPIO 5 (a ruler for filming, one output lit: blue dot every 10th pixel; at every 100th a 3-px bar — white <1000, green 1000+, yellow 2000+ — then the hundreds digit as that many red dots; green ×5 at pixel 0, red ×5 at the end). Test (all effects, 8 s each) is separate. Patterns run along GPIO 4; GPIO 5 repeats them.

## Night mode
Page → Night. Between `nFr` and `nTo` (minutes after midnight, local time; the board asks pool.ntp.org once it's on the hall WiFi, `tz` = POSIX zone, default `EST5EDT,M3.2.0,M11.1.0`) only the step borders glow at `nLv` % of the brightness. **Light the stairs** (`/wake`, serial `w`) walks the middles up (with their borders) at 250 ms a step, holds `nWk` s, then walks them back to the outline. Night overrides the chosen mode inside the window; Test overrides night. No sensor or touch input yet; no MQTT wake (owner: hold off on MQTT).

## Code shape
- Core 1 = `loop()` → `ledLoop()` (LEDs only). Core 0 = `netTask` (WiFi, web, MQTT, NVS saves, OTA).
- **Flash safety:** FastLED's RMT interrupt isn't cache-safe. LEDs start only after WiFi is up (`netReady`), every NVS write goes through `FlashHold`, `WiFi.persistent(false)`, OTA holds the LEDs. Keep it that way.
- Arduino prototype scan: keep the page in `page.h`; define functions after the types they use.

## Unverified
- This stairs-only build hasn't been compiled or run yet (cut down from window build 8500d5f on Nov 27 2026).
