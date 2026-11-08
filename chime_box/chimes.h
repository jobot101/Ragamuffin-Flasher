// chimes.h: the shared contract between the Hall chimes BOX firmware modules. DESIGN.md (same folder) is the spec.
// Modules: chime_box.ino (setup/loop only), core.cpp (core 1: every coil write), config.cpp (NVS), net.cpp
// (WiFi/web/MQTT/serial), page.cpp (the phone page), cal.cpp (sweep/tune/walk task), ui.cpp (the BOX screen).
// Rule: only core.cpp, running on core 1, ever writes a driver register or the tester GPIO.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#if __has_include("version.h")
#include "version.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION "box-dev"
#endif

// ---------------- network ----------------
#define LATEST_URL "https://jobot101.github.io/Ragamuffin-Flasher/chimebox/"   // BOX builds only (chimebox.yml)
#define AP_SSID    "HallChimes"     // own WiFi, fallback only; password in NVS (default: defaultApPass())
#define HOME_SSID  "ClarkShop"      // preset name on the page; never a password in source
extern const char* DEVICE_NAME;     // "hall-chimes" -> hall-chimes.local, MQTT client id
extern const char* TOPIC_BASE;      // "ragamuffin/hall/chimes"

// ---------------- pins (ESP32-S3-BOX dock) ----------------
constexpr int I2C_SDA = 41, I2C_SCL = 40;   // STEMMA QT to the driver chain (Wire, I2C port 0)
constexpr int BOOT_BTN = 0;
// BOX internals, never used for anything else: LCD 4/5/6/7/45/48, internal I2C 8/18 (touch), touch INT 3,
// audio 2/15/16/17/46/47, USB 19/20, UART0 TX 43.
extern const int HDR_PINS[];  extern const int N_HDR;   // {9,10,11,12,13,14,21,38,39,42,44}: pins a setting may use
extern const int ADC_PINS[];  constexpr int N_ADC = 6;  // {9,10,11,12,13,14}: header pins with an ADC (piezo)

// ---------------- outputs (solenoids and other attachments) ----------------
constexpr uint8_t MAX_BOARDS = 8;     // Adafruit MCP23017 solenoid drivers at 0x20..0x27
constexpr uint8_t BOARD_BASE = 0x20;
constexpr uint8_t MAX_OUT    = 64;    // output index i = board * 8 + pin;  "output number" shown to people = i + 1
constexpr uint8_t NUM_TUBES  = 25, LOW_NOTE = 48;   // default map: outputs 1..25 = tubes C3..C5 (MIDI 48..72)
enum OutKind : uint8_t { K_OFF = 0, K_TUBE = 1, K_SOL = 2, K_OTHER = 3 };   // K_SOL = small solenoid / beater
struct OutCfg {               // stored as one NVS blob "outs"; size change -> defaults
  char     name[12];          // "T1".."T25", or what the owner types ("kick", "wood")
  uint8_t  kind;              // OutKind; K_OFF never fires
  uint8_t  note;              // MIDI note that strikes it, 0 = none (several outputs may share a note)
  uint16_t minUs, fullUs;     // pulse at velocity 1 / 127 (calibration)
  uint16_t capUs;             // its own cap; the effective cap is outCap(i)
  uint16_t mA;                // coil current, for the supply budget (JF-Z05 tube: 400; small solenoids less)
  uint16_t wearK;             // round 2: "check this one" after wearK * 1000 lifetime strikes, 0 = off
  uint8_t  role;              // round 2: DrumRole, so a drum preset can find its instrument
  uint8_t  spare;
};
extern OutCfg outs[MAX_OUT];

// ---------------- safety limits (no setting can go past these) ----------------
constexpr uint32_t HARD_CAP_US  = 30000;   // owner's rule: short fast taps only, never a long push, no hold
constexpr uint32_t MIN_PULSE_US = 1000;
constexpr uint32_t REARM_US     = 25000;   // min gap between strikes on one output
constexpr uint32_t STALE_US     = 60000;   // a queued strike this late is dropped AND logged
constexpr uint32_t WATCHDOG_US  = 100000;  // a core-1 pass this late after the last one -> everything off first
constexpr float    HEAT_CAP_MS  = 3000.0f; // heat guard: leaky bucket of on-time per output...
constexpr float    HEAT_LEAK    = 0.25f;   // ...leaking 0.25 ms per ms
constexpr uint8_t  QSIZE        = 64;      // strike queue
constexpr uint8_t  MAX_ON_COUNT = 16;      // never more coils on at once than this, whatever the budget
constexpr int      SOL_MS_MIN = 3, SOL_MS_MAX = 30;   // one-MOSFET tester on a GPIO
constexpr uint32_t SOL_REARM_US = 150000;
constexpr uint16_t MIN_COIL_MA  = 20;      // a live output (kind != K_OFF) always counts at least this against the supply
constexpr uint32_t HOLD_WAIT_MS = 150;     // flash-write hold: longest wait for core 1 to go quiet before a save is aborted

