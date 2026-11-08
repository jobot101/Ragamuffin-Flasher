# Round 2: Verna's voice, power sense, wear counters, MIDI record, world-rhythm sequencer, foot pedals

This round adds to `DESIGN.md`, and every safety rule there still holds. The owner asked for all of it on 2026-11-08.

## 0. Files I (the lead) add to the sketch folder

- **`audio.cpp`** plays Verna's prompts on the BOX speaker:
  - Hardware: ES8311 codec at 0x18 on the internal I2C bus, I2S MCLK 2 / BCLK 17 / LRCK 47 / DOUT 15, speaker amp enable on GPIO 46.
  - `audioInitCodec()` must be called from `uiTask` right after `lcd.init()`, because the codec shares LovyanGFX's I2C port with touch.
  - `audioTask` runs on core 0.
  - `vernaSay(line)` works from any core and never blocks. It rate-limits: the same line plays at most every 20 s (12 s for no-power), and any line at most every 3 s.
- **`verna_lines.cpp`** is generated: `VERNA[V_COUNT]`, 32 clips in 8-bit mu-law.
- **`presets.cpp`** is generated: `PRESETS[]` and `NUM_PRESETS`, 29 world rhythms from the Tribe of Abraham drum sequencer (MIT). The credit goes on the page.

## 1. chimes.h additions (the contract)

```cpp
// ---- Verna's prompts (audio.cpp, verna_lines.cpp) ----
enum VernaLine : uint8_t { V_READY, V_NO_POWER, V_POWER_BACK, V_MUTED, V_UNMUTED, V_PANIC, V_NO_DRIVER, V_TOO_HOT,
  V_NOT_SET_UP, V_UPDATING, V_TUNE_ON, V_TUNE_TOUCH_SHARP, V_TUNE_TOUCH_FLAT, V_TUNE_SHARP, V_TUNE_FLAT,
  V_TUNE_NO_SIGNAL, V_CAL_MIN, V_CAL_FULL, V_CAL_DONE, V_PATTERN_SAVED, V_BOARD_BACK_1, V_BOARD_BACK_2,
  V_BOARD_BACK_3, V_BOARD_BACK_4, V_BOARD_BACK_5, V_BOARD_BACK_6, V_BOARD_BACK_7, V_BOARD_BACK_8, V_WEAR_CHECK,
  V_REC_START, V_REC_SAVED, V_REC_STOP, V_COUNT };
struct VernaClip { const char* key; const char* text; const uint8_t* pcm; uint32_t n; };  // pcm = mu-law
extern const VernaClip VERNA[V_COUNT];
void vernaSay(uint8_t line);          // any core, never blocks, rate-limited
void audioInitCodec();                // uiTask only, after lcd.init()
void audioTask(void*);                // core 0, created in setup()
extern volatile int vernaPlaying;     // line playing now, -1 quiet
extern volatile uint32_t vernaCount;

// ---- power sense ----
extern volatile int pwrState;         // -1 = no sense pin set, 0 = no 24 V, 1 = 24 V present (debounced 100 ms)
extern volatile uint32_t nNoPower;    // strikes refused because the supply was off

// ---- wear ----
extern uint32_t wearCount[MAX_OUT];   // lifetime strikes, NVS blob "wear", saved by core 0 (see 4)
extern volatile bool wearDirty;
// OutCfg gains: uint16_t wearK;   // check-this-one threshold in thousands of strikes, 0 = off
//               uint8_t  role;    // DrumRole, for loading presets

// ---- sequencer, Tribe-style ----
enum DrumRole : uint8_t { R_NONE, R_KICK, R_SNARE, R_HIHAT, R_OPEN, R_RIM, R_CLAP, R_TOMLO, R_TOMHI, R_COWBELL, R_SHAKER, R_COUNT };
extern const char* const ROLE_NAMES[R_COUNT];       // "", "Kick", "Snare", "Hi-Hat", "Open HH", "Rim", "Clap", "Lo Tom", "Hi Tom", "Cowbell", "Shaker"
struct TimeSig { const char* label; uint8_t beats, sub, steps; };   // sub = steps per beat group
extern const TimeSig TIME_SIGS[7];   // 4/4 (4,4,16) 3/4 (3,4,12) 6/8 (2,6,12) 5/4 (5,4,20) 7/8 (7,2,14) 9/8 (3,6,18) 12/8 (4,6,24)
// Pattern gains: uint8_t ts; (index into TIME_SIGS; steps still a separate field, defaulting to TIME_SIGS[ts].steps)
//                uint8_t rowRole[SEQ_ROWS]; (the instrument a row was made for, a label only)
// swing range becomes 0..75 %
struct DrumPreset { const char* name; const char* desc; uint8_t ts, bpm, swing, rows, steps; uint8_t role[5]; const uint8_t* lv; };
extern const DrumPreset PRESETS[];  extern const int NUM_PRESETS;   // lv = rows*steps levels 0..3
void loadPreset(int slot, int preset);   // core 0: fills pats[slot]; rows get the first usable output with that role
extern volatile int curSlot;             // the slot the page/screen/pedals are working on (0..SEQ_SLOTS-1)

// ---- MIDI record ----
extern volatile int recSlot;             // -1 = off; else armed/recording into this slot
extern volatile int recState;            // 0 off, 1 armed (waits for the first note), 2 recording, 3 done (core 0 saves)
// M_REC message: a = slot (0..3) to arm, 255 = stop

// ---- foot pedals ----
constexpr int NUM_PEDALS = 4;
enum PedalAct : uint8_t { P_NONE, P_PLAYSTOP, P_NEXT, P_PREV, P_TAP, P_REC, P_STRIKE, P_PANIC, P_COUNT };
// Cfg gains: int pedalPin[NUM_PEDALS] (-1 off, header pins, boot-only), uint8_t pedalAct[NUM_PEDALS], uint8_t pedalOut[NUM_PEDALS]
//            (output index for P_STRIKE), int pedalInv[NUM_PEDALS] (0 = auto: released level sampled at boot),
//            int cc64Act (what a MIDI sustain pedal does, default P_NONE), int voice (1), int voiceVol (70), int pwrPin (-1)
extern volatile uint8_t pedalDown[NUM_PEDALS];
```

