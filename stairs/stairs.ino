// Hall stair lights: any ESP32-S3 (DevKit or Super Mini), addressable strip on GPIO 4, second run on GPIO 5 (stairs + landing).
// USB: commands and the boot line work on the native USB port and on a DevKit's UART port (Serial0).
// Then join WiFi "Stairs" (open, no password) -> 192.168.4.1
//
// What it does: the whole strip shows one colour at one brightness. That's the only control, on purpose.
//               A Test button runs every colour and the busy effects so a new strip can be checked end to end.
// Strip: WS2812B / WS2811 / SK6812-RGB style (one data wire), including addressable neon flex, 5, 12 or 24 V.
//        Colour order is a setting (GRB for most WS2812B, RGB/BRG for many WS2811 and neon), so no rebuild for a new reel.
// Wiring: data out through a 74AHCT125 (5 V) + 330 R at the board, all grounds tied, BIN to GND on WS2815.
// Power: a supply limit in mA (default 4200 = a 5 V 5 A supply with margin). Brightness is cut so the estimate
//        never goes over it. Set "mA per pixel at full white" from the reel's W/m (W/m / V / pixels per m * 1000).
// Network: always its own "Stairs" AP (192.168.4.1, captive page); join the hall WiFi from the page (ClarkShop preset)
//          -> http://hall-stairs.local, http://hall-stairs or its IP. MQTT is optional (blank broker = off).
//          Credentials live in NVS, never in this public repo.
// Firmware: page -> Install latest (phone fetches the newest build from Pages and posts it to /update),
//           or Upload file, or from a computer:  curl -F "f=@stairs.bin" http://hall-stairs.local/update
// Core split: core 1 = the LEDs (loop). core 0 = WiFi, web, MQTT, updates (netTask). LEDs hold still during an update.

#include <WiFi.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <PubSubClient.h>
#include <FastLED.h>

#if __has_include("version.h")
#include "version.h"                // CI writes FW_VERSION = the short commit SHA (same as stairs/manifest.json)
#endif
#ifndef FW_VERSION
#define FW_VERSION  "dev"
#endif
#define LATEST_URL  "https://jobot101.github.io/Ragamuffin-Flasher/stairs/"
#define DEF_NAME    "stairs"        // board name (page setting): hall-<name>.local, own WiFi "<Name>", ragamuffin/hall/<name>/*
#define AP_PASS     nullptr       // open network: the owner wants no password on "Stairs"
#define HALL_SSID   "STARLINK"      // the hall network: page default + preset. Password typed once (page or USB serial), NVS only
#define HOME_SSID   "ClarkShop"     // shop preset; never put a password in this public repo
#define PIN_A       4               // output 1 (Super Mini GPIO 4)
#define PIN_B       5               // output 2 (Super Mini GPIO 5), 0 pixels = off
#define MAX_PIX     4000            // per output (~120 ms per frame at 4000; solid colour only redraws on change)
#define BOOT_BTN    0

struct Cfg {
  int nA = 300, nB = 0;             // pixels (chips) per output; changing them restarts the board
  int ord = 0;                      // wire colour order, index into ORDERS
  int limMA = 4200;                 // supply limit, 0 = no limit: GPIO 4's supply, or both outputs when limB = 0
  int limB = 0;                     // GPIO 5's own supply limit, 0 = GPIO 5 shares limMA with GPIO 4
  int pixMA = 60;                   // one pixel at full white (WS2812B 5 V = 60)
  int trimB = 100;                  // output 2 level %
  int fadeMs = 600;
  // step map: the first pixel of each step, top to bottom (pixel numbers grow going down), per output.
  // Tuned on the stairs Nov 28 2026 (stairs/HANDOFF.md): start-end/startBorder/endBorder. GPIO 5's first entry is the landing walls.
  String stepsA = "0-136/-1/15,137-309/15/25,310-467/13/22,468-631/27/-1,632-783/15/-1,784-939/15/17,940-1091,1092-1243,1244-1397,1398-1545,1546-1699";
  String stepsB = "0-511/15/-1,512-675/22/22,681-833/17/17,851-1003/18/18,1009-1161/18/19,1183-1334/19/16,1339-1491/18/19,1492-1644/20/15,1659-1811/-1/20,1819-1971/20/15,1993-2135/15/14";
  uint8_t br = 255, bg = 0, bb = 255;   // border colour (magenta, as tuned)
  int borderPx = 15;                    // border at each end of a step
  int middlePx = 0;                     // middle stripe width, 0 = everything between the borders
  // night mode: between nightFrom and nightTo (minutes after midnight, local time from the internet) only the
  // borders glow, at nightLv % of the brightness; "wake" (page button or serial w, later a touch plate or sensor)
  // walks the middles up to full, holds nightWake seconds, then lets them go back to the outline.
  bool nightOn = false;
  int nightFrom = 22 * 60, nightTo = 6 * 60 + 30, nightLv = 8, nightWake = 60;
  String tz = "EST5EDT,M3.2.0,M11.1.0";   // POSIX time zone: Pennsylvania
} cfg;
const char* ORDERS[] = {"GRB", "RGB", "BRG", "RBG", "GBR", "BGR"};
const int NORD = 6;

struct Look { uint8_t r, g, b; int bri; bool test; int mode; int spd; };
Look look = {255, 147, 41, 40, true, 0, 50};
// Modes (the everyday choices; Test is separate). Speed 50 = 1x.
const int NMODES = 16;
const char* MODEN[] = {"solid", "pulse + colour", "breathe", "spin", "ripple", "glass", "candle",
                       "christmas", "candy cane", "snowfall", "twinkle", "runner", "walk up", "measure 4", "measure 5", "arrow"};
bool modeUsesColour(int m) { return m == 0 || m == 2 || m == 4 || m == 11 || m == 12 || m == 15; }
// NVS namespace stays "stairs" (this started as the stair lights) so a board moved over keeps its WiFi and strip settings   // fresh board: Test on at 40 % so a new strip shows everything straight away
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lookVer = 1;
volatile bool updating = false;
// Flash safety: the LED driver's RMT interrupt isn't cache-safe, so a flash write (NVS, WiFi calibration, OTA)
// while a frame is going out crashes the chip ("Cache disabled but cached memory region accessed").
// LEDs start only after WiFi is up (its first-boot calibration write is done), and every NVS write holds them.
volatile bool netReady = false, ledHold = false, ledBusy = false;
struct FlashHold {
  FlashHold()  { ledHold = true; while (ledBusy) delay(1); delay(3 + cfgFrameMs()); }
  ~FlashHold() { ledHold = false; }
  static int cfgFrameMs();
};
volatile int estMA = 0; volatile bool capped = false;
uint32_t lookDirtyAt = 0;           // save look to NVS 2 s after the last change (sliders send a lot)