// ---------------- settings (NVS namespace "chime"; live unless noted) ----------------
struct Cfg {
  int   capUs    = 30000;  // global per-strike cap (3000..HARD_CAP_US)
  int   supplyMa = 1500;   // 24 V supply current budget (RID-50A label still to be read)
  int   portB    = 0;      // driver outputs on MCP port A (0) or B (1), all boards; change only via M_PORT
  int   midiCh   = 0;      // 0 = omni, 1..16
  float gamma    = 1.6f;   // pulse = min + (full - min) * (vel/127)^gamma
  int   fold     = 0;      // a note no output answers is folded by octaves into 48..72
  int   latUs    = 12000;  // fixed scheduling latency (0..30000)
  int   solo     = 0;      // 0 = all outputs; n = only output n (1-based) may fire (bench bring-up)
  int   trig     = 0;      // contact triggers on 0x20 port B; change only via M_TRIG (core 1 owns it)
  int   solMs    = 12;     // tester pulse ms
  // round 2, live:
  int     voice = 1, voiceVol = 70;                // Verna's prompts on the BOX speaker, volume 0..100
  int     cc64Act = 0;                             // PedalAct for a MIDI sustain pedal (CC64 >= 64 = press), 0 = none
  uint8_t pedalAct[4] = {0, 0, 0, 0};              // PedalAct per foot pedal
  uint8_t pedalOut[4] = {0, 0, 0, 0};              // output index a P_STRIKE pedal strikes
  // applied at boot only (saving restarts the board):
  int   midiPin = 14, btnPin = 10, solPin = 9, solInv = 0, pzPin = -1;
  int   pwrPin = -1;                               // round 2: 24 V sense (100k/15k divider), HIGH = power, -1 = none
  int   pedalPin[4] = {-1, -1, -1, -1};            // round 2: foot pedal inputs (header pins), -1 = none
  int   pedalInv[4] = {0, 0, 0, 0};                // 0 = auto (released level sampled at boot), 1 = pressed HIGH, 2 = LOW
};
extern Cfg cfg;
extern String staSsid, staPass, apPass, mqttHost, mqttUser, mqttPass;
extern uint16_t mqttPort;

// ---------------- core 0 -> core 1 ----------------
enum : uint8_t {
  M_NOTE,    // a = MIDI note, v = velocity
  M_OUT,     // a = output index (0-based), v = velocity
  M_EXACT,   // a = output index, us = exact pulse (still clamped in fire())
  M_TAP,     // tester tap
  M_RESCAN,  // all off, re-init every driver
  M_PORT,    // a = 0/1: all off, switch output port, re-init
  M_TRIG,    // a = 0/1: triggers off/on, re-init
  M_DEMO,    // a = song 1..NUM_SONGS, 0 = stop
  M_SEQ,     // a = slot 0..SEQ_SLOTS-1 to play, 255 = stop
  M_REC,     // round 2: a = slot 0..SEQ_SLOTS-1 to arm MIDI record, 255 = stop recording
  M_PEDAL,   // round 2: a = PedalAct to run now (the page/screen/serial can fire a pedal action), v = pedal 0..3
};
struct Msg { uint8_t kind, a, v; uint32_t us, t0; };
struct NoteMsg { uint8_t note, vel; };          // played notes -> MQTT
extern QueueHandle_t netToChimes, chimesToNet;
void sendMsg(uint8_t kind, uint8_t a, uint8_t v, uint32_t us);   // core 0 only; never blocks
void requestPanic(bool mute, const char* src);                   // core 0 only; core 1 acts within one pass
bool requestUnmute(const char* src);                             // core 0 only; clears muted (refused while updating or
                                                                 // a coil may be stuck), logs it, never fires anything;
                                                                 // false if refused

