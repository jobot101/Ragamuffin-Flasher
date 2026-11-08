// net.cpp: core 0. WiFi (own WiFi + paced hall join), captive DNS, mDNS, the web server, MQTT, OTA updates and the
// USB serial console. It also copies new log lines to USB serial (the only place that prints).
// It never touches a coil: every strike goes to core 1 as a Msg (sendMsg), every stop through requestPanic().
// Nothing in this task blocks for long, so a web or serial panic is always served: MQTT has its own task
// (mqttTask), and the WiFi scan is async.
#include "chimes.h"
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>

const char* DEVICE_NAME = "hall-chimes";
const char* TOPIC_BASE  = "ragamuffin/hall/chimes";
// Subscribed: <base>/note "60" or "60,100"   <base>/mute "1"/"0"   <base>/panic (any)   <base>/tap (any)
// Published:  <base>/status "online"/"offline" (retained, LWT)   <base>/played "60,100"

volatile bool apOn = false;   // own WiFi (AP_SSID) up

// Round 2 helpers defined elsewhere but not in chimes.h yet (requested from the lead):
bool savePinsR2(int midi, int btn, int sol, int inv, int pz, int pwr, const int ped[NUM_PEDALS],
                const int pedInv[NUM_PEDALS]);      // config.cpp: every boot-only pin in one write
extern volatile int walkVel, walkGapMs;              // cal.cpp: the next walk's velocity and gap (ms)

static WebServer server(80);
static DNSServer dns;
// MQTT lives in its own task (mqttTask): a connect to a broker that is down blocks for seconds, and the web
// /panic, /mute and the serial console must never wait behind it. mqtt and wifiClient are that task's only.
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static char mqttHostBuf[64];            // PubSubClient keeps the pointer, so it must not move
static char mqttUserBuf[64], mqttPassBuf[64];
static uint16_t mqttPortNow = 1883;
static volatile bool mqttUp = false;    // for statusJson from any task (mqtt.connected() is mqttTask only)
static volatile bool mqttReconf = true; // the web handler changed the broker: mqttTask re-reads it
static SemaphoreHandle_t mqttMux = nullptr;   // guards mqttHost/User/Pass/Port between the web handler and mqttTask
static uint32_t mqttConnAt = 0;         // millis() of the last connect (retained messages arrive right after it)

static volatile uint8_t staWhy = 0;     // last disconnect reason, from the WiFi event
static uint32_t staTry = 0, staNext = 0;
static String joinSsid, joinPass;       // what the pacing last joined; a change means saveWifi() just began a join
static uint32_t upSince = 0, downSince = 0, apRestartAt = 0;
static bool wasUp = false;

// ---------------- small helpers ----------------
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t msToUs(float ms, uint32_t lo, uint32_t hi) {   // ms from a form -> us, inside lo..hi
  if (!(ms > 0.0f)) return lo;                                 // also NaN
  if (ms > 1000.0f) ms = 1000.0f;
  uint32_t us = (uint32_t)lroundf(ms * 1000.0f);
  return us < lo ? lo : us > hi ? hi : us;
}

static void addf(String& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void addf(String& s, const char* fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(b)) { s += b; return; }
  char* h = (char*)malloc(n + 1);
  if (!h) return;
  va_start(ap, fmt);
  vsnprintf(h, n + 1, fmt, ap);
  va_end(ap);
  s += h;
  free(h);
}

static void addStr(String& s, const char* v) {   // a quoted, escaped JSON string
  s += '"';
  for (; *v; v++) {
    char c = *v;
    if (c == '"' || c == '\\') { s += '\\'; s += c; }
    else if ((uint8_t)c < 0x20) { char e[8]; snprintf(e, sizeof(e), "\\u%04x", (unsigned)(uint8_t)c); s += e; }
    else s += c;
  }
  s += '"';
}

static void copyName(char* dst, size_t n, const String& src) {   // printable ASCII only, NUL-terminated
  size_t k = 0;
  for (size_t i = 0; i < src.length() && k + 1 < n; i++) {
    char c = src[i];
    if (c >= 0x20 && c <= 0x7E) dst[k++] = c;
  }
  while (k < n) dst[k++] = 0;
}

static String apPassNow() { return apPass.length() >= 8 ? apPass : defaultApPass(); }

// Round 2: "check this one" (computed here, so the flag never depends on who last saved)
static bool wearFlag(int i) {
  uint32_t k = outs[i].wearK;
  return k && wearCount[i] >= k * 1000u;
}

// A pin with a job other than piezo (the piezo scan skips it): MIDI, button, tester, 24 V sense, pedals.
static bool pinBusy(int p) {
  if (p == cfg.midiPin || p == cfg.btnPin || p == cfg.solPin || p == cfg.pwrPin) return true;
  for (int k = 0; k < NUM_PEDALS; k++) if (p == cfg.pedalPin[k]) return true;
  return false;
}

// Every boot-only pin checked together: MIDI 14 or 44; every other role -1 (off) or a header pin (the piezo an ADC
// pin), and no pin used twice. True (and the reason in why) if something is wrong.
static_assert(NUM_PEDALS == 4, "pin names below");
static bool pinsProblem(int midi, int btn, int sol, int pz, int pwr, const int ped[NUM_PEDALS], char* why, size_t n) {
  if (midi != 14 && midi != 44) { snprintf(why, n, "MIDI pin must be 14 or 44"); return true; }
  static const char* const NM[4 + NUM_PEDALS] = {"button", "tester", "piezo", "power sense", "pedal 1", "pedal 2",
                                                 "pedal 3", "pedal 4"};
  const int pin[4 + NUM_PEDALS] = {btn, sol, pz, pwr, ped[0], ped[1], ped[2], ped[3]};
  for (int a = 0; a < 4 + NUM_PEDALS; a++) {
    int p = pin[a];
    if (p == -1) continue;
    if (!hdrPin(p)) { snprintf(why, n, "%s pin %d not allowed", NM[a], p); return true; }
    if (a == 2 && !adcPin(p)) { snprintf(why, n, "piezo pin %d has no ADC (9-14 only)", p); return true; }
    if (p == midi) { snprintf(why, n, "%s pin %d is the MIDI pin", NM[a], p); return true; }
    if (a >= 3 && p == 9) { snprintf(why, n, "%s can't use G9: the tester MOSFET is wired there", NM[a]); return true; }
    for (int b = 0; b < a; b++)
      if (pin[b] == p) { snprintf(why, n, "%s pin %d is already the %s pin", NM[a], p, NM[b]); return true; }
  }
  return false;
}

// Verna's prompt by key ("tune_on"), or by its number.
static int vernaByKey(const char* k) {
  if (!k || !*k) return -1;
  for (int i = 0; i < V_COUNT; i++) if (VERNA[i].key && !strcmp(VERNA[i].key, k)) return i;
  char* end = nullptr;
  long v = strtol(k, &end, 10);
  return (end != k && !*end && v >= 0 && v < V_COUNT) ? (int)v : -1;
}

static volatile bool calFullArmed = false;   // a sweep ran: the first min set for its output says V_CAL_FULL
static void calFullCheck(int out) {          // out = 0-based output whose min was just set
  if (calFullArmed && out == sweepOut && sweepN > 0) { calFullArmed = false; vernaSay(V_CAL_FULL); }
}

