// Ragamuffin Hall chimes — ESP32-S3 version
//
// Keyboard 5-pin MIDI Out -> 3.3V MIDI interface (H11L1 opto) -> S3 UART RX
// S3 I2C -> 4x Adafruit I2C 8-Ch Solenoid Driver (MCP23017) @ 0x20..0x23 -> 25 solenoids
// Wi-Fi: its own "Chimes" WiFi (pw ragamuffin) -> 192.168.4.1, always on; join the hall WiFi from that page
//        -> http://hall-chimes.local. MQTT control + status. Firmware: page -> Install latest (no need to open the box)
//
// Core split (safety): core 1 = MIDI + solenoid timing only. core 0 = Wi-Fi/web/MQTT/updates.
// A stalled network can never hold a coil on. The two sides talk through queues.
//
// Libraries (Arduino Library Manager):
//   - Adafruit MCP23017 Arduino Library
//   - MIDI Library (FortySevenEffects)
//   - PubSubClient (Nick O'Leary)
// Board: ESP32S3 Dev Module (ESP32 Arduino core 3.x). Any S3 board works — set pins below.
// Flashed from the Ragamuffin Flasher (board "Hall chimes"); built by .github/workflows/chime.yml.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <MIDI.h>

#if __has_include("version.h")
#include "version.h"                // CI writes FW_VERSION = the short commit SHA (same as chime/manifest.json)
#endif
#ifndef FW_VERSION
#define FW_VERSION  "dev"
#endif

// ---------------- network config ----------------
// Nothing secret lives here: the repo is public. The hall WiFi password and the MQTT login are typed once
// on the board's page and kept in NVS (namespace "chime").
#define LATEST_URL  "https://jobot101.github.io/Ragamuffin-Flasher/chime/"
#define AP_SSID     "Chimes"
#define AP_PASS     "ragamuffin"
#define HOME_SSID   "ClarkShop"                 // preset network on the page
const char* DEVICE_NAME   = "hall-chimes";      // MQTT client id + hostname (hall-chimes.local)
const char* TOPIC_BASE    = "ragamuffin/hall/chimes";
// Subscribed:
//   <base>/note   payload "60" or "60,100"   (MIDI note, optional velocity 1-127)
//   <base>/mute   payload "1" / "0"
// Published:
//   <base>/status "online" / "offline" (retained, LWT)
//   <base>/played "60,100"                  (every strike, for other devices to react to)

// ---------------- pins ----------------
const int I2C_SDA     = 8;      // DevKitC-1 defaults; change for your board
const int I2C_SCL     = 9;
const int MIDI_RX_PIN = 18;     // from the MIDI interface's RX/data-out pin
const int MIDI_TX_PIN = -1;     // not used

// ---------------- instrument config ----------------
const uint8_t NUM_CHIMES = 25;  // C to C, two octaves
const uint8_t LOW_NOTE   = 48;  // MIDI note of chime 0 (48 = C3)

const uint8_t NUM_BOARDS = 4;
const uint8_t boardAddr[NUM_BOARDS] = {0x20, 0x21, 0x22, 0x23};
const uint8_t solPin[8] = {0, 1, 2, 3, 4, 5, 6, 7};   // MCP23017 port A = solenoid outputs 0-7

const uint32_t PULSE_MIN_US = 3000;    // softest hit
const uint32_t PULSE_MAX_US = 18000;   // hardest hit
const uint32_t PULSE_CAP_US = 35000;   // hard safety cap
const uint32_t REARM_US     = 25000;   // min gap between hits on one chime

// Polyphony limit (no bulk caps on the 24 V rail): never more than MAX_ACTIVE coils on at once,
// which keeps peak draw under the Mean Well's 10 A. Extra notes wait in line for a free coil
// (a few ms at most) instead of being dropped, so a big chord comes out as a tiny roll.
const uint8_t  MAX_ACTIVE     = 4;
const uint32_t PENDING_MAX_US = 50000;  // a waiting note older than this is dropped (would sound late)
const uint8_t  PENDING_SIZE   = 16;