CRGB* ledA = nullptr; CRGB* ledB = nullptr;
WebServer server(80);
DNSServer dns;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String devName = DEF_NAME, HOSTNAME_S, AP_S, TOPIC_S;   // set from the name at boot
#define HOSTNAME   HOSTNAME_S.c_str()
#define AP_SSID    AP_S.c_str()
#define TOPIC_BASE TOPIC_S.c_str()
String cleanName(String n) {          // lower-case letters, digits and dashes only
  n.trim(); n.toLowerCase(); String o;
  for (char c : n) if (isalnum(c) || c == '-') o += c;
  return o.length() ? o.substring(0, 20) : String(DEF_NAME);
}
void applyName() {
  HOSTNAME_S = "hall-" + devName;
  AP_S = devName; AP_S[0] = toupper(AP_S[0]);
  TOPIC_S = "ragamuffin/hall/" + devName;
}
String staSsid, staPass, mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;
uint8_t staWhy = 0;
uint32_t staTry = 0, staNext = 0;
volatile bool pubState = true;

String esc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return s; }

// ---------- look ----------
Look getLook() { portENTER_CRITICAL(&mux); Look l = look; portEXIT_CRITICAL(&mux); return l; }
void setLook(const Look& l) {
  portENTER_CRITICAL(&mux); look = l; look.bri = constrain(look.bri, 0, 100); lookVer++; portEXIT_CRITICAL(&mux);
  lookDirtyAt = millis() | 1; pubState = true;
}
bool parseHex(String h, uint8_t& r, uint8_t& g, uint8_t& b) {
  h.trim(); if (h.startsWith("#")) h = h.substring(1);
  if (h.length() != 6) return false;
  for (char ch : h) if (!isxdigit(ch)) return false;
  uint32_t v = strtoul(h.c_str(), nullptr, 16); r = v >> 16; g = v >> 8; b = v; return true;
}
// apply "c=rrggbb&b=30&test=0" (page query, MQTT payload or serial)
bool applyLookArgs(std::function<bool(const char*)> has, std::function<String(const char*)> get) {
  Look l = getLook(); bool any = false;
  if (has("c")) { uint8_t r, g, b; if (!parseHex(get("c"), r, g, b)) return false; l.r = r; l.g = g; l.b = b; l.test = false; if (!modeUsesColour(l.mode)) l.mode = 0; any = true; }
  if (has("b")) { l.bri = constrain(get("b").toInt(), 0, 100); any = true; }
  if (has("test")) { l.test = get("test").toInt(); any = true; }
  if (has("m")) { l.mode = constrain(get("m").toInt(), 0, NMODES - 1); l.test = false; any = true; }
  if (has("s")) { l.spd = constrain(get("s").toInt(), 1, 100); any = true; }
  if (any) setLook(l);
  return true;
}

// ---------- config ----------
// (after struct Look: Arduino puts its prototypes above the first function, so no function may come before the types)
const int MAX_STEPS = 40;
int nStA = 0, nStB = 0, stA[MAX_STEPS], stB[MAX_STEPS], enA[MAX_STEPS], enB[MAX_STEPS];   // start, end (exclusive)
int bsA[MAX_STEPS], beA[MAX_STEPS], bsB[MAX_STEPS], beB[MAX_STEPS];                       // border at the start / end, -1 = the global width
// step editor (page → Steps): light one step so its edges can be nudged; the map isn't saved until /cfg
volatile uint32_t stepPrevUntil = 0; volatile int prevI = 0; volatile bool prevB = false;
// map string: one entry per step, top first, "start-end" (end included) or just "start" (runs to the next step),
// optionally "/startBorder/endBorder" in pixels (e.g. "138-300/20/12"); without them the global border width is used.
// Pixels between one step's end and the next step's start stay dark (a loop of strip round a corner).
void parseSteps(const String& s, int* out, int* outE, int* bS, int* bE, int& n, int npix) {
  int a[MAX_STEPS], b[MAX_STEPS], ba[MAX_STEPS], bb[MAX_STEPS], m = 0, from = 0;
  String t = s + ",";
  for (int i = 0; i < (int)t.length() && m < MAX_STEPS; i++) {
    if (t[i] != ',') continue;
    String tok = t.substring(from, i); tok.trim(); from = i + 1;
    if (!tok.length() || !isdigit(tok[0])) continue;
    int dash = tok.indexOf('-'), s1 = tok.indexOf('/'), s2 = s1 > 0 ? tok.indexOf('/', s1 + 1) : -1;
    if (dash > s1 && s1 > 0) dash = -1;
    a[m] = tok.toInt(); b[m] = dash > 0 ? tok.substring(dash + 1).toInt() + 1 : -1;
    ba[m] = s1 > 0 ? tok.substring(s1 + 1).toInt() : -1; bb[m] = s2 > 0 ? tok.substring(s2 + 1).toInt() : -1; m++;
  }
  n = 0;
  for (int k = 0; k < m; k++) {
    int st = a[k], en = min(b[k] >= 0 ? b[k] : (k + 1 < m ? a[k + 1] : npix), npix);
    if (st >= npix || en <= st || (n && st < outE[n - 1])) continue;
    out[n] = st; outE[n] = en; bS[n] = ba[k]; bE[n] = bb[k]; n++;
  }
  if (!n && npix > 0) { out[0] = 0; outE[0] = npix; bS[0] = bE[0] = -1; n = 1; }   // no map: the whole output is one step
}
void parseAllSteps() { parseSteps(cfg.stepsA, stA, enA, bsA, beA, nStA, cfg.nA); parseSteps(cfg.stepsB, stB, enB, bsB, beB, nStB, cfg.nB); }