Every new NVS key is listed in section 4.

## 2. Behaviour

### Prompt triggers
Every trigger goes through `vernaSay()`. Core 1 may call it, because it only does a non-blocking queue send.

| Prompt | When |
|---|---|
| `V_READY` | Once, about 3 s after boot, when the codec is up. |
| `V_NO_POWER` | `requestOut` is refused because `pwrState == 0`. Refuse it, count it, and log "plug in the power supply". |
| `V_POWER_BACK` | `pwrState` goes from 0 to 1 after boot. |
| `V_MUTED` | A strike is attempted from the page, screen, pedal or serial while muted. MIDI too, at most once a minute. |
| `V_UNMUTED` | On unmute. |
| `V_PANIC` | A panic that mutes. Not MIDI CC120/123. |
| `V_NO_DRIVER` | Refused because the board isn't OK. |
| `V_TOO_HOT` | A heat refusal. |
| `V_NOT_SET_UP` | A strike to a K_OFF output from the page, screen or pedal. Not MIDI, which stays silent. |
| `V_UPDATING` | OTA start. Then wait about 1.5 s before muting the codec, since updating stops nothing in audio. |
| Tune result | Within ±8 cents: `V_TUNE_ON`. From 8 to 25 cents: touch sharp or flat. Over 25: sharp or flat. No signal: `V_TUNE_NO_SIGNAL`. |
| Sweep coaching | Sweep start: `V_CAL_MIN`. The first `/cal` that sets min for that output while a sweep result is showing: `V_CAL_FULL`. Sweep end: `V_CAL_DONE`. |
| `V_PATTERN_SAVED` | Every `savePattern` from the page, screen or pedal. Not from record, which says `V_REC_SAVED`. |
| `V_BOARD_BACK_n` | A board that was B_FAULT, or was seen and then went missing, comes back OK. n = board index + 1. |
| `V_WEAR_CHECK` | When an output's `wearCount` crosses `wearK*1000`. Then once per boot, the first time that output fires. |
| Recording | Arm: `V_REC_START`. Saved: `V_REC_SAVED`. Stopped empty: `V_REC_STOP`. |