// ---------------- flash-write hold (core 0 takes it, core 1 honours it) ----------------
// An NVS or OTA flash write stalls core 1 (cache off), and a coil that is on then stays on. So before any flash write
// core 0 takes a hold: core 1 starts no strike and no tester tap while it is in force (queued strikes wait, and are
// stale-dropped and logged as usual), and acks once nothing is on and the zeros are written. holdCoils() waits for
// that ack; on timeout it releases the hold and returns false, and the caller must NOT write.
// holdWord = generation << 8 | number of holders (core 0 writes it under a lock; core 1 reads it once per pass).
extern volatile uint32_t holdWord, holdAck;     // holdAck: generation of the last quiet held pass (core 1)
bool holdCoils(uint32_t timeoutMs);             // core 0 only
void releaseCoils();                            // core 0 only, once per successful holdCoils()

// ---------------- live state (written by core 1, read anywhere) ----------------
enum : uint8_t { B_MISSING = 0, B_OK = 1, B_FAULT = 2 };
extern volatile uint8_t  bState[MAX_BOARDS];
extern volatile uint32_t bErr[MAX_BOARDS];
extern volatile uint8_t  outOn[MAX_OUT];        // 1 while that coil is on
extern volatile float    heat[MAX_OUT];         // bucket, ms
extern volatile uint32_t strikes[MAX_OUT], refusedAt[MAX_OUT];   // refusedAt: millis()|1 of last heat refusal
extern volatile uint8_t  qn, qPeak, coilsOn;
extern volatile int      maOn;                  // sum of mA of coils on now
extern volatile uint32_t nPlayed, nDropped, nHeat, nFault, nClamped, nPanics, nWatchdog, nMidi;
extern volatile uint16_t lastPlayed;            // output index << 8 | vel
extern volatile uint32_t lastPlayedMs;
extern volatile bool     muted, updating, sustain;
extern volatile uint8_t  panicReq;              // bit 0 panic, bit 1 also mute (set by requestPanic)
extern const char* volatile panicSrc;
extern volatile uint32_t solTaps;
extern volatile uint8_t  stuckMask;             // bit b: board b's latch couldn't be zeroed, a coil may be held on.
                                                // Core 1 keeps retrying the zeros and keeps the chimes muted meanwhile.

// ---------------- songs and the sequencer ----------------
struct DN { uint8_t note, len; };               // MIDI note (0 = rest), length in eighth notes
struct Song { const char* name; const DN* n; uint16_t count; uint16_t eighthMs; };
extern const Song SONGS[];  extern const int NUM_SONGS;   // Westminster Quarters, Amazing Grace (F), Ode to Joy
extern volatile int demoSong;                   // 0 = none, else 1..NUM_SONGS

constexpr int SEQ_SLOTS = 4, SEQ_ROWS = 12, SEQ_STEPS = 32;
constexpr uint8_t SEQ_NONE = 255;
struct Pattern {              // NVS blob "pat0".."pat3"; size change -> defaults
  char    name[12];
  uint8_t steps;              // 1..SEQ_STEPS, each step a 16th note
  uint8_t bpm;                // 30..240
  uint8_t swing;              // 0..75 %: delays every odd step by swing% of a step
  uint8_t loop;               // 1 = loop until stopped, 0 = play once
  uint8_t row[SEQ_ROWS];      // output index per row, SEQ_NONE = empty row
  uint8_t vel[SEQ_ROWS][SEQ_STEPS];   // 0 = rest, else velocity 1..127
  uint8_t ts;                 // round 2: time signature, index into TIME_SIGS (beat-group rules; steps default)
  uint8_t rowRole[SEQ_ROWS];  // round 2: the DrumRole a row was made for (a label; R_NONE = plain row)
};
extern Pattern pats[SEQ_SLOTS];
extern volatile int seqSlot;                    // playing slot, -1 = stopped
extern volatile int seqStep;                    // step playing now (for the cursor)
// Edit tracking (core 0): the page re-reads /outs or /seq when a revision moves, and both the page and the screen
// show "unsaved" from patDirty. Every edit of outs[] calls outsEdited(), every pattern edit patEdited(slot).
extern volatile uint32_t outsRev, patRev[SEQ_SLOTS];
extern volatile bool     patDirty[SEQ_SLOTS];   // edited since its last successful save
void outsEdited();
void patEdited(int slot);