void loadAll() {
  FlashHold fh; Preferences prefs; prefs.begin("stairs", true);
  cfg.nA = prefs.getInt("nA", cfg.nA); cfg.nB = prefs.getInt("nB", cfg.nB); cfg.ord = prefs.getInt("ord", cfg.ord);
  cfg.limMA = prefs.getInt("lim", cfg.limMA); cfg.limB = prefs.getInt("limB", cfg.limB); cfg.pixMA = prefs.getInt("pma", cfg.pixMA);
  cfg.trimB = prefs.getInt("trim", cfg.trimB); cfg.fadeMs = prefs.getInt("fade", cfg.fadeMs);
  devName = cleanName(prefs.getString("name", DEF_NAME)); applyName();
  cfg.stepsA = prefs.getString("stA", cfg.stepsA); cfg.stepsB = prefs.getString("stB", cfg.stepsB);
  cfg.br = prefs.getUChar("bcr", cfg.br); cfg.bg = prefs.getUChar("bcg", cfg.bg); cfg.bb = prefs.getUChar("bcb", cfg.bb);
  cfg.borderPx = prefs.getInt("bw", cfg.borderPx); cfg.middlePx = prefs.getInt("mw", cfg.middlePx);
  cfg.nightOn = prefs.getBool("nOn", cfg.nightOn); cfg.nightFrom = prefs.getInt("nFr", cfg.nightFrom); cfg.nightTo = prefs.getInt("nTo", cfg.nightTo);
  cfg.nightLv = prefs.getInt("nLv", cfg.nightLv); cfg.nightWake = prefs.getInt("nWk", cfg.nightWake); cfg.tz = prefs.getString("tz", cfg.tz);
  look.r = prefs.getUChar("r", look.r); look.g = prefs.getUChar("g", look.g); look.b = prefs.getUChar("b", look.b);
  look.bri = prefs.getInt("bri", look.bri); look.test = prefs.getBool("test", look.test);
  look.mode = constrain((int)prefs.getInt("mode", 0), 0, NMODES - 1); look.spd = constrain((int)prefs.getInt("spd", 50), 1, 100);
  staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("wpass", "");
  mqttHost = prefs.getString("mhost", ""); mqttPort = prefs.getUShort("mport", 1883);
  mqttUser = prefs.getString("muser", ""); mqttPass = prefs.getString("mpass", "");
  prefs.end();
  cfg.nA = constrain(cfg.nA, 1, MAX_PIX); cfg.nB = constrain(cfg.nB, 0, MAX_PIX);
  if (cfg.ord < 0 || cfg.ord >= NORD) cfg.ord = 0;
  parseAllSteps();
}
void saveCfg() {
  FlashHold fh; Preferences prefs; prefs.begin("stairs", false);
  prefs.putInt("nA", cfg.nA); prefs.putInt("nB", cfg.nB); prefs.putInt("ord", cfg.ord);
  prefs.putInt("lim", cfg.limMA); prefs.putInt("limB", cfg.limB); prefs.putInt("pma", cfg.pixMA); prefs.putInt("trim", cfg.trimB); prefs.putInt("fade", cfg.fadeMs);
  prefs.putString("stA", cfg.stepsA); prefs.putString("stB", cfg.stepsB);
  prefs.putUChar("bcr", cfg.br); prefs.putUChar("bcg", cfg.bg); prefs.putUChar("bcb", cfg.bb);
  prefs.putInt("bw", cfg.borderPx); prefs.putInt("mw", cfg.middlePx);
  prefs.putBool("nOn", cfg.nightOn); prefs.putInt("nFr", cfg.nightFrom); prefs.putInt("nTo", cfg.nightTo);
  prefs.putInt("nLv", cfg.nightLv); prefs.putInt("nWk", cfg.nightWake); prefs.putString("tz", cfg.tz);
  prefs.end();
}
void saveLook() {
  Look l = getLook();
  FlashHold fh; Preferences prefs; prefs.begin("stairs", false);
  prefs.putUChar("r", l.r); prefs.putUChar("g", l.g); prefs.putUChar("b", l.b); prefs.putInt("bri", l.bri); prefs.putBool("test", l.test); prefs.putInt("mode", l.mode); prefs.putInt("spd", l.spd);
  prefs.end();
}

int FlashHold::cfgFrameMs() { return (cfg.nA + cfg.nB) * 30 / 1000 + 1; }   // ~30 us per pixel on the wire

// ---------- LEDs (core 1) ----------
// FastLED sends bytes as given (RGB); the strip's real order is applied here so a new reel is a setting, not a build.
static inline CRGB wire(const CRGB& c) {
  const char* o = ORDERS[cfg.ord]; CRGB w;
  for (int k = 0; k < 3; k++) w.raw[k] = o[k] == 'R' ? c.r : o[k] == 'G' ? c.g : c.b;
  return w;
}
uint8_t hue = 0, fx = 0;
const uint8_t NFX = 7;
const char* FXN[] = {"colour sweep", "rainbow", "glitter", "confetti", "sinelon", "juggle", "party"};
void effect(CRGB* l, int n) {
  if (n <= 0) return;
  switch (fx) {
    case 0: fill_solid(l, n, CHSV(hue, 255, 255)); break;
    case 1: fill_rainbow(l, n, hue, constrain(510 / n, 1, 255)); break;
    case 2: fill_rainbow(l, n, hue, constrain(510 / n, 1, 255)); for (int k = 0; k < 1 + n / 60; k++) if (random8() < 80) l[random16(n)] += CRGB::White; break;
    case 3: fadeToBlackBy(l, n, 10); for (int k = 0; k < 1 + n / 100; k++) l[random16(n)] += CHSV(hue + random8(64), 200, 255); break;
    case 4: fadeToBlackBy(l, n, 20); l[beatsin16(13, 0, n - 1)] += CHSV(hue, 255, 192); break;
    case 5: { fadeToBlackBy(l, n, 20); uint8_t h = 0; for (int i = 0; i < 8; i++) { l[beatsin16(i + 7, 0, n - 1)] |= CHSV(h, 200, 255); h += 32; } } break;
    default: { uint8_t beat = beatsin8(62, 64, 255); for (int i = 0; i < n; i++) l[i] = ColorFromPalette(PartyColors_p, hue + i * 2, beat - hue + i * 10); }
  }
}
// test effects draw in logical RGB and fade in place, so keep logical buffers and convert on the way out
CRGB* tA = nullptr; CRGB* tB = nullptr;


// sumA / sumB: r+g+b summed over every pixel of GPIO 4 / GPIO 5 at full scale.
// The supply limit sets what 100 % means: full brightness is first cut so no output goes over its own supply
// (limB > 0: GPIO 4 on limMA, GPIO 5 on limB; else both share limMA), then the brightness slider is a share of that.
// So every move of the slider changes the light, even when the strip could draw far more than the supplies give.
volatile int estA = 0, estB = 0;
void showFrame(float briPct, uint32_t sumA, uint32_t sumB) {
  auto mA = [&](uint32_t sum, int br) { return (int)((double)sum / 765.0 * cfg.pixMA * br / 255.0); };
  int a = mA(sumA, 255), b = mA(sumB, 255), top = 255; bool cut = false;
  auto fit = [&](int ma, int lim) { if (lim > 0 && ma > lim) { top = min(top, max(1, (int)(255LL * lim / ma))); cut = true; } };
  if (cfg.limB > 0) { fit(a, cfg.limMA); fit(b, cfg.limB); } else fit(a + b, cfg.limMA);
  uint8_t bri = briPct <= 0.05f ? 0 : (uint8_t)max(1, (int)(top * briPct / 100.0f + .5f));
  estA = mA(sumA, bri); estB = mA(sumB, bri); estMA = estA + estB; capped = cut;
  if (ledHold || updating) return;
  ledBusy = true;
  FastLED.setBrightness(bri);
  FastLED.show();
  ledBusy = false;
}

// stretch a 256-step pattern along the whole GPIO 4 run (GPIO 5 copies it in renderMode)
void layV(const CRGB* v) { for (int i = 0; i < cfg.nA; i++) tA[i] = v[(i * 256) / cfg.nA]; }

