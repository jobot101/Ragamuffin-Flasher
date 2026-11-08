# Hall chimes BOX firmware: design

The ESP32-S3-BOX (original, not the BOX-3) on its dock drives the Ragamuffin Hall chimes. It controls 25 brass tubes,
plus any smaller solenoids or other attachments the owner adds. Each output is one solenoid on an Adafruit MCP23017
solenoid driver board. `chimes.h` is the contract between modules; this file is the spec. Background for this build:
`C:\Users\mark\Downloads\CLAUDE_CODE_HANDOFF.md`. The firmware it replaces (single-file, working on the board) is
`C:\Users\mark\Documents\Ragamuffin Workshop\Ragamuffin Hall\Chimes\backups\chime_box_one-solenoid-test_2026-11-08.ino`.
Reuse its proven parts verbatim where they fit: the LovyanGFX `LGFX_Box` class, WiFi join pacing, MQTT, OTA `/update`, and the
Install latest / Upload file page code.

## Hardware

- **Drivers:** Adafruit MCP23017 8-channel solenoid drivers at 0x20..0x27, chained over STEMMA QT.
  - Bus: G41 SDA / G40 SCL, Arduino `Wire` (I2C port 0), 400 kHz, `Wire.setTimeOut(10)`.
  - The boards are on/off only, with no PWM, and run on 24 V.
  - Outputs are on port A (pins 0-7) by default. Port B is a runtime setting for all boards.
  - Output index `i = board*8 + pin`, so output number `i+1` is what people see.
  - Drive the boards with plain `Wire` register writes, not the Adafruit library. The registers, with IOCON.BANK=0, are:
    - IODIRA 0x00, IODIRB 0x01
    - GPPUA 0x0C, GPPUB 0x0D
    - GPIOB 0x13
    - OLATA 0x14, OLATB 0x15
- **Default map:** outputs 1..25 are the tubes C3..C5, with MIDI note = 47 + output number.
  - Their pulse runs from 3 ms min to 18 ms full. Tubes 1..5 go to 20.5 ms full.
  - Cap 30 ms, 400 mA each. The coils are JF-Z05, 24 V / 60 Ω.
  - Outputs 26..64 start as K_OFF, named "", with no note. Their values are min 2 ms, full 8 ms, cap 15 ms and 200 mA, which are the defaults for when the owner turns one on as a small solenoid.
- **Pins:**
  - MIDI in: G14 by default, through a 6N138 opto. UART1 RX at 31250 baud. The only other choice is 44.
  - Panel PANIC button: G10 to GND, INPUT_PULLUP. Can be set to -1.
  - Optional one-MOSFET tester: G9. Can be set to -1, and can be inverted.
  - Optional piezo: ADC pins 9..14 only. Default -1.
  - Each header pin has one job, decided in this order: tester, MIDI, button, piezo. A clash, or a pin that isn't allowed, disables the later role; MIDI moves to its other pin instead (see `checkPins`). The tester goes first because its pin may be wired to a MOSFET gate, where a button pull-up or the UART RX pull-up would hold the solenoid on. So an old `spin` of 10 or 14 keeps the tester, and the default button / MIDI pin gives way.
- **Triggers:** contacts to GND on 0x20 port B, only when outputs are on port A and `cfg.trig` is on (default off).
  - B0..B5 strike outputs 1, 8, 13, 17, 20, 25 at velocity 110.
  - B6 behaves like the panic button.
  - B7 is never read. On current MCP23017 silicon (datasheet rev C on) GPA7/GPB7 are output-only, and a changing input there can corrupt SDA. So pin 7 of the port that isn't driving coils is an output held low on every board. Leave it unwired.
  - Pull-ups on 0x20 port B (B0..B6) are only enabled while triggers are on. Every other pin of the port that isn't an output is left as an input with no pull-up.
