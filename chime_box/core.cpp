// core.cpp: the instrument, on core 1. The only code that ever writes a driver register or the tester GPIO.
// Spec: DESIGN.md "Safety" and "Core 1". Short fast taps only: every coil goes on through fire(), clamped to
// outCap() (never past HARD_CAP_US), with its off time set in the same block. No hold, no delay, no Serial, no NVS.
#include "chimes.h"
#include <Wire.h>
#include <HardwareSerial.h>
#include <driver/gpio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

// ---------------- live state (core 1 writes, anyone reads) ----------------
volatile uint8_t  bState[MAX_BOARDS];
volatile uint32_t bErr[MAX_BOARDS];
volatile uint8_t  outOn[MAX_OUT];
volatile float    heat[MAX_OUT];
volatile uint32_t strikes[MAX_OUT], refusedAt[MAX_OUT];
volatile uint8_t  qn = 0, qPeak = 0, coilsOn = 0;
volatile int      maOn = 0;
volatile uint32_t nPlayed = 0, nDropped = 0, nHeat = 0, nFault = 0, nClamped = 0, nPanics = 0, nWatchdog = 0, nMidi = 0;
volatile uint16_t lastPlayed = 0;
volatile uint32_t lastPlayedMs = 0;
volatile bool     muted = false, updating = false, sustain = false;
volatile uint8_t  panicReq = 0;
const char* volatile panicSrc = "";
volatile uint32_t solTaps = 0;
volatile uint8_t  stuckMask = 0;
static volatile uint32_t lastPanicMs = 0;   // millis() of the last panic from any source (button hold rule)
volatile int      demoSong = 0;
volatile int      seqSlot = -1;
volatile int      seqStep = 0;
volatile uint32_t logCount = 0;
// round 2 (ROUND2.md)
volatile int      pwrState = -1;            // -1 no sense pin, 0 no 24 V, 1 24 V present
volatile uint32_t nNoPower = 0;
volatile uint32_t wearCount[MAX_OUT];       // loaded by loadCfg() before coreSetup(); never zeroed here
volatile bool     wearDirty = false;
volatile int      recSlot = -1, recState = 0, recNotes = 0;
volatile uint8_t  pedalDown[NUM_PEDALS];
volatile int      curSlot = 0;              // loadCfg() may set it from NVS "cur"; clamped in coreSetup()

bool wearCheck(int i) {
  if (i < 0 || i >= MAX_OUT) return false;
  uint32_t k = outs[i].wearK;
  return k && wearCount[i] >= k * 1000;
}

// volatile ++ and += are deprecated in C++20 (this core builds gnu++2b), so counters go through this
template <class T> static inline void bump(volatile T& x) { x = x + 1; }

// ---------------- songs (MIDI note, length in eighths; 0 = rest) ----------------
// Westminster Quarters, the hour: changes 2-5 in E, then three E3 strikes. 5/4: three quarters and a half.
static const DN WESTMINSTER[] = {
  {64, 2}, {68, 2}, {66, 2}, {59, 4},     // E4 G#4 F#4 B3
  {64, 2}, {66, 2}, {68, 2}, {64, 4},     // E4 F#4 G#4 E4
  {68, 2}, {64, 2}, {66, 2}, {59, 4},     // G#4 E4 F#4 B3
  {59, 2}, {66, 2}, {68, 2}, {64, 4},     // B3 F#4 G#4 E4
  {0, 4},                                 // breath before the hour
  {52, 8}, {52, 8}, {52, 8},              // E3 x3
};
// Amazing Grace (New Britain) in F, 3/4: pickup C4, high note C5.
static const DN AMAZING_GRACE[] = {
  {60, 2},                                // A-
  {65, 4}, {69, 1}, {65, 1},              // maz-ing
  {69, 4}, {67, 2},                       // grace how
  {65, 4}, {62, 2},                       // sweet the
  {60, 4}, {60, 2},                       // sound that
  {65, 4}, {69, 1}, {65, 1},              // saved a
  {69, 4}, {67, 2},                       // wretch like
  {72, 10}, {69, 2},                      // me... I
  {72, 4}, {69, 1}, {65, 1},              // once was
  {69, 4}, {67, 2},                       // lost but
  {65, 4}, {62, 2},                       // now am
  {60, 4}, {60, 2},                       // found, was
  {65, 4}, {69, 1}, {65, 1},              // blind but
  {69, 4}, {67, 2},                       // now I
  {65, 10},                               // see
};
// Ode to Joy in C, 4/4.
static const DN ODE_TO_JOY[] = {
  {64, 2}, {64, 2}, {65, 2}, {67, 2},  {67, 2}, {65, 2}, {64, 2}, {62, 2},
  {60, 2}, {60, 2}, {62, 2}, {64, 2},  {64, 3}, {62, 1}, {62, 4},
  {64, 2}, {64, 2}, {65, 2}, {67, 2},  {67, 2}, {65, 2}, {64, 2}, {62, 2},
  {60, 2}, {60, 2}, {62, 2}, {64, 2},  {62, 3}, {60, 1}, {60, 4},
  {62, 2}, {62, 2}, {64, 2}, {60, 2},  {62, 2}, {64, 1}, {65, 1}, {64, 2}, {60, 2},
  {62, 2}, {64, 1}, {65, 1}, {64, 2}, {62, 2},  {60, 2}, {62, 2}, {55, 4},
  {64, 2}, {64, 2}, {65, 2}, {67, 2},  {67, 2}, {65, 2}, {64, 2}, {62, 2},
  {60, 2}, {60, 2}, {62, 2}, {64, 2},  {62, 3}, {60, 1}, {60, 4},
};
#define NN(a) (uint16_t)(sizeof(a) / sizeof(a[0]))
const Song SONGS[] = {
  {"Westminster Quarters", WESTMINSTER, NN(WESTMINSTER), 320},
  {"Amazing Grace", AMAZING_GRACE, NN(AMAZING_GRACE), 360},
  {"Ode to Joy", ODE_TO_JOY, NN(ODE_TO_JOY), 260},
};
const int NUM_SONGS = (int)(sizeof(SONGS) / sizeof(SONGS[0]));

// ---------------- event log ring (both cores) ----------------
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
static char     logBuf[LOG_N][64];
static uint32_t logMs[LOG_N];

void evlog(const char* fmt, ...) {
  char tmp[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);   // format outside the critical section
  va_end(ap);
  uint32_t ms = millis();
  portENTER_CRITICAL(&logMux);
  uint32_t n = logCount;
  memcpy(logBuf[n % LOG_N], tmp, sizeof(tmp));
  logMs[n % LOG_N] = ms;
  logCount = n + 1;
  portEXIT_CRITICAL(&logMux);
}

String logLine(uint32_t i) {
  char tmp[64];
  uint32_t ms = 0;
  bool ok = false;
  portENTER_CRITICAL(&logMux);
  uint32_t n = logCount;
  if (i < n && n - i <= (uint32_t)LOG_N) {
    memcpy(tmp, logBuf[i % LOG_N], sizeof(tmp));
    ms = logMs[i % LOG_N];
    ok = true;
  }
  portEXIT_CRITICAL(&logMux);
  if (!ok) return String();
  char out[80];
  snprintf(out, sizeof(out), "%lus %s", (unsigned long)(ms / 1000), tmp);
  return String(out);
}

// once per `ms` at most (core-1 rate limits for log lines; counters still count every event)
static bool every(uint32_t& last, uint32_t ms) {
  uint32_t n = millis();
  if (last && (int32_t)(n - last) < (int32_t)ms) return false;   // signed: last = n|1 can be 1 ms ahead
  last = n | 1;
  return true;
}

// ---------------- core 0 -> core 1 ----------------
static portMUX_TYPE panicMux = portMUX_INITIALIZER_UNLOCKED;

void sendMsg(uint8_t kind, uint8_t a, uint8_t v, uint32_t us) {
  Msg m = {kind, a, v, us, (uint32_t)micros()};
  if (netToChimes && xQueueSend(netToChimes, &m, 0) == pdTRUE) return;
  static uint32_t lastFull = 0;
  static uint32_t lost = 0;
  lost++;
  if (every(lastFull, 1000)) evlog("msg queue full: dropped kind %u (%lu lost)", (unsigned)kind, (unsigned long)lost);
}

// src must be a string literal (kept as a pointer)
void requestPanic(bool mute, const char* src) {
  calAbort = true;
  lastPanicMs = millis();
  if (mute) muted = true;        // refused straight away; core 1 does the all-off within one pass
  portENTER_CRITICAL(&panicMux);
  panicReq = (uint8_t)(panicReq | (mute ? 3 : 1));
  panicSrc = src ? src : "?";
  portEXIT_CRITICAL(&panicMux);
}