// measure: a ruler for filming the stairs, one output at a time (the other stays dark).
// Read by counting, not by colour (phone cameras shift colours):
//   every 10th pixel = a blue dot;
//   every 100th pixel = a 3-pixel bar, then the hundreds digit as that many red dots, 3 pixels apart
//     (e.g. pixel 700 = bar + 7 red dots); the bar is white below 1000, green from 1000, yellow from 2000;
//   pixel 0 = 5 green, the last pixel = 5 red.
void measure(CRGB* l, int n) {
  fill_solid(l, n, CRGB::Black);
  for (int i = 0; i < n; i += 10) if (i % 100 == 0 || i % 100 >= 40) l[i] = CRGB(0, 0, 255);
  for (int i = 100; i < n; i += 100) {
    CRGB bar = i < 1000 ? CRGB::White : i < 2000 ? CRGB(0, 255, 0) : CRGB(255, 200, 0);
    for (int k = -1; k <= 1; k++) if (i + k < n) l[i + k] = bar;
    for (int j = 1; j <= (i / 100) % 10; j++) if (i + 1 + 3 * j < n) l[i + 1 + 3 * j] = CRGB(255, 0, 0);
  }
  for (int k = 0; k < 5 && k < n; k++) { l[k] = CRGB(0, 255, 0); l[n - 1 - k] = CRGB(255, 0, 0); }
}

// runner: every step gets a border at both ends (border colour) and a stripe of the chosen colour up the middle.
void paintStep(CRGB* l, int from, int to, int bwS, int bwE, CRGB mid, uint8_t lvBorder, uint8_t lvMid) {
  int len = to - from; if (len <= 0) return;
  fill_solid(l + from, len, CRGB::Black);
  CRGB bc(cfg.br, cfg.bg, cfg.bb); bc.nscale8_video(lvBorder); mid.nscale8_video(lvMid);
  if (bwS < 0) bwS = cfg.borderPx; if (bwE < 0) bwE = cfg.borderPx;
  bwS = min(bwS, len); bwE = min(bwE, len - bwS);
  int m0 = from + bwS, m1 = to - bwE;
  if (cfg.middlePx > 0 && cfg.middlePx < m1 - m0) { int c = (m0 + m1) / 2; m0 = c - cfg.middlePx / 2; m1 = m0 + cfg.middlePx; }
  if (bwS > 0) fill_solid(l + from, bwS, bc);
  if (bwE > 0) fill_solid(l + to - bwE, bwE, bc);
  if (m1 > m0) fill_solid(l + m0, m1 - m0, mid);
}
// steps in walking order: k = 0 is the bottom of GPIO 5 (lower flight), up through the landing, then GPIO 4 bottom to top
void renderSteps(CRGB mid, uint32_t t, bool walk) {
  const uint32_t STEP = 400, HOLD = 5000, GAP = 1500;           // ms at speed 1x
  int total = nStB + nStA;
  fill_solid(tA, cfg.nA, CRGB::Black); if (tB) fill_solid(tB, cfg.nB, CRGB::Black);   // gaps between steps stay dark
  uint32_t up = (uint32_t)total * STEP, cycle = up + HOLD + up + GAP, p = t % cycle;
  for (int k = 0; k < total; k++) {
    bool onB = k < nStB;
    int i = onB ? nStB - 1 - k : nStA - 1 - (k - nStB);
    int* st = onB ? stB : stA; int* en = onB ? enB : enA; int* bs = onB ? bsB : bsA; int* be = onB ? beB : beA;
    uint8_t lv = 255;
    if (walk) {                                                  // middles on one by one going up, hold, off one by one going up; borders stay lit
      uint32_t on = k * STEP, off = up + HOLD + k * STEP;
      lv = p < on ? 0 : p < on + STEP ? (p - on) * 255 / STEP : p < off ? 255 : p < off + STEP ? 255 - (p - off) * 255 / STEP : 0;
    }
    paintStep(onB ? tB : tA, st[i], en[i], bs[i], be[i], mid, 255, lv);
  }
}

// arrow: a big arrow climbing the stairs, drawn across several steps: the point narrow on its top step, the head
// widening over the steps below, then a thinner shaft. Step borders stay lit; the arrow is the chosen colour.
void renderArrow(CRGB mid, uint32_t t) {
  const float STEP = 300, HEAD = 4, SHAFT = 4;   // ms per step at 1x; head and shaft height in steps
  int total = nStB + nStA;
  fill_solid(tA, cfg.nA, CRGB::Black); if (tB) fill_solid(tB, cfg.nB, CRGB::Black);
  float tip = fmodf(t / STEP, total + HEAD + SHAFT + 2);   // 0 = the bottom step
  for (int k = 0; k < total; k++) {
    bool onB = k < nStB;
    int i = onB ? nStB - 1 - k : nStA - 1 - (k - nStB);
    int* st = onB ? stB : stA; int* en = onB ? enB : enA; int* bs = onB ? bsB : bsA; int* be = onB ? beB : beA;
    CRGB* l = onB ? tB : tA;
    paintStep(l, st[i], en[i], bs[i], be[i], CRGB::Black, 255, 0);   // borders only
    float d = tip - k, hw = 0;
    if (d >= 0 && d < HEAD) hw = 0.06f + 0.44f * d / HEAD;           // the head widens below the point
    else if (d >= HEAD && d < HEAD + SHAFT) hw = 0.12f;              // the shaft
    if (hw <= 0) continue;
    int len = en[i] - st[i], ws = min(bs[i] < 0 ? cfg.borderPx : bs[i], len), we = min(be[i] < 0 ? cfg.borderPx : be[i], len - ws);
    int m0 = st[i] + ws, m1 = en[i] - we; if (m1 <= m0) continue;
    float c = (m0 + m1) / 2.0f, h = hw * (m1 - m0);
    int a = max(m0, (int)(c - h)), b = min(m1, (int)(c + h) + 1);
    if (b > a) fill_solid(l + a, b - a, mid);
  }
}

// ---------- night mode ----------
volatile uint32_t wakeAt = 0;            // millis() of the last wake, 0 = none
void wakeStairs() { wakeAt = millis() | 1; }
int localMinutes() {                     // minutes after midnight, -1 until the clock is set from the internet
  time_t t = time(nullptr); if (t < 1700000000) return -1;
  struct tm lt; localtime_r(&t, &lt); return lt.tm_hour * 60 + lt.tm_min;
}
bool nightNow() {
  if (!cfg.nightOn) return false;
  int m = localMinutes(); if (m < 0) return false;
  return cfg.nightFrom <= cfg.nightTo ? (m >= cfg.nightFrom && m < cfg.nightTo) : (m >= cfg.nightFrom || m < cfg.nightTo);
}
// the outline: every step's borders at nightLv %; after a wake the middles walk up (and the borders come up with them),
// hold, then walk back down to the outline in the same order
void renderNight(CRGB mid, uint32_t now) {
  const uint32_t STEP = 250;
  int total = nStB + nStA;
  fill_solid(tA, cfg.nA, CRGB::Black); if (tB) fill_solid(tB, cfg.nB, CRGB::Black);
  uint32_t up = (uint32_t)total * STEP, hold = (uint32_t)cfg.nightWake * 1000, d = wakeAt ? now - wakeAt : 0xFFFFFFFF;
  uint8_t outline = cfg.nightLv * 255 / 100;
  for (int k = 0; k < total; k++) {
    bool onB = k < nStB;
    int i = onB ? nStB - 1 - k : nStA - 1 - (k - nStB);
    int* st = onB ? stB : stA; int* en = onB ? enB : enA; int* bs = onB ? bsB : bsA; int* be = onB ? beB : beA;
    uint32_t on = k * STEP, off = up + hold + k * STEP;
    uint8_t lv = d < on ? 0 : d < on + STEP ? (d - on) * 255 / STEP : d < off ? 255 : d < off + STEP ? 255 - (d - off) * 255 / STEP : 0;
    paintStep(onB ? tB : tA, st[i], en[i], bs[i], be[i], mid, max(outline, lv), lv);
  }
  if (wakeAt && d > up + hold + up + STEP) wakeAt = 0;
}