// ---------------- status ----------------
String statusJson() {
  String s;
  s.reserve(8192);
  uint32_t nowMs = millis();
  uint16_t lp = lastPlayed;

  addf(s, "{\"fw\":\"%s\",\"box\":1,\"muted\":%d,\"updating\":%d,\"supma\":%d,\"maOn\":%d,\"maxOn\":%d,\"on\":%u,"
          "\"queue\":%u,\"qpeak\":%u,",
       FW_VERSION, (int)muted, (int)updating, cfg.supplyMa, (int)maOn, maxOnNow(), (unsigned)coilsOn,
       (unsigned)qn, (unsigned)qPeak);
  addf(s, "\"played\":%lu,\"dropped\":%lu,\"heatRefused\":%lu,\"faultRefused\":%lu,\"clamped\":%lu,\"panics\":%lu,"
          "\"watchdog\":%lu,\"midi\":%lu,\"sustain\":%d,\"demo\":%d,",
       (unsigned long)nPlayed, (unsigned long)nDropped, (unsigned long)nHeat, (unsigned long)nFault,
       (unsigned long)nClamped, (unsigned long)nPanics, (unsigned long)nWatchdog, (unsigned long)nMidi,
       (int)sustain, (int)demoSong);
  s += "\"songs\":[";
  for (int k = 0; k < NUM_SONGS; k++) { if (k) s += ','; addStr(s, SONGS[k].name ? SONGS[k].name : ""); }
  addf(s, "],\"seq\":{\"slot\":%d,\"step\":%d},\"last\":[%u,%u],\"hardCap\":%lu,\"stuck\":%u,",
       (int)seqSlot, (int)seqStep, (unsigned)(lp >> 8), (unsigned)(lp & 255), (unsigned long)(HARD_CAP_US / 1000),
       (unsigned)stuckMask);
  // round 2
  addf(s, "\"power\":{\"pin\":%d,\"ok\":%d,\"asks\":%lu},\"pedals\":[", cfg.pwrPin, (int)pwrState,
       (unsigned long)nNoPower);
  for (int k = 0; k < NUM_PEDALS; k++)
    addf(s, "%s{\"pin\":%d,\"act\":%u,\"out\":%u,\"inv\":%d,\"down\":%u}", k ? "," : "", cfg.pedalPin[k],
         (unsigned)cfg.pedalAct[k], (unsigned)cfg.pedalOut[k] + 1, cfg.pedalInv[k], (unsigned)pedalDown[k]);
  addf(s, "],\"rec\":{\"slot\":%d,\"state\":%d,\"notes\":%d},\"cur\":%d,", (int)recSlot, (int)recState,
       (int)recNotes, (int)curSlot);
  int vp = vernaPlaying;
  addf(s, "\"verna\":{\"on\":%d,\"vol\":%d,\"playing\":%d,\"key\":\"%s\",\"count\":%lu},", cfg.voice, cfg.voiceVol, vp,
       (vp >= 0 && vp < V_COUNT && VERNA[vp].key) ? VERNA[vp].key : "", (unsigned long)vernaCount);
  addf(s, "\"rev\":{\"outs\":%lu,\"pat\":[", (unsigned long)outsRev);
  for (int k = 0; k < SEQ_SLOTS; k++) addf(s, "%s%lu", k ? "," : "", (unsigned long)patRev[k]);
  s += "],\"dirty\":[";
  for (int k = 0; k < SEQ_SLOTS; k++) addf(s, "%s%d", k ? "," : "", patDirty[k] ? 1 : 0);
  s += "]},";

  s += "\"boards\":[";
  for (int b = 0; b < MAX_BOARDS; b++)
    addf(s, "%s{\"a\":%d,\"s\":%u,\"e\":%lu}", b ? "," : "", BOARD_BASE + b, (unsigned)bState[b], (unsigned long)bErr[b]);

  s += "],\"live\":{\"on\":[";
  for (int i = 0; i < MAX_OUT; i++) { if (i) s += ','; s += outOn[i] ? '1' : '0'; }
  s += "],\"heat\":[";
  for (int i = 0; i < MAX_OUT; i++) {
    float h = heat[i];
    if (i) s += ',';
    s += clampi((int)(h * 100.0f / HEAT_CAP_MS + 0.5f), 0, 100);
  }
  s += "],\"hot\":[";
  for (int i = 0; i < MAX_OUT; i++) {
    float h = heat[i];
    uint32_t ra = refusedAt[i];
    bool hot = (ra && (int32_t)(nowMs - ra) < 3000) ||h + outCap(i) / 1000.0f > HEAT_CAP_MS;
    if (i) s += ',';
    s += hot ? '1' : '0';
  }
  s += "],\"strikes\":[";
  for (int i = 0; i < MAX_OUT; i++) { if (i) s += ','; s += (unsigned long)strikes[i]; }
  s += "],\"wear\":[";
  for (int i = 0; i < MAX_OUT; i++) { if (i) s += ','; s += (unsigned long)wearCount[i]; }
  s += "],\"check\":[";
  for (int i = 0; i < MAX_OUT; i++) { if (i) s += ','; s += wearFlag(i) ? '1' : '0'; }

  addf(s, "]},\"cfg\":{\"cap\":%.1f,\"supma\":%d,\"port\":%d,\"mch\":%d,\"gam\":%.2f,\"fold\":%d,\"lat\":%.1f,"
          "\"solo\":%d,\"trig\":%d,",
       cfg.capUs / 1000.0, cfg.supplyMa, cfg.portB, cfg.midiCh, (double)cfg.gamma, cfg.fold, cfg.latUs / 1000.0,
       cfg.solo, cfg.trig);
  addf(s, "\"midip\":%d,\"btnp\":%d,\"solp\":%d,\"sinv\":%d,\"sms\":%d,\"pzp\":%d,",
       cfg.midiPin, cfg.btnPin, cfg.solPin, cfg.solInv, cfg.solMs, cfg.pzPin);
  addf(s, "\"pwrp\":%d,\"cc64\":%d,\"voice\":%d,\"vol\":%d},", cfg.pwrPin, cfg.cc64Act, cfg.voice, cfg.voiceVol);

  int cs = calState;
  char msg[sizeof(calMsg)];
  memcpy(msg, calMsg, sizeof(msg)); msg[sizeof(msg) - 1] = 0;
  addf(s, "\"cal\":{\"state\":%d,\"out\":%d,\"cur\":%lu,\"msg\":", cs, cs ? (int)calOut : (int)sweepOut,
       (unsigned long)sweepCur);
  addStr(s, msg);
  s += ",\"steps\":[";
  int sn = clampi(sweepN, 0, SWEEP_MAX);
  for (int k = 0; k < sn; k++) addf(s, "%s[%u,%u]", k ? "," : "", (unsigned)sweepSteps[k].us, (unsigned)sweepSteps[k].peak);
  char tn[sizeof(tuneNote)];
  memcpy(tn, tuneNote, sizeof(tn)); tn[sizeof(tn) - 1] = 0;
  addf(s, "]},\"tune\":{\"out\":%d,\"ok\":%d,\"hz\":%.1f,\"cents\":%d,\"note\":",
       (int)tuneOut, (int)tuneOk, (double)tuneHz, (int)tuneCents);
  addStr(s, tn);

  addf(s, "},\"pz\":{\"done\":%d,\"pins\":[", (int)pzDone);
  for (int k = 0; k < N_ADC; k++) {              // -1 = a pin with another job, which the scan skips (cal.cpp)
    int p = ADC_PINS[k];
    addf(s, "%s%d", k ? "," : "", pinBusy(p) ? -1 : p);
  }
  s += "],\"pk\":[";
  for (int k = 0; k < N_ADC; k++) addf(s, "%s%u", k ? "," : "", (unsigned)pzPeak[k]);

  s += "]},\"log\":[";
  uint32_t lc = logCount, first = lc > 10 ? lc - 10 : 0;
  for (uint32_t k = first; k < lc; k++) { if (k > first) s += ','; addStr(s, logLine(k).c_str()); }

  bool up = WiFi.status() == WL_CONNECTED;
  s += "],\"net\":{\"ssid\":";
  addStr(s, staSsid.c_str());
  addf(s, ",\"up\":%d,\"ip\":\"%s\",\"why\":%d,\"ap\":%d,\"apssid\":", (int)up,
       up ? WiFi.localIP().toString().c_str() : "", up ? 0 : (int)staWhy, (int)apOn);
  addStr(s, AP_SSID);
  s += "},\"mqtt\":{\"host\":";
  addStr(s, mqttHost.c_str());
  addf(s, ",\"port\":%u,\"user\":", (unsigned)mqttPort);
  addStr(s, mqttUser.c_str());
  addf(s, ",\"up\":%d}}", (int)mqttUp);
  return s;
}

static String outsJson() {
  String s;
  s.reserve(64 * 88 + 32);
  addf(s, "{\"rev\":%lu,\"outs\":[", (unsigned long)outsRev);
  for (int i = 0; i < MAX_OUT; i++) {
    const OutCfg& o = outs[i];
    char nm[sizeof(o.name) + 1];
    memcpy(nm, o.name, sizeof(o.name)); nm[sizeof(o.name)] = 0;
    s += i ? ",{\"n\":" : "{\"n\":";
    addStr(s, nm);
    addf(s, ",\"k\":%u,\"no\":%u,\"mi\":%u,\"fu\":%u,\"c\":%u,\"ma\":%u,\"w\":%u,\"r\":%u}", (unsigned)o.kind,
         (unsigned)o.note, (unsigned)o.minUs, (unsigned)o.fullUs, (unsigned)o.capUs, (unsigned)o.mA,
         (unsigned)o.wearK, (unsigned)o.role);
  }
  s += "]}";
  return s;
}