### Power sense (core 1)
- With `cfg.pwrPin >= 0`, the pin is INPUT, wired to 24 V through a 100 k / 15 k divider, so it sits at about 3.1 V when the supply is on.
- Read it every pass and debounce for 100 ms. HIGH means power.
- While `pwrState == 0`, `requestOut` refuses everything, so nothing queues up waiting.
- Show it in `/status` as `"power":{"pin","ok","asks"}`, as a banner on the page and screen, and as "NO 24 V" in the screen's STAT state.

### Wear
- `fire()` increments `wearCount[i]` and sets `wearDirty`.
- Core 0 (`net.cpp`) saves blob "wear" when it's dirty and 10 minutes have passed since the last save. It also saves on panic-mute, before OTA, and on `/wearsave`. Every save calls `waitQuiet()`.
- `/status` `live.wear` holds 64 counts. An output is flagged "check" when `wearK && wearCount >= wearK*1000`.
- `/wearreset?o=N` resets one output after service, then saves.
- Defaults: tubes 250 (k strikes), small solenoids 100.

### Sequencer (Tribe of Abraham style)
- Time signatures come from `TIME_SIGS`. Beat-group rules in the grid follow `sub`, for example 7/8 groups in 2s and 12/8 in 6s.
- Choosing a time signature sets `steps = TIME_SIGS[ts].steps`. Steps can still be changed afterwards.
- Swing runs from 0 to 75 %.
- Cell levels: off, ghost (60), medium (100), accent (127). Clicking cycles through them, and right-clicking clears (on the page).
- On the page, Space plays and stops.
- `loadPreset` copies name, ts, bpm, swing and steps.
  - Each preset row is matched by role: the first output with `outs[].role == role` that isn't K_OFF.
  - If none matches, the row is left unassigned (`SEQ_NONE`) with its `rowRole` label kept, so the owner sees "Kick: not assigned".
  - Levels map 1→60, 2→100, 3→127.
  - Loading doesn't save. `V_PATTERN_SAVED` comes when they press Save.
- The preset list, with each description and the credit line, is on the page's Program tab. On the screen, PROG has a PRESET button that cycles the list and shows the name.

### MIDI record (core 1)
- The `M_REC` message arms slot `a`, from the page, screen, pedal or serial `rec <1-4>`.
- **When the slot is playing (overdub):**
  - Each note-on is quantised to the nearest step of the running pattern. For a note just before a step, round to that step.
  - The note's row is the row whose output answers that note, or else the first empty row, which takes the first usable output answering the note.
  - The cell's velocity is set from the note velocity, snapped to 60, 100 or 127.
  - Recording stops on the second press or at stop. The state becomes 3 (done) and core 0 saves.
- **When the slot isn't playing:** armed waits for the first note, which starts a free-running clock at the slot's bpm, ts and steps with step 0 = that note. It records for one pattern length, then stops, saves and plays it back once.
- Notes still strike normally during recording, so you hear what you play.
- Core 1 writes only single bytes of `pats[]`. Core 0 sees `recState == 3`, calls `savePattern` and says `V_REC_SAVED`, or `V_REC_STOP` if nothing was recorded.