void renderMode(const Look& L, uint32_t ms) {
  if (L.mode == 13 || L.mode == 14) {         // measure: one output's ruler, the other dark
    fill_solid(tA, cfg.nA, CRGB::Black); fill_solid(tB, cfg.nB, CRGB::Black);
    if (L.mode == 13) measure(tA, cfg.nA); else measure(tB, cfg.nB);
    return;
  }
  static const CRGB GLASS[] = {CRGB(0xc6, 0x2b, 0x29), CRGB(0xfd, 0xcd, 0x09), CRGB(0x60, 0xb6, 0xe5), CRGB(0x8d, 0xad, 0x3e)};
  static CRGB v[256];
  static uint32_t tAcc = 0, last = 0;            // speed-scaled clock, so moving the speed slider never jumps
  tAcc += (uint32_t)((ms - last) * (L.spd / 50.0f)); last = ms;
  uint32_t t = tAcc;
  CRGB base(L.r, L.g, L.b);
  if (L.mode == 11 || L.mode == 12) { renderSteps(base, t, L.mode == 12); return; }
  if (L.mode == 15) { renderArrow(base, t); return; }
  switch (L.mode) {
    case 1: {                                     // pulse + colour: ~6 s breath, hue walks the wheel in ~60 s
      uint8_t b = 30 + scale8(sin8(t / 24), 225);
      fill_solid(tA, cfg.nA, CHSV(t / 235, 230, b));
    } break;
    case 2: {                                     // breathe: the chosen colour, ~6 s
      CRGB c = base; c.nscale8_video(30 + scale8(sin8(t / 24), 225)); fill_solid(tA, cfg.nA, c);
    } break;
    case 3:                                       // spin: a rainbow running along the strip, ~10 s a lap
      for (int i = 0; i < 256; i++) v[i] = CHSV(i + t / 40, 255, 255);
      layV(v); break;
    case 4: {                                     // ripple: swells of the chosen colour travel along the strip
      for (int i = 0; i < cfg.nA; i++) {
        CRGB c = base; c.nscale8_video(20 + scale8(sin8(t / 10 - i * 512 / cfg.nA), 235)); tA[i] = c;
      }
    } break;
    case 5: {                                     // glass: the hall's red / yellow / blue / green, swapping every ~6 s
      uint32_t step = t / 6000, f = t % 6000;
      uint8_t amt = f < 4500 ? 0 : (f - 4500) * 255 / 1500;
      for (int i = 0; i < 256; i++) v[i] = blend(GLASS[(i / 64 + step) % 4], GLASS[(i / 64 + step + 1) % 4], amt);
      layV(v);
    } break;
    case 6:                                       // candle: warm, uneven flicker
      for (int i = 0; i < cfg.nA; i++) {
        uint8_t n = inoise8(i * 40, t / 3);
        CRGB c(255, 100 + scale8(n, 50), 18); c.nscale8_video(110 + scale8(n, 145)); tA[i] = c;
      } break;
    case 7: {                                     // christmas: red and green bands drifting along, white glints
      const CRGB RED(200, 0, 0), GRN(0, 150, 20);
      for (int i = 0; i < 256; i++) { uint8_t p = i * 4 + t / 60; v[i] = blend(RED, GRN, sin8(p)); }
      layV(v);
      for (int k = 0; k < 1 + cfg.nA / 80; k++) if (random8() < 40) tA[random16(cfg.nA)] = CRGB(255, 240, 200);
    } break;
    case 8:                                       // candy cane: red and white stripes spinning
      for (int i = 0; i < 256; i++) v[i] = ((i + t / 30) / 16) % 2 ? CRGB(220, 0, 0) : CRGB(255, 230, 210);
      layV(v); break;
    case 9:                                       // snowfall: deep winter blue, flakes that glow and melt away
      for (int i = 0; i < cfg.nA; i++) tA[i] = blend(tA[i], CRGB(0, 10, 60), 6);
      for (int k = 0; k < 1 + cfg.nA / 120; k++) if (random8() < 30) tA[random16(cfg.nA)] = CRGB(200, 220, 255);
      break;
    case 10: {                                    // twinkle: warm fairy lights, each on its own slow shimmer
      for (int i = 0; i < cfg.nA; i++) {
        uint8_t n = inoise8(i * 300, t / 6);
        uint8_t b = n < 90 ? 25 : scale8(n - 90, 255) * 1 + 25;
        CRGB c(255, 170, 70); c.nscale8_video(b); tA[i] = c;
      }
    } break;
  }
  for (int i = 0; i < cfg.nB; i++) tB[i] = tA[i % cfg.nA];   // GPIO 5 follows the same pattern
}