// Pattern rows are given as the output index 0..63, 255 = none (as stored in Pattern.row; the page reads it so).
// /seqrow's o is the 1-based output number, 0 = none.
// /presets: the world-rhythm list (ts = index into TIME_SIGS; roles = DrumRole per row).
static String presetsJson() {
  String s;
  s.reserve(NUM_PRESETS * 220 + 16);
  s += '[';
  for (int k = 0; k < NUM_PRESETS; k++) {
    const DrumPreset& d = PRESETS[k];
    s += k ? ",{\"name\":" : "{\"name\":";
    addStr(s, d.name ? d.name : "");
    s += ",\"desc\":";
    addStr(s, d.desc ? d.desc : "");
    addf(s, ",\"ts\":%u,\"bpm\":%u,\"swing\":%u,\"rows\":%u,\"steps\":%u,\"roles\":[", (unsigned)d.ts, (unsigned)d.bpm,
         (unsigned)d.swing, (unsigned)d.rows, (unsigned)d.steps);
    int rows = d.rows < (int)sizeof(d.role) ? d.rows : (int)sizeof(d.role);
    for (int r = 0; r < rows; r++) addf(s, "%s%u", r ? "," : "", (unsigned)d.role[r]);
    s += "]}";
  }
  s += ']';
  return s;
}

// /meta: the fixed name tables, for the page (they never change while running).
static String metaJson() {
  String s;
  s.reserve(2600);
  s += "{\"roles\":[";
  for (int r = 0; r < R_COUNT; r++) { if (r) s += ','; addStr(s, ROLE_NAMES[r]); }
  s += "],\"ts\":[";
  for (int t = 0; t < NUM_TS; t++)
    addf(s, "%s{\"l\":\"%s\",\"beats\":%u,\"sub\":%u,\"steps\":%u}", t ? "," : "", TIME_SIGS[t].label,
         (unsigned)TIME_SIGS[t].beats, (unsigned)TIME_SIGS[t].sub, (unsigned)TIME_SIGS[t].steps);
  s += "],\"acts\":[";
  for (int a = 0; a < P_COUNT; a++) { if (a) s += ','; addStr(s, PEDAL_ACT_NAMES[a]); }
  s += "],\"verna\":[";
  for (int v = 0; v < V_COUNT; v++) {
    s += v ? ",{\"k\":" : "{\"k\":";
    addStr(s, VERNA[v].key ? VERNA[v].key : "");
    s += ",\"t\":";
    addStr(s, VERNA[v].text ? VERNA[v].text : "");
    s += '}';
  }
  s += "]}";
  return s;
}

static String seqJson(int slot) {
  const Pattern& p = pats[slot];
  String s;
  s.reserve(2048);
  char nm[sizeof(p.name) + 1];
  memcpy(nm, p.name, sizeof(p.name)); nm[sizeof(p.name)] = 0;
  addf(s, "{\"s\":%d,\"rev\":%lu,\"dirty\":%d,\"name\":", slot, (unsigned long)patRev[slot], patDirty[slot] ? 1 : 0);
  addStr(s, nm);
  unsigned ts = p.ts < NUM_TS ? p.ts : 0;
  addf(s, ",\"steps\":%u,\"bpm\":%u,\"swing\":%u,\"loop\":%u,\"ts\":%u,\"tsl\":\"%s\",\"sub\":%u,\"row\":[",
       (unsigned)p.steps, (unsigned)p.bpm, (unsigned)p.swing, (unsigned)p.loop, ts, TIME_SIGS[ts].label,
       (unsigned)TIME_SIGS[ts].sub);
  for (int r = 0; r < SEQ_ROWS; r++) {
    if (r) s += ',';
    s += (unsigned)(p.row[r] >= MAX_OUT ? SEQ_NONE : p.row[r]);
  }
  s += "],\"role\":[";
  for (int r = 0; r < SEQ_ROWS; r++) { if (r) s += ','; s += (unsigned)(p.rowRole[r] < R_COUNT ? p.rowRole[r] : 0); }
  s += "],\"vel\":[";
  for (int r = 0; r < SEQ_ROWS; r++) {
    s += r ? ",[" : "[";
    for (int c = 0; c < SEQ_STEPS; c++) { if (c) s += ','; s += (unsigned)p.vel[r][c]; }
    s += ']';
  }
  s += "]}";
  return s;
}

// ---------------- WiFi ----------------
static void apStart(const char* why) {
  WiFi.softAP(AP_SSID, apPassNow().c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  evlog("own WiFi \"%s\" on (%s) -> http://%s", AP_SSID, why, WiFi.softAPIP().toString().c_str());
}

static void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  apOn = false;
  evlog("own WiFi off (on the hall WiFi 60 s)");
}

// Paced join: try 15 s, then wait 45 s. saveWifi() (config.cpp) starts a join itself; we notice the change and
// count that as the current try.
static void staService(uint32_t ms) {
  if (staSsid != joinSsid || staPass != joinPass) {
    joinSsid = staSsid; joinPass = staPass;
    staWhy = 0;
    staTry = staSsid.length() ? (ms | 1) : 0;
    staNext = ms + 45000;
  }
  if (!staSsid.length()) return;
  if (WiFi.status() == WL_CONNECTED) { staTry = 0; staNext = ms + 45000; return; }
  if (staTry && (int32_t)(ms - staTry) > 15000) { WiFi.disconnect(); staTry = 0; staNext = ms + 45000; }
  else if (!staTry && (int32_t)(ms - staNext) >= 0) {
    staWhy = 0;
    WiFi.disconnect();
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    staTry = ms | 1;
  }
}

// Own WiFi only as a fallback: off after 60 s on the hall WiFi, back on after 20 s without it.
static void apService(uint32_t ms) {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {
    wasUp = up;
    if (up) evlog("hall WiFi %s: http://%s  http://%s.local", staSsid.c_str(), WiFi.localIP().toString().c_str(), DEVICE_NAME);
    else evlog("hall WiFi lost (reason %d)", (int)staWhy);
  }
  if (up) {
    downSince = 0;
    if (!upSince) upSince = ms | 1;
    if (apOn && (int32_t)(ms - upSince) > 60000) apStop();   // signed: upSince = ms|1 can be 1 ms ahead
  } else {
    upSince = 0;
    if (!downSince) downSince = ms | 1;
    if (!apOn && (int32_t)(ms - downSince) > 20000) apStart("no hall WiFi for 20 s");
  }
  if (apRestartAt && (int32_t)(ms - apRestartAt) >= 0) {   // new own WiFi password, after the reply went out
    apRestartAt = 0;
    if (apOn) { WiFi.softAP(AP_SSID, apPassNow().c_str()); evlog("own WiFi password changed"); }
  }
}

// ---------------- MQTT (mqttTask only, except where noted) ----------------
static void unmute(const char* src) { requestUnmute(src); }

static bool wordIs(const char* w, const char* const* list) {
  for (; *list; list++) if (!strcmp(w, *list)) return true;
  return false;
}

static void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[32];
  len = len < sizeof(msg) - 1 ? len : sizeof(msg) - 1;
  memcpy(msg, payload, len); msg[len] = '\0';
  String t(topic);
  if (t.endsWith("/note")) {
    int note = 0, vel = 100;
    sscanf(msg, "%d,%d", &note, &vel);
    sendMsg(M_NOTE, (uint8_t)clampi(note, 0, 127), (uint8_t)clampi(vel, 1, 127), 0);
  } else if (t.endsWith("/mute")) {
    // Strict: anything not plainly a mute or an unmute word is ignored. An unmute arriving just after a connect
    // is ignored too: that is a retained message replayed on every reconnect, and a reconnect must never unmute.
    static const char* const ON[]  = {"1", "on", "true", "mute", nullptr};
    static const char* const OFF[] = {"0", "off", "false", "unmute", nullptr};
    char w[sizeof(msg)];
    size_t k = 0;
    for (const char* p = msg; *p && k + 1 < sizeof(w); p++)
      if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') w[k++] = (char)tolower((unsigned char)*p);
    w[k] = 0;
    if (wordIs(w, ON)) requestPanic(true, "MQTT mute");
    else if (wordIs(w, OFF)) {
      if (millis() - mqttConnAt < 3000) evlog("MQTT unmute ignored: just connected (retained?)");
      else unmute("MQTT");
    } else evlog("MQTT mute \"%.12s\" not understood, ignored", w);
  } else if (t.endsWith("/panic")) requestPanic(true, "MQTT");
  else if (t.endsWith("/tap")) sendMsg(M_TAP, 0, 0, 0);
}

