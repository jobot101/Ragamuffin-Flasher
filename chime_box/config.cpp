// config.cpp: settings, the outputs table and the sequencer patterns, kept in NVS namespace "chime".
// Every NVS access goes through one Preferences object under one mutex. Every write first takes the flash-write
// hold (holdCoils): core 1 then starts nothing, and the write only goes ahead once core 1 says no coil is on.
// A flash write stalls core 1, so a coil on at that moment would stay on; if core 1 doesn't go quiet in time the
// save is aborted and logged, never written anyway. Everything read back is clamped, so a bad value can't reach a coil.
#include "chimes.h"

// ---------------- the globals this file owns ----------------
Cfg cfg;
OutCfg outs[MAX_OUT];
Pattern pats[SEQ_SLOTS];
String staSsid, staPass, apPass, mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;

const int HDR_PINS[] = {9, 10, 11, 12, 13, 14, 21, 38, 39, 42, 44};
const int N_HDR = sizeof(HDR_PINS) / sizeof(HDR_PINS[0]);
const int ADC_PINS[N_ADC] = {9, 10, 11, 12, 13, 14};

// ---------------- round 2 tables ----------------
const char* const ROLE_NAMES[R_COUNT] = {"", "Kick", "Snare", "Hi-Hat", "Open HH", "Rim", "Clap", "Lo Tom", "Hi Tom",
                                        "Cowbell", "Shaker"};
// label, beats, sub (steps per beat group), steps (16th-note steps in one bar)
const TimeSig TIME_SIGS[NUM_TS] = {
  {"4/4", 4, 4, 16}, {"3/4", 3, 4, 12}, {"6/8", 2, 6, 12}, {"5/4", 5, 4, 20},
  {"7/8", 7, 2, 14}, {"9/8", 3, 6, 18}, {"12/8", 4, 6, 24},
};
const char* const PEDAL_ACT_NAMES[P_COUNT] = {"none", "play/stop", "next pattern", "prev pattern", "tap tempo",
                                              "record", "strike", "panic"};

// The round-1 layouts of the "outs" and "pat0..3" blobs, so a board updated from round 1 keeps what the owner saved.
struct OutCfgV1 {
  char     name[12];
  uint8_t  kind, note;
  uint16_t minUs, fullUs, capUs, mA;
};
struct PatternV1 {
  char    name[12];
  uint8_t steps, bpm, swing, loop;
  uint8_t row[SEQ_ROWS];
  uint8_t vel[SEQ_ROWS][SEQ_STEPS];
};
static_assert(sizeof(OutCfgV1) == 22, "round-1 OutCfg was 22 bytes");
static_assert(sizeof(PatternV1) == 12 + 4 + SEQ_ROWS + SEQ_ROWS * SEQ_STEPS, "round-1 Pattern layout");
static_assert(sizeof(OutCfg) != sizeof(OutCfgV1) && sizeof(Pattern) != sizeof(PatternV1),
              "the blob size tells the layouts apart");

// Round 2: every boot-only pin in one write. Not in chimes.h yet (requested); net.cpp declares it the same way.
bool savePinsR2(int midi, int btn, int sol, int inv, int pz, int pwr, const int ped[NUM_PEDALS],
                const int pedInv[NUM_PEDALS]);

static Preferences prefs;
static SemaphoreHandle_t prefsMux = nullptr;   // made in loadCfg, which runs first, before any task

static void lockPrefs() {
  if (!prefsMux) prefsMux = xSemaphoreCreateMutex();
  xSemaphoreTake(prefsMux, portMAX_DELAY);
}
static void unlockPrefs() { xSemaphoreGive(prefsMux); }

// ---------------- the flash-write hold (see chimes.h) ----------------
volatile uint32_t holdWord = 0, holdAck = 0;
static portMUX_TYPE holdMux = portMUX_INITIALIZER_UNLOCKED;