- **Recommended hardware:** the MCP23017s are not reset by an ESP32 reset, so a latched coil stays on until firmware zeroes it. Wire the expanders' RESET line to the ESP32 EN/reset net, or to a header GPIO with a pull-down, so any ESP reset also resets the drivers (all pins become inputs and the MOSFET gates are pulled off).

## Safety (non-negotiable; reviewers check every line against this)

1. **Off at boot.** The MCP23017s keep their latches across an ESP reset (crash, watchdog, brownout, restart).
   - The very first statement of `setup()` is `driversOffNow()`: Wire up, then OLATA=0 and OLATB=0 to every address 0x20..0x27. It needs no config and runs before `Serial.begin()` and `loadCfg()`.
   - `loadCfg()` is read-only: no flash write at boot. Stale keys are removed later, on core 0, under the flash-write hold (`nvsTidy()`).
   - `coreSetup()` then runs, before any task, WiFi or screen. It sets the tester pin to its off level, then makes it an output.
   - It scans 0x20..0x27. For each board that answers, it writes OLATA=0 and OLATB=0 *before* setting IODIR.
   - It then calls `allOff()`.
   - Every rescan does the same off-first init.
   - Deliberate restarts (`/pins`, OTA) are muted and take the flash-write hold first, so nothing is on as the board goes down.
2. **One place turns a coil on:** `fire()` in core.cpp.
   - It clamps the pulse to `outCap(i)`, and `outCap` itself never exceeds `HARD_CAP_US` (30 ms). It logs and counts every clamp.
   - It sets the coil's off time in the same statement block.
   - There is no hold mode, no "on until stop", and no path that skips `fire()`.
3. **Supply budget.** A queued strike fires only if both of these hold:
   - `maOn + outs[i].mA <= cfg.supplyMa`
   - `coilsOn < MAX_ON_COUNT`

   Exception: when no coil is on, one strike may go even if it alone exceeds the budget, so nothing can deadlock. Log that once per minute.

   Otherwise the strike waits in the queue, in due order. If it is still waiting `STALE_US` after its due time, it is dropped, logged and counted. Nothing is dropped silently, and that includes a full queue.
4. **Heat guard.** Each output has a bucket that holds `HEAT_CAP_MS`.
   - Each strike adds `us/1000` to it, and it leaks `HEAT_LEAK` ms per ms.
   - A strike that would overflow the bucket is refused. Set `refusedAt`, count `nHeat`, and log at most once a second.
5. **Rearm.** An output can't fire again until `REARM_US` after its last strike. Its queued strikes wait. Other outputs behind it may go.
6. **Panic:** all off, queue cleared, demo and sequencer stopped, `calAbort` set, `nPanics++`, logged. The sources are:
   - the panel button, BOOT (G0) and trigger B6. A press always panics *and mutes*. Holding the button 1.5 s unmutes, but only if the chimes were already muted before that press *and* there was no panic from any source in the last 5 s *and* that button had been released for more than 2 s. So a burst of emergency presses, contact chatter, or a press right after another panic source can never unmute.
   - the screen's PANIC tab panics on any touch sample on it, even a finger that is still "held" or landed within the 150 ms press filter.
   - MIDI CC120 or CC123 (panic only, no mute, since DAWs send these on stop)
   - the web `/panic`, serial `panic`, MQTT `<base>/panic`, and the screen's PANIC tab (all panic and mute)

   Core 0 never writes a coil. It calls `requestPanic()`, which sets `calAbort` and `muted` straight away when muting, then sets `panicReq`. Core 1 acts at the start of its next pass, well under 20 ms. Muting or `updating` going true also does all off, clears the queue, and stops the demo and sequencer.