// ---------------- calibration / tuning task (cal.cpp, core 0) ----------------
enum : int { CAL_IDLE = 0, CAL_SWEEP = 1, CAL_TUNE = 2, CAL_PZSCAN = 3, CAL_WALK = 4 };
struct Step { uint16_t us, peak; };
constexpr int SWEEP_MAX = 32;
extern volatile int  calCmd, calOut, calState;  // calCmd/calOut set by net/ui; calState by the task
extern volatile bool calAbort;                  // panic/mute set it; the task stops within ~10 ms
extern Step sweepSteps[SWEEP_MAX];
extern volatile int  sweepN, sweepOut;
extern volatile uint32_t sweepCur;
extern char calMsg[64];
extern volatile int  tuneOut, tuneCents, tuneOk;
extern volatile float tuneHz;
extern char tuneNote[8];
extern volatile uint16_t pzPeak[N_ADC];
extern volatile int pzDone;
bool calBusy();                                 // calState || calCmd
void calTask(void*);

// ---------------- event log (both cores; core 0 copies new lines to USB serial) ----------------
constexpr int LOG_N = 16;
void evlog(const char* fmt, ...);
String logLine(uint32_t i);                     // i = 0..logCount-1 (only the last LOG_N are kept)
extern volatile uint32_t logCount;

// ---------------- helpers (core.cpp) ----------------
String   noteName(int midi);                    // 60 -> "C4"
uint32_t outCap(int i);                         // min(outs[i].capUs, cfg.capUs, HARD_CAP_US)
bool     outUsable(int i);                      // kind != K_OFF && its board is B_OK && solo allows it
int      maxOnNow();                            // coils allowed on at once for the budget (display)
String   outLabel(int i);                       // name, or "out N" when the name is empty

// ---------------- config.cpp (NVS; every write goes through these, under one mutex and a flash-write hold) -------
// The saves return false when nothing was written (NVS open failed, or core 1 didn't go quiet in time).
void   loadCfg();                               // read only: settings, outputs, patterns, WiFi/MQTT strings; pins
void   nvsTidy();                               // core 0, once after boot: removes stale keys (a write, so held)
bool   saveCfg();                               // the live settings in Cfg (not pins)
bool   savePins(int midi, int btn, int sol, int inv, int pz);   // validated by the caller
bool   saveOuts();
bool   savePattern(int slot);                   // clears patDirty[slot] on success
void   saveWifi(const String& ssid, const String& pass);       // also (re)starts the join
bool   saveMqtt();
void   saveApPass(const String& p);             // "" = back to the default
void   outDefaults(int i);
void   patternDefaults(int slot);
String defaultApPass();                         // "chimes" + last 4 hex digits of the MAC
bool   hdrPin(int p);
bool   adcPin(int p);

// ================= ROUND 2 (spec: ROUND2.md) =================
// ---- Verna's spoken prompts on the BOX speaker (audio.cpp; data in verna_lines.cpp, generated) ----
enum VernaLine : uint8_t { V_READY, V_NO_POWER, V_POWER_BACK, V_MUTED, V_UNMUTED, V_PANIC, V_NO_DRIVER, V_TOO_HOT,
  V_NOT_SET_UP, V_UPDATING, V_TUNE_ON, V_TUNE_TOUCH_SHARP, V_TUNE_TOUCH_FLAT, V_TUNE_SHARP, V_TUNE_FLAT,
  V_TUNE_NO_SIGNAL, V_CAL_MIN, V_CAL_FULL, V_CAL_DONE, V_PATTERN_SAVED, V_BOARD_BACK_1, V_BOARD_BACK_2,
  V_BOARD_BACK_3, V_BOARD_BACK_4, V_BOARD_BACK_5, V_BOARD_BACK_6, V_BOARD_BACK_7, V_BOARD_BACK_8, V_WEAR_CHECK,
  V_REC_START, V_REC_SAVED, V_REC_STOP, V_COUNT };
struct VernaClip { const char* key; const char* text; const uint8_t* pcm; uint32_t n; };   // pcm = 8-bit mu-law, 16 kHz
extern const VernaClip VERNA[V_COUNT];
void vernaSay(uint8_t line);                    // any core, never blocks; rate-limited (same line 20 s, any 3 s)
void audioInitCodec();                          // uiTask only, once, right after lcd.init() (shares lgfx's I2C port)
void audioTask(void*);                          // core 0, created in setup()
extern volatile int vernaPlaying;               // line playing now, -1 = quiet
extern volatile uint32_t vernaCount;