// The one core-0 unmute path (page, MQTT, serial, screen). Unmuting never fires anything: core 1 just stops
// refusing strikes. Refused while a firmware update is writing, and while a driver latch may still hold a coil on.
bool requestUnmute(const char* src) {
  if (updating) return false;
  if (stuckMask) {
    evlog("unmute refused (%s): a coil may be stuck on", src ? src : "?");
    return false;
  }
  bool was = muted;
  muted = false;
  if (was) evlog("unmute requested (%s)", src ? src : "?");
  return true;
}

// ---------------- helpers ----------------
String noteName(int midi) {
  static const char* const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (midi <= 0 || midi > 127) return String("--");
  return String(NAMES[midi % 12]) + String(midi / 12 - 1);
}

uint32_t outCap(int i) {
  if (i < 0 || i >= MAX_OUT) return MIN_PULSE_US;
  uint32_t c = HARD_CAP_US;
  if ((uint32_t)outs[i].capUs < c) c = outs[i].capUs;
  int g = cfg.capUs;
  if (g < (int)MIN_PULSE_US) g = (int)MIN_PULSE_US;
  if ((uint32_t)g < c) c = (uint32_t)g;
  if (c < MIN_PULSE_US) c = MIN_PULSE_US;
  return c;                      // never above HARD_CAP_US
}

bool outUsable(int i) {
  if (i < 0 || i >= MAX_OUT) return false;
  if (outs[i].kind == K_OFF) return false;
  if (bState[i / 8] != B_OK) return false;
  return cfg.solo == 0 || cfg.solo == i + 1;
}

// worst case for display: the biggest coil among the usable outputs, at least 1 (the lone-strike exception)
int maxOnNow() {
  int big = 0;
  for (int i = 0; i < MAX_OUT; i++)
    if (outUsable(i) && outs[i].mA > big) big = outs[i].mA;
  int n = big > 0 ? cfg.supplyMa / big : MAX_ON_COUNT;
  if (n > MAX_ON_COUNT) n = MAX_ON_COUNT;
  if (n < 1) n = 1;
  return n;
}

String outLabel(int i) {
  if (i < 0 || i >= MAX_OUT) return String("?");
  char nm[sizeof(outs[0].name) + 1];
  memcpy(nm, outs[i].name, sizeof(outs[0].name));
  nm[sizeof(outs[0].name)] = 0;
  if (nm[0]) return String(nm);
  return String("out ") + String(i + 1);
}

// ======================= core 1 only from here =======================
// MCP23017 registers, IOCON.BANK = 0
static const uint8_t R_IODIRA = 0x00, R_IODIRB = 0x01, R_GPPUA = 0x0C, R_GPPUB = 0x0D, R_GPIOB = 0x13,
                     R_OLATA = 0x14, R_OLATB = 0x15;

static HardwareSerial MidiSerial(1);

static uint8_t  olat[MAX_BOARDS], olatSent[MAX_BOARDS];   // wanted / last written output latch per board
static bool     portBNow = false;                         // output port the boards were set up for
static bool     onFlag[MAX_OUT];
static uint32_t onAt[MAX_OUT], offAt[MAX_OUT];
static const uint32_t OFF_GAP_US = 15000;                 // a coil stays off at least this long between strikes
static uint16_t onMaOf[MAX_OUT];                          // mA counted when it went on
static int      onCount = 0, onMa = 0;

struct QItem { uint8_t i, vel; uint32_t us, due; };
static QItem q[QSIZE];
static int   qCount = 0;

static int  tPin = -1;           // tester GPIO, fixed at boot
static bool tInv = false, tOn = false;
static uint32_t tOnAt = 0, tOffAt = 0;

static bool     midiOn = false;
static uint32_t lastPass = 0;
static bool     muteEdge = false;  // last seen (muted || updating)
static bool     stuckNew = false;  // a board just became stuck: panic and mute at the next safe point

static uint32_t lgClamp = 0, lgStale = 0, lgFull = 0, lgHeat = 0, lgFault = 0, lgBudget = 0, lgGone = 0, lgHold = 0;
static uint32_t lgPower = 0, lgRec = 0, sayMidiMute = 0;

// where a strike request came from (internal only): decides which of Verna's prompts a refusal may trigger
enum : uint8_t { SRC_MIDI, SRC_UI, SRC_PEDAL, SRC_TRIG, SRC_AUTO };   // UI = page/screen/serial/MQTT; AUTO = demo, seq
static uint32_t bootMs = 0;
static bool     everOk[MAX_BOARDS];   // board was OK at some point this boot (for "board N is back")
static bool     wearSaid[MAX_OUT];    // V_WEAR_CHECK already said for this output this boot
static int      pwrPinNow = -1;       // power sense pin, fixed at boot
static bool     pwrRaw = false;
static uint32_t pwrChanged = 0;

// "muted" prompt for a refused strike: never for the demo/sequencer, MIDI at most once a minute
static void sayMuted(uint8_t src) {
  if (src == SRC_AUTO || updating) return;
  if (src == SRC_MIDI && !every(sayMidiMute, 60000)) return;
  vernaSay(V_MUTED);
}

static void allOff();
static void boardFault(uint8_t b, const char* why);
static void trigInit();

// ---------------- raw driver access ----------------
static bool regWrite(uint8_t b, uint8_t reg, uint8_t val) {
  Wire.beginTransmission((uint8_t)(BOARD_BASE + b));
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool regRead(uint8_t b, uint8_t reg, uint8_t& val) {
  Wire.beginTransmission((uint8_t)(BOARD_BASE + b));
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)(BOARD_BASE + b), (size_t)1) != 1) return false;
  int v = Wire.read();
  if (v < 0) return false;
  val = (uint8_t)v;
  return true;
}

static inline bool trigActive() { return cfg.trig && !portBNow && bState[0] == B_OK; }

// Both latches to 0. True only when both writes ACKed: then the latch is known off, and a stuck mark clears.
static bool zeroLatches(uint8_t b) {
  bool ok = regWrite(b, R_OLATA, 0);
  ok = regWrite(b, R_OLATB, 0) && ok;
  if (ok && (stuckMask & (1u << b))) {
    stuckMask = (uint8_t)(stuckMask & ~(1u << b));
    evlog("driver 0x%02X latches zeroed: its coils are off", (unsigned)(BOARD_BASE + b));
  }
  return ok;
}

// Bring the bus up once (the very first statement of setup() does it, before the config is even read).
static bool wireUp = false;
static void wireBegin() {
  if (wireUp) return;
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Wire.setTimeOut(10);
  wireUp = true;
}

// setup(), first statement: the MCP23017s are not reset by an ESP reset (crash, watchdog, brownout, restart), so a
// coil that was on at that moment is still latched on. Zero every possible address now; needs no config. A missing
// address NACKs at once. Their IODIR is left alone until coreSetup's off-first init.
void driversOffNow() {
  wireBegin();
  for (uint8_t b = 0; b < MAX_BOARDS; b++) {
    regWrite(b, R_OLATA, 0);
    regWrite(b, R_OLATB, 0);
  }
}

// Off first: both latches to 0 BEFORE any pin becomes an output. The other port stays inputs with no pull-ups
// (0x20 port B has pull-ups while the triggers are on), except its pin 7: on current MCP23017 silicon (datasheet
// rev C on) GPA7/GPB7 are output-only, and a changing input there can corrupt SDA, so pin 7 is an output held low.
static bool boardInit(uint8_t b) {
  if (!zeroLatches(b)) return false;
  uint8_t pub = (b == 0 && cfg.trig && !portBNow) ? 0x7F : 0x00;
  bool ok = regWrite(b, R_GPPUA, 0x00) && regWrite(b, R_GPPUB, pub) &&
            regWrite(b, R_IODIRA, portBNow ? 0x7F : 0x00) && regWrite(b, R_IODIRB, portBNow ? 0x00 : 0x7F);
  olat[b] = 0;
  olatSent[b] = 0;
  return ok;
}