// copy the broker settings (under the lock) into this task's own buffers
static void mqttTakeConfig() {
  if (mqttMux) xSemaphoreTake(mqttMux, portMAX_DELAY);
  strlcpy(mqttHostBuf, mqttHost.c_str(), sizeof(mqttHostBuf));
  strlcpy(mqttUserBuf, mqttUser.c_str(), sizeof(mqttUserBuf));
  strlcpy(mqttPassBuf, mqttPass.c_str(), sizeof(mqttPassBuf));
  mqttPortNow = mqttPort ? mqttPort : 1883;
  if (mqttMux) xSemaphoreGive(mqttMux);
  mqtt.setServer(mqttHostBuf, mqttPortNow);
}

static void mqttService(uint32_t ms) {
  static uint32_t lastTry = 0, backoff = 5000;
  char topic[96];
  if (mqttReconf) {
    mqttReconf = false;
    if (mqtt.connected()) mqtt.disconnect();
    mqttTakeConfig();
    lastTry = 0;
    backoff = 5000;
  }
  bool up = WiFi.status() == WL_CONNECTED;
  if (!up || !mqttHostBuf[0] || updating) {
    if (mqtt.connected()) mqtt.disconnect();
    NoteMsg m;
    while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {}
  } else if (!mqtt.connected()) {
    NoteMsg m;
    while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {}
    if (!lastTry || (int32_t)(ms - lastTry) > (int32_t)backoff) {       // 5 s, doubling to 60 s while the broker stays away
      lastTry = ms | 1;
      snprintf(topic, sizeof(topic), "%s/status", TOPIC_BASE);
      bool ok = mqttUserBuf[0]
        ? mqtt.connect(DEVICE_NAME, mqttUserBuf, mqttPassBuf, topic, 1, true, "offline")
        : mqtt.connect(DEVICE_NAME, topic, 1, true, "offline");
      if (ok) {
        mqttConnAt = millis();
        backoff = 5000;
        mqtt.publish(topic, "online", true);
        static const char* const SUBS[] = {"note", "mute", "panic", "tap"};
        for (const char* sub : SUBS) { snprintf(topic, sizeof(topic), "%s/%s", TOPIC_BASE, sub); mqtt.subscribe(topic); }
        evlog("MQTT connected to %s", mqttHostBuf);
      } else if (backoff < 60000) {
        backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
      }
    }
  } else {
    mqtt.loop();
    NoteMsg m;
    while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {
      char buf[12];
      snprintf(buf, sizeof(buf), "%u,%u", (unsigned)m.note, (unsigned)m.vel);
      snprintf(topic, sizeof(topic), "%s/played", TOPIC_BASE);
      mqtt.publish(topic, buf);
    }
  }
  bool now = mqtt.connected();
  if (mqttUp && !now && up && mqttHostBuf[0] && !updating) evlog("MQTT lost");
  mqttUp = now;
}

static void mqttTask(void*) {
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);
  for (;;) {
    mqttService(millis());
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------- calibration starts (only when the cal task is free) ----------------
static bool calStart(int cmd, int out) {
  if (calBusy()) return false;
  calOut = out;
  calAbort = false;
  calCmd = cmd;
  return true;
}

// ---------------- web ----------------
static int argI(const char* k, int d) { return server.hasArg(k) ? server.arg(k).toInt() : d; }
static float argF(const char* k, float d) { return server.hasArg(k) ? server.arg(k).toFloat() : d; }
static void sendStatus(int code = 200) { server.send(code, "application/json", statusJson()); }
static void bad(const char* why) { server.send(400, "text/plain", why); }
static int argSlot() { return server.hasArg("s") ? argI("s", -1) : -1; }   // pattern slot, 0..SEQ_SLOTS-1

// Number fields of a settings form. A field that is absent or blank leaves the value alone (the page sends every
// field of a card, and a cleared box must not become 0 = the minimum). A field that isn't a number names itself in
// badKey, and the handler answers 400 before changing anything.
static char badKey[16];
static bool numArg(const char* k, double& v) {
  if (!server.hasArg(k)) return false;
  String a = server.arg(k);
  a.trim();
  if (!a.length()) return false;
  char* end = nullptr;
  double d = strtod(a.c_str(), &end);
  if (end == a.c_str() || *end || !isfinite(d)) {
    if (!badKey[0]) strlcpy(badKey, k, sizeof(badKey));
    return false;
  }
  v = d;
  return true;
}
static int toI(double v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)lround(v); }
static bool badArgs() {               // answer 400 naming the first bad field; true if it did
  if (!badKey[0]) return false;
  char why[48];
  snprintf(why, sizeof(why), "%s: not a number", badKey);
  server.send(400, "text/plain", why);
  return true;
}

// Port and trig belong to core 1: ask it by message, wait for it to take them, then save.
// Resent a few times in case the queue was full. Returns false if core 1 didn't take it.
// A deliberate restart after a pin change: the chimes are muted already and the reply has gone out. The wear
// counts are kept first, then the hold is taken so nothing is on, or can go on, as the board goes down.
static void restartAfterReply() {
  delay(300);
  if (wearDirty) saveWear();
  holdCoils(HOLD_WAIT_MS);
  ESP.restart();
}