// ---- 24 V power sense (core 1) ----
extern volatile int pwrState;                   // -1 = no sense pin, 0 = no 24 V, 1 = 24 V present (100 ms debounce)
extern volatile uint32_t nNoPower;              // strikes refused because the supply was off

// ---- wear counters ----
extern volatile uint32_t wearCount[MAX_OUT];    // lifetime strikes per output; fire() adds, core 0 saves blob "wear"
extern volatile bool wearDirty;
bool saveWear();                                // config.cpp; under the flash-write hold
bool wearCheck(int i);                          // outs[i].wearK && wearCount[i] >= wearK * 1000

// ---- sequencer: time signatures, drum roles, world-rhythm presets (presets.cpp, generated; MIT, Tribe of Abraham) ----
enum DrumRole : uint8_t { R_NONE, R_KICK, R_SNARE, R_HIHAT, R_OPEN, R_RIM, R_CLAP, R_TOMLO, R_TOMHI, R_COWBELL, R_SHAKER, R_COUNT };
extern const char* const ROLE_NAMES[R_COUNT];   // "", "Kick", "Snare", "Hi-Hat", "Open HH", "Rim", "Clap", "Lo Tom", "Hi Tom",
                                                // "Cowbell", "Shaker"   (defined in config.cpp)
struct TimeSig { const char* label; uint8_t beats, sub, steps; };   // sub = steps per beat group
constexpr int NUM_TS = 7;
extern const TimeSig TIME_SIGS[NUM_TS];         // 4/4 3/4 6/8 5/4 7/8 9/8 12/8 (config.cpp)
struct DrumPreset { const char* name; const char* desc; uint8_t ts, bpm, swing, rows, steps; uint8_t role[5]; const uint8_t* lv; };
extern const DrumPreset PRESETS[];  extern const int NUM_PRESETS;   // lv = rows*steps levels 0 off 1 ghost 2 medium 3 accent
void loadPreset(int slot, int preset);          // core 0 (config.cpp): fills pats[slot], rows matched by outs[].role
extern volatile int curSlot;                    // the slot the page/screen/pedals work on (0..SEQ_SLOTS-1), NVS "cur"

// ---- MIDI record (core 1) ----
extern volatile int recSlot;                    // -1 = off, else the slot armed/recording
extern volatile int recState;                   // 0 off, 1 armed (waits for the first note), 2 recording, 3 done: core 0 saves
extern volatile int recNotes;                   // notes written in this take

// ---- foot pedals (core 1) ----
constexpr int NUM_PEDALS = 4;
enum PedalAct : uint8_t { P_NONE, P_PLAYSTOP, P_NEXT, P_PREV, P_TAP, P_REC, P_STRIKE, P_PANIC, P_COUNT };
extern const char* const PEDAL_ACT_NAMES[P_COUNT];   // "none","play/stop","next pattern","prev pattern","tap tempo",
                                                     // "record","strike","panic"   (config.cpp)
// The test cycle (one walk through every connected output) is started from the SCREEN only (TEST ALL on the STAT and
// OUTS tabs, ui.cpp, core 0): if (!calBusy()) { calOut = 0; calCmd = CAL_WALK; } else calAbort = true;
// and from the /test page. There is no physical button or pedal action for it (owner's choice).
extern volatile uint8_t pedalDown[NUM_PEDALS];
bool savePinsR2(int midi, int btn, int sol, int inv, int pz, int pwr, const int ped[NUM_PEDALS], const int pedInv[NUM_PEDALS]);
                                                // config.cpp: every boot-only pin in one write (savePins keeps the R2 pins)
extern volatile int walkVel, walkGapMs;         // cal.cpp: the next walk's velocity / gap; back to 100 / 450 after each walk

// ---------------- module entry points ----------------
void driversOffNow();                           // core 1, the very first statement of setup(): OLATA = OLATB = 0 on
                                                // 0x20..0x27 (the MCP23017s keep their latches across an ESP reset)
void coreSetup();                               // core 1, after loadCfg(): everything off, drivers, inputs
void coreLoop();                                // core 1, every loop()
void netTask(void*);                            // core 0: WiFi, web, serial console, log -> serial; starts mqttTask
                                                // (MQTT has its own task, so a broker timeout never delays a panic)
String statusJson();
extern volatile bool apOn;
void uiTask(void*);                             // core 0: the BOX screen
extern const char PAGE[];                       // page.cpp: the phone page (PROGMEM)
extern const char TEST_PAGE[];                  // testpage.cpp: /test, cycle once through every connected solenoid