### Foot pedals (core 1)
- There are up to 4 pedals on header pins. The allowed pins exclude every other role; prefer 11, 12, 13, 21, 38, 39 and 42.
- The released level is sampled at boot, so normally-open and normally-closed pedals both work. `pedalInv` can force a polarity.
- Debounce for 15 ms and act on the press. Actions:
  - `P_PLAYSTOP`: plays or stops `curSlot`.
  - `P_NEXT` / `P_PREV`: changes `curSlot`. If it's playing, switch at the end of the current bar.
  - `P_TAP`: tap tempo. Average the last 3 intervals, between 250 and 2000 ms, then set the bpm of `curSlot`, held to 30..240.
  - `P_REC`: arms or stops recording on `curSlot`.
  - `P_STRIKE`: strikes `pedalOut` at velocity 110. A kick-drum pedal on a beater solenoid uses this.
  - `P_PANIC`: the same rule as the panel button.
- A MIDI CC64 pedal (≥ 64 = press) runs `cfg.cc64Act`. The default is none, so CC64 keeps today's no-effect hook.
- Pedal state goes in `/status` as `"pedals":[{"pin","act","out","down"}]`.

## 3. Endpoints (net.cpp)

| Endpoint | What it does |
|---|---|
| `/say?l=<key>` | Plays a prompt by key, for testing. |
| `/voice?on=&vol=` | Turns Verna on or off and sets the volume. |
| `/wearreset?o=N` | Resets one output's wear count. |
| `/wearsave` | Saves the wear counts now. |
| `/presets` | Returns `[{"name","desc","ts","bpm","swing","rows","steps","roles":[..]}]`. |
| `/preset?s=&p=` | Loads preset p into slot s and returns `/seq`. |
| `/seqmeta` | Gains `ts=`. |
| `/seqrow` | Gains `role=`. |
| `/rec?s=` or `/rec?stop=1` | Arms or stops recording. |
| `/pedal?n=&pin=&act=&out=&inv=` | A pin change saves and restarts, like `/pins`. An action or output change is live. |
| `/set` | Gains `cc64=`, `voice=`, `vol=`, `cur=`. |
| `/pins` | Gains `pwr=`. |
| `/outs` | Gains `"w":wearK` and `"r":role`. |
| `/out` | Gains `wear=` (k strikes) and `role=`. |

`/status` gains `power`, `pedals`, `rec:{slot,state}`, `cur`, `verna:{on,vol,playing,count}`, `live.wear` and `live.check`.

## 4. NVS
- **New keys:** `pwrp`, `ped0..ped3`, `pda0..pda3`, `pdo0..pdo3`, `pdi0..pdi3`, `cc64a`, `voice`, `vvol`, `wear` (blob), `cur`.
- **Changed sizes:** the "outs" blob grows (adds `wearK` and `role`), and so do the "pat0..3" blobs (`ts` and `rowRole`).
  - Migrate rather than reset: if the old size matches the previous struct, read it into the old layout and copy the fields.
  - The first deploy of round 1 has only just happened, so the owner may have saved outputs or patterns. Keep them.

## 5. Screen (ui.cpp, presentation per BRAND-UI.md)
- Call `audioInitCodec()` once after `lcd.init()`.
- **Banners:** "NO 24 V: plug in the power supply" across the top of every tab while `pwrState == 0`. A small speaker lamp in the bar while Verna talks.
- **OUTS:** a "CHECK" tag on outputs over their wear threshold, and the lifetime count in the detail view.
- **PROG:**
  - time signature, cycling with beat-group rules
  - PRESET, which cycles and loads into the slot
  - REC (red while armed or recording)
  - Recording state on the slot line
- **SETUP:** the pins for power sense and pedals.
- **STAT:** pedal lamps, and "NO 24 V" in the state.

## 6. Page (page.cpp, presentation per BRAND-UI.md)
- **Program:** the preset list with descriptions and the credit "Presets from the Tribe of Abraham drum sequencer (MIT)", the time signature select, swing to 75, right-click clear, Space play/stop, REC, and the row role label.
- **Outputs:** the wear count, the check flag, Reset wear, the wear threshold, and the role select.
- **Settings:** Verna (on, volume, test buttons for every prompt), power sense pin with the wiring note (100 k / 15 k divider), foot pedals (pin, action, output, polarity), and the MIDI sustain pedal action.
- **Banners:** "No 24 V: plug in the power supply" on every tab when `power.ok == 0`.