// force: every board (boot, rescan, port/trigger change). Otherwise only boards that aren't OK (auto-rescan).
// The triggers are re-read only on a forced scan or when board 0 itself was set up again, so the periodic
// auto-rescan (0x24..0x27 are usually just absent) never resets a trigger or the B6 hold-to-unmute.
static void scanBoards(bool force) {
  portBNow = cfg.portB != 0;
  bool board0 = false;
  for (uint8_t b = 0; b < MAX_BOARDS; b++) {
    uint8_t was = bState[b];
    if (!force && was == B_OK) continue;
    // a board's coils are off before its init (init writes zeros first anyway)
    for (int p = 0; p < 8; p++) {
      int i = b * 8 + p;
      if (onFlag[i]) { onFlag[i] = false; onCount--; onMa -= onMaOf[i]; }
      outOn[i] = 0;
    }
    olat[b] = 0;
    olatSent[b] = 0;
    // a missing address NACKs the first write at once
    Wire.beginTransmission((uint8_t)(BOARD_BASE + b));
    bool here = Wire.endTransmission() == 0;
    if (!here) {
      if (force && was != B_MISSING) {
        bState[b] = B_MISSING;
        evlog("driver 0x%02X not found", (unsigned)(BOARD_BASE + b));
      }
      continue;
    }
    if (b == 0) board0 = true;
    if (boardInit(b)) {
      bState[b] = B_OK;
      if (was != B_OK) {
        evlog("driver 0x%02X OK%s", (unsigned)(BOARD_BASE + b), was == B_FAULT ? " again" : "");
        if (was == B_FAULT || everOk[b]) vernaSay((uint8_t)(V_BOARD_BACK_1 + b));   // faulted, or seen then gone
      }
      everOk[b] = true;
    } else {
      boardFault(b, "init");
    }
  }
  if (force || board0) trigInit();
}

// I2C access failed: mark it, try zeros on both latches, forget its coils, log. When the zeros don't ACK either,
// the latch may still hold a coil on: mark the board stuck. Core 1 then retries the zeros every few ms whatever
// else is going on, and keeps the chimes muted until they land.
static void boardFault(uint8_t b, const char* why) {
  bump(bErr[b]);
  uint8_t was = bState[b];
  bState[b] = B_FAULT;
  if (!zeroLatches(b) && !(stuckMask & (1u << b))) {
    stuckMask = (uint8_t)(stuckMask | (1u << b));
    stuckNew = true;
    evlog("driver 0x%02X: coil may be stuck on, cut 24 V", (unsigned)(BOARD_BASE + b));
  }
  olat[b] = 0;
  olatSent[b] = 0;
  for (int p = 0; p < 8; p++) {
    int i = b * 8 + p;
    if (onFlag[i]) { onFlag[i] = false; onCount--; onMa -= onMaOf[i]; }
    outOn[i] = 0;
  }
  if (was != B_FAULT) evlog("driver 0x%02X FAULT (%s): its coils off", (unsigned)(BOARD_BASE + b), why);
}

static inline void testerWrite(bool on) {
  if (tPin >= 0) digitalWrite(tPin, (on != tInv) ? HIGH : LOW);
}

// Everything off now: tester, every coil, both latches of every board that has answered.
static void allOff() {
  testerWrite(false);
  tOn = false;
  for (int i = 0; i < MAX_OUT; i++) { onFlag[i] = false; outOn[i] = 0; }
  onCount = 0;
  onMa = 0;
  for (uint8_t b = 0; b < MAX_BOARDS; b++) {
    olat[b] = 0;
    olatSent[b] = 0;
    if (bState[b] == B_MISSING && !(stuckMask & (1u << b))) continue;
    if (!zeroLatches(b) && bState[b] == B_OK) boardFault(b, "all off");
  }
  coilsOn = 0;
  maOn = 0;
}

// A stuck board: retry its zeros, one board every 3 ms, with no condition on what else is on, queued or playing.
static uint32_t lastZero = 0;
static uint8_t  zeroNext = 0;
static void serviceStuck(uint32_t now) {
  uint8_t m = stuckMask;
  if (!m || now - lastZero < 3000) return;
  lastZero = now;
  for (int k = 0; k < MAX_BOARDS; k++) {
    uint8_t b = (uint8_t)((zeroNext + k) % MAX_BOARDS);
    if (!(m & (1u << b))) continue;
    zeroLatches(b);
    zeroNext = (uint8_t)(b + 1);
    return;
  }
}

// a non-panic path that clears the queue counts and logs what it drops (nothing is dropped silently)
static void dropQueue(const char* why);

// one OLAT write per board per pass, only when it changed
static void flushOut() {
  for (uint8_t b = 0; b < MAX_BOARDS; b++) {
    if (bState[b] != B_OK || olat[b] == olatSent[b]) continue;
    if (regWrite(b, portBNow ? R_OLATB : R_OLATA, olat[b])) olatSent[b] = olat[b];
    else boardFault(b, "write");
  }
}

// ---------------- the strike queue ----------------
static void clearQueue() { qCount = 0; qn = 0; }

static void dropQueue(const char* why) {
  if (qCount) {
    nDropped = nDropped + (uint32_t)qCount;
    evlog("%s: dropped %d queued (%lu dropped)", why, qCount, (unsigned long)nDropped);
  }
  clearQueue();
}

static void qRemove(int k) {
  for (int j = k; j < qCount - 1; j++) q[j] = q[j + 1];
  qCount--;
  qn = (uint8_t)qCount;
}

static void refuseFault(int i) {
  bump(nFault);
  if (every(lgFault, 2000))
    evlog("out %d refused: driver 0x%02X not OK (%lu)", i + 1, (unsigned)(BOARD_BASE + i / 8), (unsigned long)nFault);
  vernaSay(V_NO_DRIVER);
}

// THE one place a coil goes on. Clamp, count, log; the off time is set in the same block as the on bit.
static void fire(uint8_t i, uint32_t us, uint8_t vel, uint32_t now) {
  uint32_t cap = outCap(i);
  if (us > cap) {
    bump(nClamped);
    if (every(lgClamp, 1000))
      evlog("clamp out %d: %lu -> %lu us (%lu)", i + 1, (unsigned long)us, (unsigned long)cap, (unsigned long)nClamped);
    us = cap;
  }
  if (us > HARD_CAP_US) us = HARD_CAP_US;   // outCap already guarantees this; belt and braces
  if (us < MIN_PULSE_US) us = MIN_PULSE_US;
  uint8_t b = i / 8;
  {
    olat[b] = (uint8_t)(olat[b] | (1u << (i % 8)));
    onFlag[i] = true;
    onAt[i] = now;
    offAt[i] = now + us;
  }
  onMaOf[i] = outs[i].mA;
  onCount++;
  onMa += onMaOf[i];
  outOn[i] = 1;
  heat[i] = heat[i] + us / 1000.0f;
  bump(strikes[i]);
  bump(nPlayed);
  bump(wearCount[i]);                          // lifetime count; core 0 saves blob "wear" when dirty
  wearDirty = true;
  if (!wearCheck(i)) wearSaid[i] = false;      // (reset after service: the next crossing speaks again)
  else if (!wearSaid[i]) { wearSaid[i] = true; vernaSay(V_WEAR_CHECK); }   // the crossing, or first strike this boot
  lastPlayed = (uint16_t)((uint16_t)i << 8 | vel);
  lastPlayedMs = millis();
  if (chimesToNet) {
    NoteMsg m = {outs[i].note, vel};
    xQueueSend(chimesToNet, &m, 0);
  }
}

// Queue a strike. us = 0: velocity curve. Due = t0 + fixed latency, kept sorted by due time.
// src: SRC_* (which prompt a refusal may trigger; it never changes what is allowed).
static bool requestOut(int i, uint8_t vel, uint32_t us, uint32_t t0, uint8_t src) {
  if (updating) return false;
  if (muted) { sayMuted(src); return false; }
  if (i < 0 || i >= MAX_OUT) return false;
  if (pwrState == 0) {                         // no 24 V: refuse everything, so nothing queues up waiting
    bump(nNoPower);
    if (every(lgPower, 5000)) evlog("no 24 V: plug in the power supply (%lu refused)", (unsigned long)nNoPower);
    vernaSay(V_NO_POWER);
    return false;
  }
  const OutCfg& o = outs[i];
  if (o.kind == K_OFF) {
    if (src == SRC_UI || src == SRC_PEDAL) vernaSay(V_NOT_SET_UP);   // never for MIDI or the sequencer
    return false;
  }
  if (cfg.solo && cfg.solo != i + 1) return false;
  if (bState[i / 8] != B_OK) { refuseFault(i); return false; }
  if (!us) {
    if (!vel) return false;
    if (vel > 127) vel = 127;
    uint32_t mn = o.minUs, fu = o.fullUs;
    if (fu < mn) fu = mn;
    float g = cfg.gamma;
    if (!(g > 0.1f && g < 10.0f)) g = 1.6f;
    us = mn + (uint32_t)((float)(fu - mn) * powf(vel / 127.0f, g) + 0.5f);
  }
  int lat = cfg.latUs;
  if (lat < 0) lat = 0;
  if (lat > 30000) lat = 30000;
  if (!t0) t0 = micros();
  uint32_t due = t0 + (uint32_t)lat;
  if (qCount >= QSIZE) {
    bump(nDropped);
    if (every(lgFull, 1000)) evlog("queue full: dropped out %d (%lu dropped)", i + 1, (unsigned long)nDropped);
    return false;
  }
  int k = qCount;
  while (k > 0 && (int32_t)(q[k - 1].due - due) > 0) { q[k] = q[k - 1]; k--; }
  q[k].i = (uint8_t)i;
  q[k].vel = vel;
  q[k].us = us;
  q[k].due = due;
  qCount++;
  qn = (uint8_t)qCount;
  if (qCount > qPeak) qPeak = (uint8_t)qCount;
  return true;
}