float trim[NUM_CHIMES];

// ---------------- external triggers (active LOW) ----------------
// Each pin idles HIGH on the internal pull-up. Shorting it to GND fires the action once.
// Wire: switch / contact closure / open-collector output between the pin and GND. Nothing else.
// note >= 0 -> strike that MIDI note at vel.   note == -1 -> toggle mute.
struct Trigger { int pin; int note; uint8_t vel; };
const Trigger triggers[] = {
  { 4, 48, 110},   // C3
  { 5, 55, 110},   // G3
  { 6, 60, 110},   // C4
  { 7, 64, 110},   // E4
  {15, 67, 110},   // G4
  {16, 72, 110},   // C5
  {21, 60, 127},   // spare — reassign
  {38, -1,   0},   // mute toggle
};
const uint8_t NUM_TRIGGERS = sizeof(triggers) / sizeof(triggers[0]);
const uint32_t TRIG_DEBOUNCE_MS = 15;   // must be stable LOW this long to count

bool     trigStable[NUM_TRIGGERS];      // true = LOW (pressed)
uint32_t trigChangedAt[NUM_TRIGGERS];

// ---------------- globals ----------------
Adafruit_MCP23X17 mcp[NUM_BOARDS];
bool boardOk[NUM_BOARDS];

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

struct NoteMsg { uint8_t note; uint8_t vel; };
QueueHandle_t netToChimes;   // notes from MQTT / the page -> core 1
QueueHandle_t chimesToNet;   // played notes -> core 0 for publishing

uint32_t onAt[NUM_CHIMES];
uint32_t offAt[NUM_CHIMES];
bool     firing[NUM_CHIMES];
volatile bool muted = false;
volatile uint32_t played = 0;           // strikes since boot (page status)
volatile uint16_t lastPlayed = 0;       // note << 8 | vel

struct Pending { uint8_t note; uint8_t vel; uint32_t at; };
Pending pend[PENDING_SIZE];
uint8_t pendHead = 0, pendCount = 0;

// ======================= CORE 1: instrument =======================
inline void setChime(uint8_t idx, bool on) {
  uint8_t b = idx / 8;
  if (!boardOk[b]) return;
  mcp[b].digitalWrite(solPin[idx % 8], on ? HIGH : LOW);
}

void allOff() {
  for (uint8_t i = 0; i < NUM_CHIMES; i++) { setChime(i, false); firing[i] = false; }
}

uint8_t activeCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_CHIMES; i++) if (firing[i]) n++;
  return n;
}

inline bool canFire(int idx, uint32_t now) {
  return !firing[idx] && (now - onAt[idx]) >= REARM_US;
}

void fire(uint8_t idx, uint8_t note, uint8_t vel, uint32_t now) {
  float v = vel / 127.0f;
  v = v * v * 0.5f + v * 0.5f;
  uint32_t pulse = PULSE_MIN_US + (uint32_t)((PULSE_MAX_US - PULSE_MIN_US) * v * trim[idx]);
  if (pulse > PULSE_CAP_US) pulse = PULSE_CAP_US;

  setChime(idx, true);
  firing[idx] = true;
  onAt[idx]   = now;
  offAt[idx]  = now + pulse;
  played++;
  lastPlayed = (uint16_t)note << 8 | vel;

  NoteMsg m = {note, vel};
  xQueueSend(chimesToNet, &m, 0);   // never blocks; drops if the network side is backed up
}

void strike(uint8_t note, uint8_t vel) {
  if (muted || vel == 0) return;
  int idx = (int)note - LOW_NOTE;
  if (idx < 0 || idx >= NUM_CHIMES) return;

  uint32_t now = micros();
  if (!canFire(idx, now)) return;

  // free coil and nobody waiting -> fire now; otherwise get in line (keeps chord order)
  if (pendCount == 0 && activeCount() < MAX_ACTIVE) { fire(idx, note, vel, now); return; }
  if (pendCount == PENDING_SIZE) { pendHead = (pendHead + 1) % PENDING_SIZE; pendCount--; }  // drop oldest
  pend[(pendHead + pendCount) % PENDING_SIZE] = {note, vel, now};
  pendCount++;
}