7. **Watchdog.** If a core-1 pass starts more than `WATCHDOG_US` after the previous one, call `allOff()`, clear the queue, count it and log it.
8. **I2C faults.** If an output write fails, set the board to `B_FAULT`, add to `bErr`, and try to write zeros to both OLAT registers.
   - Clear that board's coils and log it.
   - If the zeros don't both ACK, the latch may still hold a coil on: the board is marked in `stuckMask`. Core 1 retries its zeros every 3 ms (one stuck board per try), whatever else is on, queued or playing, and clears the mark only when both writes ACK.
   - While any board is stuck: panic and mute, keep muted (every unmute path is refused), and show and log "coil may be stuck on: cut 24 V".
   - Its outputs are refused, counted in `nFault`, and logged at most every 2 s.
   - Every 3 s, while no coil is on and the queue is empty, core 1 rescans boards that aren't OK. A faulted board that answers comes back OK after an off-first init. That rescan only re-reads the triggers when board 0 itself was set up again.
9. **Flash writes stall core 1, so they wait for quiet.** An NVS write or erase disables the cache and parks core 1; a coil that is on would stay on. So every flash write takes the flash-write hold first (`holdCoils()`, inside `beginWrite()`, after the config mutex):
   - while a hold is in force core 1 starts no strike (`serviceQueue()` is skipped) and no tester tap; queued strikes wait, and are stale-dropped and logged as usual.
   - core 1 acks once a whole pass ran under the hold with no coil and no tester on and every OK board's zeros written.
   - the write goes ahead only after that ack. If none comes within `HOLD_WAIT_MS` (150 ms) the save is aborted and logged, never written anyway, and the endpoint says so (503).
   - OTA takes the hold for the whole upload, and `updating` is true, which mutes.
10. **Core 1 never blocks.** No `Serial` printing from core 1, only `evlog()`. Core 0 prints the log. No `delay()` and no NVS on core 1.

## Core 1 (core.cpp): the instrument

`coreLoop()`, once per pass:
1. Watchdog check, then `panicReq`.
2. Tester off-time.
3. MIDI bytes. Running status, realtime ignored, SysEx skipped, channel filter `cfg.midiCh`.
   - Note-on with vel > 0 strikes every usable output whose `note` matches.
   - If none matches and `cfg.fold` is on, fold the note into 48..72 by octaves and try again.
   - CC120/123 panic with no mute. CC64 sets `sustain`, which has no effect: it is a hook for a future damper.
4. Inputs: the panel button and BOOT go through the panic-button logic. Triggers are polled every 2 ms when enabled.
5. Messages from `netToChimes`.
6. Demo player.
7. Sequencer.
8. Mute/update edge.
9. Heat leak and off-times.
10. `serviceQueue()`.
11. `flushOut()`: one OLAT write per board per pass, only when it changed. A failure goes to the fault path.
12. Update `coilsOn`, `maOn` and `outOn[]`.
13. The 3 s auto-rescan.

- **Strike request:** `requestOut(i, vel, us, t0)`.
  - Refused while muted or updating, for an index out of range, for K_OFF, when `cfg.solo` blocks it, and for a board that isn't OK (counted and logged as in rule 8).
  - `us = 0` means `minUs + (fullUs - minUs) * (vel/127)^gamma`.
  - It queues `{i, vel, us, due = t0 + cfg.latUs}`, sorted by due time.
  - The fixed latency keeps strikes evenly timed despite I2C time.