void ledLoop() {
  static uint32_t seen = 0, lastShow = 0, fadeT0 = 0;
  static float cr = 0, cg = 0, cb = 0, cbri = 0, fr, fg, fb, fbri;   // current + fade start
  static bool wasTest = false;
  if (updating || ledHold || !netReady) { delay(5); return; }
  uint32_t now = millis();
  Look L = getLook();
  if (lookVer != seen) { seen = lookVer; fr = cr; fg = cg; fb = cb; fbri = cbri; fadeT0 = now; }

  if ((int32_t)(stepPrevUntil - now) > 0) {   // step editor: the chosen step dim white with its borders dim yellow, first pixel green, last red, neighbours dim blue
    fill_solid(ledA, cfg.nA, CRGB::Black); if (ledB) fill_solid(ledB, cfg.nB, CRGB::Black);
    bool b = prevB; int i = prevI, ns = b ? nStB : nStA;
    int* st = b ? stB : stA; int* en = b ? enB : enA; CRGB* led = b ? ledB : ledA;
    uint32_t sum = 0;
    if (led && i >= 0 && i < ns) {
      if (i > 0) fill_solid(led + st[i - 1], en[i - 1] - st[i - 1], wire(CRGB(0, 0, 40)));
      if (i + 1 < ns) fill_solid(led + st[i + 1], en[i + 1] - st[i + 1], wire(CRGB(0, 0, 40)));
      fill_solid(led + st[i], en[i] - st[i], wire(CRGB(50, 50, 50)));
      int* bs = b ? bsB : bsA; int* be = b ? beB : beA, len = en[i] - st[i];   // borders dim yellow
      int ws = min(bs[i] < 0 ? cfg.borderPx : bs[i], len), we = min(be[i] < 0 ? cfg.borderPx : be[i], len - ws);
      if (ws > 0) fill_solid(led + st[i], ws, wire(CRGB(70, 55, 0)));
      if (we > 0) fill_solid(led + en[i] - we, we, wire(CRGB(70, 55, 0)));
      led[st[i]] = wire(CRGB(0, 255, 0)); led[en[i] - 1] = wire(CRGB(255, 0, 0));
      sum = (uint32_t)(en[i] - st[i]) * 150 + 510;
    }
    showFrame(100, b ? 0 : sum, b ? sum : 0); wasTest = true; delay(30);
    return;
  }

  if (!L.test && nightNow()) {                // night: the outline, the middles only after a wake
    renderNight(CRGB(L.r, L.g, L.b), now);
    uint32_t sum = 0, sumB = 0;
    for (int i = 0; i < cfg.nA; i++) { ledA[i] = wire(tA[i]); sum += tA[i].r + tA[i].g + tA[i].b; }
    for (int i = 0; i < cfg.nB; i++) { CRGB c = tB[i]; c.nscale8_video(cfg.trimB * 255 / 100); ledB[i] = wire(c); sumB += c.r + c.g + c.b; }
    showFrame(L.bri, sum, sumB); wasTest = true; delay(8);
    cr = cg = cb = 0; cbri = 0;
    return;
  }
  if (!L.test && L.mode > 0) {                // an animated mode
    renderMode(L, now);
    uint32_t sum = 0, sumB = 0;
    for (int i = 0; i < cfg.nA; i++) { ledA[i] = wire(tA[i]); sum += tA[i].r + tA[i].g + tA[i].b; }
    for (int i = 0; i < cfg.nB; i++) { CRGB c = tB[i]; c.nscale8_video(cfg.trimB * 255 / 100); ledB[i] = wire(c); sumB += c.r + c.g + c.b; }
    showFrame(L.bri, sum, sumB); wasTest = true; delay(8);
    cr = cg = cb = 0; cbri = 0;
    return;
  }
  if (L.test) {
    if (!wasTest) { fill_solid(tA, cfg.nA, CRGB::Black); if (cfg.nB) fill_solid(tB, cfg.nB, CRGB::Black); }
    wasTest = true;
    EVERY_N_MILLISECONDS(20) { hue++; }
    EVERY_N_SECONDS(8) { fx = (fx + 1) % NFX; Serial.printf("test: %s\n", FXN[fx]); }
    effect(tA, cfg.nA);
    effect(tB, cfg.nB);
    uint32_t sum = 0, sumB = 0;
    for (int i = 0; i < cfg.nA; i++) { ledA[i] = wire(tA[i]); sum += tA[i].r + tA[i].g + tA[i].b; }
    for (int i = 0; i < cfg.nB; i++) { CRGB c = tB[i]; c.nscale8_video(cfg.trimB * 255 / 100); ledB[i] = wire(c); sumB += c.r + c.g + c.b; }
    showFrame(L.bri, sum, sumB);
    cr = cg = cb = 0; cbri = 0;      // after a test, the colour fades in from black
    return;
  }
  wasTest = false;

  float t = cfg.fadeMs > 0 ? min(1.0f, (now - fadeT0) / (float)cfg.fadeMs) : 1.0f;
  bool moving = t < 1.0f;
  cr = fr + (L.r - fr) * t; cg = fg + (L.g - fg) * t; cb = fb + (L.b - fb) * t; cbri = fbri + (L.bri - fbri) * t;
  // static: redraw only while fading, plus every 2 s so a glitched pixel heals itself
  if (!moving && now - lastShow < 2000 && lastShow) { delay(5); return; }
  CRGB c((uint8_t)(cr + .5f), (uint8_t)(cg + .5f), (uint8_t)(cb + .5f));
  CRGB a = wire(c), cB = c; cB.nscale8_video(cfg.trimB * 255 / 100); CRGB bw = wire(cB);
  fill_solid(ledA, cfg.nA, a); if (cfg.nB) fill_solid(ledB, cfg.nB, bw);
  showFrame(cbri, (uint32_t)cfg.nA * (c.r + c.g + c.b), (uint32_t)cfg.nB * (cB.r + cB.g + cB.b));
  lastShow = now | 1;
  if (moving) delay(15);
}

// ---------- web ----------
#include "page.h"   // PAGE: the phone page, gzipped. Generated from stairs/app (React): edit there, then npm run build


String stateJson() {
  Look L = getLook();
  bool up = WiFi.status() == WL_CONNECTED;
  char b[1800];
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"name\":\"%s\",\"host\":\"%s\",\"c\":\"%02x%02x%02x\",\"b\":%d,\"test\":%d,\"fx\":\"%s\",\"m\":%d,\"s\":%d,\"nA\":%d,\"nB\":%d,\"ord\":%d,\"trim\":%d,\"fade\":%d,"
    "\"lim\":%d,\"limB\":%d,\"pma\":%d,\"ma\":%d,\"maA\":%d,\"maB\":%d,\"cap\":%d,\"stA\":\"%s\",\"stB\":\"%s\",\"bc\":\"%02x%02x%02x\",\"bw\":%d,\"mw\":%d,"
    "\"night\":{\"on\":%d,\"from\":%d,\"to\":%d,\"lv\":%d,\"wake\":%d,\"tz\":\"%s\",\"now\":%d,\"active\":%d,\"lit\":%d},"
    "\"net\":{\"ssid\":\"%s\",\"up\":%d,\"ip\":\"%s\",\"why\":%d},\"mqtt\":{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"up\":%d}}",
    FW_VERSION, devName.c_str(), HOSTNAME, L.r, L.g, L.b, L.bri, L.test, FXN[fx], L.mode, L.spd, cfg.nA, cfg.nB, cfg.ord, cfg.trimB, cfg.fadeMs,
    cfg.limMA, cfg.limB, cfg.pixMA, (int)estMA, (int)estA, (int)estB, (int)capped, esc(cfg.stepsA).c_str(), esc(cfg.stepsB).c_str(), cfg.br, cfg.bg, cfg.bb, cfg.borderPx, cfg.middlePx,
    (int)cfg.nightOn, cfg.nightFrom, cfg.nightTo, cfg.nightLv, cfg.nightWake, esc(cfg.tz).c_str(), localMinutes(), (int)nightNow(), (int)(wakeAt != 0 && nightNow()),
    esc(staSsid).c_str(), up, up ? WiFi.localIP().toString().c_str() : "", up ? 0 : staWhy,
    esc(mqttHost).c_str(), mqttPort, esc(mqttUser).c_str(), (int)mqtt.connected());
  return b;
}

void sendState() { server.send(200, "application/json", stateJson()); }
void sendErr(const char* e) { server.send(400, "application/json", String("{\"err\":\"") + e + "\"}"); }