void servicePending() {
  uint32_t now = micros();
  while (pendCount && activeCount() < MAX_ACTIVE) {
    Pending p = pend[pendHead];
    pendHead = (pendHead + 1) % PENDING_SIZE;
    pendCount--;
    if (now - p.at > PENDING_MAX_US) continue;      // too late to sound right
    int idx = (int)p.note - LOW_NOTE;
    if (!canFire(idx, now)) continue;               // same note already went
    fire(idx, p.note, p.vel, now);
  }
}

void handleNoteOn(byte ch, byte note, byte vel) { strike(note, vel); }

void serviceTriggers() {
  uint32_t ms = millis();
  for (uint8_t i = 0; i < NUM_TRIGGERS; i++) {
    bool low = (digitalRead(triggers[i].pin) == LOW);
    if (low == trigStable[i]) { trigChangedAt[i] = ms; continue; }
    if (ms - trigChangedAt[i] < TRIG_DEBOUNCE_MS) continue;   // not stable yet
    trigStable[i] = low;
    if (!low) continue;                                        // fire on the HIGH->GND edge only
    if (triggers[i].note < 0) muted = !muted;
    else strike((uint8_t)triggers[i].note, triggers[i].vel);
  }
}

// ======================= CORE 0: network =======================
Preferences prefs;
WebServer server(80);
DNSServer dns;

String staSsid, staPass;                 // hall WiFi (NVS)
String mqttHost, mqttUser, mqttPass;     // blank host = MQTT off (NVS)
uint16_t mqttPort = 1883;
uint8_t staWhy = 0;                      // last disconnect reason
uint32_t staTry = 0;                     // when the current join attempt started (0 = not trying)
uint32_t staNext = 0;                    // next attempt
bool updating = false;

// paced joins: a station that retries nonstop hops channels and keeps knocking phones off the Chimes AP
void staBegin() { staWhy = 0; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str()); staTry = millis() | 1; }
void staService() {
  if (!staSsid.length()) return;
  if (WiFi.status() == WL_CONNECTED) { staTry = 0; staNext = millis() + 45000; return; }
  if (staTry && millis() - staTry > 15000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }  // give up for now
  else if (!staTry && (int32_t)(millis() - staNext) >= 0) staBegin();
}

void loadNet() {
  prefs.begin("chime", true);
  staSsid  = prefs.getString("ssid", "");  staPass  = prefs.getString("wpass", "");
  mqttHost = prefs.getString("mhost", ""); mqttPort = prefs.getUShort("mport", 1883);
  mqttUser = prefs.getString("muser", ""); mqttPass = prefs.getString("mpass", "");
  prefs.end();
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[32];
  len = min(len, (unsigned int)sizeof(msg) - 1);
  memcpy(msg, payload, len);
  msg[len] = '\0';

  String t(topic);
  if (t.endsWith("/note")) {
    int note = 0, vel = 100;
    sscanf(msg, "%d,%d", &note, &vel);
    NoteMsg m = {(uint8_t)constrain(note, 0, 127), (uint8_t)constrain(vel, 1, 127)};
    xQueueSend(netToChimes, &m, 0);
  } else if (t.endsWith("/mute")) {
    muted = (msg[0] == '1');        // core 1 sees this and kills all coils
  }
}

String esc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return s; }

