// cal.cpp: the calibration / tuning task (core 0, priority 1). Spec: DESIGN.md "cal.cpp".
// Sweep, tune, piezo pin scan and walk. This task never writes a coil: it only queues strikes with sendMsg(),
// and core 1 still clamps, budgets and heat-guards every one. Panic, mute or /calstop set calAbort; every wait
// here checks it (and muted / updating) every 5 ms, so a run stops within ~10 ms.
#include "chimes.h"
#include <math.h>

volatile int  calCmd = CAL_IDLE, calOut = 0, calState = CAL_IDLE;
volatile bool calAbort = false;
Step sweepSteps[SWEEP_MAX];
volatile int  sweepN = 0, sweepOut = -1;
volatile uint32_t sweepCur = 0;
char calMsg[64] = "";
volatile int  tuneOut = -1, tuneCents = 0, tuneOk = 0;
volatile float tuneHz = 0;
char tuneNote[8] = "";
volatile uint16_t pzPeak[N_ADC];
volatile int pzDone = 0;
// Walk settings, set by net.cpp (/walk?v=&gap=, serial "walk [vel] [gap]") just before it starts a walk; back to
// the defaults after each walk, so a walk started without them (the screen) uses 100 / 450 ms.
// Not in chimes.h yet (requested): net.cpp declares them extern.
constexpr int WALK_VEL = 100, WALK_GAP_MS = 450;
volatile int walkVel = WALK_VEL, walkGapMs = WALK_GAP_MS;

bool calBusy() { return calState != CAL_IDLE || calCmd != CAL_IDLE; }

// ---------------- timing helpers ----------------
static bool stopNow() { return calAbort || muted || updating; }

static const char* whyStop() {
  if (updating) return "updating";
  if (muted) return "muted";
  return "stopped";
}

static void sleepMs(uint32_t ms) {
  TickType_t t = pdMS_TO_TICKS(ms);
  vTaskDelay(t ? t : 1);
}

// Wait ms, checking every 5 ms. False if the run should stop.
static bool waitMs(uint32_t ms) {
  uint32_t t0 = millis();
  for (;;) {
    if (stopNow()) return false;
    uint32_t el = millis() - t0;
    if (el >= ms) return true;
    uint32_t left = ms - el;
    sleepMs(left < 5 ? left : 5);
  }
}

static bool piezoSet() { return cfg.pzPin >= 0 && adcPin(cfg.pzPin); }

// A pin with another job (MIDI, button, tester, 24 V sense, a foot pedal): the piezo scan never reads it.
static bool pinInUse(int p) {
  if (p == cfg.midiPin || p == cfg.btnPin || p == cfg.solPin || p == cfg.pwrPin) return true;
  for (int k = 0; k < NUM_PEDALS; k++) if (p == cfg.pedalPin[k]) return true;
  return false;
}

// Piezo peak-to-peak over ms, yielding every 10 ms.
static uint16_t readPeak(int pin, uint32_t ms) {
  int lo = 65535, hi = 0;
  uint32_t t0 = millis(), ty = t0;
  while (millis() - t0 < ms) {
    int v = analogRead(pin);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    if (millis() - ty >= 10) {
      if (stopNow()) break;
      vTaskDelay(1);
      ty = millis();
    }
  }
  return hi > lo ? (uint16_t)(hi - lo) : 0;
}