// paced joins: a station that retries nonstop hops channels and keeps knocking phones off the Stairs AP
void staBegin() { staWhy = 0; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str()); staTry = millis() | 1; }
void staService() {
  if (!staSsid.length()) return;
  if (WiFi.status() == WL_CONNECTED) { staTry = 0; staNext = millis() + 45000; return; }
  if (staTry && millis() - staTry > 20000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }
  else if (!staTry && (int32_t)(millis() - staNext) >= 0) staBegin();
}

void setupWeb() {
  server.on("/", [] { server.sendHeader("Content-Encoding", "gzip"); server.send_P(200, "text/html", (PGM_P)PAGE, PAGE_LEN); });
  server.on("/state", sendState);
  server.on("/set", [] {
    if (!applyLookArgs([](const char* k) { return server.hasArg(k); }, [](const char* k) { return server.arg(k); }))
      return sendErr("colour must be rrggbb");
    sendState();
  });
  server.on("/cfg", [] {
    int oA = cfg.nA, oB = cfg.nB;
    auto I = [](const char* k, int d) { return server.hasArg(k) ? (int)server.arg(k).toInt() : d; };
    cfg.nA = constrain(I("nA", cfg.nA), 1, MAX_PIX); cfg.nB = constrain(I("nB", cfg.nB), 0, MAX_PIX);
    cfg.ord = constrain(I("ord", cfg.ord), 0, NORD - 1); cfg.trimB = constrain(I("trim", cfg.trimB), 1, 100);
    cfg.fadeMs = constrain(I("fade", cfg.fadeMs), 0, 5000); cfg.limMA = constrain(I("lim", cfg.limMA), 0, 200000); cfg.limB = constrain(I("limB", cfg.limB), 0, 200000);
    cfg.pixMA = constrain(I("pma", cfg.pixMA), 1, 500);
    if (server.hasArg("stA")) cfg.stepsA = server.arg("stA");
    if (server.hasArg("stB")) cfg.stepsB = server.arg("stB");
    if (server.hasArg("bc")) { uint8_t r, g, b; if (parseHex(server.arg("bc"), r, g, b)) { cfg.br = r; cfg.bg = g; cfg.bb = b; } }
    cfg.borderPx = constrain(I("bw", cfg.borderPx), 0, 500); cfg.middlePx = constrain(I("mw", cfg.middlePx), 0, 2000);
    if (server.hasArg("nOn")) cfg.nightOn = server.arg("nOn").toInt();
    cfg.nightFrom = constrain(I("nFr", cfg.nightFrom), 0, 1439); cfg.nightTo = constrain(I("nTo", cfg.nightTo), 0, 1439);
    cfg.nightLv = constrain(I("nLv", cfg.nightLv), 0, 100); cfg.nightWake = constrain(I("nWk", cfg.nightWake), 5, 3600);
    if (server.hasArg("tz") && server.arg("tz").length()) { cfg.tz = server.arg("tz"); setenv("TZ", cfg.tz.c_str(), 1); tzset(); }
    parseAllSteps();
    saveCfg();
    portENTER_CRITICAL(&mux); lookVer++; portEXIT_CRITICAL(&mux);   // redraw with the new order / level
    if (server.hasArg("name") && cleanName(server.arg("name")) != devName) {
      devName = cleanName(server.arg("name"));
      { FlashHold fh; Preferences prefs; prefs.begin("stairs", false); prefs.putString("name", devName); prefs.end(); }
      server.send(200, "application/json", String("{\"restart\":1,\"name\":\"") + devName + "\"}"); delay(400); ESP.restart();
    }
    if (cfg.nA != oA || cfg.nB != oB) {
      server.send(200, "application/json", "{\"restart\":1}"); delay(400); ESP.restart();
    }
    sendState();
  });
  server.on("/step", [] {                  // preview an edited step map without saving: stA, stB, out=A|B, i; done=1 ends it
    if (server.hasArg("stA")) parseSteps(server.arg("stA"), stA, enA, bsA, beA, nStA, cfg.nA);
    if (server.hasArg("stB")) parseSteps(server.arg("stB"), stB, enB, bsB, beB, nStB, cfg.nB);
    if (server.hasArg("i")) { prevB = server.arg("out") == "B"; prevI = server.arg("i").toInt(); stepPrevUntil = millis() + 60000; }
    if (server.hasArg("done")) { stepPrevUntil = 0; parseAllSteps(); }   // back to the saved map
    portENTER_CRITICAL(&mux); lookVer++; portEXIT_CRITICAL(&mux);
    sendState();
  });
  server.on("/scan", [] {
    int n = WiFi.scanNetworks();
    String j = "[";
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i); if (!s.length() || j.indexOf("\"" + esc(s) + "\"") >= 0) continue;
      if (j.length() > 1) j += ",";
      j += "\"" + esc(s) + "\"";
    }
    WiFi.scanDelete();
    server.send(200, "application/json", j + "]");
  });
  server.on("/wifi", [] {
    if (server.hasArg("forget")) { staSsid = staPass = ""; WiFi.disconnect(false, true); }
    else { staSsid = server.arg("ssid"); staPass = server.arg("pass"); }
    FlashHold fh; Preferences prefs; prefs.begin("stairs", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
    sendState();
    if (staSsid.length()) staBegin(); else staTry = 0;
  });
  server.on("/mqtt", [] {
    mqttHost = server.arg("host"); mqttHost.trim();
    mqttPort = constrain((int)server.arg("port").toInt(), 1, 65535);
    mqttUser = server.arg("user");
    if (server.hasArg("pass")) mqttPass = server.arg("pass");
    FlashHold fh; Preferences prefs; prefs.begin("stairs", false);
    prefs.putString("mhost", mqttHost); prefs.putUShort("mport", mqttPort);
    prefs.putString("muser", mqttUser); prefs.putString("mpass", mqttPass);
    prefs.end();
    mqtt.disconnect();
    mqtt.setServer(mqttHost.c_str(), mqttPort);   // PubSubClient keeps the pointer: mqttHost is global
    sendState();
  });
  server.on("/wake", [] { wakeStairs(); sendState(); });   // light the stairs (night mode)
  server.on("/restart", [] { server.send(200, "text/plain", "restarting"); delay(300); ESP.restart(); });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    String msg = ok ? String("ok") : String("update failed: ") + Update.errorString();
    server.send(200, "text/plain", msg);
    delay(300);
    if (ok) ESP.restart();
    updating = false;
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { updating = true; while (ledBusy) delay(1); delay(150); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) Update.end(true);
    else if (u.status == UPLOAD_FILE_ABORTED) { Update.abort(); updating = false; }
  });
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });  // captive portal
  server.begin();
}