static bool askCore1(uint8_t kind, int want, volatile int* live) {
  for (int k = 0; k < 100; k++) {
    if (k % 20 == 0) sendMsg(kind, (uint8_t)want, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (*live == want) return true;
  }
  return false;
}

static void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/setup", [] { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });
  server.on("/status", [] { sendStatus(); });
  server.on("/state", [] { sendStatus(); });

  server.on("/panic", [] { requestPanic(true, "web"); sendStatus(); });
  server.on("/mute", [] {
    bool on = server.hasArg("on") ? argI("on", 1) != 0 : !muted;
    if (on) requestPanic(true, "web mute"); else unmute("web");
    sendStatus();
  });

  server.on("/play", [] {
    int o = server.hasArg("o") ? argI("o", 0) : argI("t", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    sendMsg(M_OUT, (uint8_t)(o - 1), (uint8_t)clampi(argI("v", 100), 1, 127), 0);
    sendStatus();
  });
  server.on("/n", [] {
    sendMsg(M_NOTE, (uint8_t)clampi(argI("note", 60), 0, 127), (uint8_t)clampi(argI("v", 100), 1, 127), 0);
    sendStatus();
  });
  server.on("/try", [] {                    // exact pulse, still clamped by fire()
    int o = argI("o", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    sendMsg(M_EXACT, (uint8_t)(o - 1), 100, msToUs(argF("ms", 0), MIN_PULSE_US, HARD_CAP_US));
    sendStatus();
  });
  server.on("/demo", [] { sendMsg(M_DEMO, (uint8_t)clampi(argI("n", 0), 0, NUM_SONGS), 0, 0); sendStatus(); });
  server.on("/tap", [] { sendMsg(M_TAP, 0, 0, 0); sendStatus(); });
  server.on("/rescan", [] { sendMsg(M_RESCAN, 0, 0, 0); sendStatus(); });

  server.on("/test", [] { server.send_P(200, "text/html", TEST_PAGE); });
  server.on("/walk", [] {                   // v = velocity 20..127 (100), gap = ms between strikes 250..1500 (450)
    double v = 100, g = 450;
    badKey[0] = 0;
    numArg("v", v);
    numArg("gap", g);
    if (badArgs()) return;
    if (calBusy()) { sendStatus(409); return; }
    walkVel = toI(v, 20, 127);
    walkGapMs = toI(g, 250, 1500);
    bool ok = calStart(CAL_WALK, 0);
    if (!ok) { walkVel = 100; walkGapMs = 450; }   // not started: the next walk (screen, pedal) keeps the defaults
    sendStatus(ok ? 200 : 409);
  });
  server.on("/sweep", [] {
    int o = argI("o", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    sendStatus(calStart(CAL_SWEEP, o - 1) ? 200 : 409);
  });
  server.on("/tune", [] {
    int o = argI("o", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    sendStatus(calStart(CAL_TUNE, o - 1) ? 200 : 409);
  });
  server.on("/pzscan", [] { sendStatus(calStart(CAL_PZSCAN, 0) ? 200 : 409); });
  server.on("/calstop", [] { calCmd = CAL_IDLE; calAbort = true; sendStatus(); });

  // ---- outputs ----
  server.on("/outs", [] { server.send(200, "application/json", outsJson()); });
  server.on("/out", [] {
    int o = argI("o", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    OutCfg n = outs[o - 1];
    uint8_t oldKind = n.kind;
    double v, role = 0;
    badKey[0] = 0;
    if (server.hasArg("name")) copyName(n.name, sizeof(n.name), server.arg("name"));
    if (numArg("kind", v)) n.kind = (uint8_t)toI(v, K_OFF, K_OTHER);
    if (numArg("note", v)) n.note = (uint8_t)toI(v, 0, 127);
    bool hMin = numArg("min", v);
    if (hMin)              n.minUs  = (uint16_t)msToUs((float)v, MIN_PULSE_US, HARD_CAP_US);
    if (numArg("full", v)) n.fullUs = (uint16_t)msToUs((float)v, MIN_PULSE_US, HARD_CAP_US);
    if (numArg("cap", v))  n.capUs  = (uint16_t)msToUs((float)v, MIN_PULSE_US, HARD_CAP_US);
    if (numArg("ma", v))   n.mA = (uint16_t)toI(v, 0, 3000);
    bool hWear = numArg("wear", v);          // wear check threshold, thousands of strikes, 0 = off
    if (hWear)             n.wearK = (uint16_t)toI(v, 0, 60000);
    bool hRole = numArg("role", role);
    if (badArgs()) return;
    if (hRole && (role < 0 || role >= (int)R_COUNT || role != floor(role))) { bad("role: 0..10 (see /meta roles)"); return; }
    if (hRole) n.role = (uint8_t)role;
    // a kind change carries the new kind's wear default along, unless the owner set a threshold of their own
    if (!hWear && n.kind != oldKind && n.wearK == (oldKind == K_TUBE ? 250 : 100)) n.wearK = n.kind == K_TUBE ? 250 : 100;
    if (n.fullUs < n.minUs) n.fullUs = n.minUs;
    if (n.kind != K_OFF && n.mA < MIN_COIL_MA) n.mA = MIN_COIL_MA;   // every live coil counts against the supply
    n.name[sizeof(n.name) - 1] = 0;
    memcpy(&outs[o - 1], &n, sizeof(n));    // whole entry at once; fire() clamps whatever core 1 sees
    outsEdited();
    if (!saveOuts()) { server.send(503, "text/plain", "set live, but not kept after a restart: save again"); return; }
    evlog("output %d saved: %s", o, outLabel(o - 1).c_str());
    if (hMin) calFullCheck(o - 1);           // sweep coaching: "now tap where it stops getting louder"
    server.send(200, "application/json", outsJson());
  });
  // Calibrate's "Copy to all of this kind": one change, one NVS write (not one write per output).
  server.on("/outcopy", [] {
    int k = argI("k", -1);
    if (k < K_TUBE || k > K_OTHER) { bad("no such kind"); return; }
    double mi = 0, fu = 0;
    badKey[0] = 0;
    bool hm = numArg("min", mi), hf = numArg("full", fu);
    if (badArgs()) return;
    if (!hm && !hf) { bad("nothing to copy"); return; }
    int count = 0;
    for (int i = 0; i < MAX_OUT; i++) {
      if (outs[i].kind != k) continue;
      OutCfg n = outs[i];
      if (hm) n.minUs  = (uint16_t)msToUs((float)mi, MIN_PULSE_US, HARD_CAP_US);
      if (hf) n.fullUs = (uint16_t)msToUs((float)fu, MIN_PULSE_US, HARD_CAP_US);
      if (n.fullUs < n.minUs) n.fullUs = n.minUs;
      memcpy(&outs[i], &n, sizeof(n));
      count++;
    }
    outsEdited();
    if (!saveOuts()) { server.send(503, "text/plain", "set live, but not kept after a restart: save again"); return; }
    evlog("calibration copied to %d outputs", count);
    if (hm && sweepOut >= 0 && sweepOut < MAX_OUT && outs[sweepOut].kind == k) calFullCheck(sweepOut);
    server.send(200, "application/json", outsJson());
  });
  server.on("/outreset", [] {
    int o = argI("o", -1);
    if (o < 0 || o > MAX_OUT) { bad("no such output"); return; }
    if (o == 0) for (int i = 0; i < MAX_OUT; i++) outDefaults(i);
    else outDefaults(o - 1);
    outsEdited();
    if (!saveOuts()) { server.send(503, "text/plain", "reset live, but not kept after a restart: try again"); return; }
    if (o) evlog("output %d reset", o); else evlog("all outputs reset");
    server.send(200, "application/json", outsJson());
  });

  // ---- wear ----
  server.on("/wearreset", [] {              // after service: o = 1-based output
    int o = argI("o", 0);
    if (o < 1 || o > MAX_OUT) { bad("no such output"); return; }
    wearCount[o - 1] = 0;
    wearDirty = true;
    if (!saveWear()) { server.send(503, "text/plain", "reset live, but not kept after a restart: try again"); return; }
    evlog("output %d wear count reset", o);
    sendStatus();
  });
  server.on("/wearsave", [] {
    if (!saveWear()) { server.send(503, "text/plain", "wear counts not saved: try again"); return; }
    sendStatus();
  });

  // ---- Verna ----
  server.on("/say", [] {                    // l = prompt key (see /meta verna), for testing
    int v = vernaByKey(server.arg("l").c_str());
    if (v < 0) { bad("no such prompt"); return; }
    vernaSay((uint8_t)v);                    // still rate-limited, and silent while the voice is off
    sendStatus();
  });
  server.on("/voice", [] {                  // on = 0/1, vol = 0..100
    double on = 0, vol = 0;
    badKey[0] = 0;
    bool hOn = numArg("on", on), hVol = numArg("vol", vol);
    if (badArgs()) return;
    if (hOn)  cfg.voice = on != 0 ? 1 : 0;
    if (hVol) cfg.voiceVol = toI(vol, 0, 100);
    if (!saveCfg()) { server.send(503, "text/plain", "set live, but not kept after a restart: save again"); return; }
    sendStatus();
  });
  server.on("/meta", [] { server.send(200, "application/json", metaJson()); });

  // ---- presets, record, pedals ----
  server.on("/presets", [] { server.send(200, "application/json", presetsJson()); });
  server.on("/preset", [] {                 // s = slot 0..3, p = preset 0..NUM_PRESETS-1 (index into /presets)
    int s = argSlot(), p = server.hasArg("p") ? argI("p", -1) : -1;
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    if (p < 0 || p >= NUM_PRESETS || !server.arg("p").length()) { bad("no such preset"); return; }
    if (recState != 0 && recSlot == s) { server.send(409, "text/plain", "that pattern is recording: stop it first"); return; }
    loadPreset(s, p);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/rec", [] {                    // s = slot 0..3 to arm, or stop=1
    if (server.hasArg("stop") && argI("stop", 0)) { sendMsg(M_REC, 255, 0, 0); sendStatus(); return; }
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    sendMsg(M_REC, (uint8_t)s, 0, 0);
    sendStatus();
  });
  // n = pedal 0..3. act (PedalAct) and out (1-based output a strike pedal strikes) are live; pin and inv (0 auto,
  // 1 pressed HIGH, 2 pressed LOW) are boot-only: saved, then the board restarts, like /pins. press=1 runs the
  // pedal's action now, as if pressed.
  server.on("/pedal", [] {
    int n = server.hasArg("n") && server.arg("n").length() ? argI("n", -1) : -1;
    if (n < 0 || n >= NUM_PEDALS) { bad("no such pedal (n = 0..3)"); return; }
    double pin = 0, act = 0, out = 0, inv = 0, press = 0;
    badKey[0] = 0;
    bool hP = numArg("pin", pin), hA = numArg("act", act), hO = numArg("out", out), hI = numArg("inv", inv);
    bool hG = numArg("press", press);
    if (badArgs()) return;
    if (hA && (act < 0 || act >= (int)P_COUNT || act != floor(act))) { bad("no such pedal action (see /meta acts)"); return; }
    if (hO && (out < 1 || out > MAX_OUT || out != floor(out))) { bad("no such output"); return; }
    if (hI && inv != 0 && inv != 1 && inv != 2) { bad("inv: 0 auto, 1 pressed HIGH, 2 pressed LOW"); return; }
    if (hP && pin != floor(pin)) { bad("pedal pin: a GPIO number, or -1 for none"); return; }
    int newPin = hP ? (int)pin : cfg.pedalPin[n], newInv = hI ? (int)inv : cfg.pedalInv[n];
    int newAct = hA ? (int)act : (int)cfg.pedalAct[n];
    if (newAct == (int)P_PANIC && newInv == 0 && newPin >= 0) {
      bad("a panic pedal needs its polarity set: pressed = HIGH (inv=1) or LOW (inv=2), not auto"); return;
    }
    bool pinChange = newPin != cfg.pedalPin[n] || newInv != cfg.pedalInv[n];
    int ped[NUM_PEDALS], pinv[NUM_PEDALS];
    for (int k = 0; k < NUM_PEDALS; k++) { ped[k] = cfg.pedalPin[k]; pinv[k] = cfg.pedalInv[k]; }
    ped[n] = newPin; pinv[n] = newInv;
    if (pinChange) {
      char why[64] = "";
      if (pinsProblem(cfg.midiPin, cfg.btnPin, cfg.solPin, cfg.pzPin, cfg.pwrPin, ped, why, sizeof(why))) { bad(why); return; }
    }
    if (hA || hO) {
      if (hA) cfg.pedalAct[n] = (uint8_t)act;
      if (hO) cfg.pedalOut[n] = (uint8_t)((int)out - 1);
      if (!saveCfg()) { server.send(503, "text/plain", "set live, but not kept after a restart: save again"); return; }
    }
    if (hG && press != 0 && !pinChange) sendMsg(M_PEDAL, cfg.pedalAct[n], (uint8_t)n, 0);
    if (!pinChange) { sendStatus(); return; }
    requestPanic(true, "pedal pin saved");
    if (!savePinsR2(cfg.midiPin, cfg.btnPin, cfg.solPin, cfg.solInv, cfg.pzPin, cfg.pwrPin, ped, pinv)) {
      server.send(503, "text/plain", "pedal pin not saved, not restarting: try again"); return;
    }
    evlog("pedal %d: pin %d, polarity %d saved, restarting", n + 1, newPin, newInv);
    sendStatus();
    restartAfterReply();
  });

  // ---- sequencer patterns (s = slot 0..3; r = row 0..11; c = step 0..31) ----
  server.on("/seq", [] {
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqcell", [] {
    int s = argSlot(), r = argI("r", -1), c = argI("c", -1);
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    if (r < 0 || r >= SEQ_ROWS || c < 0 || c >= SEQ_STEPS) { bad("no such cell"); return; }
    pats[s].vel[r][c] = (uint8_t)clampi(argI("v", 0), 0, 127);
    patEdited(s);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqrow", [] {                 // o = 1-based output (0 = none), role = DrumRole label; each optional
    int s = argSlot(), r = argI("r", -1);
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    if (r < 0 || r >= SEQ_ROWS) { bad("no such row"); return; }
    double o = 0, role = 0;
    badKey[0] = 0;
    bool hO = numArg("o", o), hR = numArg("role", role);
    if (badArgs()) return;
    if (hO && (o < 0 || o > MAX_OUT || o != floor(o))) { bad("no such output"); return; }
    if (hR && (role < 0 || role >= (int)R_COUNT || role != floor(role))) { bad("role: 0..10 (see /meta roles)"); return; }
    if (hO) pats[s].row[r] = o ? (uint8_t)(o - 1) : SEQ_NONE;
    if (hR) pats[s].rowRole[r] = (uint8_t)role;
    if (hO || hR) patEdited(s);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqmeta", [] {
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    Pattern& p = pats[s];
    double steps = 0, bpm = 0, swing = 0, loop = 0, ts = 0;   // parsed first; nothing changes if any field is bad
    badKey[0] = 0;
    bool hSt = numArg("steps", steps), hB = numArg("bpm", bpm), hSw = numArg("swing", swing), hL = numArg("loop", loop);
    bool hTs = numArg("ts", ts);
    if (badArgs()) return;
    if (hTs && (ts < 0 || ts >= NUM_TS || ts != floor(ts))) { bad("ts: 0..6 (see /meta ts)"); return; }
    if (server.hasArg("name")) { char nm[sizeof(p.name)]; copyName(nm, sizeof(nm), server.arg("name")); memcpy(p.name, nm, sizeof(nm)); }
    // A new time signature sets steps to its bar length (a steps= sent with it is the old value, so it loses);
    // steps= alone still changes the length afterwards.
    bool tsChange = hTs && (int)ts != p.ts;
    if (tsChange) { p.ts = (uint8_t)ts; p.steps = TIME_SIGS[p.ts].steps; }
    else if (hSt) p.steps = (uint8_t)toI(steps, 1, SEQ_STEPS);
    if (hB)  p.bpm   = (uint8_t)toI(bpm, 30, 240);
    if (hSw) p.swing = (uint8_t)toI(swing, 0, 75);
    if (hL)  p.loop  = loop != 0 ? 1 : 0;
    patEdited(s);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqclear", [] {               // clears the cells; rows, name and tempo stay
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    for (int r = 0; r < SEQ_ROWS; r++) for (int c = 0; c < SEQ_STEPS; c++) pats[s].vel[r][c] = 0;
    patEdited(s);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqsave", [] {
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    if (!savePattern(s)) { server.send(503, "text/plain", "pattern not saved: try again"); return; }
    evlog("pattern %d saved", s + 1);
    vernaSay(V_PATTERN_SAVED);
    server.send(200, "application/json", seqJson(s));
  });
  server.on("/seqplay", [] {
    int s = argSlot();
    if (s < 0 || s >= SEQ_SLOTS) { bad("no such slot"); return; }
    sendMsg(M_SEQ, (uint8_t)s, 0, 0);
    sendStatus();
  });
  server.on("/seqstop", [] { sendMsg(M_SEQ, SEQ_NONE, 0, 0); sendStatus(); });

  // ---- settings ----
  server.on("/set", [] {
    // parse every field first: a bad one changes nothing; a blank one leaves that setting as it is
    double cap = 0, sup = 0, mch = 0, gam = 0, fold = 0, lat = 0, solo = 0, sms = 0, port = 0, trig = 0;
    double cc64 = 0, voice = 0, vol = 0, cur = 0;
    badKey[0] = 0;
    bool hCap = numArg("cap", cap), hSup = numArg("supma", sup), hMch = numArg("mch", mch), hGam = numArg("gam", gam);
    bool hFold = numArg("fold", fold), hLat = numArg("lat", lat), hSolo = numArg("solo", solo), hSms = numArg("sms", sms);
    bool hPort = numArg("port", port), hTrig = numArg("trig", trig);
    bool hCc = numArg("cc64", cc64), hVoice = numArg("voice", voice), hVol = numArg("vol", vol), hCur = numArg("cur", cur);
    if (badArgs()) return;
    if (hCc && (cc64 < 0 || cc64 >= (int)P_COUNT || cc64 != floor(cc64))) { bad("cc64: no such pedal action (see /meta acts)"); return; }
    if (hCur && (cur < 0 || cur >= SEQ_SLOTS || cur != floor(cur))) { bad("cur: a slot 0..3"); return; }
    if (hCc)    cfg.cc64Act  = (int)cc64;
    if (hVoice) cfg.voice    = voice != 0 ? 1 : 0;
    if (hVol)   cfg.voiceVol = toI(vol, 0, 100);
    if (hCur)   curSlot      = (int)cur;
    if (hCap)  cfg.capUs    = (int)msToUs((float)cap, 3000, HARD_CAP_US);
    if (hSup)  cfg.supplyMa = toI(sup, 100, 20000);
    if (hMch)  cfg.midiCh   = toI(mch, 0, 16);
    if (hGam && gam >= 0.3 && gam <= 4.0) cfg.gamma = (float)gam;
    if (hFold) cfg.fold  = fold != 0 ? 1 : 0;
    if (hLat)  cfg.latUs = (int)lround((lat > 0 ? (lat > 30 ? 30 : lat) : 0) * 1000.0);
    if (hSolo) cfg.solo  = toI(solo, 0, MAX_OUT);
    if (hSms)  cfg.solMs = toI(sms, SOL_MS_MIN, SOL_MS_MAX);
    // port and trig belong to core 1, and changing one means all off + re-init: only when it really changes
    bool ok = true;
    int wantPort = port != 0 ? 1 : 0, wantTrig = trig != 0 ? 1 : 0;
    if (hPort && wantPort != cfg.portB) ok &= askCore1(M_PORT, wantPort, &cfg.portB);
    if (hTrig && wantTrig != cfg.trig)  ok &= askCore1(M_TRIG, wantTrig, &cfg.trig);
    bool saved = saveCfg();                 // port/trig saved as core 1 now has them
    if (!ok) { evlog("port/trigger change not taken by core 1, not saved"); server.send(503, "text/plain", "port/trigger change not applied, try again"); return; }
    if (!saved) { server.send(503, "text/plain", "set live, but not kept after a restart: save again"); return; }
    sendStatus();
  });

  server.on("/pins", [] {                   // applied at boot only; checked like loadCfg, then saved and restarted
    int midi = argI("midi", cfg.midiPin), btn = argI("btn", cfg.btnPin), sol = argI("sol", cfg.solPin);
    int inv = argI("inv", cfg.solInv) ? 1 : 0, pz = argI("pz", cfg.pzPin), pwr = argI("pwr", cfg.pwrPin);
    char why[64] = "";
    // checked against every role, the pedals too (they keep their pins; /pedal changes those)
    if (pinsProblem(midi, btn, sol, pz, pwr, cfg.pedalPin, why, sizeof(why))) { bad(why); return; }
    requestPanic(true, "pins saved");
    if (!savePinsR2(midi, btn, sol, inv, pz, pwr, cfg.pedalPin, cfg.pedalInv)) {
      server.send(503, "text/plain", "pins not saved, not restarting: try again"); return;
    }
    evlog("pins saved (MIDI %d, button %d, tester %d%s, piezo %d, 24 V sense %d), restarting", midi, btn, sol,
          inv ? " inv" : "", pz, pwr);
    sendStatus();
    restartAfterReply();                    // muted, wear kept, held: nothing is on, or can go on, as it restarts
  });

  server.on("/appass", [] {
    String p = server.arg("p");
    if (p.length() && (p.length() < 8 || p.length() > 63)) { bad("own WiFi password must be 8-63 characters"); return; }
    for (size_t i = 0; i < p.length(); i++)
      if (p[i] < 0x20 || p[i] > 0x7E) { bad("own WiFi password: plain letters, digits and symbols only"); return; }
    saveApPass(p);
    apRestartAt = (millis() + 800) | 1;     // the reply goes out on the old password first
    sendStatus();
  });

  // Async: a blocking scan would stall this task (and so the web /panic) for seconds. /scan?start=1 starts one and
  // answers {"scanning":1}; the page then polls /scan until it gets the list (or {"failed":1}).
  server.on("/scan", [] {
    int n = WiFi.scanComplete();
    if (server.hasArg("start") && n != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      n = WiFi.scanNetworks(true);          // returns at once
    }
    if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "{\"scanning\":1}"); return; }
    if (n < 0) { server.send(200, "application/json", "{\"failed\":1}"); return; }
    String j = "[";
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      String q;
      addStr(q, s.c_str());
      if (j.indexOf(q) >= 0) continue;
      if (j.length() > 1) j += ',';
      j += q;
    }
    WiFi.scanDelete();
    server.send(200, "application/json", j + "]");
  });

  server.on("/wifi", [] {
    if (server.hasArg("forget")) saveWifi("", "");
    else {
      String s = server.arg("ssid"), p = server.arg("pass");
      s.trim();
      if (!s.length() || s.length() > 32) { bad("network name must be 1-32 characters"); return; }
      if (p.length() > 63 || (p.length() && p.length() < 8)) { bad("WiFi password must be 8-63 characters (or none)"); return; }
      saveWifi(s, p);
    }
    sendStatus();
  });

  server.on("/mqtt", [] {
    String h = server.arg("host");
    h.trim();
    if (h.length() >= sizeof(mqttHostBuf)) { bad("broker name too long"); return; }
    if (server.arg("user").length() >= sizeof(mqttUserBuf) || server.arg("pass").length() >= sizeof(mqttPassBuf)) {
      bad("MQTT user and password: 63 characters at most"); return;
    }
    xSemaphoreTake(mqttMux, portMAX_DELAY);  // mqttTask copies these under the same lock
    mqttHost = h;
    mqttPort = (uint16_t)clampi(argI("port", 1883), 1, 65535);
    mqttUser = server.arg("user");
    if (server.hasArg("pass")) mqttPass = server.arg("pass");
    saveMqtt();
    xSemaphoreGive(mqttMux);
    mqttReconf = true;                       // mqttTask disconnects and uses the new broker
    evlog("MQTT set: %s", mqttHost.length() ? mqttHost.c_str() : "off");
    sendStatus();
  });

  // Firmware upload: muted and "updating" for the whole write, and the flash-write hold taken before the first
  // write (and kept through the restart), so no coil is on, or can go on, while flash is busy.
  static bool otaHeld = false;
  server.on("/update", HTTP_POST, [] {
    bool ok = otaHeld && !Update.hasError();
    server.send(200, "text/plain", ok ? "ok" : otaHeld ? "update failed" : "update refused: coils not quiet, try again");
    evlog("%s", ok ? "update written, restarting" : "update failed");
    if (ok) { delay(300); ESP.restart(); }
    if (otaHeld) releaseCoils();
    otaHeld = false;
    updating = false;
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      updating = true;
      calAbort = true;
      requestPanic(true, "update");
      if (cfg.voice) {                       // "Updating the chimes": let it play out before flash gets busy
        vernaSay(V_UPDATING);
        uint32_t t0 = millis();
        while (millis() - t0 < 2500 && (vernaPlaying == V_UPDATING || millis() - t0 < 400)) delay(10);
      }
      if (wearDirty) saveWear();             // the lifetime counts survive the update
      if (otaHeld) releaseCoils();           // a restarted upload: one hold per upload
      otaHeld = holdCoils(HOLD_WAIT_MS);
      if (!otaHeld) { evlog("update: coils not quiet, refused"); return; }
      evlog("update: writing %s", u.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) evlog("update: can't start");
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (otaHeld && Update.isRunning()) Update.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
      if (otaHeld && Update.isRunning()) Update.end(true);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      if (otaHeld) releaseCoils();
      otaHeld = false;
      updating = false;
      evlog("update: upload stopped");
    }
  });

  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });   // captive page
  server.begin();
}