// Fire what is due, in due order. Rearm-waiting outputs let others behind them go; the supply budget does not.
static void serviceQueue(uint32_t now) {
  int k = 0;
  while (k < qCount) {
    QItem& e = q[k];
    int32_t late = (int32_t)(now - e.due);
    if (late < 0) break;                       // sorted: nothing further is due
    int i = e.i;
    if (late > (int32_t)STALE_US) {
      bump(nDropped);
      if (every(lgStale, 1000))
        evlog("stale: dropped out %d, %ld ms late (%lu dropped)", i + 1, (long)(late / 1000), (unsigned long)nDropped);
      qRemove(k);
      continue;
    }
    if (bState[i / 8] != B_OK) { refuseFault(i); qRemove(k); continue; }
    if (outs[i].kind == K_OFF || (cfg.solo && cfg.solo != i + 1)) {
      bump(nDropped);
      if (every(lgGone, 1000)) evlog("dropped out %d: turned off or not solo (%lu dropped)", i + 1, (unsigned long)nDropped);
      qRemove(k);
      continue;
    }
    // rearm: it waits. Counted from the strike AND from its off time, so a long pulse can never be followed in
    // the same pass by the next strike (that would skip the off write and merge two strikes into one long ON).
    if (onFlag[i] || now - onAt[i] < REARM_US || now - offAt[i] < OFF_GAP_US) { k++; continue; }
    float add = e.us / 1000.0f;
    if (heat[i] + add > HEAT_CAP_MS) {
      bump(nHeat);
      refusedAt[i] = millis() | 1;
      if (every(lgHeat, 1000)) evlog("out %d too hot: refused (%lu)", i + 1, (unsigned long)nHeat);
      vernaSay(V_TOO_HOT);
      qRemove(k);
      continue;
    }
    if (onCount >= MAX_ON_COUNT) break;
    int need = outs[i].mA;
    if (onMa + need > cfg.supplyMa) {
      if (onCount > 0) break;                  // waits for a coil to drop out
      // nothing on: one strike may go alone, or the queue could never move
      if (every(lgBudget, 60000))
        evlog("out %d alone over budget: %d > %d mA", i + 1, need, cfg.supplyMa);
    }
    uint32_t us = e.us;
    uint8_t vel = e.vel;
    qRemove(k);
    fire((uint8_t)i, us, vel, now);
  }
}

// ---------------- heat and off times ----------------
static uint32_t lastLeak = 0;
static void serviceOff(uint32_t now) {
  float dtMs = (float)(now - lastLeak) / 1000.0f;
  lastLeak = now;
  if (dtMs > 1000.0f) dtMs = 1000.0f;
  for (int i = 0; i < MAX_OUT; i++) {
    float h = heat[i];
    if (h > 0.0f) {
      h -= dtMs * HEAT_LEAK;
      heat[i] = h > 0.0f ? h : 0.0f;
    }
    if (!onFlag[i]) continue;
    if ((int32_t)(now - offAt[i]) >= 0 || now - onAt[i] >= HARD_CAP_US) {
      uint8_t b = i / 8;
      olat[b] = (uint8_t)(olat[b] & ~(1u << (i % 8)));
      onFlag[i] = false;
      onCount--;
      onMa -= onMaOf[i];
      outOn[i] = 0;
    }
  }
}

// ---------------- tester GPIO: strike only ----------------
static void testerTap() {
  if (muted && tPin >= 0) sayMuted(SRC_UI);
  if (muted || updating || tPin < 0 || tOn) return;
  if (holdWord & 0xFF) {                       // a flash write is waiting for quiet: no tap now
    if (every(lgHold, 1000)) evlog("tester tap refused: saving settings");
    return;
  }
  uint32_t now = micros();
  if (solTaps && now - tOnAt < SOL_REARM_US) return;
  int ms = cfg.solMs;
  if (ms < SOL_MS_MIN) ms = SOL_MS_MIN;
  if (ms > SOL_MS_MAX) ms = SOL_MS_MAX;
  uint32_t us = (uint32_t)ms * 1000;
  if (us > HARD_CAP_US) us = HARD_CAP_US;
  {
    testerWrite(true);
    tOn = true;
    tOnAt = now;
    tOffAt = now + us;
  }
  bump(solTaps);
}

static inline void testerService(uint32_t now) {
  if (tOn && ((int32_t)(now - tOffAt) >= 0 || now - tOnAt >= HARD_CAP_US)) { testerWrite(false); tOn = false; }
}

// ---------------- demo and sequencer ----------------
static int      demoIdx = 0;
static uint32_t demoNext = 0;
static int      seqK = 0;
static uint32_t seqT = 0;     // on-grid start of step seqK (swing added on top)
static int      seqNext = -1;     // pedal next/prev while playing: switch to this slot at the end of the bar
static bool     seqOnce = false;  // play one pass whatever the loop setting (a free-run take's playback)
static int      skipStep = -1;    // overdub: cells just recorded on the step about to play aren't struck twice
static uint16_t skipRows = 0;

static void stopDemo() { demoSong = 0; demoIdx = 0; }
static void stopSeq() { seqSlot = -1; seqStep = 0; seqK = 0; seqNext = -1; seqOnce = false; skipStep = -1; }

static inline int slotNow() { int c = curSlot; return (c >= 0 && c < SEQ_SLOTS) ? c : 0; }
static inline int patSteps(const Pattern& p) {
  int s = p.steps;
  return s < 1 ? 1 : (s > SEQ_STEPS ? SEQ_STEPS : s);
}
static inline uint32_t patStepUs(const Pattern& p) {   // 60000/bpm/4 ms: a 16th
  uint32_t bpm = p.bpm;
  if (bpm < 30) bpm = 30;
  if (bpm > 240) bpm = 240;
  return 15000000UL / bpm;
}
static inline uint32_t patSwingUs(const Pattern& p, uint32_t stepUs) {   // odd steps are late by swing% of a step
  uint32_t sw = p.swing > 75 ? 75 : p.swing;
  return stepUs * sw / 100;
}
// steps in a bar of the pattern's time signature (TIME_SIGS[ts].steps); a pattern shorter than that is one bar
static inline int patBar(const Pattern& p, int steps) {
  int ts = p.ts < NUM_TS ? p.ts : 0;
  int b = TIME_SIGS[ts].steps;
  return (b < 1 || b > steps) ? steps : b;
}

static void recDone(bool playback);

// a MIDI note -> every output with that note (fold into 48..72 if none, when fold is on)
static void noteStrike(uint8_t note, uint8_t vel, uint32_t t0, uint8_t src) {
  if (!note || !vel) return;
  for (int pass = 0; pass < 2; pass++) {
    int n = 0;
    for (int i = 0; i < MAX_OUT; i++) {
      if (outs[i].kind == K_OFF || outs[i].note != note) continue;
      n++;
      requestOut(i, vel, 0, t0, src);
    }
    if (n || !cfg.fold || pass) return;
    uint8_t f = note;
    while (f < 48) f = (uint8_t)(f + 12);
    while (f > 72) f = (uint8_t)(f - 12);
    if (f == note) return;
    note = f;
  }
}

static void startDemo(int s) {
  if (s < 1 || s > NUM_SONGS) return;
  if (muted || updating) { evlog("demo: muted, not started"); sayMuted(SRC_UI); return; }
  if (seqSlot >= 0) stopSeq();
  demoIdx = 0;
  demoNext = micros();
  demoSong = s;
  evlog("demo: %s", SONGS[s - 1].name);
}

static void serviceDemo(uint32_t now) {
  int s = demoSong;
  if (s < 1 || s > NUM_SONGS) return;
  const Song& sg = SONGS[s - 1];
  if ((int32_t)(now - demoNext) > 200000) demoNext = now;   // fell far behind (stall): carry on from now
  while ((int32_t)(now - demoNext) >= 0) {
    if (demoIdx >= sg.count) { stopDemo(); evlog("demo: done"); return; }
    const DN& d = sg.n[demoIdx++];
    if (d.note) noteStrike(d.note, 100, demoNext, SRC_AUTO);
    demoNext += (uint32_t)d.len * sg.eighthMs * 1000;    // absolute: no drift
  }
}