// ---------------- sweep ----------------
static void runSweep(int i) {
  sweepN = 0;
  sweepCur = 0;
  if (i < 0 || i >= MAX_OUT) {
    snprintf(calMsg, sizeof calMsg, "Sweep: no such output");
    evlog("cal: sweep refused, bad output %d", i + 1);
    return;
  }
  sweepOut = i;
  if (muted || updating) {
    snprintf(calMsg, sizeof calMsg, "Sweep: unmute first");
    evlog("cal: sweep out %d refused, %s", i + 1, whyStop());
    return;
  }
  if (!outUsable(i)) {
    snprintf(calMsg, sizeof calMsg, "Sweep: output %d is off or its board is missing", i + 1);
    evlog("cal: sweep out %d refused, not usable", i + 1);
    return;
  }
  bool pz = piezoSet();
  uint32_t cap = outCap(i);
  // Tubes start at 3 ms. A small solenoid (or anything whose softest is already under 3 ms) starts at the 1 ms
  // floor, so its real softest pulse can be found; with a short cap it steps 0.5 ms (both fit SWEEP_MAX).
  bool small = outs[i].kind == K_SOL || outs[i].minUs < 3000;
  uint32_t us = small ? MIN_PULSE_US : 3000;
  if (cap < us) us = cap;
  uint32_t step = (small && (cap - us) / 500 + 1 <= (uint32_t)SWEEP_MAX) ? 500 : 1000;
  int total = (int)((cap - us) / step) + 1;
  if (total > SWEEP_MAX) total = SWEEP_MAX;
  evlog("cal: sweep out %d (%s) %.1f..%.1f ms by %.1f%s", i + 1, outLabel(i).c_str(), us / 1000.0, cap / 1000.0,
        step / 1000.0, pz ? "" : ", no piezo");
  vernaSay(V_CAL_MIN);           // "listen for the first clean strike, then tap min"
  bool done = true;
  for (; us <= cap && sweepN < SWEEP_MAX; us += step) {
    if (stopNow()) { done = false; break; }
    sweepCur = us;
    snprintf(calMsg, sizeof calMsg, "Sweep out %d: %.1f ms (%d of %d)%s", i + 1, us / 1000.0,
             (int)sweepN + 1, total, pz ? "" : ", listen");
    sendMsg(M_EXACT, (uint8_t)i, 100, us);
    if (!waitMs((uint32_t)cfg.latUs / 1000 + 25)) { done = false; break; }
    uint16_t pk = pz ? readPeak(cfg.pzPin, 150) : 0;
    int n = sweepN;
    sweepSteps[n].us = (uint16_t)us;
    sweepSteps[n].peak = pk;
    sweepN = n + 1;              // the step is complete before it is counted
    if (!waitMs(550)) { done = false; break; }
  }
  sweepCur = 0;
  if (done) {
    snprintf(calMsg, sizeof calMsg, "Sweep out %d done: %d steps", i + 1, (int)sweepN);
    evlog("cal: sweep out %d done, %d steps", i + 1, (int)sweepN);
    vernaSay(V_CAL_DONE);        // V_CAL_FULL follows from net.cpp when min is set for this output
  } else {
    snprintf(calMsg, sizeof calMsg, "Sweep out %d %s after %d steps", i + 1, whyStop(), (int)sweepN);
    evlog("cal: sweep out %d %s after %d steps", i + 1, whyStop(), (int)sweepN);
  }
}

// ---------------- tune (YIN pitch, pitch class only) ----------------
// 4096 samples at about 16 kHz, taken as 8 blocks of 512. Each block is one gap-free run (checked by its
// timestamps, retaken if a gap shows up); the YIN difference is summed within each block, so the short
// yields between blocks never land inside a lag.
static constexpr int TUNE_N = 4096, TBLK = 512, NBLK = TUNE_N / TBLK;
static constexpr uint32_t TPER_US = 62;        // ~16.1 kHz
static constexpr uint32_t TGAP_US = 4 * TPER_US;
static constexpr int TAU_MIN = 8, TAU_MAX = 200;
static int16_t smp[TUNE_N];                    // static: too big for the stack
static float   yd[TAU_MAX + 1];
static const char* const PC[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// One block. Runs at priority 2 so the other core-0 tasks at priority 1 can't cut into it (~32 ms).
static bool grabBlock(int pin, int16_t* p, float* rate) {
  UBaseType_t pr = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, pr + 1);
  uint32_t first = micros(), next = first, last = first;
  bool gap = false;
  for (int k = 0; k < TBLK; k++) {
    while ((int32_t)(micros() - next) < 0) {}
    uint32_t now = micros();
    if (k == 0) first = now;
    else if (now - last > TGAP_US) gap = true;
    last = now;
    p[k] = (int16_t)analogRead(pin);
    next += TPER_US;
  }
  vTaskPrioritySet(nullptr, pr);
  uint32_t span = last - first;
  *rate = span ? (float)(TBLK - 1) * 1e6f / (float)span : 0.0f;
  return !gap && span > 0;
}