// Take a hold, then wait until core 1 has run a whole pass under it (or a later one) with nothing on.
bool holdCoils(uint32_t timeoutMs) {
  portENTER_CRITICAL(&holdMux);
  uint32_t w = holdWord;
  uint32_t n = (w & 0xFF) < 0xFF ? (w & 0xFF) + 1 : 0xFF;
  uint32_t g = ((w >> 8) + 1) & 0xFFFFFF;
  holdWord = (g << 8) | n;
  portEXIT_CRITICAL(&holdMux);
  uint32_t t0 = millis();
  for (;;) {
    if (((holdAck - g) & 0xFFFFFF) < 0x800000) return true;   // core 1 acked generation g or a later one
    if (millis() - t0 >= timeoutMs) break;
    vTaskDelay(1);
  }
  releaseCoils();
  return false;
}

void releaseCoils() {
  portENTER_CRITICAL(&holdMux);
  uint32_t w = holdWord;
  if (w & 0xFF) holdWord = w - 1;
  portEXIT_CRITICAL(&holdMux);
}

// ---------------- edit tracking ----------------
volatile uint32_t outsRev = 0, patRev[SEQ_SLOTS];
volatile bool     patDirty[SEQ_SLOTS];
static portMUX_TYPE editMux = portMUX_INITIALIZER_UNLOCKED;

void outsEdited() {
  portENTER_CRITICAL(&editMux);
  outsRev = outsRev + 1;
  portEXIT_CRITICAL(&editMux);
}

void patEdited(int slot) {
  if (slot < 0 || slot >= SEQ_SLOTS) return;
  portENTER_CRITICAL(&editMux);
  patRev[slot] = patRev[slot] + 1;
  patDirty[slot] = true;
  portEXIT_CRITICAL(&editMux);
}

// Open for writing: take the mutex, then the hold. False (and nothing written) if core 1 didn't go quiet in time.
// Taking the mutex first means a second saver that waited for the first one still gets its own quiet check.
static bool beginWrite() {
  lockPrefs();
  if (!holdCoils(HOLD_WAIT_MS)) {
    unlockPrefs();
    evlog("NVS: coils not quiet in %lu ms, NOT saved", (unsigned long)HOLD_WAIT_MS);
    return false;
  }
  if (prefs.begin("chime", false)) return true;
  releaseCoils();
  unlockPrefs();
  evlog("NVS: open failed, not saved");
  return false;
}
static void endWrite() { prefs.end(); releaseCoils(); unlockPrefs(); }   // end() commits, still under the hold

// Reads that keep the default (and the log quiet) when a key isn't there yet.
static int    gInt(const char* k, int d)       { return prefs.isKey(k) ? prefs.getInt(k, d) : d; }
static float  gFloat(const char* k, float d)   { return prefs.isKey(k) ? prefs.getFloat(k, d) : d; }
static String gStr(const char* k)              { return prefs.isKey(k) ? prefs.getString(k, "") : String(); }

static bool putBlob(const char* k, const void* p, size_t n) {
  if (prefs.putBytes(k, p, n) == n) return true;
  evlog("NVS: %s not saved", k);
  return false;
}

// ---------------- clamping ----------------
static void clampCfg() {          // the live settings core 0 may change (port and trig belong to core 1)
  cfg.capUs    = constrain(cfg.capUs, 3000, (int)HARD_CAP_US);
  cfg.supplyMa = constrain(cfg.supplyMa, 100, 20000);
  cfg.midiCh   = constrain(cfg.midiCh, 0, 16);
  if (!(cfg.gamma >= 0.3f && cfg.gamma <= 4.0f)) cfg.gamma = 1.6f;   // also catches NaN
  cfg.fold     = cfg.fold ? 1 : 0;
  cfg.latUs    = constrain(cfg.latUs, 0, 30000);
  cfg.solo     = constrain(cfg.solo, 0, (int)MAX_OUT);
  cfg.solMs    = constrain(cfg.solMs, SOL_MS_MIN, SOL_MS_MAX);
  cfg.voice    = cfg.voice ? 1 : 0;
  cfg.voiceVol = constrain(cfg.voiceVol, 0, 100);
  cfg.cc64Act  = constrain(cfg.cc64Act, 0, (int)P_COUNT - 1);
  for (int k = 0; k < NUM_PEDALS; k++) {
    if (cfg.pedalAct[k] >= P_COUNT) cfg.pedalAct[k] = P_NONE;
    if (cfg.pedalOut[k] >= MAX_OUT) cfg.pedalOut[k] = 0;
  }
  if (curSlot < 0 || curSlot >= SEQ_SLOTS) curSlot = 0;
}