static void startSeq(int slot) {
  if (slot < 0 || slot >= SEQ_SLOTS) return;
  if (muted || updating) { evlog("seq: muted, not started"); sayMuted(SRC_UI); return; }
  if (demoSong) stopDemo();
  seqK = 0;
  seqT = micros();
  seqStep = 0;
  seqNext = -1;
  seqOnce = false;
  skipStep = -1;
  seqSlot = slot;
  evlog("seq: playing %d %.12s", slot + 1, pats[slot].name);
}

// Steps follow p.steps (the time signature's beat groups are only drawn); swing up to 75 %.
static void serviceSeq(uint32_t now) {
  int slot = seqSlot;
  if (slot < 0 || slot >= SEQ_SLOTS) return;
  const Pattern& p = pats[slot];
  int steps = patSteps(p);
  if (seqK >= steps) {                         // steps shortened while playing
    if ((!p.loop || seqOnce) && seqNext < 0) { stopSeq(); evlog("seq: done"); return; }
    seqK = 0;
  }
  uint32_t stepUs = patStepUs(p);
  uint32_t at = seqT + ((seqK & 1) ? patSwingUs(p, stepUs) : 0);
  if ((int32_t)(now - at) < 0) return;
  if ((int32_t)(now - at) > 200000) { seqT = now; at = now; }   // fell far behind: carry on from now
  seqStep = seqK;
  bool skip = skipStep == seqK;
  for (int r = 0; r < SEQ_ROWS; r++) {
    uint8_t o = p.row[r];
    if (o == SEQ_NONE || o >= MAX_OUT) continue;
    uint8_t v = p.vel[r][seqK];
    if (!v) continue;
    if (skip && ((skipRows >> r) & 1)) continue;   // just played live and recorded here
    requestOut(o, v > 127 ? 127 : v, 0, at, SRC_AUTO);
  }
  if (skip) { skipStep = -1; skipRows = 0; }
  seqT += stepUs;
  seqK++;
  if (seqK >= steps) {
    seqK = 0;
    if ((!p.loop || seqOnce) && seqNext < 0) { stopSeq(); evlog("seq: done"); return; }
  }
  // a pedal's next/prev: switch at the end of the bar (or of the pattern), keeping the beat
  int nx = seqNext;
  if (nx >= 0 && seqK % patBar(p, steps) == 0) {
    seqNext = -1;
    if (nx < SEQ_SLOTS && nx != slot) {
      seqOnce = false;
      skipStep = -1;
      seqK = 0;
      seqSlot = nx;
      evlog("seq: playing %d %.12s", nx + 1, pats[nx].name);
    }
  }
}

// ---------------- MIDI record (ROUND2.md) ----------------
// Overdub when the slot is playing: each note-on goes to the nearest step of the running pattern. Otherwise the
// first note starts a free-running take (step 0 = that note) for one pattern length, which then plays back once.
// Core 1 writes only single bytes of pats[]; recState 3 = take over, core 0 saves it and speaks.
static bool     recFree = false;      // the take runs on its own clock (slot not playing)
static uint32_t recT0 = 0, recStepUs = 0, recSwUs = 0, recDoneMs = 0;
static int      recSteps = 0;

// The step nearest time t, among baseK-1..baseK+1; base = on-grid time of step baseK, odd steps swung by swUs.
static int nearestStep(uint32_t t, uint32_t base, int baseK, uint32_t stepUs, uint32_t swUs) {
  int best = baseK;
  uint32_t bestD = 0xFFFFFFFFu;
  for (int d = -1; d <= 1; d++) {
    int k = baseK + d;
    uint32_t at = base + (uint32_t)((int32_t)stepUs * d) + ((k & 1) ? swUs : 0);
    int32_t df = (int32_t)(t - at);
    uint32_t a = df < 0 ? (uint32_t)(-df) : (uint32_t)df;
    if (a < bestD) { bestD = a; best = k; }
  }
  return best;
}

static inline uint8_t recVel(uint8_t v) { return v < 80 ? 60 : (v < 114 ? 100 : 127); }   // ghost/medium/accent

static bool rowEmptyCells(const Pattern& p, int r) {
  for (int k = 0; k < SEQ_STEPS; k++) if (p.vel[r][k]) return false;
  return true;
}

// The row for a note: the row whose output answers it, else the first empty row (preferring one labelled with that
// output's drum role, then an unlabelled one, then any empty row with no cells), which takes the first usable
// output answering the note. -1 = nowhere to put it.
static int recRow(Pattern& p, uint8_t note) {
  uint8_t n = note;
  bool any = false;
  for (int i = 0; i < MAX_OUT && !any; i++) any = outs[i].kind != K_OFF && outs[i].note == n;
  if (!any && cfg.fold) {                      // as noteStrike: a note nothing answers folds into 48..72
    while (n < 48) n = (uint8_t)(n + 12);
    while (n > 72) n = (uint8_t)(n - 12);
  }
  for (int r = 0; r < SEQ_ROWS; r++) {
    uint8_t o = p.row[r];
    if (o < MAX_OUT && outs[o].kind != K_OFF && outs[o].note == n) return r;
  }
  int out = -1;
  for (int i = 0; i < MAX_OUT && out < 0; i++) if (outUsable(i) && outs[i].note == n) out = i;
  if (out < 0) return -1;
  uint8_t role = outs[out].role;
  int r = -1, how = 0;
  for (int k = 0; k < SEQ_ROWS && r < 0; k++)
    if (p.row[k] == SEQ_NONE && role != R_NONE && p.rowRole[k] == role) { r = k; how = 1; }
  for (int k = 0; k < SEQ_ROWS && r < 0; k++)
    if (p.row[k] == SEQ_NONE && p.rowRole[k] == R_NONE) { r = k; how = 2; }
  for (int k = 0; k < SEQ_ROWS && r < 0; k++)
    if (p.row[k] == SEQ_NONE && rowEmptyCells(p, k)) { r = k; how = 3; }
  if (r < 0) return -1;
  if (how == 2) for (int k = 0; k < SEQ_STEPS; k++) if (p.vel[r][k]) p.vel[r][k] = 0;   // leftovers would play
  if (how != 1) p.rowRole[r] = role;
  p.row[r] = (uint8_t)out;                     // last: the row only plays once its cells are as meant
  return r;
}

// A played note-on (MIDI in, or a note from the page/screen/MQTT). It still strikes normally; this only records.
static void recNote(uint8_t note, uint8_t vel, uint32_t t) {
  int st = recState, s = recSlot;
  if ((st != 1 && st != 2) || s < 0 || s >= SEQ_SLOTS || !note || !vel) return;
  Pattern& p = pats[s];
  int k;
  bool ahead = false;
  if (st == 1 && seqSlot == s) { recState = 2; recFree = false; st = 2; }   // started playing meanwhile: overdub
  if (st == 1) {                               // armed, slot not playing: this note starts the take, at step 0
    recSteps = patSteps(p);
    recStepUs = patStepUs(p);
    recSwUs = patSwingUs(p, recStepUs);
    recT0 = t;
    recFree = true;
    recState = 2;
    evlog("rec: take started, slot %d, %d steps", s + 1, recSteps);
    k = 0;
  } else if (recFree) {
    uint32_t el = (int32_t)(t - recT0) > 0 ? t - recT0 : 0;
    if (el >= (uint32_t)recSteps * recStepUs) return;   // after the take (it ends this pass)
    int k0 = (int)(el / recStepUs);
    k = nearestStep(t, recT0 + (uint32_t)k0 * recStepUs, k0, recStepUs, recSwUs);
    if (k < 0) k = 0;
    if (k >= recSteps) k = 0;                  // the last half step rounds to the next downbeat
  } else {
    if (seqSlot != s) return;
    int steps = patSteps(p);
    uint32_t stepUs = patStepUs(p);
    int base = seqK;                           // seqK: the next step to play, on-grid at seqT
    k = nearestStep(t, seqT, base, stepUs, patSwingUs(p, stepUs));
    ahead = k >= base;
    k = ((k % steps) + steps) % steps;
  }
  int r = recRow(p, note);
  if (r < 0) {
    if (every(lgRec, 2000)) evlog("rec: no free row (or no usable output) for note %u", (unsigned)note);
    return;
  }
  p.vel[r][k] = recVel(vel);
  if (ahead) {                                 // that step hasn't played yet: don't strike it a second time
    if (skipStep != k) { skipStep = k; skipRows = 0; }
    skipRows = (uint16_t)(skipRows | (1u << r));
  }
  recNotes = recNotes + 1;
  patEdited(s);
}

