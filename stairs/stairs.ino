// Hall stair lights: one ESP32-S3 (Super Mini), addressable strip on GPIO 4, optional second run on GPIO 5.
// Flashed from the Ragamuffin Flasher (board "Stair lights"). Then join WiFi "Stairs" (pw ragamuffin) -> 192.168.4.1
//
// What it does: the whole stair shows one colour at one brightness. That's the only control, on purpose.
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
#define AP_SSID     "Stairs"
#define AP_PASS     "ragamuffin"
#define HOSTNAME    "hall-stairs"
#define HOME_SSID   "ClarkShop"     // preset network; its password is typed once on the page (NVS), never in the repo
#define TOPIC_BASE  "ragamuffin/hall/stairs"
#define PIN_A       4               // output 1 (Super Mini GPIO 4)
#define PIN_B       5               // output 2 (Super Mini GPIO 5), 0 pixels = off
#define MAX_PIX     4000            // per output (~120 ms per frame at 4000; solid colour only redraws on change)
#define BOOT_BTN    0

struct Cfg {
  int nA = 300, nB = 0;             // pixels (chips) per output; changing them restarts the board
  int ord = 0;                      // wire colour order, index into ORDERS
  int limMA = 4200;                 // supply limit, 0 = no limit
  int pixMA = 60;                   // one pixel at full white (WS2812B 5 V = 60)
  int trimB = 100;                  // output 2 level % (dark treads on one flight)
  int fadeMs = 600;
} cfg;
const char* ORDERS[] = {"GRB", "RGB", "BRG", "RBG", "GBR", "BGR"};
const int NORD = 6;

struct Look { uint8_t r, g, b; int bri; bool test; };
Look look = {255, 147, 41, 40, true};   // fresh board: Test on at 40 % so a new strip shows everything straight away
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lookVer = 1;
volatile bool updating = false;
volatile int estMA = 0; volatile bool capped = false;
uint32_t lookDirtyAt = 0;           // save look to NVS 2 s after the last change (sliders send a lot)

CRGB* ledA = nullptr; CRGB* ledB = nullptr;
WebServer server(80);
DNSServer dns;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

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
  if (has("c")) { uint8_t r, g, b; if (!parseHex(get("c"), r, g, b)) return false; l.r = r; l.g = g; l.b = b; l.test = false; any = true; }
  if (has("b")) { l.bri = constrain(get("b").toInt(), 0, 100); any = true; }
  if (has("test")) { l.test = get("test").toInt(); any = true; }
  if (any) setLook(l);
  return true;
}

// ---------- config ----------
void loadAll() {
  Preferences prefs; prefs.begin("stairs", true);
  cfg.nA = prefs.getInt("nA", cfg.nA); cfg.nB = prefs.getInt("nB", cfg.nB); cfg.ord = prefs.getInt("ord", cfg.ord);
  cfg.limMA = prefs.getInt("lim", cfg.limMA); cfg.pixMA = prefs.getInt("pma", cfg.pixMA);
  cfg.trimB = prefs.getInt("trim", cfg.trimB); cfg.fadeMs = prefs.getInt("fade", cfg.fadeMs);
  look.r = prefs.getUChar("r", look.r); look.g = prefs.getUChar("g", look.g); look.b = prefs.getUChar("b", look.b);
  look.bri = prefs.getInt("bri", look.bri); look.test = prefs.getBool("test", look.test);
  staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("wpass", "");
  mqttHost = prefs.getString("mhost", ""); mqttPort = prefs.getUShort("mport", 1883);
  mqttUser = prefs.getString("muser", ""); mqttPass = prefs.getString("mpass", "");
  prefs.end();
  cfg.nA = constrain(cfg.nA, 1, MAX_PIX); cfg.nB = constrain(cfg.nB, 0, MAX_PIX);
  if (cfg.ord < 0 || cfg.ord >= NORD) cfg.ord = 0;
}
void saveCfg() {
  Preferences prefs; prefs.begin("stairs", false);
  prefs.putInt("nA", cfg.nA); prefs.putInt("nB", cfg.nB); prefs.putInt("ord", cfg.ord);
  prefs.putInt("lim", cfg.limMA); prefs.putInt("pma", cfg.pixMA); prefs.putInt("trim", cfg.trimB); prefs.putInt("fade", cfg.fadeMs);
  prefs.end();
}
void saveLook() {
  Look l = getLook();
  Preferences prefs; prefs.begin("stairs", false);
  prefs.putUChar("r", l.r); prefs.putUChar("g", l.g); prefs.putUChar("b", l.b); prefs.putInt("bri", l.bri); prefs.putBool("test", l.test);
  prefs.end();
}

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