// ---------------- USB serial console ----------------
static void help() {
  Serial.println(
    "commands:\n"
    "  scan                 rescan the driver boards\n"
    "  fire <ms> [out]      one exact strike (default output 1), clamped to its cap\n"
    "  out <n> [vel]        strike output n (alias: tube)\n"
    "  n <note> [vel]       play a MIDI note\n"
    "  sweep <n>            calibration sweep of output n\n"
    "  walk [vel] [gap]     strike every output in turn (vel 20-127, gap 250-1500 ms)\n"
    "  demo <1-3|0>         demo song, 0 = stop\n"
    "  seq <1-4|0>          play a pattern, 0 = stop\n"
    "  rec <1-4|0>          MIDI record into a pattern, 0 = stop\n"
    "  preset <slot> <n>    load world-rhythm preset n (1-based) into pattern 1-4 (not saved)\n"
    "  say <key>            play one of Verna's prompts (e.g. ready, tune_on)\n"
    "  tap                  tester tap\n"
    "  panic | mute 1 | unmute\n"
    "  status               status JSON\n"
    "  wifi<TAB>ssid<TAB>pass   set the hall WiFi\n"
    "  help");
}

static void printBoards() {
  for (int b = 0; b < MAX_BOARDS; b++) {
    uint8_t st = bState[b];
    Serial.printf("driver 0x%02X: %s  errors %lu\n", BOARD_BASE + b,
                  st == B_OK ? "OK" : st == B_FAULT ? "FAULT" : "not found", (unsigned long)bErr[b]);
  }
}