// End the take: state 3, core 0 saves and speaks. A free-run take that ran its full length plays back once.
static void recDone(bool playback) {
  int st = recState, s = recSlot;
  if (st != 1 && st != 2) return;
  bool wasFree = recFree && st == 2;
  recFree = false;
  recDoneMs = millis() | 1;
  recState = 3;
  evlog("rec: slot %d done, %d note(s)", s + 1, (int)recNotes);
  if (playback && wasFree && recNotes > 0 && !muted && !updating && s >= 0 && s < SEQ_SLOTS) {
    if (demoSong) stopDemo();
    seqK = 0;
    seqT = recT0 + (uint32_t)recSteps * recStepUs;   // on the beat the take ended on
    seqStep = 0;
    seqNext = -1;
    seqOnce = true;
    skipStep = -1;
    seqSlot = s;
    evlog("rec: playing the take back");
  }
}

static void recArm(int slot) {
  if (slot < 0 || slot >= SEQ_SLOTS || updating) return;
  if (muted) { evlog("rec: muted, not armed"); sayMuted(SRC_UI); return; }
  if (recState == 3 && millis() - recDoneMs < 2000) { evlog("rec: still saving the last take"); return; }
  if (demoSong) { stopDemo(); evlog("demo: stopped (recording)"); }
  recNotes = 0;
  recFree = false;
  skipStep = -1;
  recSlot = slot;
  if (seqSlot == slot) { recState = 2; evlog("rec: overdub into slot %d", slot + 1); }
  else { recState = 1; evlog("rec: slot %d armed, waiting for the first note", slot + 1); }
  vernaSay(V_REC_START);
}

// once per pass: overdub follows the playing slot; a free-run take ends after one pattern length
static void serviceRec(uint32_t now) {
  int st = recState;
  if (st == 1 && seqSlot == recSlot) { recState = 2; recFree = false; evlog("rec: overdub into slot %d", recSlot + 1); }
  else if (st == 2 && !recFree && seqSlot != recSlot) recDone(false);          // stopped or switched: take over
  else if (st == 2 && recFree && seqSlot == recSlot) recFree = false;          // started meanwhile: now overdub
  else if (st == 2 && recFree && (int32_t)(now - (recT0 + (uint32_t)recSteps * recStepUs)) >= 0) recDone(true);
}

// ---------------- panic ----------------
static void doPanic(bool mute, const char* src) {
  lastPanicMs = millis();
  if (mute) muted = true;
  allOff();
  clearQueue();
  stopDemo();
  stopSeq();
  recDone(false);
  if (mute) vernaSay(V_PANIC);                 // not MIDI CC120/123 (those don't mute)
  calAbort = true;
  bump(nPanics);
  muteEdge = muted || updating;                // the mute edge below needn't repeat this
  evlog("PANIC (%s)%s", src ? src : "?", mute ? ", muted" : "");
}

// Panel button, BOOT and trigger B6: a press always panics and mutes. Held 1.5 s it unmutes, but only when the
// chimes were already muted before that press, calmly: no panic from ANY source (any button, screen, page, MQTT,
// serial, a stuck coil) in the last 5 s, and this button released for more than 2 s before the press. So a burst
// of emergency presses, contact chatter, or a press right after another panic can never arm the unmute.
// seq: nPanics right after this press's own panic; any other panic during the hold cancels the unmute.
struct PBtn { bool raw, stable, wasMuted, holdDone; uint32_t changed, downAt, upAt; const char* name; uint32_t seq; };
static PBtn btnPanel = {false, false, false, true, 0, 0, 0, "panic button", 0};
static PBtn btnBoot  = {false, false, false, true, 0, 0, 0, "BOOT", 0};
static PBtn btnTrig  = {false, false, false, true, 0, 0, 0, "trigger B6", 0};
static int  btnPin = -1;

static void btnUpdate(PBtn& b, bool low, uint32_t ms) {
  if (low != b.raw) { b.raw = low; b.changed = ms; }
  if (b.raw != b.stable && ms - b.changed >= 15) {
    b.stable = b.raw;
    if (b.stable) {
      b.wasMuted = muted && ms - lastPanicMs > 5000 && ms - b.upAt > 2000;   // decided before this press panics
      b.downAt = ms;
      b.holdDone = false;
      doPanic(true, b.name);
      b.seq = nPanics;
    } else {
      b.upAt = ms;
    }
  }
  if (b.stable && !b.holdDone && ms - b.downAt >= 1500) {
    b.holdDone = true;
    if (b.wasMuted && nPanics != b.seq) evlog("unmute cancelled (%s held): another panic came in", b.name);
    else if (b.wasMuted && stuckMask) evlog("unmute refused (%s held): a coil may be stuck on", b.name);
    else if (b.wasMuted && !updating) {
      muted = false;
      muteEdge = muted || updating;
      evlog("unmuted (%s held)", b.name);
      vernaSay(V_UNMUTED);
    }
  }
}

// ---------------- triggers: contacts to GND on 0x20 port B ----------------
// B0..B5 strike, B6 is the panic contact, B7 is never read (output-only on current MCP23017s; held low).
static const uint8_t TRIG_OUT[6] = {0, 7, 12, 16, 19, 24};   // B0..B5 -> outputs 1, 8, 13, 17, 20, 25
static bool     trigStable[6], trigRaw[6];
static uint32_t trigChanged[6];
static uint32_t trigLast = 0;

static void trigInit() {
  uint32_t ms = millis();
  uint8_t g = 0xFF;
  if (trigActive() && !regRead(0, R_GPIOB, g)) { boardFault(0, "trigger read"); g = 0xFF; }
  for (int k = 0; k < 6; k++) {                // a contact already closed doesn't count as a press
    trigStable[k] = trigRaw[k] = !((g >> k) & 1);
    trigChanged[k] = ms;
  }
  btnTrig.raw = btnTrig.stable = !((g >> 6) & 1);
  btnTrig.changed = ms;
  btnTrig.holdDone = true;
}

static void serviceTriggers(uint32_t now) {
  if (!trigActive() || now - trigLast < 2000) return;
  trigLast = now;
  uint8_t g;
  if (!regRead(0, R_GPIOB, g)) { boardFault(0, "trigger read"); return; }
  uint32_t ms = millis();
  for (int k = 0; k < 6; k++) {
    bool low = !((g >> k) & 1);
    if (low != trigRaw[k]) { trigRaw[k] = low; trigChanged[k] = ms; }
    if (trigRaw[k] == trigStable[k] || ms - trigChanged[k] < 10) continue;
    trigStable[k] = trigRaw[k];
    if (trigStable[k]) requestOut(TRIG_OUT[k], 110, 0, now, SRC_TRIG);
  }
  btnUpdate(btnTrig, !((g >> 6) & 1), ms);
}

// ---------------- foot pedals (header pins, boot-only) ----------------
// INPUT_PULLUP; the released level is sampled ~50 ms after boot (no delay on core 1) unless pedalInv forces the
// polarity, so normally-open and normally-closed pedals both work. 15 ms debounce, act on the press. A P_PANIC
// pedal runs through btnUpdate(), the panel button's rule (press = panic + mute, calm 1.5 s hold = unmute).
static int      pedPin[NUM_PEDALS] = {-1, -1, -1, -1};
static bool     pedPressHigh[NUM_PEDALS], pedRaw[NUM_PEDALS], pedStable[NUM_PEDALS];
static uint32_t pedChanged[NUM_PEDALS];
static bool     pedReady = false;
static PBtn     pedBtn[NUM_PEDALS] = {
  {false, false, false, true, 0, 0, 0, "pedal 1", 0}, {false, false, false, true, 0, 0, 0, "pedal 2", 0},
  {false, false, false, true, 0, 0, 0, "pedal 3", 0}, {false, false, false, true, 0, 0, 0, "pedal 4", 0},
};
static uint32_t tapLast = 0, tapIv[3];
static int      tapN = 0;

// tap tempo: the average of the last 3 intervals (each 250..2000 ms) sets the bpm of curSlot, held to 30..240
static void tapTempo(uint32_t ms) {
  uint32_t iv = ms - tapLast;
  tapLast = ms;
  if (iv < 250 || iv > 2000) { tapN = 0; return; }   // first tap, or a pause: start counting again
  tapIv[0] = tapIv[1];
  tapIv[1] = tapIv[2];
  tapIv[2] = iv;
  if (tapN < 3) tapN++;
  if (tapN < 3) return;
  uint32_t avg = (tapIv[0] + tapIv[1] + tapIv[2]) / 3;
  int bpm = (int)((60000 + avg / 2) / avg);
  if (bpm < 30) bpm = 30;
  if (bpm > 240) bpm = 240;
  int s = slotNow();
  pats[s].bpm = (uint8_t)bpm;                  // a single byte
  patEdited(s);
  evlog("tap tempo: pattern %d at %d bpm", s + 1, bpm);
}