String stateJson() {
  bool up = WiFi.status() == WL_CONNECTED;
  String boards = "[";
  for (uint8_t b = 0; b < NUM_BOARDS; b++) boards += String(b ? "," : "") + (boardOk[b] ? "1" : "0");
  boards += "]";
  char b[512];
  uint16_t lp = lastPlayed;
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"boards\":%s,\"muted\":%d,\"played\":%lu,\"last\":[%u,%u],\"low\":%u,\"n\":%u,"
    "\"net\":{\"ssid\":\"%s\",\"up\":%d,\"ip\":\"%s\",\"why\":%d},"
    "\"mqtt\":{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"up\":%d}}",
    FW_VERSION, boards.c_str(), (int)muted, (unsigned long)played, lp >> 8, lp & 255, LOW_NOTE, NUM_CHIMES,
    esc(staSsid).c_str(), up, up ? WiFi.localIP().toString().c_str() : "", up ? 0 : staWhy,
    esc(mqttHost).c_str(), mqttPort, esc(mqttUser).c_str(), (int)mqtt.connected());
  return b;
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Hall chimes</title><style>
:root{--bg:#121212;--card:#1d1d1d;--fg:#eee;--dim:#999;--acc:#e8a33d;--bad:#e05a4f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 12px}h1 small{color:var(--dim);font-weight:400}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px}
button{font:inherit;border:0;border-radius:10px;padding:12px;background:#333;color:var(--fg);width:100%;touch-action:manipulation}
.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;color:var(--dim)}
input[type=range]{width:100%}label input{width:160px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)}
#keys{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-top:8px}#keys button{padding:10px 0;font-size:14px}
#keys button.s{background:#2a2a2a;color:var(--dim)}#keys button.on{background:var(--acc);color:#111}
.mute{background:var(--bad);color:#111;font-weight:700}#st{font:13px ui-monospace,monospace;color:var(--dim)}
</style></head><body><main>
<h1>Hall chimes <small id="fw"></small></h1>
<div class="card"><div id="st">…</div>
<label>velocity <b id="vv">100</b></label><input type="range" id="vel" min="1" max="127" value="100">
<div id="keys"></div><div class="row"><button id="mute">Mute</button></div></div>
<div class="card"><b>Hall WiFi</b> <small id="nets"></small>
<label>network <input id="ssid" list="aps"></label><datalist id="aps"></datalist>
<label>password <input id="wpass" type="password"></label>
<div class="row"><button id="clark">ClarkShop</button><button id="scan">Scan</button></div>
<div class="row"><button id="join">Join</button><button id="forget">Forget</button></div></div>
<div class="card"><b>MQTT</b> <small id="mq"></small>
<label>broker <input id="mhost" placeholder="blank = off"></label><label>port <input id="mport" type="number"></label>
<label>user <input id="muser"></label><label>password <input id="mpass" type="password" placeholder="unchanged"></label>
<div class="row"><button id="msave">Save</button></div></div>
<div class="card"><b>Firmware</b> <small id="fwv"></small>
<div class="row"><button id="latest">Install latest</button></div>
<p style="color:var(--dim);font-size:13px">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up). The chimes are muted while it writes.</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HOME=')HTML" HOME_SSID R"HTML(';
const $=id=>document.getElementById(id),go=u=>fetch(u).then(r=>r.json()).then(show).catch(()=>{});
const NAMES=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
let c={},built=0;
const WHY={2:'auth expired',15:'wrong password',201:'not found (2.4 GHz only)',202:'wrong password',203:'refused',204:'wrong password',205:'refused'};
function keys(j){if(built)return;built=1;for(let i=0;i<j.n;i++){const n=j.low+i,b=document.createElement('button');
 b.textContent=NAMES[n%12]+(Math.floor(n/12)-1);b.dataset.n=n;if(NAMES[n%12].length>1)b.className='s';
 b.onclick=()=>go(`/n?note=${n}&v=${$('vel').value}`);$('keys').append(b)}}
function show(j){if(!j){$('st').textContent='no reply';return}c=j;keys(j);const n=j.net||{},m=j.mqtt||{};
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · hall-chimes.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set — own WiFi only';
 $('mq').textContent=m.host?(m.up?'connected':'not connected'):'off';
 $('fw').textContent=j.fw;
 const found=j.boards.map((b,i)=>b?'0x'+(32+i).toString(16):null).filter(Boolean);
 $('st').textContent=`drivers ${found.length?found.join(' '):'none found'} · played ${j.played}${j.played?' · last '+NAMES[j.last[0]%12]+(Math.floor(j.last[0]/12)-1)+' v'+j.last[1]:''}`;
 document.querySelectorAll('#keys button').forEach(b=>b.classList.toggle('on',j.played&&+b.dataset.n===j.last[0]));
 $('mute').textContent=j.muted?'Muted — tap to unmute':'Mute';$('mute').className=j.muted?'mute':''}