static void command(String l) {
  if (l.startsWith("wifi\t")) {
    int t = l.indexOf('\t', 5);
    String s = t > 0 ? l.substring(5, t) : l.substring(5), p = t > 0 ? l.substring(t + 1) : "";
    saveWifi(s, p);
    Serial.printf("hall WiFi set to \"%s\", joining\n", staSsid.c_str());
    return;
  }
  l.trim();
  if (!l.length()) return;
  char buf[64];
  strlcpy(buf, l.c_str(), sizeof(buf));
  char* save = nullptr;
  char* cmd = strtok_r(buf, " \t", &save);
  char* a1  = strtok_r(nullptr, " \t", &save);
  char* a2  = strtok_r(nullptr, " \t", &save);
  if (!cmd) return;
  String c(cmd);
  c.toLowerCase();

  if (c == "scan") {
    sendMsg(M_RESCAN, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    printBoards();
  } else if (c == "fire") {
    if (!a1) { Serial.println("fire <ms> [out]"); return; }
    int o = a2 ? atoi(a2) : 1;
    if (o < 1 || o > MAX_OUT) { Serial.println("no such output"); return; }
    uint32_t us = msToUs((float)atof(a1), MIN_PULSE_US, HARD_CAP_US);
    sendMsg(M_EXACT, (uint8_t)(o - 1), 100, us);
    Serial.printf("fire output %d, %.1f ms\n", o, us / 1000.0);
  } else if (c == "out" || c == "tube") {
    int o = a1 ? atoi(a1) : 0;
    if (o < 1 || o > MAX_OUT) { Serial.println("out <n> [vel]"); return; }
    sendMsg(M_OUT, (uint8_t)(o - 1), (uint8_t)clampi(a2 ? atoi(a2) : 100, 1, 127), 0);
  } else if (c == "n") {
    if (!a1) { Serial.println("n <note> [vel]"); return; }
    sendMsg(M_NOTE, (uint8_t)clampi(atoi(a1), 0, 127), (uint8_t)clampi(a2 ? atoi(a2) : 100, 1, 127), 0);
  } else if (c == "sweep") {
    int o = a1 ? atoi(a1) : 0;
    if (o < 1 || o > MAX_OUT) { Serial.println("sweep <n>"); return; }
    Serial.println(calStart(CAL_SWEEP, o - 1) ? "sweep started" : "busy");
  } else if (c == "walk") {
    if (calBusy()) { Serial.println("busy"); return; }
    walkVel = clampi(a1 ? atoi(a1) : 100, 20, 127);
    walkGapMs = clampi(a2 ? atoi(a2) : 450, 250, 1500);
    bool ok = calStart(CAL_WALK, 0);
    if (!ok) { walkVel = 100; walkGapMs = 450; }
    Serial.println(ok ? "walk started" : "busy");
  } else if (c == "rec") {
    int s = a1 ? atoi(a1) : -1;
    if (!a1 || s < 0 || s > SEQ_SLOTS) { Serial.println("rec <1-4|0>"); return; }
    sendMsg(M_REC, s ? (uint8_t)(s - 1) : 255, 0, 0);
  } else if (c == "preset") {
    int s = a1 ? atoi(a1) : 0, p = a2 ? atoi(a2) : 0;
    if (s < 1 || s > SEQ_SLOTS || p < 1 || p > NUM_PRESETS) { Serial.printf("preset <1-4> <1-%d>\n", NUM_PRESETS); return; }
    if (recState != 0 && recSlot == s - 1) { Serial.println("that pattern is recording: stop it first"); return; }
    loadPreset(s - 1, p - 1);
    Serial.printf("pattern %d: %s (not saved)\n", s, PRESETS[p - 1].name);
  } else if (c == "say") {
    int v = vernaByKey(a1);
    if (v < 0) {
      Serial.print("say <key>:");
      for (int k = 0; k < V_COUNT; k++) Serial.printf(" %s", VERNA[k].key);
      Serial.println();
      return;
    }
    vernaSay((uint8_t)v);
  } else if (c == "demo") {
    sendMsg(M_DEMO, (uint8_t)clampi(a1 ? atoi(a1) : 0, 0, NUM_SONGS), 0, 0);
  } else if (c == "seq") {
    int s = clampi(a1 ? atoi(a1) : 0, 0, SEQ_SLOTS);
    sendMsg(M_SEQ, s ? (uint8_t)(s - 1) : SEQ_NONE, 0, 0);
  } else if (c == "tap") {
    sendMsg(M_TAP, 0, 0, 0);
  } else if (c == "panic") {
    requestPanic(true, "serial");
  } else if (c == "mute") {
    if (a1 && a1[0] == '0') unmute("serial"); else requestPanic(true, "serial mute");
  } else if (c == "unmute") {
    unmute("serial");
  } else if (c == "status") {
    Serial.println(statusJson());
  } else if (c == "help" || c == "?") {
    help();
  } else {
    Serial.println("? (help)");
  }
}

static void serialService() {
  static String line;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.length() < 200) line += ch; continue; }
    String l = line;
    line = "";
    command(l);
  }
}

