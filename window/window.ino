// Hall window lights (the rose window ring): any ESP32-S3 (DevKit or Super Mini), addressable strip on GPIO 4, optional second run on GPIO 5.
// USB: commands and the boot line work on the native USB port and on a DevKit's UART port (Serial0).
// Flashed from the Ragamuffin Flasher (board "Hall window"). Then join WiFi "Window" (pw ragamuffin) -> 192.168.4.1
//
// What it does: the whole ring shows one colour at one brightness. That's the only control, on purpose.
//               A Test button runs every colour and the busy effects so a new strip can be checked end to end.
// Strip: WS2812B / WS2811 / SK6812-RGB style (one data wire), including addressable neon flex, 5, 12 or 24 V.
//        Colour order is a setting (GRB for most WS2812B, RGB/BRG for many WS2811 and neon), so no rebuild for a new reel.
// Wiring: data out through a 74AHCT125 (5 V) + 330 R at the board, all grounds tied, BIN to GND on WS2815.
// Power: a supply limit in mA (default 4200 = a 5 V 5 A supply with margin). Brightness is cut so the estimate
//        never goes over it. Set "mA per pixel at full white" from the reel's W/m (W/m / V / pixels per m * 1000).
// Network: always its own "Stairs" AP (192.168.4.1, captive page); join the hall WiFi from the page (ClarkShop preset)
//          -> http://hall-window.local, http://hall-window or its IP. MQTT is optional (blank broker = off).
//          Credentials live in NVS, never in this public repo.
// Firmware: page -> Install latest (phone fetches the newest build from Pages and posts it to /update),
//           or Upload file, or from a computer:  curl -F "f=@window.bin" http://hall-window.local/update
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
#include "version.h"                // CI writes FW_VERSION = the short commit SHA (same as window/manifest.json)
#endif
#ifndef FW_VERSION
#define FW_VERSION  "dev"
#endif
#define LATEST_URL  "https://jobot101.github.io/Ragamuffin-Flasher/window/"
#define AP_SSID     "Window"
#define AP_PASS     "ragamuffin"
#define HOSTNAME    "hall-window"
#define HALL_SSID   "STARLINK"      // the hall network: page default + preset. Password typed once (page or USB serial), NVS only
#define HOME_SSID   "ClarkShop"     // shop preset; never put a password in this public repo
#define TOPIC_BASE  "ragamuffin/hall/window"
#define PIN_A       4               // output 1 (Super Mini GPIO 4)
#define PIN_B       5               // output 2 (Super Mini GPIO 5), 0 pixels = off
#define MAX_PIX     4000            // per output (~120 ms per frame at 4000; solid colour only redraws on change)
#define BOOT_BTN    0

struct Cfg {
  int nA = 300, nB = 0;             // pixels (chips) per output; changing them restarts the board
  int ord = 0;                      // wire colour order, index into ORDERS
  int limMA = 4200;                 // supply limit, 0 = no limit
  int pixMA = 60;                   // one pixel at full white (WS2812B 5 V = 60)
  int trimB = 100;                  // output 2 level %
  int fadeMs = 600;
} cfg;
const char* ORDERS[] = {"GRB", "RGB", "BRG", "RBG", "GBR", "BGR"};
const int NORD = 6;

struct Look { uint8_t r, g, b; int bri; bool test; };
Look look = {255, 147, 41, 40, true};
// NVS namespace stays "stairs" (this started as the stair lights) so a board moved over keeps its WiFi and strip settings   // fresh board: Test on at 40 % so a new strip shows everything straight away
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
#include "page.h"   // PAGE: the phone page (Ragamuffin Hall brand)


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
  if (staTry && millis() - staTry > 20000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }
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
    for (Stream* S : {(Stream*)&Serial, (Stream*)&Serial0})
      S->printf("hall WiFi \"%s\": http://%s  http://%s.local\n", staSsid.c_str(), WiFi.localIP().toString().c_str(), HOSTNAME);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
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
void serialCmd(Stream& S, String& line) {
  while (S.available()) {
    char ch = S.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim(); if (!line.length()) continue;
    if (line.startsWith("wifi\t")) {
      int t = line.indexOf('\t', 5);
      staSsid = t < 0 ? line.substring(5) : line.substring(5, t); staPass = t < 0 ? "" : line.substring(t + 1);
      Preferences prefs; prefs.begin("stairs", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
      staBegin(); S.printf("joining %s\n", staSsid.c_str());
    } else {
      char c = line[0]; String a = line.substring(1); a.trim(); Look L = getLook();
      if (c == 'c') { uint8_t r, g, b; if (parseHex(a, r, g, b)) { L.r = r; L.g = g; L.b = b; L.test = false; setLook(L); } else S.println("c<rrggbb>"); }
      else if (c == 'b') { L.bri = a.toInt(); setLook(L); }
      else if (c == 't') { L.test = !L.test; setLook(L); }
      else if (c == 'n') { cfg.nA = constrain((int)a.toInt(), 1, MAX_PIX); saveCfg(); S.println("restarting"); delay(200); ESP.restart(); }
      S.println(stateJson());
      if (c == '?' || c == 'h') S.println("c<rrggbb> colour · b<0-100> brightness · t test · n<pixels> GPIO 4 count · wifi<TAB>ssid<TAB>pass · ? state");
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
  FastLED.clear(true);
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