// One pedal action. n = pedal 0..3 (its output for P_STRIKE), -1 = the MIDI sustain pedal (uses pedal 1's output).
// Every strike goes through requestOut like any other.
static void pedalAction(uint8_t a, int n) {
  switch (a) {
    case P_PLAYSTOP:
      if (seqSlot >= 0) { evlog("seq: stopped (pedal)"); stopSeq(); }
      else startSeq(slotNow());
      break;
    case P_NEXT:
    case P_PREV: {
      int c = (slotNow() + (a == P_NEXT ? 1 : SEQ_SLOTS - 1)) % SEQ_SLOTS;
      curSlot = c;
      int s = seqSlot;
      if (s >= 0) seqNext = (c != s) ? c : -1;   // playing: switch at the end of the bar
      evlog("pedal: pattern %d%s", c + 1, (s >= 0 && c != s) ? " at the end of the bar" : "");
      break;
    }
    case P_TAP: tapTempo(millis()); break;
    case P_REC:
      if (recState == 1 || recState == 2) recDone(false);
      else recArm(slotNow());
      break;
    case P_STRIKE: {
      int o = cfg.pedalOut[(n >= 0 && n < NUM_PEDALS) ? n : 0];
      if (o >= MAX_OUT && !muted && !updating) { vernaSay(V_NOT_SET_UP); break; }
      requestOut(o, 110, 0, 0, SRC_PEDAL);   // muted: says so; out of range: refused
      break;
    }
    case P_PANIC: doPanic(true, "pedal"); break;
    default: break;
  }
}

static void servicePedals(uint32_t ms) {
  if (!pedReady) {
    if (ms - bootMs < 50) return;              // let the pull-ups settle
    for (int n = 0; n < NUM_PEDALS; n++) {
      int p = pedPin[n];
      if (p < 0) continue;
      int inv = cfg.pedalInv[n];
      bool hi = digitalRead(p) == HIGH;
      pedPressHigh[n] = inv == 1 ? true : (inv == 2 ? false : !hi);   // auto: pressed = not the released level
      pedRaw[n] = pedStable[n] = false;
      pedChanged[n] = ms;
      pedBtn[n].raw = pedBtn[n].stable = false;
      pedBtn[n].changed = ms;
      pedBtn[n].holdDone = true;
      evlog("pedal %d on G%d, pressed %s", n + 1, p, pedPressHigh[n] ? "HIGH" : "LOW");
    }
    pedReady = true;
    return;
  }
  for (int n = 0; n < NUM_PEDALS; n++) {
    int p = pedPin[n];
    if (p < 0) continue;
    bool down = (digitalRead(p) == HIGH) == pedPressHigh[n];
    uint8_t act = cfg.pedalAct[n];
    PBtn& b = pedBtn[n];
    if (act == P_PANIC && cfg.pedalInv[n] == 0) {
      // A panic pedal needs its polarity set (high/low): an auto-learned one is backwards if the pedal was held or
      // unplugged at boot, and then stepping on it would do nothing until the foot lifts.
      static uint32_t lgPol[NUM_PEDALS];
      if (!lgPol[n] || (int32_t)(ms - lgPol[n]) > 60000) {
        lgPol[n] = ms | 1;
        evlog("pedal %d: panic needs its polarity set (pressed = high or low), ignored", n + 1);
      }
      pedalDown[n] = 0;
      continue;
    }
    if (act == P_PANIC) {
      btnUpdate(b, down, ms);
      pedRaw[n] = pedStable[n] = b.stable;     // kept in step, so changing the action never fakes a press
      pedChanged[n] = ms;
      pedalDown[n] = b.stable ? 1 : 0;
      continue;
    }
    if (down != pedRaw[n]) { pedRaw[n] = down; pedChanged[n] = ms; }
    if (pedRaw[n] != pedStable[n] && ms - pedChanged[n] >= 15) {
      pedStable[n] = pedRaw[n];
      pedalDown[n] = pedStable[n] ? 1 : 0;
      if (pedStable[n]) pedalAction(act, n);
    }
    b.raw = b.stable = pedStable[n];
    b.changed = ms;
    b.holdDone = true;
  }
}

// a pin a pedal or the power sense may take: a header pin with no other job (never the tester's MOSFET gate)
static bool pinFree(int p, int pedalsBefore) {
  if (p < 0 || !hdrPin(p)) return false;
  if (p == 9) return false;                    // G9 is where the tester MOSFET is wired: a pull-up there could hold it on
  if (p == cfg.solPin || p == cfg.midiPin || p == cfg.btnPin || p == cfg.pzPin || p == pwrPinNow) return false;
  for (int k = 0; k < pedalsBefore; k++) if (pedPin[k] == p) return false;
  return true;
}

// ---------------- MIDI in ----------------
static uint8_t midiStatus = 0, midiData[2], midiCount = 0;

static void midiByte(uint8_t b, uint32_t t0) {
  if (b >= 0xF8) return;                        // realtime: ignored, running status kept
  if (b & 0x80) {                               // status; 0xF0..0xF7 (SysEx, system common) clears it,
    midiStatus = (b < 0xF0) ? b : 0;            // so their data bytes are skipped
    midiCount = 0;
    return;
  }
  if (!midiStatus) return;
  uint8_t type = midiStatus & 0xF0;
  uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
  midiData[midiCount++] = b;
  if (midiCount < need) return;
  midiCount = 0;                                // running status: the next data byte starts a new message
  if (cfg.midiCh && (midiStatus & 0x0F) + 1 != cfg.midiCh) return;
  bump(nMidi);
  if (type == 0x90 && midiData[1] > 0) {
    noteStrike(midiData[0], midiData[1], t0, SRC_MIDI);
    recNote(midiData[0], midiData[1], t0);
  } else if (type == 0xB0) {
    if (midiData[0] == 120 || midiData[0] == 123) doPanic(false, "MIDI CC");   // DAWs send these on stop: no mute
    else if (midiData[0] == 64) {              // sustain pedal: cfg.cc64Act on the press (P_NONE = no effect)
      bool on = midiData[1] >= 64;
      bool press = on && !sustain;
      sustain = on;
      int a = cfg.cc64Act;
      if (press && a > P_NONE && a < P_COUNT) pedalAction((uint8_t)a, -1);
    }
  }
}

// ---------------- messages from core 0 ----------------
static void handleMsg(const Msg& m) {
  switch (m.kind) {
    case M_NOTE:  noteStrike(m.a, m.v, m.t0, SRC_UI); recNote(m.a, m.v, m.t0); break;
    case M_OUT:   requestOut(m.a, m.v, 0, m.t0, SRC_UI); break;
    case M_EXACT: if (m.us) requestOut(m.a, m.v ? m.v : 100, m.us, m.t0, SRC_UI); break;   // still clamped in fire()
    case M_TAP:   testerTap(); break;
    case M_RESCAN:
      allOff(); dropQueue("rescan");
      evlog("rescan");
      scanBoards(true); allOff();
      lastPass = micros();
      break;
    case M_PORT:
      if ((m.a ? 1 : 0) == cfg.portB) break;   // no change: nothing goes off, nothing re-inits
      allOff(); dropQueue("port change");
      cfg.portB = m.a ? 1 : 0;
      evlog("outputs on port %c", m.a ? 'B' : 'A');
      scanBoards(true); allOff();
      lastPass = micros();
      break;
    case M_TRIG:
      if ((m.a ? 1 : 0) == cfg.trig) break;
      allOff(); dropQueue("trigger change");
      cfg.trig = m.a ? 1 : 0;
      evlog("triggers %s", m.a ? "on" : "off");
      scanBoards(true); allOff();
      lastPass = micros();
      break;
    case M_DEMO:  if (m.a == 0) { if (demoSong) evlog("demo: stopped"); stopDemo(); } else startDemo(m.a); break;
    case M_SEQ:   if (m.a == SEQ_NONE) { if (seqSlot >= 0) evlog("seq: stopped"); stopSeq(); } else startSeq(m.a); break;
    case M_REC:                                // a slot arms; 255, or any M_REC while armed/recording, stops
      if (m.a == SEQ_NONE || recState == 1 || recState == 2) recDone(false);
      else recArm(m.a);
      break;
    case M_PEDAL: if (m.a > P_NONE && m.a < P_COUNT) pedalAction(m.a, m.v < NUM_PEDALS ? m.v : 0); break;
    default: break;
  }
}