// Wear threshold (thousands of strikes) an output of this kind starts with: tubes 250, small solenoids and the rest 100.
static uint16_t defaultWearK(uint8_t kind) { return kind == K_TUBE ? 250 : 100; }

static void clampOut(OutCfg& o) {
  o.name[sizeof(o.name) - 1] = 0;
  if (o.kind > K_OTHER) o.kind = K_OFF;
  if (o.note > 127) o.note = 0;
  o.minUs  = constrain(o.minUs, (uint16_t)MIN_PULSE_US, (uint16_t)HARD_CAP_US);
  o.fullUs = constrain(o.fullUs, o.minUs, (uint16_t)HARD_CAP_US);
  o.capUs  = constrain(o.capUs, (uint16_t)MIN_PULSE_US, (uint16_t)HARD_CAP_US);
  if (o.mA > 3000) o.mA = 3000;
  if (o.kind != K_OFF && o.mA < MIN_COIL_MA) o.mA = MIN_COIL_MA;   // a live coil always counts against the supply
  if (o.role >= R_COUNT) o.role = R_NONE;
  o.spare = 0;
}

static void clampPattern(Pattern& p) {
  p.name[sizeof(p.name) - 1] = 0;
  p.steps = constrain(p.steps, (uint8_t)1, (uint8_t)SEQ_STEPS);
  p.bpm   = constrain(p.bpm, (uint8_t)30, (uint8_t)240);
  if (p.swing > 75) p.swing = 75;
  p.loop  = p.loop ? 1 : 0;
  if (p.ts >= NUM_TS) p.ts = 0;
  for (int r = 0; r < SEQ_ROWS; r++) {
    if (p.row[r] >= MAX_OUT) p.row[r] = SEQ_NONE;
    if (p.rowRole[r] >= R_COUNT) p.rowRole[r] = R_NONE;
    for (int c = 0; c < SEQ_STEPS; c++) if (p.vel[r][c] > 127) p.vel[r][c] = 127;
  }
}

// ---------------- pins ----------------
bool hdrPin(int p) { for (int i = 0; i < N_HDR; i++) if (HDR_PINS[i] == p) return true; return false; }
bool adcPin(int p) { for (int i = 0; i < N_ADC; i++) if (ADC_PINS[i] == p) return true; return false; }

// One job per header pin, decided in this order: tester, MIDI, button, piezo. The tester comes first because its
// pin may be wired to a MOSFET gate: a later role there would put a pull-up (button) or the UART RX pull-up (MIDI)
// on that gate and hold the solenoid on. So an old 'spin' of 10 or 14 (the one-solenoid firmware allowed both)
// keeps the tester, and the new default button G10 / MIDI G14 gives way. A clash or a pin that isn't allowed turns
// the later role off (MIDI moves to its other pin instead), and it is logged.
// A pin an earlier role already holds: tester, MIDI, button, piezo; for pedal k (>= 0) also the power sense pin and
// pedals 0..k-1. -1 never matches (p >= 0 here).
static bool pinHeld(int p, int pedal) {
  if (p == cfg.solPin || p == cfg.midiPin || p == cfg.btnPin || p == cfg.pzPin) return true;
  if (pedal < 0) return false;
  if (p == cfg.pwrPin) return true;
  for (int k = 0; k < pedal && k < NUM_PEDALS; k++) if (p == cfg.pedalPin[k]) return true;
  return false;
}