static void logService() {          // new log lines -> USB serial (core 0 is the only place that prints)
  static uint32_t printed = 0;
  uint32_t lc = logCount;
  if (lc - printed > (uint32_t)LOG_N) printed = lc - LOG_N;
  while (printed < lc) Serial.println(logLine(printed++));
}

// ---------------- round 2 housekeeping (every net pass) ----------------
static void round2Service(uint32_t ms) {
  static bool first = true;
  static uint32_t lastWearSave = 0, seenPanics = 0;
  if (first) { first = false; lastWearSave = ms; seenPanics = nPanics; }
  if (calState == CAL_SWEEP) calFullArmed = true;     // a sweep is (re)running: coach its min once more
  // Wear counts: on a muting panic, and every 10 minutes while they've changed.
  uint32_t np = nPanics;
  if (np != seenPanics) {
    seenPanics = np;
    if (muted && wearDirty && !updating) { saveWear(); lastWearSave = ms; }
  }
  if (wearDirty && !updating && ms - lastWearSave >= 600000UL) { lastWearSave = ms; saveWear(); }
  // A finished MIDI take (core 1 set recState 3): keep it, then tell the player.
  if (recState == 3) {
    int s = recSlot, n = recNotes;
    if (n > 0 && s >= 0 && s < SEQ_SLOTS) {
      if (savePattern(s)) { evlog("recording kept in pattern %d (%d notes)", s + 1, n); vernaSay(V_REC_SAVED); }
      else evlog("recording in pattern %d NOT saved: press Save", s + 1);
    } else {
      evlog("recording stopped, nothing played");
      vernaSay(V_REC_STOP);
    }
    recSlot = -1;
    recState = 0;
  }
  // curSlot changes on core 1 too (pedals): keep it in NVS "cur", 5 s after the last change, through the held save.
  static int savedCur = -1, lastCur = -1;
  static uint32_t curChangedAt = 0;
  int cur = curSlot;
  if (savedCur < 0) savedCur = lastCur = cur;          // the value loadCfg read at boot
  if (cur != lastCur) { lastCur = cur; curChangedAt = ms | 1; }
  if (curChangedAt && cur != savedCur && !updating && ms - curChangedAt >= 5000) {
    if (cur >= 0 && cur < SEQ_SLOTS && saveCfg()) { savedCur = cur; curChangedAt = 0; }
    else curChangedAt = ms | 1;                        // not quiet in time: try again in 5 s
  }
  if (cur == savedCur) curChangedAt = 0;
}

// ---------------- the task ----------------
void netTask(void*) {
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t i) { staWhy = i.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);   // keep it tiny: the event task may run on core 1
  WiFi.persistent(false);                // no WiFi-library flash writes behind waitQuiet's back
  WiFi.setHostname(DEVICE_NAME);
  WiFi.mode(WIFI_AP_STA);
  apStart("boot");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);          // the paced join below does it
  joinSsid = staSsid; joinPass = staPass;
  if (staSsid.length()) { WiFi.begin(staSsid.c_str(), staPass.c_str()); staTry = millis() | 1; }
  if (MDNS.begin(DEVICE_NAME)) MDNS.addService("http", "tcp", 80);
  mqttMux = xSemaphoreCreateMutex();
  setupWeb();
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, nullptr, 1, nullptr, 0);
  nvsTidy();                             // core 1 is running by now, so its flash-write hold works

  logService();
  Serial.printf("%s  own WiFi \"%s\" (password %s) -> http://%s   hall WiFi: %s   (help for commands)\n",
                FW_VERSION, AP_SSID, apPassNow().c_str(), WiFi.softAPIP().toString().c_str(),
                staSsid.length() ? staSsid.c_str() : "not set");

  for (;;) {
    uint32_t ms = millis();
    if (apOn) dns.processNextRequest();
    server.handleClient();
    staService(ms);
    apService(ms);
    serialService();
    round2Service(ms);
    logService();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