- **Demo:** the `SONGS` tables, defined in core.cpp. Each note is played through the MIDI-note mapping at velocity 100.
  - Westminster Quarters: the hour, changes 2-5 in E (E4 G#4 F#4 B3 / E4 F#4 G#4 E4 / G#4 E4 F#4 B3 / B3 F#4 G#4 E4), then three E3 strikes.
  - Amazing Grace in F, 3/4.
  - Ode to Joy in C.
  - Absolute scheduling, `next += len*eighthMs`, so there's no drift. It stops on panic or mute.
- **Sequencer:** `M_SEQ` starts the slot (or stops it with 255), and `seqSlot` and `seqStep` are published.
  - Step length is `60000/bpm/4` ms. Odd steps are delayed by `swing%` of a step.
  - At each step, every row with a row output and vel > 0 calls `requestOut(row output, vel)`.
  - At the end it loops if `loop`, otherwise it stops.
  - Absolute scheduling. Muting and panic stop it. Starting the sequencer stops the demo, and the other way round.
  - Core 0 may edit `pats[]` while it plays: single-byte fields only, so no tearing matters.

## Core 0

### config.cpp
- **Loading:** `loadCfg()` opens NVS "chime" read-only and clamps every value to its range.
  - Stale keys `ppin` and `pthr` are removed by `nvsTidy()`, on core 0 once the tasks run (a write, so it takes the hold).
  - Outputs come from blob "outs" and patterns from blobs "pat0".."pat3". A size mismatch means defaults.
  - Each output is clamped: `minUs` within MIN_PULSE_US..HARD_CAP_US, `fullUs` within minUs..HARD_CAP_US, `capUs` within MIN_PULSE_US..HARD_CAP_US, and `mA` within 0..3000, at least `MIN_COIL_MA` (20) for any output that isn't K_OFF, so no live coil leaves the supply budget. Names are NUL-terminated.
- **Pins** are validated as in Hardware. If `apPass` is shorter than 8 characters, it becomes `defaultApPass()`.
- **Existing NVS keys**, which must keep working:
  - `ssid wpass mhost mport muser mpass`
  - `spin` (tester pin), `sinv`, `sms`
- **New keys:** `cap supma port mch gam fold lat solo trig midip btnp pzpin appass outs pat0..pat3`
- **Pattern defaults:**
  - slot 1: "Beat", 16 steps, 100 bpm, loop. Its rows are outputs 26..28, which are off by default so they stay silent until enabled.
  - slot 2: "Chime walk", rows = tubes 1, 5, 8, 13, 17, 20, 25, with an ascending pattern.
  - slots 3 and 4: empty, "Pattern 3" and "Pattern 4".
- **Saving:** every save function takes one FreeRTOS mutex around `prefs`, then the flash-write hold (Safety 9). The saves return false when nothing was written.
- **Edit tracking:** `outsRev` and `patRev[]` are bumped on every edit (`outsEdited()`, `patEdited()`), from the page or the screen. `patDirty[]` is set by every pattern edit and cleared by a successful `savePattern()`; the page's "Save *" and the screen's SAVE* both show it.

### net.cpp
- **WiFi:** AP+STA at boot. The own WiFi is `AP_SSID` with `apPass`, and gets a captive DNS.
  - The hall WiFi join is paced: try for 15 s, retry every 45 s.
  - After 60 s on the hall WiFi, turn the own WiFi off. After 20 s without the hall WiFi, turn it back on. Log both.
  - mDNS `hall-chimes`.
- **Nothing in the net task blocks for long**, so the web `/panic` and `/mute` and the serial `panic` are always served promptly.
- **MQTT** runs in its own core-0 task (`mqttTask`), so a broker that is down (a connect blocks for seconds) never delays the web or serial panic. Retry every 5 s, doubling to 60 s while the broker stays away. The web handler changes the broker strings under a mutex and the task re-reads them. Subscribe to `note`, `mute`, `panic` and `tap`, and publish `played` and `status` (LWT).
  - `<base>/mute` is parsed strictly, ignoring case and spaces: `1`/`on`/`true`/`mute` mute; `0`/`off`/`false`/`unmute` unmute. Anything else is logged and ignored. An unmute within 3 s of a connect is ignored: that is a retained message replayed on reconnect, and a reconnect must never unmute.
- **Serial console** (115200, USB CDC). Print the log lines from core 0. Commands:
  - `scan`
  - `fire <ms> [out]`
  - `out <n> [vel]` (alias `tube`)
  - `sweep <n>`, `walk`
  - `demo <1-3|0>`, `seq <1-4|0>`
  - `panic`, `unmute`, `mute 1`
  - `status`
  - `n <note> [vel]`, `tap`
  - `wifi<TAB>ssid<TAB>pass`
  - `help`
- **Web endpoints.** All are GET unless noted. Every endpoint that changes state answers with `statusJson()`, unless noted.
  - `/` sends PAGE. `/setup` redirects to `/`.
  - `/status` (alias `/state`)
  - `/panic`, and `/mute[?on=0|1]` (no argument toggles; `on=1` goes through `requestPanic(true, …)`)
  - `/play?o=N&v=V` plays output N, 1-based (alias `t=`). `/n?note=&v=`
  - `/try?o=&ms=` is an exact strike, still clamped.
  - `/demo?n=`, `/tap`, `/rescan`
  - `/walk` strikes every usable output in order, 450 ms apart. `/sweep?o=`, `/tune?o=`, `/pzscan`, `/calstop`
  - **Number fields** in `/out`, `/outcopy`, `/set` and `/seqmeta`: a field that is absent or blank leaves that value as it is; a field that isn't a number gets 400 naming it, and nothing changes. A save that fails answers 503 with a reason.
  - `/outs` returns `{"rev":outsRev,"outs":[{"n":name,"k":kind,"no":note,"mi":minUs,"fu":fullUs,"c":capUs,"ma":mA},...64]}`.
  - `/out?o=N&name=&kind=&note=&min=&full=&cap=&ma=` sets fields of output N (ms as floats for min, full and cap), then clamps and saves. It returns `/outs`.
  - `/outcopy?k=kind&min=&full=` sets min and/or full (ms) on every output of that kind, with one save. It returns `/outs`.
  - `/outreset?o=N` resets one output (N=0 resets all), then returns `/outs`.
  - `/seq?s=` returns the pattern: `{"s":slot,"rev":..,"dirty":0/1,"name":..,"steps":..,"bpm":..,"swing":..,"loop":..,"row":[12],"vel":[[32]x12]}`.
  - Pattern edits, each returning `/seq` of that slot:
    - `/seqcell?s=&r=&c=&v=`
    - `/seqrow?s=&r=&o=` (o is a 1-based output, 0 = none)
    - `/seqmeta?s=&name=&steps=&bpm=&swing=&loop=`
    - `/seqclear?s=`
  - `/seqsave?s=` saves to NVS. `/seqplay?s=` and `/seqstop`.
  - `/set?cap=(ms)&supma=&mch=&gam=&fold=&lat=(ms)&solo=&sms=&port=&trig=`: `port` and `trig` are applied by message, and only when they differ from the current value (each change means all off and a re-init; core 1 also ignores an M_PORT/M_TRIG that changes nothing). Then save.
  - `/pins?midi=&btn=&sol=&inv=&pz=`: validate, answer 400 with a reason on error. Otherwise panic with mute, save (503 and no restart if that fails), answer, take the hold, restart.
  - `/appass?p=`, `/wifi?ssid=&pass=` or `?forget=1`, `/mqtt?host=&port=&user=[&pass=]`
  - `/scan?start=1` starts an async WiFi scan and answers `{"scanning":1}`; `/scan` answers `{"scanning":1}` until the list (`["ssid",..]`) is ready, or `{"failed":1}`. The page polls it.
  - `/update` (POST, multipart): mute and `updating` for the whole write, and the flash-write hold taken before the first write (refused if core 1 doesn't go quiet).
  - A non-panic clear of the queue (rescan, port or trigger change, watchdog) adds what it dropped to `nDropped` and logs it.
- **`statusJson()`** returns:

  ```
  {"fw","box":1,"muted","updating","supma","maOn","maxOn","on","queue","qpeak","played","dropped","heatRefused",
   "faultRefused","clamped","panics","watchdog","midi","sustain","demo","songs":[names],"seq":{"slot","step"},
   "last":[out,vel],"hardCap","stuck":mask,
   "rev":{"outs":n,"pat":[4],"dirty":[4 0/1]},
   "boards":[{"a":addr,"s":state,"e":errors} x8],
   "live":{"on":[64 0/1],"heat":[64 pct],"hot":[64 0/1],"strikes":[64]},
   "cfg":{cap,supma,port,mch,gam,fold,lat,solo,trig,midip,btnp,solp,sinv,sms,pzp},
   "cal":{"state","out","cur","msg","steps":[[us,peak]..]},"tune":{"out","ok","hz","cents","note"},
   "pz":{"done","pins":[6],"pk":[6]},"log":[last 10],"net":{ssid,up,ip,why,ap,apssid},"mqtt":{host,port,user,up}}
  ```

  `hot` means it was refused in the last 3 s, or its bucket can't take one more capped strike. `stuck` is `stuckMask` (Safety 8). In `pz.pins` a pin with another job (MIDI, button, tester) is -1: the scan skips it.

### page.cpp: the phone page (main interface)
- **Format:** one PROGMEM HTML page in the existing dark style, so reuse the CSS from the backup. Mobile first, max-width 520.
- **Header:** sticky, with the title, firmware version, a big red **PANIC** button and a **Mute** toggle on every tab. A one-line summary sits under them.
- **Tabs:**
  - **Play:** a key for every usable output, labelled with its name and note. Velocity slider. Demo songs with Stop. Walk all. Tester TAP.
  - **Outputs** (the status panel and config): one row per output that is on, or on a board that is present.
    - Live state: on/firing, heat bar, hot, strikes, fault.
    - Inline edit: name, kind (off/tube/small solenoid/other), MIDI note, min/full/cap in ms, mA.
    - Buttons: Save output, Reset. A "show all 64" toggle.
    - The supply budget shown: maOn / supma, and the max coils at once.
  - **Program** (the sequencer):
    - Choose slot 1-4, and set name, steps, bpm, swing and loop.
    - A grid of 12 rows × steps. Each row has an output dropdown. A cell click cycles 0, 60, 100, 127, then back to 0.
    - Play/Stop, a playing-step cursor from `/status` `seq.step`, Save, Clear.
  - **Calibrate:** choose an output.
    - min and full in ms, Try min, Try full, Save, Copy to all of the same kind, Defaults.
    - Sweep from 3 ms to its cap in 1 ms steps, with a live step list showing the piezo peak if a piezo is set. A tap sets min or full. Suggestions are computed client-side from the peaks.
    - "Even out loudness": a walk button.
  - **Tune:** choose an output, then Strike and measure. Shows the note, Hz and cents, *pitch class only*. Also Find piezo pin (Listen 3 s, bars per pin, "use GPIO n", which posts `/pins`).
  - **Status:** boards table with Rescan, counters, the log.
  - **Settings:**
    - Coils & supply: cap, supply mA, port A/B, solo.
    - MIDI: channel, gamma, fold, latency, triggers.
    - Pins (restart): MIDI 14/44, button, tester plus invert plus pulse, piezo.
    - Hall WiFi (ClarkShop preset button, Scan, Join, Forget), own WiFi password, MQTT.
    - Firmware: Install latest from LATEST_URL. It requires `"box": true` in the manifest, compares the version, and warns about plain chime.bin. Also Upload file.
- **Behaviour:** poll `/status` every 1 s (0.4 s while cal, tune, seq or demo is active). Fetch `/outs` and `/seq` after edits, and from the poll: retried until they arrive, and again whenever `rev` says they changed (an edit on the BOX screen too). A `/seqcell` reply only updates its own cell, and only when no newer tap on that cell is on its way. The Outputs panel shows "firing" for a coil that is on or struck since the last poll. Blank number fields are not sent; Save output refuses blanks.
- **Rules:** no secrets in the page. `HOME_SSID` and `LATEST_URL` are concatenated in from chimes.h.

### cal.cpp: calibration task (core 0, priority 1)
- **Timing:** waits use `vTaskDelay`, checking `calAbort` every 5 ms. The busy ADC loops yield every 10 ms.
- **Sweep(out):** strikes from 3 ms in 1 ms steps up to `outCap(out)` with `M_EXACT`. A small solenoid (K_SOL, or any output whose min is under 3 ms) starts at `MIN_PULSE_US` (1 ms), in 0.5 ms steps when that fits `SWEEP_MAX`.
  - After each one, wait `latUs + 25 ms`, then read the piezo peak-to-peak for 150 ms if a piezo is set, then wait 550 ms.
  - Record `sweepSteps`. It needs to be unmuted.
- **Tune(out):** needs a piezo pin and to be unmuted.
  - Strike at velocity 100, wait `latUs + 40 ms`, then take 4096 samples at about 16 kHz, measuring the real rate.
  - Remove DC. If peak-to-peak is under 40, say "no signal".
  - Use a YIN difference function over lags 8..200, with a 0.2 threshold and parabolic interpolation.
  - Convert to a MIDI number. Compare the **pitch class only** against the output's note, and report cents within ±600.
- **PzScan:** for 3 s, read every ADC pin not used by another role, and report each pin's peak-to-peak.
- **Walk:** every usable output in order, at velocity 100, 450 ms apart.

### ui.cpp: the BOX screen (bench and front panel)
- **Display:** portrait 240×320 (`setRotation(0)`, the owner's choice). LovyanGFX `LGFX_Box`, copied from the backup.
  - Touch is a TT21100 on I2C port 1, pins 8/18, so it never touches `Wire`.
  - Colours are dark oak and brass, as before.
- **Layout:** a top bar, 22 px, shows "Hall chimes" and the IP or WiFi state. Five tabs sit at the bottom, 44 px high: **STAT | OUTS | PROG | SETUP | PANIC**. PANIC is red, and touching it panics with mute at once, then opens the page.
- **STAT:** a big state (READY / MUTED / FAULT 0x.. / NO DRIVERS), drivers found, coils on / max with mA, queue, played, last output, drop / hot / clamp counters, MIDI pin and channel, demo / seq / cal activity, and the last 6 log lines.
- **OUTS, the status panel of every solenoid or attachment.** One row per output that is on, or on a present board, 10 rows per page, with ▲/▼ page buttons.
  - Each row shows the output number, name, kind letter (T/S/O), and board/pin.
  - State colour: off grey, idle oak, firing brass flash, hot red, board fault red outline.
  - Also a heat bar and the strike count.
  - Tapping a row shows that output's detail on the panel: note, min/full/cap, mA, strikes, heat, and the board's state. It does not strike.
- **PROG, programming chimes or beats.** A top control row: slot ◀ n ▶, PLAY/STOP, BPM −/value/+.
  - A grid of 12 rows × 16 visible steps, with ◀▶ switching steps 1-16 and 17-32 when steps > 16.
  - Each row label shows its output name. Tapping a label cycles the row output through the usable outputs, then none.
  - Tapping a cell cycles 0, 60, 100, 127. The playing step column is highlighted.
  - A bottom row: SAVE (marked * when unsaved, from the shared `patDirty`; NOT SAVED for 2 s if the save failed), CLEAR, steps −/+, LOOP. SAVE is safe while a pattern plays (Safety 9).
  - The grid only redraws cells that changed.
- **SETUP:** the IP (large), hall WiFi, hostname, own WiFi name and password and whether it is on, firmware, pins, port, cap and supply.
- **PANIC:** "ALL OFF", the muted state, and a big Unmute button when muted. The sources are listed, built from the live settings (panel button pin or off; B6 only with triggers on). The tab panics on any touch sample (Safety 6).
- **Rules:** the screen only queues messages, calls `requestPanic()`, or edits `pats[]` and saves through config.cpp. Redraw only what changed, and keep the loop at about 25 Hz.

## Build

- **Toolchain:** arduino-cli, esp32 core 3.x. CI pins 3.2.0, and this PC has 3.3.10, so use only APIs present in both.
- **FQBN:** `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`
- **Libraries:** LovyanGFX and PubSubClient.
- **Rules:** no warnings in this code with `--warnings default`. Nothing secret in the source.