static void checkPins() {
  if (cfg.solPin < -1) cfg.solPin = -1;
  if (cfg.btnPin < -1) cfg.btnPin = -1;
  if (cfg.pzPin < -1)  cfg.pzPin = -1;
  if (cfg.solPin >= 0 && !hdrPin(cfg.solPin)) { evlog("tester pin %d not allowed, tester off", cfg.solPin); cfg.solPin = -1; }
  if (cfg.midiPin != 14 && cfg.midiPin != 44) { evlog("MIDI pin %d not allowed, using 14", cfg.midiPin); cfg.midiPin = 14; }
  if (cfg.midiPin == cfg.solPin) {
    int other = cfg.midiPin == 14 ? 44 : 14;
    evlog("MIDI pin %d is the tester pin, MIDI on %d", cfg.midiPin, other);
    cfg.midiPin = other;
  }
  if (cfg.btnPin >= 0 && (!hdrPin(cfg.btnPin) || cfg.btnPin == cfg.midiPin || cfg.btnPin == cfg.solPin)) {
    evlog("button pin %d not allowed or in use, button off", cfg.btnPin); cfg.btnPin = -1;
  }
  if (cfg.pzPin >= 0 && (!adcPin(cfg.pzPin) || cfg.pzPin == cfg.midiPin || cfg.pzPin == cfg.btnPin ||
                         cfg.pzPin == cfg.solPin)) {
    evlog("piezo pin %d not allowed or in use, piezo off", cfg.pzPin); cfg.pzPin = -1;
  }
  cfg.solInv = cfg.solInv ? 1 : 0;
  // Round 2, after the four above: the 24 V sense pin, then the foot pedals in order. Each must be a header pin
  // no earlier role holds (pedals also never share a pin with each other); otherwise that one is turned off.
  if (cfg.pwrPin < -1) cfg.pwrPin = -1;
  // never G9: the tester MOSFET may still be wired there even with the tester off, and a pull-up would hold it on
  if (cfg.pwrPin >= 0 && (!hdrPin(cfg.pwrPin) || cfg.pwrPin == 9 || pinHeld(cfg.pwrPin, -1))) {
    evlog("power sense pin %d not allowed or in use, power sense off", cfg.pwrPin); cfg.pwrPin = -1;
  }
  for (int k = 0; k < NUM_PEDALS; k++) {
    int& p = cfg.pedalPin[k];
    if (p < -1) p = -1;
    if (p >= 0 && (!hdrPin(p) || p == 9 || pinHeld(p, k))) { evlog("pedal %d pin %d not allowed or in use, pedal off", k + 1, p); p = -1; }
    cfg.pedalInv[k] = constrain(cfg.pedalInv[k], 0, 2);
  }
}