function fill(j){show(j);$('ssid').value=(j.net&&j.net.ssid)||HOME;$('fwv').textContent='running '+j.fw;
 $('mhost').value=j.mqtt.host;$('mport').value=j.mqtt.port;$('muser').value=j.mqtt.user}
$('vel').oninput=()=>$('vv').textContent=$('vel').value;
$('mute').onclick=()=>go('/mute?on='+(c.muted?0:1));
$('scan').onclick=()=>{$('scan').textContent='scanning…';fetch('/scan').then(r=>r.json()).then(a=>{$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s)));$('scan').textContent=a.length+' found';if(a.length&&!$('ssid').value)$('ssid').value=a[0]}).catch(()=>$('scan').textContent='Scan')};
$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
$('join').onclick=()=>{if(!$('ssid').value)return;const q=new URLSearchParams({ssid:$('ssid').value,pass:$('wpass').value});go('/wifi?'+q)};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('msave').onclick=()=>{const q=new URLSearchParams({host:$('mhost').value,port:$('mport').value||1883,user:$('muser').value});
 if($('mpass').value)q.set('pass',$('mpass').value);go('/mqtt?'+q);$('mpass').value=''};
function send(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'chime.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),6000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)send(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');try{
 b.textContent='checking…';const m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json();
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'chime.bin',{cache:'no-store'});if(!r.ok)throw 0;
 send(await r.blob(),b)}catch(e){b.textContent='no internet — mobile data on, open in Chrome'}};
fetch('/state').then(r=>r.json()).then(fill);setInterval(()=>go('/state'),700);
</script></main></body></html>)HTML";

void sendState() { server.send(200, "application/json", stateJson()); }
int arg(const char* k, int d) { return server.hasArg(k) ? server.arg(k).toInt() : d; }

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/state", sendState);
  server.on("/n", [] {                   // test a tube from the page: same queue as MQTT, core 1 strikes it
    NoteMsg m = {(uint8_t)constrain(arg("note", 60), 0, 127), (uint8_t)constrain(arg("v", 100), 1, 127)};
    xQueueSend(netToChimes, &m, 0);
    sendState();
  });
  server.on("/mute", [] { muted = arg("on", 1) != 0; sendState(); });
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
    prefs.begin("chime", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
    sendState();
    if (staSsid.length()) staBegin(); else staTry = 0;
  });
  server.on("/mqtt", [] {
    mqttHost = server.arg("host"); mqttHost.trim();
    mqttPort = constrain(arg("port", 1883), 1, 65535);
    mqttUser = server.arg("user");
    if (server.hasArg("pass")) mqttPass = server.arg("pass");
    prefs.begin("chime", false);
    prefs.putString("mhost", mqttHost); prefs.putUShort("mport", mqttPort);
    prefs.putString("muser", mqttUser); prefs.putString("mpass", mqttPass);
    prefs.end();
    mqtt.disconnect();
    mqtt.setServer(mqttHost.c_str(), mqttPort);   // PubSubClient keeps the pointer: mqttHost is global
    sendState();
  });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "ok" : "update failed");
    if (ok) { delay(300); ESP.restart(); }
    updating = false;
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { updating = true; muted = true; Update.begin(UPDATE_SIZE_UNKNOWN); }  // coils off
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) Update.end(true);
    else if (u.status == UPLOAD_FILE_ABORTED) { Update.abort(); updating = false; }
  });
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });  // captive portal
  server.begin();
}