static void tuneFail(int i, const char* why) {
  tuneOk = 0;
  tuneHz = 0;
  snprintf(calMsg, sizeof calMsg, "Tune out %d: %s", i + 1, why);
  evlog("cal: tune out %d: %s", i + 1, why);
}

static void runTune(int i) {
  tuneOut = i;
  tuneOk = 0;
  tuneHz = 0;
  tuneCents = 0;
  tuneNote[0] = 0;
  if (i < 0 || i >= MAX_OUT) {
    snprintf(calMsg, sizeof calMsg, "Tune: no such output");
    evlog("cal: tune refused, bad output %d", i + 1);
    return;
  }
  if (!piezoSet()) { tuneFail(i, "no piezo pin set"); return; }
  if (outs[i].note == 0) { tuneFail(i, "it has no note"); return; }
  if (muted || updating) { tuneFail(i, "unmute first"); return; }
  if (!outUsable(i)) { tuneFail(i, "off or its board is missing"); return; }
  int pin = cfg.pzPin;

  snprintf(calMsg, sizeof calMsg, "Tune out %d: striking", i + 1);
  sendMsg(M_OUT, (uint8_t)i, 100, 0);
  if (!waitMs((uint32_t)cfg.latUs / 1000 + 40)) { tuneFail(i, whyStop()); return; }

  snprintf(calMsg, sizeof calMsg, "Tune out %d: listening", i + 1);
  bool good[NBLK];
  int nGood = 0;
  float rateSum = 0;
  for (int b = 0; b < NBLK; b++) {
    good[b] = false;
    for (int t = 0; t < 3 && !good[b]; t++) {
      if (stopNow()) { tuneFail(i, whyStop()); return; }
      float r = 0;
      good[b] = grabBlock(pin, smp + b * TBLK, &r);
      if (good[b]) { nGood++; rateSum += r; }
      sleepMs(2);
    }
  }
  if (nGood < 2) { tuneFail(i, "sampling kept getting cut, try again"); return; }
  float rate = rateSum / (float)nGood;

  // Peak-to-peak, then remove DC per block.
  int lo = 65535, hi = 0;
  for (int b = 0; b < NBLK; b++) {
    if (!good[b]) continue;
    int16_t* p = smp + b * TBLK;
    int32_t sum = 0;
    for (int k = 0; k < TBLK; k++) {
      if (p[k] < lo) lo = p[k];
      if (p[k] > hi) hi = p[k];
      sum += p[k];
    }
    int16_t mean = (int16_t)(sum / TBLK);
    for (int k = 0; k < TBLK; k++) p[k] = (int16_t)(p[k] - mean);
  }
  if (hi - lo < 40) {
    char m[40];
    snprintf(m, sizeof m, "no signal (p-p %d)", hi > lo ? hi - lo : 0);
    tuneFail(i, m);
    vernaSay(V_TUNE_NO_SIGNAL);
    return;
  }

  // YIN difference, each lag averaged over the pairs inside each good block.
  for (int tau = 0; tau <= TAU_MAX; tau++) yd[tau] = 0;
  for (int b = 0; b < NBLK; b++) {
    if (!good[b]) continue;
    const int16_t* p = smp + b * TBLK;
    for (int tau = 1; tau <= TAU_MAX; tau++) {
      float s = 0;
      int n = TBLK - tau;
      for (int j = 0; j < n; j++) {
        int32_t d = (int32_t)p[j] - (int32_t)p[j + tau];
        s += (float)(d * d);
      }
      yd[tau] += s / (float)n;
    }
    if (stopNow()) { tuneFail(i, whyStop()); return; }
    vTaskDelay(1);
  }
  // Cumulative mean normalised difference, in place.
  float run = 0;
  yd[0] = 1.0f;
  for (int tau = 1; tau <= TAU_MAX; tau++) {
    run += yd[tau];
    yd[tau] = run > 0 ? yd[tau] * (float)tau / run : 1.0f;
  }
  // First dip under 0.2, then down to its bottom.
  int best = -1;
  for (int tau = TAU_MIN; tau <= TAU_MAX; tau++) {
    if (yd[tau] < 0.2f) {
      while (tau + 1 <= TAU_MAX && yd[tau + 1] < yd[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) {
    float mn = 1e9f;
    for (int tau = TAU_MIN; tau <= TAU_MAX; tau++) if (yd[tau] < mn) mn = yd[tau];
    char m[40];
    snprintf(m, sizeof m, "no clear pitch (best %.2f)", (double)mn);
    tuneFail(i, m);
    vernaSay(V_TUNE_NO_SIGNAL);  // "I couldn't hear that one": no pitch to judge
    return;
  }
  float bt = (float)best;
  if (best > TAU_MIN && best < TAU_MAX) {     // parabolic interpolation
    float a = yd[best - 1], c0 = yd[best], c = yd[best + 1];
    float den = a - 2.0f * c0 + c;
    if (fabsf(den) > 1e-9f) bt += 0.5f * (a - c) / den;
  }
  float hz = rate / bt;
  float midi = 69.0f + 12.0f * log2f(hz / 440.0f);

  // Pitch class only: the tube may sound an octave off its written note.
  float cents = fmodf((midi - (float)outs[i].note) * 100.0f, 1200.0f);
  if (cents > 600.0f) cents -= 1200.0f;
  if (cents < -600.0f) cents += 1200.0f;
  int nearest = (int)lroundf(midi);
  int pc = ((nearest % 12) + 12) % 12;
  snprintf(tuneNote, sizeof tuneNote, "%s", PC[pc]);
  tuneHz = hz;
  tuneCents = (int)lroundf(cents);
  tuneOk = 1;
  snprintf(calMsg, sizeof calMsg, "Out %d: %s %+d cents (%.1f Hz), wants %s", i + 1, PC[pc], (int)tuneCents,
           (double)hz, PC[outs[i].note % 12]);
  evlog("cal: tune out %d: %.1f Hz, %s %+d c vs %s (%.0f S/s)", i + 1, (double)hz, PC[pc], (int)tuneCents,
        PC[outs[i].note % 12], (double)rate);
  // Spoken result: within 8 cents "right on", 8..25 "a touch" sharp/flat, over 25 sharp/flat.
  int c = tuneCents, ac = c < 0 ? -c : c;
  vernaSay(ac <= 8 ? V_TUNE_ON : ac <= 25 ? (c > 0 ? V_TUNE_TOUCH_SHARP : V_TUNE_TOUCH_FLAT)
                                          : (c > 0 ? V_TUNE_SHARP : V_TUNE_FLAT));
}

// ---------------- piezo pin scan ----------------
// Reads every ADC header pin that has no other job (MIDI, button, tester, power sense, pedals) for 3 s. Never touches a pin in use:
// an analogRead would take it over, and on the tester pin that could leave the MOSFET gate floating.
static void runPzScan() {
  pzDone = 0;
  bool use[N_ADC];
  int lo[N_ADC], hi[N_ADC];
  int nUse = 0;
  for (int k = 0; k < N_ADC; k++) {
    int p = ADC_PINS[k];
    use[k] = !pinInUse(p);
    lo[k] = 65535;
    hi[k] = 0;
    pzPeak[k] = 0;
    if (use[k]) nUse++;
  }
  if (!nUse) {
    snprintf(calMsg, sizeof calMsg, "Find piezo: every ADC pin has another job");
    evlog("cal: pz scan refused, no free ADC pin");
    return;
  }
  evlog("cal: pz scan, %d pins", nUse);
  snprintf(calMsg, sizeof calMsg, "Listening 3 s: tap the piezo");
  uint32_t t0 = millis(), ty = t0;
  bool done = true;
  while (millis() - t0 < 3000) {
    for (int k = 0; k < N_ADC; k++) {
      if (!use[k]) continue;
      int v = analogRead(ADC_PINS[k]);
      if (v < lo[k]) lo[k] = v;
      if (v > hi[k]) hi[k] = v;
    }
    if (millis() - ty >= 10) {
      for (int k = 0; k < N_ADC; k++) if (use[k] && hi[k] > lo[k]) pzPeak[k] = (uint16_t)(hi[k] - lo[k]);
      if (calAbort || updating) { done = false; break; }   // no coil involved, so mute alone doesn't stop it
      vTaskDelay(1);
      ty = millis();
    }
  }
  int bestK = -1;
  uint16_t bestP = 0;
  for (int k = 0; k < N_ADC; k++) {
    if (use[k] && hi[k] > lo[k]) pzPeak[k] = (uint16_t)(hi[k] - lo[k]);
    if (use[k] && pzPeak[k] > bestP) { bestP = pzPeak[k]; bestK = k; }
  }
  if (!done) {
    snprintf(calMsg, sizeof calMsg, "Find piezo: %s", whyStop());
    evlog("cal: pz scan %s", whyStop());
    return;
  }
  pzDone = 1;
  if (bestK < 0 || bestP < 40) {
    snprintf(calMsg, sizeof calMsg, "No pin heard a tap. Tap the piezo while it listens.");
    evlog("cal: pz scan done, nothing heard");
  } else {
    snprintf(calMsg, sizeof calMsg, "Loudest: GPIO %d (p-p %u)", ADC_PINS[bestK], (unsigned)bestP);
    evlog("cal: pz scan done, loudest GPIO %d p-p %u", ADC_PINS[bestK], (unsigned)bestP);
  }
}

// ---------------- walk ----------------
static void runWalk() {
  int vel = constrain((int)walkVel, 20, 127), gap = constrain((int)walkGapMs, 250, 1500);
  walkVel = WALK_VEL; walkGapMs = WALK_GAP_MS;   // this walk has them now
  if (muted || updating) {
    snprintf(calMsg, sizeof calMsg, "Walk: unmute first");
    evlog("cal: walk refused, %s", whyStop());
    return;
  }
  int total = 0;
  for (int i = 0; i < MAX_OUT; i++) if (outUsable(i)) total++;
  if (!total) {
    snprintf(calMsg, sizeof calMsg, "Walk: no outputs ready");
    evlog("cal: walk refused, no usable outputs");
    return;
  }
  evlog("cal: walk, %d outputs, vel %d, %d ms apart", total, vel, gap);
  int n = 0;
  bool done = true;
  for (int i = 0; i < MAX_OUT; i++) {
    if (stopNow()) { done = false; break; }
    if (!outUsable(i)) continue;
    n++;
    snprintf(calMsg, sizeof calMsg, "Walk: out %d %s (%d of %d)", i + 1, outLabel(i).c_str(), n, total);
    sendMsg(M_OUT, (uint8_t)i, (uint8_t)vel, 0);
    if (!waitMs((uint32_t)gap)) { done = false; break; }
  }
  if (done) {
    snprintf(calMsg, sizeof calMsg, "Walk done: %d outputs", n);
    evlog("cal: walk done, %d outputs", n);
  } else {
    snprintf(calMsg, sizeof calMsg, "Walk %s after %d of %d", whyStop(), n, total);
    evlog("cal: walk %s after %d of %d", whyStop(), n, total);
  }
}

// ---------------- the task ----------------
void calTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    int cmd = calCmd;
    if (cmd == CAL_IDLE) continue;
    // calAbort was cleared by the starter (net.cpp calStart) before calCmd; not cleared here, so a panic in the
    // gap before this task picks the command up still stops it.
    calState = cmd;
    calCmd = CAL_IDLE;
    int o = calOut;                 // 0-based output index, set before calCmd
    switch (cmd) {
      case CAL_SWEEP:  runSweep(o); break;
      case CAL_TUNE:   runTune(o); break;
      case CAL_PZSCAN: runPzScan(); break;
      case CAL_WALK:   runWalk(); break;
      default:
        snprintf(calMsg, sizeof calMsg, "Unknown calibration command %d", cmd);
        evlog("cal: unknown command %d", cmd);
        break;
    }
    sweepCur = 0;
    calState = CAL_IDLE;
  }
}