// ---------------- defaults ----------------
String defaultApPass() {           // "chimes" + the last two MAC bytes, e.g. "chimes3fa0"
  uint64_t mac = ESP.getEfuseMac();   // byte 0 of the MAC is the low byte
  char b[16];
  snprintf(b, sizeof(b), "chimes%02x%02x", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  return String(b);
}

// Built in a local copy, then copied in whole, so core 1 never sees a half-set entry.
void outDefaults(int i) {
  if (i < 0 || i >= MAX_OUT) return;
  OutCfg o;
  memset(&o, 0, sizeof(o));
  if (i < NUM_TUBES) {             // outputs 1..25: tubes T1..T25, C3..C5
    snprintf(o.name, sizeof(o.name), "T%d", i + 1);
    o.kind = K_TUBE; o.note = LOW_NOTE + i;
    o.minUs = 3000; o.fullUs = i < 5 ? 20500 : 18000; o.capUs = 30000; o.mA = 400;
    o.wearK = defaultWearK(K_TUBE);
  } else {                         // 26..64: off until the owner adds a small solenoid or attachment
    o.kind = K_OFF; o.note = 0;
    o.minUs = 2000; o.fullUs = 8000; o.capUs = 15000; o.mA = 200;
    o.wearK = defaultWearK(K_SOL);   // the small-solenoid default, for when one is turned on
  }
  o.role = R_NONE;
  memcpy(&outs[i], &o, sizeof(o));
}

void patternDefaults(int slot) {
  if (slot < 0 || slot >= SEQ_SLOTS) return;
  Pattern p;
  memset(&p, 0, sizeof(p));
  memset(p.row, SEQ_NONE, sizeof(p.row));
  memset(p.rowRole, R_NONE, sizeof(p.rowRole));
  p.steps = 16; p.bpm = 100; p.swing = 0; p.loop = 1; p.ts = 0;   // 4/4
  if (slot == 0) {                 // a simple beat on outputs 26..28 (off by default, so silent until enabled)
    strlcpy(p.name, "Beat", sizeof(p.name));
    p.row[0] = 25; p.row[1] = 26; p.row[2] = 27;          // kick, snare, hat
    p.vel[0][0] = 127; p.vel[0][8] = 127; p.vel[0][10] = 100;
    p.vel[1][4] = 110; p.vel[1][12] = 110;
    for (int c = 0; c < 16; c += 2) p.vel[2][c] = 60;
  } else if (slot == 1) {          // ascending walk up the tubes, played once
    static const uint8_t walk[] = {1, 5, 8, 13, 17, 20, 25};
    strlcpy(p.name, "Chime walk", sizeof(p.name));
    p.loop = 0;
    for (int r = 0; r < (int)sizeof(walk); r++) { p.row[r] = walk[r] - 1; p.vel[r][r * 2] = 100; }
  } else {
    snprintf(p.name, sizeof(p.name), "Pattern %d", slot + 1);
  }
  memcpy(&pats[slot], &p, sizeof(p));
}

// ---------------- world-rhythm presets (core 0) ----------------
// Fills pats[slot] from PRESETS[preset]: name, time signature, bpm, swing and steps (the slot's loop setting stays).
// Each preset row goes to the first output that isn't off and has that role; with none, the row stays unassigned
// with its role label kept, so the owner sees "Kick: not assigned". Levels 1/2/3 -> 60/100/127. Not saved here.
void loadPreset(int slot, int preset) {
  if (slot < 0 || slot >= SEQ_SLOTS || preset < 0 || preset >= NUM_PRESETS) return;
  const DrumPreset& d = PRESETS[preset];
  Pattern p;
  memset(&p, 0, sizeof(p));
  memset(p.row, SEQ_NONE, sizeof(p.row));
  memset(p.rowRole, R_NONE, sizeof(p.rowRole));
  strlcpy(p.name, d.name ? d.name : "", sizeof(p.name));
  p.ts    = d.ts < NUM_TS ? d.ts : 0;
  p.bpm   = d.bpm;
  p.swing = d.swing;
  p.loop  = pats[slot].loop;
  int st  = d.steps ? d.steps : TIME_SIGS[p.ts].steps;
  p.steps = (uint8_t)constrain(st, 1, (int)SEQ_STEPS);
  int rows = d.rows;
  if (rows > (int)sizeof(d.role)) rows = sizeof(d.role);
  if (rows > SEQ_ROWS) rows = SEQ_ROWS;
  int assigned = 0;
  for (int r = 0; r < rows; r++) {
    uint8_t role = d.role[r] < R_COUNT ? d.role[r] : (uint8_t)R_NONE;
    p.rowRole[r] = role;
    if (role != R_NONE)
      for (int i = 0; i < MAX_OUT; i++)
        if (outs[i].kind != K_OFF && outs[i].role == role) { p.row[r] = (uint8_t)i; assigned++; break; }
    if (!d.lv) continue;
    for (int c = 0; c < p.steps && c < d.steps; c++) {
      uint8_t lv = d.lv[r * d.steps + c];
      p.vel[r][c] = lv == 0 ? 0 : lv == 1 ? 60 : lv == 2 ? 100 : 127;
    }
  }
  clampPattern(p);
  memcpy(&pats[slot], &p, sizeof(p));   // byte copies only: core 1 may be playing it (single-byte fields)
  patEdited(slot);
  evlog("pattern %d: preset \"%s\" loaded, %d of %d rows assigned", slot + 1, p.name, assigned, rows);
}

// ---------------- load (setup, core 1, before any task; read only, so no flash write) ----------------
// Round-1 blobs, read into the old layout and copied over; the new fields get their defaults.
static bool migrateOuts() {
  OutCfgV1* old = (OutCfgV1*)malloc(sizeof(OutCfgV1) * MAX_OUT);
  if (!old) return false;
  bool ok = prefs.getBytes("outs", old, sizeof(OutCfgV1) * MAX_OUT) == sizeof(OutCfgV1) * MAX_OUT;
  if (ok) {
    for (int i = 0; i < MAX_OUT; i++) {
      OutCfg o;
      memset(&o, 0, sizeof(o));
      memcpy(o.name, old[i].name, sizeof(o.name));
      o.kind = old[i].kind; o.note = old[i].note;
      o.minUs = old[i].minUs; o.fullUs = old[i].fullUs; o.capUs = old[i].capUs; o.mA = old[i].mA;
      o.wearK = defaultWearK(o.kind);
      o.role = R_NONE;
      memcpy(&outs[i], &o, sizeof(o));
    }
  }
  free(old);
  return ok;
}

static bool migratePattern(const char* k, int slot) {
  PatternV1* old = (PatternV1*)malloc(sizeof(PatternV1));
  if (!old) return false;
  bool ok = prefs.getBytes(k, old, sizeof(PatternV1)) == sizeof(PatternV1);
  if (ok) {
    Pattern p;
    memset(&p, 0, sizeof(p));
    memcpy(p.name, old->name, sizeof(p.name));
    p.steps = old->steps; p.bpm = old->bpm; p.swing = old->swing; p.loop = old->loop;
    memcpy(p.row, old->row, sizeof(p.row));
    memcpy(p.vel, old->vel, sizeof(p.vel));
    p.ts = 0;   // 4/4: round 1 had no time signatures
    memset(p.rowRole, R_NONE, sizeof(p.rowRole));
    memcpy(&pats[slot], &p, sizeof(p));
  }
  free(old);
  return ok;
}

void loadCfg() {
  lockPrefs();
  if (!prefs.begin("chime", true)) {    // read only: this fails when nothing was ever saved
    unlockPrefs();
    for (int i = 0; i < MAX_OUT; i++) outDefaults(i);
    for (int s = 0; s < SEQ_SLOTS; s++) patternDefaults(s);
    checkPins();
    apPass = defaultApPass();
    evlog("NVS: nothing saved yet, defaults");
    return;
  }

  staSsid  = gStr("ssid");   staPass  = gStr("wpass");
  mqttHost = gStr("mhost");  mqttUser = gStr("muser");  mqttPass = gStr("mpass");
  mqttPort = prefs.isKey("mport") ? prefs.getUShort("mport", 1883) : 1883;
  if (!mqttPort) mqttPort = 1883;
  apPass   = gStr("appass");

  cfg.capUs    = gInt("cap", cfg.capUs);
  cfg.supplyMa = gInt("supma", cfg.supplyMa);
  cfg.portB    = gInt("port", cfg.portB) ? 1 : 0;
  cfg.midiCh   = gInt("mch", cfg.midiCh);
  cfg.gamma    = gFloat("gam", cfg.gamma);
  cfg.fold     = gInt("fold", cfg.fold);
  cfg.latUs    = gInt("lat", cfg.latUs);
  cfg.solo     = gInt("solo", cfg.solo);
  cfg.trig     = gInt("trig", cfg.trig) ? 1 : 0;
  cfg.solMs    = gInt("sms", cfg.solMs);
  cfg.midiPin  = gInt("midip", cfg.midiPin);
  cfg.btnPin   = gInt("btnp", cfg.btnPin);
  cfg.solPin   = gInt("spin", cfg.solPin);
  cfg.solInv   = prefs.isKey("sinv") ? prefs.getBool("sinv", false) : cfg.solInv;
  cfg.pzPin    = gInt("pzpin", cfg.pzPin);
  // round 2
  cfg.voice    = gInt("voice", cfg.voice);
  cfg.voiceVol = gInt("vvol", cfg.voiceVol);
  cfg.cc64Act  = gInt("cc64a", cfg.cc64Act);
  cfg.pwrPin   = gInt("pwrp", cfg.pwrPin);
  for (int k = 0; k < NUM_PEDALS; k++) {
    char key[8];
    snprintf(key, sizeof(key), "ped%d", k); cfg.pedalPin[k] = gInt(key, cfg.pedalPin[k]);
    snprintf(key, sizeof(key), "pda%d", k); cfg.pedalAct[k] = (uint8_t)constrain(gInt(key, cfg.pedalAct[k]), 0, (int)P_COUNT - 1);
    snprintf(key, sizeof(key), "pdo%d", k); cfg.pedalOut[k] = (uint8_t)constrain(gInt(key, cfg.pedalOut[k]), 0, (int)MAX_OUT - 1);
    snprintf(key, sizeof(key), "pdi%d", k); cfg.pedalInv[k] = gInt(key, cfg.pedalInv[k]);
  }
  curSlot = gInt("cur", 0);

  size_t on = prefs.getBytesLength("outs");
  if (on == sizeof(outs)) prefs.getBytes("outs", outs, sizeof(outs));
  else if (on == sizeof(OutCfgV1) * MAX_OUT && migrateOuts()) evlog("NVS: outputs kept from the round-1 layout");
  else {
    for (int i = 0; i < MAX_OUT; i++) outDefaults(i);
    if (on) evlog("NVS: outputs blob is %u bytes, not a known layout: defaults", (unsigned)on);
  }

  for (int s = 0; s < SEQ_SLOTS; s++) {
    char k[8];
    snprintf(k, sizeof(k), "pat%d", s);
    size_t pn = prefs.getBytesLength(k);
    if (pn == sizeof(Pattern)) prefs.getBytes(k, &pats[s], sizeof(Pattern));
    else if (pn == sizeof(PatternV1) && migratePattern(k, s)) evlog("NVS: pattern %d kept from the round-1 layout", s + 1);
    else {
      patternDefaults(s);
      if (pn) evlog("NVS: pattern %d blob is %u bytes, not a known layout: defaults", s + 1, (unsigned)pn);
    }
  }

  if (prefs.getBytesLength("wear") == sizeof(uint32_t) * MAX_OUT) {   // lifetime strikes per output
    uint32_t w[MAX_OUT];
    if (prefs.getBytes("wear", w, sizeof(w)) == sizeof(w))
      for (int i = 0; i < MAX_OUT; i++) wearCount[i] = w[i];
  }
  prefs.end();
  unlockPrefs();

  clampCfg();
  checkPins();
  for (int i = 0; i < MAX_OUT; i++) clampOut(outs[i]);
  for (int s = 0; s < SEQ_SLOTS; s++) clampPattern(pats[s]);
  if (apPass.length() < 8 || apPass.length() > 63) apPass = defaultApPass();
}

// Core 0, once after boot (core 1 is running, so the hold works): the old firmware's piezo keys go.
void nvsTidy() {
  lockPrefs();
  bool stale = false;
  if (prefs.begin("chime", true)) {
    stale = prefs.isKey("ppin") || prefs.isKey("pthr");
    prefs.end();
  }
  unlockPrefs();
  if (!stale || !beginWrite()) return;
  if (prefs.isKey("ppin")) prefs.remove("ppin");
  if (prefs.isKey("pthr")) prefs.remove("pthr");
  endWrite();
  evlog("NVS: old piezo keys removed");
}

// ---------------- saves (core 0) ----------------
bool saveCfg() {
  clampCfg();
  if (!beginWrite()) return false;
  prefs.putInt("cap", cfg.capUs);
  prefs.putInt("supma", cfg.supplyMa);
  prefs.putInt("port", cfg.portB ? 1 : 0);
  prefs.putInt("mch", cfg.midiCh);
  prefs.putFloat("gam", cfg.gamma);
  prefs.putInt("fold", cfg.fold);
  prefs.putInt("lat", cfg.latUs);
  prefs.putInt("solo", cfg.solo);
  prefs.putInt("trig", cfg.trig ? 1 : 0);
  prefs.putInt("sms", cfg.solMs);
  prefs.putInt("voice", cfg.voice);
  prefs.putInt("vvol", cfg.voiceVol);
  prefs.putInt("cc64a", cfg.cc64Act);
  for (int k = 0; k < NUM_PEDALS; k++) {
    char key[8];
    snprintf(key, sizeof(key), "pda%d", k); prefs.putInt(key, cfg.pedalAct[k]);
    snprintf(key, sizeof(key), "pdo%d", k); prefs.putInt(key, cfg.pedalOut[k]);
  }
  prefs.putInt("cur", curSlot);
  endWrite();
  return true;
}

bool savePins(int midi, int btn, int sol, int inv, int pz) {   // the round-2 pins stay as they are
  return savePinsR2(midi, btn, sol, inv, pz, cfg.pwrPin, cfg.pedalPin, cfg.pedalInv);
}

// Every boot-only pin in one write (validated by the caller; applied at the next boot, and loadCfg checks again).
bool savePinsR2(int midi, int btn, int sol, int inv, int pz, int pwr, const int ped[NUM_PEDALS],
                const int pedInv[NUM_PEDALS]) {
  if (!beginWrite()) return false;
  prefs.putInt("midip", midi);
  prefs.putInt("btnp", btn);
  prefs.putInt("spin", sol);
  prefs.putBool("sinv", inv != 0);
  prefs.putInt("pzpin", pz);
  prefs.putInt("pwrp", pwr);
  for (int k = 0; k < NUM_PEDALS; k++) {
    char key[8];
    snprintf(key, sizeof(key), "ped%d", k); prefs.putInt(key, ped[k]);
    snprintf(key, sizeof(key), "pdi%d", k); prefs.putInt(key, constrain(pedInv[k], 0, 2));
  }
  endWrite();
  return true;
}

// Lifetime strike counts, blob "wear". The snapshot is taken under the hold, so no strike lands mid-copy.
bool saveWear() {
  if (!beginWrite()) return false;
  uint32_t w[MAX_OUT];
  for (int i = 0; i < MAX_OUT; i++) w[i] = wearCount[i];
  wearDirty = false;
  bool ok = putBlob("wear", w, sizeof(w));
  endWrite();
  if (!ok) wearDirty = true;
  return ok;
}

bool saveOuts() {
  for (int i = 0; i < MAX_OUT; i++) clampOut(outs[i]);
  if (!beginWrite()) return false;
  bool ok = putBlob("outs", outs, sizeof(outs));
  endWrite();
  return ok;
}

bool savePattern(int slot) {
  if (slot < 0 || slot >= SEQ_SLOTS) return false;
  clampPattern(pats[slot]);
  char k[8];
  snprintf(k, sizeof(k), "pat%d", slot);
  patDirty[slot] = false;          // cleared first: an edit during the write marks it dirty again
  if (!beginWrite()) { patDirty[slot] = true; return false; }
  bool ok = putBlob(k, &pats[slot], sizeof(Pattern));
  endWrite();
  if (!ok) patDirty[slot] = true;
  return ok;
}

void saveWifi(const String& ssid, const String& pass) {
  staSsid = ssid; staSsid.trim();
  staPass = pass;
  if (beginWrite()) {
    prefs.putString("ssid", staSsid);
    prefs.putString("wpass", staPass);
    endWrite();
  }
  if (staSsid.length()) {          // join now; net.cpp's pacing carries on from here
    WiFi.disconnect();
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    evlog("hall WiFi set: %s", staSsid.c_str());
  } else {
    WiFi.disconnect(false, true);  // forget
    evlog("hall WiFi forgotten");
  }
}

bool saveMqtt() {
  mqttHost.trim();
  if (!mqttPort) mqttPort = 1883;
  if (!beginWrite()) return false;
  prefs.putString("mhost", mqttHost);
  prefs.putUShort("mport", mqttPort);
  prefs.putString("muser", mqttUser);
  prefs.putString("mpass", mqttPass);
  endWrite();
  return true;
}

void saveApPass(const String& p) {   // "" = back to the default; 8..63 characters, else kept as it was
  if (p.length() && (p.length() < 8 || p.length() > 63)) { evlog("own WiFi password must be 8-63 characters"); return; }
  if (!beginWrite()) return;
  if (p.length()) prefs.putString("appass", p);
  else if (prefs.isKey("appass")) prefs.remove("appass");
  endWrite();
  apPass = p.length() ? p : defaultApPass();
}