void netTask(void*) {
  uint32_t lastMqttTry = 0;
  char topic[96];

  loadNet();
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t i) { staWhy = i.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.printf("hall WiFi \"%s\": http://%s  http://%s.local\n", staSsid.c_str(), WiFi.localIP().toString().c_str(), DEVICE_NAME);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);   // staService() paces the retries
  if (staSsid.length()) staBegin();
  dns.start(53, "*", WiFi.softAPIP());
  MDNS.begin(DEVICE_NAME); MDNS.addService("http", "tcp", 80);
  setupWeb();
  Serial.printf("%s  WiFi \"%s\" -> http://%s\n", FW_VERSION, AP_SSID, WiFi.softAPIP().toString().c_str());

  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);

  for (;;) {
    uint32_t ms = millis();
    dns.processNextRequest();
    server.handleClient();
    staService();

    bool up = WiFi.status() == WL_CONNECTED;
    if (!up || !mqttHost.length() || updating) {
      if (mqtt.connected()) mqtt.disconnect();
      NoteMsg m; while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {}   // nobody to tell
    } else if (!mqtt.connected()) {
      if (ms - lastMqttTry > 5000) {
        lastMqttTry = ms;
        snprintf(topic, sizeof(topic), "%s/status", TOPIC_BASE);
        bool ok = mqttUser.length()
          ? mqtt.connect(DEVICE_NAME, mqttUser.c_str(), mqttPass.c_str(), topic, 1, true, "offline")
          : mqtt.connect(DEVICE_NAME, topic, 1, true, "offline");
        if (ok) {
          mqtt.publish(topic, "online", true);
          snprintf(topic, sizeof(topic), "%s/note", TOPIC_BASE); mqtt.subscribe(topic);
          snprintf(topic, sizeof(topic), "%s/mute", TOPIC_BASE); mqtt.subscribe(topic);
        }
      }
    } else {
      mqtt.loop();
      NoteMsg m;
      while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%u,%u", m.note, m.vel);
        snprintf(topic, sizeof(topic), "%s/played", TOPIC_BASE);
        mqtt.publish(topic, buf);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);

  netToChimes = xQueueCreate(32, sizeof(NoteMsg));
  chimesToNet = xQueueCreate(32, sizeof(NoteMsg));

  // Driver boards
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    boardOk[b] = mcp[b].begin_I2C(boardAddr[b], &Wire);
    Serial.printf("Driver 0x%02X: %s\n", boardAddr[b], boardOk[b] ? "OK" : "NOT FOUND");
    if (!boardOk[b]) continue;
    for (uint8_t p = 0; p < 8; p++) {
      mcp[b].pinMode(solPin[p], OUTPUT);
      mcp[b].digitalWrite(solPin[p], LOW);
    }
  }

  for (uint8_t i = 0; i < NUM_CHIMES; i++) { trim[i] = 1.0f; firing[i] = false; onAt[i] = 0; }
  for (uint8_t i = 0; i < 5; i++) trim[i] = 1.15f;   // lowest tubes get a bit more

  // Trigger inputs: pull-up, fire on short to GND
  for (uint8_t i = 0; i < NUM_TRIGGERS; i++) {
    pinMode(triggers[i].pin, INPUT_PULLUP);
    trigStable[i] = (digitalRead(triggers[i].pin) == LOW);   // anything held low at boot won't fire
    trigChangedAt[i] = millis();
  }

  // DIN MIDI on UART1. MIDI.begin() opens Serial1 on default pins, so re-open it
  // on our pin afterwards (works on ESP32 core 2.x and 3.x).
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();
  Serial1.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  // Network on core 0; this loop() runs on core 1
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);   // web server + MQTT + updates
}

void loop() {
  MIDI.read();                                  // keyboard: lowest-latency path
  serviceTriggers();                            // contact closures to GND

  NoteMsg m;
  while (xQueueReceive(netToChimes, &m, 0) == pdTRUE) strike(m.note, m.vel);

  static bool wasMuted = false;               // kill coils once on mute, not every loop
  if (muted && !wasMuted) { allOff(); pendCount = 0; }
  wasMuted = muted;

  uint32_t now = micros();
  for (uint8_t i = 0; i < NUM_CHIMES; i++) {
    if (firing[i] && (int32_t)(now - offAt[i]) >= 0) {
      setChime(i, false);
      firing[i] = false;
    }
  }

  servicePending();                             // waiting notes take the coils that just freed up
}