uint8_t briByte(float pct) { return pct <= 0.05f ? 0 : (uint8_t)max(1, (int)(pct * 2.55f + .5f)); }

void showFrame(float briPct, uint32_t sumRGB) {   // sumRGB: sum of r+g+b over every pixel at full scale
  uint8_t bri = briByte(briPct);
  int ma = (int)((double)sumRGB / 765.0 * cfg.pixMA * bri / 255.0);
  bool cut = false;
  if (cfg.limMA > 0 && ma > cfg.limMA && ma > 0) { bri = max(1, (int)((uint32_t)bri * cfg.limMA / ma)); ma = cfg.limMA; cut = true; }
  estMA = ma; capped = cut;
  FastLED.setBrightness(bri);
  FastLED.show();
}

void ledLoop() {
  static uint32_t seen = 0, lastShow = 0, fadeT0 = 0;
  static float cr = 0, cg = 0, cb = 0, cbri = 0, fr, fg, fb, fbri;   // current + fade start
  static bool wasTest = false;
  if (updating) { delay(20); return; }
  uint32_t now = millis();
  Look L = getLook();
  if (lookVer != seen) { seen = lookVer; fr = cr; fg = cg; fb = cb; fbri = cbri; fadeT0 = now; }

  if (L.test) {
    if (!wasTest) { fill_solid(tA, cfg.nA, CRGB::Black); if (cfg.nB) fill_solid(tB, cfg.nB, CRGB::Black); }
    wasTest = true;
    EVERY_N_MILLISECONDS(20) { hue++; }
    EVERY_N_SECONDS(8) { fx = (fx + 1) % NFX; Serial.printf("test: %s\n", FXN[fx]); }
    effect(tA, cfg.nA); effect(tB, cfg.nB);
    uint32_t sum = 0;
    for (int i = 0; i < cfg.nA; i++) { ledA[i] = wire(tA[i]); sum += tA[i].r + tA[i].g + tA[i].b; }
    for (int i = 0; i < cfg.nB; i++) { CRGB c = tB[i]; c.nscale8_video(cfg.trimB * 255 / 100); ledB[i] = wire(c); sum += c.r + c.g + c.b; }
    showFrame(L.bri, sum);
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
  uint32_t sum = (uint32_t)cfg.nA * (c.r + c.g + c.b) + (uint32_t)cfg.nB * (cB.r + cB.g + cB.b);
  showFrame(cbri, sum);
  lastShow = now | 1;
  if (moving) delay(15);
}

// ---------- web ----------
// Page = the Ragamuffin Hall logo (brand/BRAND.md): white paper, black ink outlines, flat glass colours, the rose window.
// Fonts come from Google when the phone has internet; on the board's own WiFi it falls back to system faces.
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#ffffff"><title>Stair lights</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=New+Rocker&family=Alegreya+Sans:wght@400;700&display=swap">
<style>
:root{--red:#c62b29;--yellow:#fdcd09;--blue:#60b6e5;--green:#8dad3e;--ink:#060709;--paper:#fff;--dim:#555}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:17px 'Alegreya Sans',system-ui,sans-serif}
main{max-width:480px;margin:0 auto;padding:14px 14px 40px}
header{text-align:center;margin:4px 0 14px}.rose{width:120px;height:120px;display:block;margin:0 auto 4px}
.word{font:34px 'New Rocker',Georgia,serif;letter-spacing:1px;margin:0;line-height:1}
.sub{display:flex;align-items:center;justify-content:center;gap:10px;margin:4px 0 0;font:700 15px 'Alegreya Sans',sans-serif;letter-spacing:4px}
.rule{height:2px;width:56px;background:linear-gradient(90deg,transparent,var(--ink))}.rule.r{background:linear-gradient(270deg,transparent,var(--ink))}
#fw{color:var(--dim);font-size:13px;margin:4px 0 0}
.card{border:2.5px solid var(--ink);border-radius:14px;padding:14px;margin-bottom:12px;background:var(--paper)}
h2{font:22px 'New Rocker',Georgia,serif;margin:0 0 8px;font-weight:400}h2 small{font:14px 'Alegreya Sans',sans-serif;color:var(--dim)}
button,select,input{font:inherit;color:var(--ink)}
button{border:2.5px solid var(--ink);border-radius:10px;padding:11px 8px;background:var(--paper);width:100%;touch-action:manipulation;font-weight:700}
button:active{background:var(--yellow)}button.on{background:var(--yellow)}
.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
.sw{height:64px;border:2.5px solid var(--ink);border-radius:10px;margin-bottom:10px;position:relative;overflow:hidden}
.sw input{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer}
.sw span{position:absolute;right:10px;bottom:6px;font:700 14px ui-monospace,monospace;background:var(--paper);border:2px solid var(--ink);border-radius:6px;padding:1px 6px}
.chips{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}.chips button{padding:8px 2px;font-size:14px;display:flex;flex-direction:column;align-items:center;gap:4px}
.chips i{width:26px;height:26px;border-radius:50%;border:2px solid var(--ink)}
.bri{display:flex;align-items:baseline;justify-content:space-between;margin:12px 0 2px}.bri b{font:30px 'New Rocker',Georgia,serif}
input[type=range]{width:100%;accent-color:var(--ink);height:34px}
.test{background:conic-gradient(var(--yellow) 0 25%,var(--green) 0 50%,var(--blue) 0 75%,var(--red) 0)}.test.on{outline:4px solid var(--ink);outline-offset:-8px}
label{display:flex;justify-content:space-between;align-items:center;gap:10px;margin:8px 0}
label input,label select{width:150px;padding:6px 8px;border:2px solid var(--ink);border-radius:8px;background:var(--paper)}
.hint{color:var(--dim);font-size:14px;margin:8px 0 0}#st{font:13px ui-monospace,monospace;color:var(--dim);margin-top:8px}
.warn{color:var(--red);font-weight:700}
.dash{height:6px;margin:4px 0 12px;background:repeating-linear-gradient(90deg,var(--red) 0 12px,var(--blue) 12px 24px);border-top:1.5px solid var(--ink);border-bottom:1.5px solid var(--ink)}
footer{text-align:center;color:var(--dim);font-size:13px}
</style></head><body><main>
<header><svg viewBox="-102 -102 204 204" class="rose"><circle r="101" fill="#ffffff"/><circle class="dash-b" r="95.5" fill="none" stroke="#60b6e5" stroke-width="6"/><circle class="dash-r" r="95.5" fill="none" stroke="#c62b29" stroke-width="6" stroke-dasharray="9.38 9.38"/><circle r="92.3" fill="none" stroke="#060709" stroke-width="1.5"/><circle r="98.7" fill="none" stroke="#060709" stroke-width="1.5"/><circle r="87.5" fill="none" stroke="#060709" stroke-width="2.4"/><g class="petals" stroke="#060709" stroke-width="2.4" stroke-linejoin="round"><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(11.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(33.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(56.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(78.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(101.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(123.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(146.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(168.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(191.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(213.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(236.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(258.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(281.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(303.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(326.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(348.75)"/></g><circle r="34.5" fill="#ffffff" stroke="#060709" stroke-width="2.4"/><g class="hub"><path d="M 0 0 L -2.4 -22.87 A 23 23 0 0 1 21.01 -9.35 Z" fill="#fdcd09"/><path d="M 0 0 L 21.01 -9.35 A 23 23 0 0 1 15.39 17.09 Z" fill="#8dad3e"/><path d="M 0 0 L 15.39 17.09 A 23 23 0 0 1 -11.5 19.92 Z" fill="#60b6e5"/><path d="M 0 0 L -11.5 19.92 A 23 23 0 0 1 -22.5 -4.78 Z" fill="#c62b29"/><path d="M 0 0 L -22.5 -4.78 A 23 23 0 0 1 -2.4 -22.87 Z" fill="#60b6e5"/><path d="M -2.4 -22.87 L 4.75 -8.23 L 21.01 -9.35 L 9.29 1.98 L 15.39 17.09 L 0.99 9.45 L -11.5 19.92 L -8.68 3.86 L -22.5 -4.78 L -6.36 -7.06 Z" fill="#ffffff" stroke="#060709" stroke-width="1.7" stroke-linejoin="miter"/><path d="M -0.68 -6.46 L 1.35 -2.34 L 5.94 -2.64 L 2.64 0.56 L 4.35 4.83 L 0.28 2.69 L -3.25 5.63 L -2.47 1.1 L -6.36 -1.35 L -1.81 -2.01 Z" fill="#060709"/><circle r="23" fill="none" stroke="#060709" stroke-width="2.4"/></g></svg><h1 class="word">RAGAMUFFIN</h1><p class="sub"><span class="rule"></span>STAIR LIGHTS<span class="rule r"></span></p><p id="fw"></p></header>
<div class="dash"></div>
<div class="card"><h2>Light</h2>
<div class="sw" id="sw"><input type="color" id="col" value="#ff9329"><span id="hex">#ff9329</span></div>
<div class="chips" id="chips"></div>
<div class="bri"><span>Brightness</span><b id="bv">40 %</b></div>
<input type="range" id="bri" min="0" max="100" value="40">
<div class="row"><button data-b="0">Off</button><button data-b="10">10 %</button><button data-b="30">30 %</button><button data-b="60">60 %</button><button data-b="100">100 %</button></div>
<div class="row"><button id="test" class="test">Test: every colour</button></div>
<div id="st">…</div></div>
<div class="card"><h2>Strip <small>set once per install</small></h2>
<label>pixels on GPIO 4 <input type="number" id="nA" min="1" max="4000"></label>
<label>pixels on GPIO 5 <input type="number" id="nB" min="0" max="4000" placeholder="0 = off"></label>
<label>colour order <select id="ord"></select></label>
<label>GPIO 5 level % <input type="number" id="trim" min="1" max="100"></label>
<label>fade ms <input type="number" id="fade" min="0" max="5000"></label>
<label>supply limit mA <input type="number" id="lim" min="0" max="200000" placeholder="0 = none"></label>
<label>mA per pixel, full white <input type="number" id="pma" min="1" max="500"></label>
<div class="row"><button id="scfg">Save</button></div>
<p class="hint">Pixels = chips, not LEDs: WS2811 neon often has 3 LEDs per chip. Changing a pixel count restarts the board.
Colour order: pick Test, then tap Red — if the stair shows green or blue, try the next order.
mA per pixel = reel W/m ÷ volts ÷ pixels per metre × 1000 (WS2812B 5 V ≈ 60).</p>
<p class="hint" id="saved"></p></div>
<div class="card"><h2>Hall WiFi <small id="nets"></small></h2>
<label>network <input id="ssid" list="aps"></label><datalist id="aps"></datalist>
<label>password <input id="wpass" type="password"></label>
<div class="row"><button id="clark">ClarkShop</button><button id="scan">Scan</button></div>
<div class="row"><button id="join">Join</button><button id="forget">Forget</button></div></div>
<div class="card"><h2>MQTT <small id="mq"></small></h2>
<label>broker <input id="mhost" placeholder="blank = off"></label><label>port <input id="mport" type="number"></label>
<label>user <input id="muser"></label><label>password <input id="mpass" type="password" placeholder="unchanged"></label>
<div class="row"><button id="msave">Save</button></div>
<p class="hint">Send to <b>ragamuffin/hall/stairs/set</b>: <code>c=ff9329&amp;b=30</code> · <code>b=0</code> · <code>test=1</code>. State is on <b>…/state</b>.</p></div>
<div class="card"><h2>Firmware <small id="fwv"></small></h2>
<div class="row"><button id="latest">Install latest</button></div>
<p class="hint">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up).</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<footer>Ragamuffin Studios, McCoysville, Pennsylvania</footer>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HOME=')HTML" HOME_SSID R"HTML(';
const $=id=>document.getElementById(id),j2=r=>r.json(),go=u=>fetch(u).then(j2).then(show).catch(()=>{});
const ORD=['GRB','RGB','BRG','RBG','GBR','BGR'];ORD.forEach((o,i)=>$('ord').add(new Option(o,i)));
const CH=[['Warm','ff9329'],['Candle','ff6a10'],['White','ffd9b0'],['Day','ffffff'],['Red','c62b29'],['Yellow','fdcd09'],['Green','8dad3e'],['Blue','60b6e5'],['Show','0a2a8a'],['Purple','7a2cff'],['Pink','ff3c8a'],['Teal','00c0a0']];
CH.forEach(([n,h])=>{const b=document.createElement('button');b.innerHTML=`<i style="background:#${h}"></i>${n}`;b.onclick=()=>setC(h);$('chips').append(b)});
const WHY={2:'auth expired',15:'wrong password',201:'not found (2.4 GHz only)',202:'wrong password',203:'refused',204:'wrong password',205:'refused'};
let c={},busy=0;
function show(j){if(!j||j.err){$('st').textContent=j?j.err:'no reply';return}c=j;const n=j.net||{},m=j.mqtt||{};
 $('fw').textContent='build '+j.fw;$('fwv').textContent='running '+j.fw;
 if(!busy){$('col').value='#'+j.c;$('hex').textContent='#'+j.c;$('sw').style.background='#'+j.c;$('bri').value=j.b;$('bv').textContent=j.b+' %'}
 $('test').classList.toggle('on',!!j.test);$('test').textContent=j.test?'Test running — tap a colour to stop':'Test: every colour';
 $('st').innerHTML=`${j.test?'test · '+j.fx:'#'+j.c} · ${j.b} % · ${j.nA}${j.nB?' + '+j.nB:''} px · ${ORD[j.ord]} · about ${j.ma} mA`+(j.cap?` <span class="warn">· held at the ${j.lim} mA limit</span>`:'');
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · hall-stairs.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set, own WiFi only';
 $('mq').textContent=m.host?(m.up?'connected':'connecting…'):'off'}
function fill(j){show(j);['nA','nB','ord','trim','fade','lim','pma'].forEach(k=>$(k).value=j[k]);$('ssid').value=(j.net&&j.net.ssid)||HOME;
 $('mhost').value=j.mqtt.host;$('mport').value=j.mqtt.port;$('muser').value=j.mqtt.user}
let tq=0,q={};const send=o=>{Object.assign(q,o);busy=Date.now();clearTimeout(tq);tq=setTimeout(()=>{const s=new URLSearchParams(q);q={};go('/set?'+s)},60)};
const setC=h=>{$('col').value='#'+h;$('hex').textContent='#'+h;$('sw').style.background='#'+h;send({c:h})};
$('col').oninput=()=>setC($('col').value.slice(1));
const setB=v=>{$('bri').value=v;$('bv').textContent=v+' %';send({b:v})};
$('bri').oninput=()=>setB($('bri').value);
document.querySelectorAll('[data-b]').forEach(b=>b.onclick=()=>setB(b.dataset.b));
$('test').onclick=()=>send({test:c.test?0:1});
$('scfg').onclick=()=>{const s=new URLSearchParams();['nA','nB','ord','trim','fade','lim','pma'].forEach(k=>s.set(k,$(k).value||0));
 fetch('/cfg?'+s).then(j2).then(j=>{if(j.err){$('saved').textContent=j.err;return}
 $('saved').textContent=j.restart?'Saved. Restarting with the new pixel count…':'Saved ✓';if(j.restart)setTimeout(()=>location.reload(),6000);else fill(j)})};
$('scan').onclick=()=>{$('scan').textContent='scanning…';fetch('/scan').then(j2).then(a=>{$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s)));$('scan').textContent=a.length+' found'}).catch(()=>$('scan').textContent='Scan')};
$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
$('join').onclick=()=>{if(!$('ssid').value)return;go('/wifi?'+new URLSearchParams({ssid:$('ssid').value,pass:$('wpass').value}))};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('msave').onclick=()=>{const s=new URLSearchParams({host:$('mhost').value,port:$('mport').value||1883,user:$('muser').value});
 if($('mpass').value)s.set('pass',$('mpass').value);go('/mqtt?'+s);$('mpass').value=''};
function up(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'stairs.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),6000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)up(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');try{
 b.textContent='checking…';const m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json();
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'stairs.bin',{cache:'no-store'});if(!r.ok)throw 0;
 up(await r.blob(),b)}catch(e){b.textContent='no internet — mobile data on, open in Chrome'}};
fetch('/state').then(j2).then(fill);setInterval(()=>{if(Date.now()-busy>1500)busy=0;go('/state')},1500);
</script></main></body></html>)HTML";

String stateJson() {
  Look L = getLook();
  bool up = WiFi.status() == WL_CONNECTED;
  char b[900];
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"c\":\"%02x%02x%02x\",\"b\":%d,\"test\":%d,\"fx\":\"%s\",\"nA\":%d,\"nB\":%d,\"ord\":%d,\"trim\":%d,\"fade\":%d,"
    "\"lim\":%d,\"pma\":%d,\"ma\":%d,\"cap\":%d,"
    "\"net\":{\"ssid\":\"%s\",\"up\":%d,\"ip\":\"%s\",\"why\":%d},\"mqtt\":{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"up\":%d}}",
    FW_VERSION, L.r, L.g, L.b, L.bri, L.test, FXN[fx], cfg.nA, cfg.nB, cfg.ord, cfg.trimB, cfg.fadeMs,
    cfg.limMA, cfg.pixMA, (int)estMA, (int)capped,
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
  if (staTry && millis() - staTry > 15000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }
  else if (!staTry && (int32_t)(millis() - staNext) >= 0) staBegin();
}

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
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
    cfg.fadeMs = constrain(I("fade", cfg.fadeMs), 0, 5000); cfg.limMA = constrain(I("lim", cfg.limMA), 0, 200000);
    cfg.pixMA = constrain(I("pma", cfg.pixMA), 1, 500);
    saveCfg();
    portENTER_CRITICAL(&mux); lookVer++; portEXIT_CRITICAL(&mux);   // redraw with the new order / level
    if (cfg.nA != oA || cfg.nB != oB) {
      server.send(200, "application/json", "{\"restart\":1}"); delay(400); ESP.restart();
    }
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
    Preferences prefs; prefs.begin("stairs", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
    sendState();
    if (staSsid.length()) staBegin(); else staTry = 0;
  });
  server.on("/mqtt", [] {
    mqttHost = server.arg("host"); mqttHost.trim();
    mqttPort = constrain((int)server.arg("port").toInt(), 1, 65535);
    mqttUser = server.arg("user");
    if (server.hasArg("pass")) mqttPass = server.arg("pass");
    Preferences prefs; prefs.begin("stairs", false);
    prefs.putString("mhost", mqttHost); prefs.putUShort("mport", mqttPort);
    prefs.putString("muser", mqttUser); prefs.putString("mpass", mqttPass);
    prefs.end();
    mqtt.disconnect();
    mqtt.setServer(mqttHost.c_str(), mqttPort);   // PubSubClient keeps the pointer: mqttHost is global
    sendState();
  });
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
    if (u.status == UPLOAD_FILE_START) { updating = true; delay(150); Update.begin(UPDATE_SIZE_UNKNOWN); }
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
    Serial.printf("hall WiFi \"%s\": http://%s  http://%s.local\n", staSsid.c_str(), WiFi.localIP().toString().c_str(), HOSTNAME);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);   // staService() paces the retries
  if (staSsid.length()) staBegin();
  dns.start(53, "*", WiFi.softAPIP());
  MDNS.begin(HOSTNAME); MDNS.addService("http", "tcp", 80);
  setupWeb();
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(1024);
  Serial.printf("%s  stair lights: %d px on GPIO %d, %d px on GPIO %d, order %s  WiFi \"%s\" -> http://%s\n",
                FW_VERSION, cfg.nA, PIN_A, cfg.nB, PIN_B, ORDERS[cfg.ord], AP_SSID, WiFi.softAPIP().toString().c_str());

  for (;;) {
    uint32_t ms = millis();
    dns.processNextRequest();
    server.handleClient();
    staService();
    if (lookDirtyAt && ms - lookDirtyAt > 2000 && !updating) { lookDirtyAt = 0; saveLook(); }

    bool up = WiFi.status() == WL_CONNECTED;
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
        Look L = getLook(); char b[96];
        snprintf(b, sizeof b, "{\"c\":\"%02x%02x%02x\",\"b\":%d,\"test\":%d,\"fw\":\"%s\"}", L.r, L.g, L.b, L.bri, L.test, FW_VERSION);
        mqtt.publish(topic, b, true);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------- serial (USB) ----------
//   wifi<TAB>ssid<TAB>pass   join the hall WiFi        c<rrggbb>  colour      b<0-100>  brightness
//   t  test on/off            n<pixels>  set GPIO 4 count (restarts)          ?  state
void serialCmd() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim(); if (!line.length()) continue;
    if (line.startsWith("wifi\t")) {
      int t = line.indexOf('\t', 5);
      staSsid = t < 0 ? line.substring(5) : line.substring(5, t); staPass = t < 0 ? "" : line.substring(t + 1);
      Preferences prefs; prefs.begin("stairs", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
      staBegin(); Serial.printf("joining %s\n", staSsid.c_str());
    } else {
      char c = line[0]; String a = line.substring(1); a.trim(); Look L = getLook();
      if (c == 'c') { uint8_t r, g, b; if (parseHex(a, r, g, b)) { L.r = r; L.g = g; L.b = b; L.test = false; setLook(L); } else Serial.println("c<rrggbb>"); }
      else if (c == 'b') { L.bri = a.toInt(); setLook(L); }
      else if (c == 't') { L.test = !L.test; setLook(L); }
      else if (c == 'n') { cfg.nA = constrain((int)a.toInt(), 1, MAX_PIX); saveCfg(); Serial.println("restarting"); delay(200); ESP.restart(); }
      Serial.println(stateJson());
      if (c == '?' || c == 'h') Serial.println("c<rrggbb> colour · b<0-100> brightness · t test · n<pixels> GPIO 4 count · wifi<TAB>ssid<TAB>pass · ? state");
    }
    line = "";
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  loadAll();
  ledA = new CRGB[cfg.nA]; tA = new CRGB[cfg.nA];
  FastLED.addLeds<WS2812B, PIN_A, RGB>(ledA, cfg.nA);
  if (cfg.nB > 0) { ledB = new CRGB[cfg.nB]; tB = new CRGB[cfg.nB]; FastLED.addLeds<WS2812B, PIN_B, RGB>(ledB, cfg.nB); }
  FastLED.setDither(DISABLE_DITHER);
  FastLED.clear(true);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);   // WiFi + web + MQTT + updates on core 0
}

void loop() {                       // core 1: LEDs only
  serialCmd();
  static bool was = true; static uint32_t tDown = 0;   // BOOT button: test on/off
  bool upb = digitalRead(BOOT_BTN);
  if (!upb && was && millis() - tDown > 250) { tDown = millis(); Look L = getLook(); L.test = !L.test; setLook(L); }
  was = upb;
  ledLoop();
}