// ---------------- setup and the pass ----------------
void coreSetup() {
  tPin = cfg.solPin;                           // pins are applied at boot only
  tInv = cfg.solInv != 0;
  if (tPin >= 0) {                             // off level first, then make it an output
    gpio_set_level((gpio_num_t)tPin, tInv ? 1 : 0);
    pinMode(tPin, OUTPUT);
    testerWrite(false);
  }
  wireBegin();                                 // already up from driversOffNow()
  for (uint8_t b = 0; b < MAX_BOARDS; b++) { bState[b] = B_MISSING; bErr[b] = 0; }
  for (int i = 0; i < MAX_OUT; i++) { onFlag[i] = false; outOn[i] = 0; heat[i] = 0; strikes[i] = 0; refusedAt[i] = 0; }
  scanBoards(true);
  allOff();
  pinMode(BOOT_BTN, INPUT_PULLUP);
  btnPin = cfg.btnPin;
  if (btnPin >= 0) pinMode(btnPin, INPUT_PULLUP);
  if (cfg.midiPin >= 0) {
    MidiSerial.begin(31250, SERIAL_8N1, (int8_t)cfg.midiPin, -1);   // RX only
    midiOn = true;
  }
  // round 2 inputs (boot-only pins): 24 V sense, then the foot pedals. Neither may take another role's pin.
  pwrPinNow = -1;
  pwrState = -1;
  if (cfg.pwrPin >= 0) {
    if (pinFree(cfg.pwrPin, 0)) {
      pinMode(cfg.pwrPin, INPUT);               // 100k/15k divider from 24 V: ~3.1 V with the supply on
      pwrPinNow = cfg.pwrPin;
      pwrRaw = digitalRead(pwrPinNow) == HIGH;
      pwrChanged = millis();
      pwrState = pwrRaw ? 1 : 0;
    } else {
      evlog("power sense pin %d not allowed or in use, off", cfg.pwrPin);
    }
  }
  for (int n = 0; n < NUM_PEDALS; n++) {
    pedPin[n] = -1;
    pedalDown[n] = 0;
    int p = cfg.pedalPin[n];
    if (p < 0) continue;
    if (!pinFree(p, n)) { evlog("pedal %d pin %d not allowed or in use, pedal off", n + 1, p); continue; }
    pinMode(p, INPUT_PULLUP);
    pedPin[n] = p;
  }
  if (curSlot < 0 || curSlot >= SEQ_SLOTS) curSlot = 0;
  recSlot = -1;
  recState = 0;
  recNotes = 0;
  stopDemo();
  stopSeq();
  muteEdge = muted || updating;
  lastLeak = micros();
  char list[40] = "";
  int found = 0;
  for (uint8_t b = 0; b < MAX_BOARDS; b++) {
    if (bState[b] != B_OK) continue;
    size_t l = strlen(list);
    snprintf(list + l, sizeof(list) - l, " %02X", (unsigned)(BOARD_BASE + b));
    found++;
  }
  evlog("boot %s: %d driver(s)%s, port %c, MIDI G%d ch %d", FW_VERSION, found, found ? list : "", portBNow ? 'B' : 'A',
        cfg.midiPin, cfg.midiCh);
  if (pwrState == 0) evlog("no 24 V: plug in the power supply");
  bootMs = millis();
}

// 24 V sense: HIGH = power, 100 ms debounce. Losing it drops the queue (nothing waits for the supply).
static void servicePower(uint32_t ms) {
  if (pwrPinNow < 0) return;
  bool hi = digitalRead(pwrPinNow) == HIGH;
  if (hi != pwrRaw) { pwrRaw = hi; pwrChanged = ms; }
  int want = pwrRaw ? 1 : 0;
  if (want == pwrState || ms - pwrChanged < 100) return;
  pwrState = want;
  if (want) { evlog("24 V back"); vernaSay(V_POWER_BACK); }
  else { evlog("no 24 V: plug in the power supply"); dropQueue("no 24 V"); }
}

void coreLoop() {
  uint32_t now = micros();

  // 1. watchdog, then panic requests from core 0
  if (lastPass && now - lastPass > WATCHDOG_US) {
    allOff();
    bump(nWatchdog);
    evlog("watchdog: pass %lu ms late, all off", (unsigned long)((now - lastPass) / 1000));
    dropQueue("watchdog");
  }
  lastPass = now;
  // the flash-write hold, read once: while it is in force this pass starts no strike and no tester tap
  uint32_t hw = holdWord;
  bool hold = (hw & 0xFF) != 0;
  uint8_t req;
  const char* src;
  portENTER_CRITICAL(&panicMux);
  req = panicReq;
  src = panicSrc;
  panicReq = 0;
  portEXIT_CRITICAL(&panicMux);
  if (req) doPanic(req & 2, src);

  // 1b. a driver latch that couldn't be zeroed: retry its zeros, and stay muted until they land
  serviceStuck(now);
  if (stuckNew) { stuckNew = false; doPanic(true, "coil may be stuck"); }
  else if (stuckMask && !muted) { muted = true; evlog("muted: a coil may be stuck on, cut 24 V"); }

  // 2. tester off time
  testerService(now);

  // 3. MIDI bytes
  if (midiOn) {
    for (int n = 0; n < 128 && MidiSerial.available() > 0; n++) {
      int c = MidiSerial.read();
      if (c < 0) break;
      midiByte((uint8_t)c, micros());
    }
  }

  // 4. inputs
  uint32_t ms = millis();
  if (btnPin >= 0) btnUpdate(btnPanel, digitalRead(btnPin) == LOW, ms);
  btnUpdate(btnBoot, digitalRead(BOOT_BTN) == LOW, ms);
  serviceTriggers(micros());
  servicePower(ms);
  servicePedals(ms);

  // 5. messages
  Msg m;
  for (int n = 0; n < 32 && netToChimes && xQueueReceive(netToChimes, &m, 0) == pdTRUE; n++) handleMsg(m);

  // 6, 7. demo, sequencer
  now = micros();
  serviceDemo(now);
  serviceSeq(now);
  serviceRec(now);

  // 8. mute / update edge
  bool mNow = muted || updating;
  if (mNow != muteEdge) {
    muteEdge = mNow;
    if (mNow) {
      allOff();
      clearQueue();
      stopDemo();
      stopSeq();
      recDone(false);
      calAbort = true;
      evlog(updating ? "updating: all off, muted" : "muted: all off");
    } else {
      evlog("unmuted");
      vernaSay(V_UNMUTED);
    }
  }

  // 8b. Verna, once, ~3 s after boot (the speaker is up by then); with no 24 V she says that instead
  static bool readySaid = false;
  if (!readySaid && ms - bootMs >= 3000) {
    readySaid = true;
    vernaSay(pwrState == 0 ? V_NO_POWER : V_READY);
  }

  // 9, 10, 11. heat leak and off times, the queue, then the driver writes
  now = micros();
  testerService(now);
  serviceOff(now);
  if (!muted && !updating && !hold) serviceQueue(now);   // under a hold, strikes wait (stale ones dropped later)
  flushOut();
  if (stuckNew) { stuckNew = false; doPanic(true, "coil may be stuck"); }   // flushOut just found one

  // 12. publish what is on (recounted from the flags)
  int c = 0, ma = 0;
  for (int i = 0; i < MAX_OUT; i++) {
    uint8_t o = onFlag[i] ? 1 : 0;
    if (o) { c++; ma += onMaOf[i]; }
    if (outOn[i] != o) outOn[i] = o;
  }
  onCount = c;
  onMa = ma;
  coilsOn = (uint8_t)c;
  maOn = ma;

  // 12b. ack the hold: this whole pass ran under it (nothing started), no coil or tester is on, and every OK
  // board has its zeros written. Core 0 may now write flash.
  if (hold && c == 0 && !tOn && !stuckMask) {   // never while a latch may still be stuck on
    bool sent = true;
    for (uint8_t b = 0; b < MAX_BOARDS; b++) if (bState[b] == B_OK && olat[b] != olatSent[b]) sent = false;
    if (sent) holdAck = hw >> 8;
  }

  // 13. every 3 s, while nothing is on or queued, look again for drivers that aren't OK
  static uint32_t lastAuto = 0;
  if (ms - lastAuto >= 3000) {
    lastAuto = ms;
    bool any = false;
    for (uint8_t b = 0; b < MAX_BOARDS; b++) if (bState[b] != B_OK) any = true;
    if (any && onCount == 0 && qCount == 0 && !tOn) {
      scanBoards(false);
      lastPass = micros();
    }
  }
}