// ---------- MQTT ----------
void onMqtt(char* topic, byte* payload, unsigned int len) {
  String m; m.reserve(len); for (unsigned i = 0; i < len; i++) m += (char)payload[i];
  m.replace(",", "&");
  // tiny k=v&k=v parser (same keys as /set)
  auto val = [&](const char* k, String& out) {
    String key = String(k) + "=";
    int p = ("&" + m).indexOf("&" + key); if (p < 0) return false;
    int s = p + key.length(), e = m.indexOf('&', s); out = m.substring(s, e < 0 ? m.length() : e); return true;
  };
  applyLookArgs([&](const char* k) { String x; return val(k, x); }, [&](const char* k) { String x; val(k, x); return x; });
}

void netTask(void*) {
  uint32_t lastTry = 0;
  char topic[64];
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t i) { staWhy = i.wifi_sta_disconnected.reason; }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    for (Stream* S : {(Stream*)&Serial, (Stream*)&Serial0})
      S->printf("hall WiFi \"%s\": http://%s  http://%s.local\n", staSsid.c_str(), WiFi.localIP().toString().c_str(), HOSTNAME);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.persistent(false);          // our NVS keeps the login; stop the WiFi driver writing flash on every join
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);   // staService() paces the retries
  // Starlink / mesh routers: scan every channel and take the strongest node, and accept WPA/WPA2/WPA3 mixes
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  if (staSsid.length()) staBegin();
  dns.start(53, "*", WiFi.softAPIP());
  MDNS.begin(HOSTNAME); MDNS.addService("http", "tcp", 80);
  setupWeb();
  delay(300);
  netReady = true;                 // WiFi calibration written: the LEDs may start
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(1024);
  for (Stream* S : {(Stream*)&Serial, (Stream*)&Serial0})
    S->printf("%s  stair lights: %d px on GPIO %d, %d px on GPIO %d, order %s  WiFi \"%s\" -> http://%s\n",
              FW_VERSION, cfg.nA, PIN_A, cfg.nB, PIN_B, ORDERS[cfg.ord], AP_SSID, WiFi.softAPIP().toString().c_str());

  for (;;) {
    uint32_t ms = millis();
    dns.processNextRequest();
    server.handleClient();
    staService();
    if (lookDirtyAt && ms - lookDirtyAt > 2000 && !updating) { lookDirtyAt = 0; saveLook(); }

    bool up = WiFi.status() == WL_CONNECTED;
    static bool clockAsked = false;   // the time, for night mode, from the internet once on the hall WiFi
    if (up && !clockAsked) { clockAsked = true; configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com"); }
    if (!up || !mqttHost.length() || updating) { if (mqtt.connected()) mqtt.disconnect(); }
    else if (!mqtt.connected()) {
      if (ms - lastTry > 5000) {
        lastTry = ms;
        snprintf(topic, sizeof topic, "%s/status", TOPIC_BASE);
        bool ok = mqttUser.length() ? mqtt.connect(HOSTNAME, mqttUser.c_str(), mqttPass.c_str(), topic, 1, true, "offline")
                                    : mqtt.connect(HOSTNAME, topic, 1, true, "offline");
        if (ok) {
          mqtt.publish(topic, "online", true);
          snprintf(topic, sizeof topic, "%s/set", TOPIC_BASE); mqtt.subscribe(topic);
          pubState = true;
        }
      }
    } else {
      mqtt.loop();
      static uint32_t lastPub = 0;
      if (pubState && ms - lastPub > 300) {
        pubState = false; lastPub = ms;
        snprintf(topic, sizeof topic, "%s/state", TOPIC_BASE);
        Look L = getLook(); char b[160];
        snprintf(b, sizeof b, "{\"c\":\"%02x%02x%02x\",\"b\":%d,\"test\":%d,\"m\":\"%s\",\"s\":%d,\"fw\":\"%s\"}", L.r, L.g, L.b, L.bri, L.test, MODEN[L.mode], L.spd, FW_VERSION);
        mqtt.publish(topic, b, true);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------- serial (USB) ----------
//   wifi<TAB>ssid<TAB>pass   join the hall WiFi        c<rrggbb>  colour      b<0-100>  brightness
//   t  test on/off            n<pixels>  set GPIO 4 count (restarts)          ?  state
void serialCmd(Stream& S, String& line) {
  while (S.available()) {
    char ch = S.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim(); if (!line.length()) continue;
    if (line == "scan") {          // what the board can actually hear: name, signal, channel, security
      int n = WiFi.scanNetworks();
      S.printf("%d networks\n", n);
      for (int i = 0; i < n; i++) S.printf("  \"%s\"  %d dBm  ch %d  auth %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
      WiFi.scanDelete();
    } else if (line.startsWith("wifi\t")) {
      int t = line.indexOf('\t', 5);
      staSsid = t < 0 ? line.substring(5) : line.substring(5, t); staPass = t < 0 ? "" : line.substring(t + 1);
      FlashHold fh; Preferences prefs; prefs.begin("stairs", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
      staBegin(); S.printf("joining %s\n", staSsid.c_str());
    } else {
      char c = line[0]; String a = line.substring(1); a.trim(); Look L = getLook();
      if (c == 'c') { uint8_t r, g, b; if (parseHex(a, r, g, b)) { L.r = r; L.g = g; L.b = b; L.test = false; setLook(L); } else S.println("c<rrggbb>"); }
      else if (c == 'b') { L.bri = a.toInt(); setLook(L); }
      else if (c == 't') { L.test = !L.test; setLook(L); }
      else if (c == 'w') { wakeStairs(); }
      else if (c == 'n') { cfg.nA = constrain((int)a.toInt(), 1, MAX_PIX); saveCfg(); S.println("restarting"); delay(200); ESP.restart(); }
      S.println(stateJson());
      if (c == '?' || c == 'h') S.println("c<rrggbb> colour · b<0-100> brightness · t test · w wake (night) · n<pixels> GPIO 4 count · wifi<TAB>ssid<TAB>pass · scan · ? state");
    }
    line = "";
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  loadAll();
  ledA = new CRGB[cfg.nA]; tA = new CRGB[cfg.nA];
  FastLED.addLeds<WS2812B, PIN_A, RGB>(ledA, cfg.nA);
  if (cfg.nB > 0) { ledB = new CRGB[cfg.nB]; tB = new CRGB[cfg.nB]; FastLED.addLeds<WS2812B, PIN_B, RGB>(ledB, cfg.nB); }
  FastLED.setDither(DISABLE_DITHER);
  FastLED.clear();                 // no show yet: LEDs wait for netReady
  pinMode(BOOT_BTN, INPUT_PULLUP);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);   // WiFi + web + MQTT + updates on core 0
}

void loop() {                       // core 1: LEDs only
  static String l0, l1;
  serialCmd(Serial, l0); serialCmd(Serial0, l1);   // native USB + the DevKit's UART port
  static bool was = true; static uint32_t tDown = 0;   // BOOT button: test on/off
  bool upb = digitalRead(BOOT_BTN);
  if (!upb && was && millis() - tDown > 250) { tDown = millis(); Look L = getLook(); L.test = !L.test; setLook(L); }
  was = upb;
  ledLoop();
}
