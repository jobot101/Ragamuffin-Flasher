// Ragamuffin Hall chimes — display + control, on the Espressif ESP32-S3-BOX (original, TT21100 touch) + its dock.
//
// This is chime/chime.ino (the bell machine: web page, "Chimes" WiFi, hall WiFi, MQTT, updates) moved onto the
// BOX's free pins, with the BOX's screen as a front panel and the one-MOSFET solenoid tester folded in.
//
//   Screen: 25 chime keys (C3..C5), Mute, Soft/Med/Hard, and a big TAP button for the solenoid.
//   Page:   http://hall-chimes.local (hall WiFi) or WiFi "Chimes" / ragamuffin -> http://192.168.4.1
//
// Wiring (dock):
//   PMOD1 GPIO41 SDA / GPIO40 SCL -> 4x Adafruit #6318 (MCP23017) @ 0x20..0x23 -> 25 tube solenoids
//   PMOD2 GPIO44 (U0RXD)           <- DIN MIDI via a 3.3 V MIDI interface (H11L1 / 6N138); receive only
//   Driver 0x20 port B0..B7        <- contact-closure triggers to GND (B6 = solenoid tap, B7 = mute)
//   Solenoid tester: one logic-level MOSFET module gate on a free dock GPIO (default 9, set on the page).
//   BOOT button (GPIO0)            -> solenoid tap
//   BOX internals left alone: LCD 4/5/6/7/45/48, internal I2C 8/18 (touch, codec), touch INT 3, audio 2/15/16/17/46/47.
//
// Safety: short taps only. Tube pulses 3..18 ms by velocity, hard cap 30 ms. Solenoid tester: strike only (no hold),
//         3..30 ms, at least 150 ms between taps. Mute and firmware updates kill every coil.
// Core split: core 1 = MIDI + triggers + all coil timing (the only code that touches a coil).
//             core 0 = WiFi / web / MQTT / updates + screen / touch. They reach core 1 through netToChimes.
//
// Build: .github/workflows/chimebox.yml publishes https://jobot101.github.io/Ragamuffin-Flasher/chimebox/
//   (the flasher's "Chimes BOX" button and the page's Install latest read it). arduino-cli, esp32 core 3.2.0,
//   FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
//   Libraries: LovyanGFX, Adafruit MCP23017 Arduino Library, PubSubClient.
// Nothing secret in here: the hall WiFi password and MQTT login live in NVS (namespace "chime"), set on the page
// or over USB serial:  wifi<TAB>ssid<TAB>password

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <LovyanGFX.hpp>

#if __has_include("version.h")
#include "version.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION  "box-dev"
#endif

// ---------------- network config ----------------
#define LATEST_URL  "https://jobot101.github.io/Ragamuffin-Flasher/chimebox/"   // BOX builds only (chimebox.yml)
#define AP_SSID     "Chimes"
#define AP_PASS     "ragamuffin"
#define HOME_SSID   "ClarkShop"
const char* DEVICE_NAME = "hall-chimes";
const char* TOPIC_BASE  = "ragamuffin/hall/chimes";
// Subscribed: <base>/note "60" or "60,100"   <base>/mute "1"/"0"   <base>/tap (any payload = solenoid tap)
// Published:  <base>/status "online"/"offline" (retained, LWT)   <base>/played "60,100"

// ---------------- pins (ESP32-S3-BOX + dock) ----------------
const int I2C_SDA     = 41;     // dock PMOD1
const int I2C_SCL     = 40;
const int MIDI_RX_PIN = 44;     // dock PMOD2 (U0RXD)
const int BOOT_BTN    = 0;
const int SOL_PINS[]  = {9, 10, 11, 12, 13, 14, 21, 38, 39, 42};   // free dock pins the tester may use
const int SOL_DEFAULT = 9;

// ---------------- instrument config ----------------
const uint8_t NUM_CHIMES = 25;  // C to C, two octaves
const uint8_t LOW_NOTE   = 48;  // MIDI note of chime 0 (48 = C3)
const uint8_t NOTE_TAP   = 255; // queue code: solenoid tester tap

const uint8_t NUM_BOARDS = 4;
const uint8_t boardAddr[NUM_BOARDS] = {0x20, 0x21, 0x22, 0x23};
const uint8_t solPin[8] = {0, 1, 2, 3, 4, 5, 6, 7};   // MCP23017 port A = solenoid outputs 0-7

const uint32_t PULSE_MIN_US = 3000;    // softest hit
const uint32_t PULSE_MAX_US = 18000;   // hardest hit
const uint32_t PULSE_CAP_US = 30000;   // hard safety cap (short taps only)
const uint32_t REARM_US     = 25000;   // min gap between hits on one chime

const uint8_t  MAX_ACTIVE     = 4;      // 24 V supply limit: at most 4 coils on at once
const uint32_t PENDING_MAX_US = 50000;
const uint8_t  PENDING_SIZE   = 16;

const int      SOL_MS_MIN = 3, SOL_MS_MAX = 30, SOL_MS_DEF = 12;
const uint32_t SOL_REARM_US = 150000;

float trim[NUM_CHIMES];

// ---------------- external triggers (active LOW, on driver 0x20 port B) ----------------
struct Trigger { uint8_t bit; int note; uint8_t vel; };   // note -1 = mute toggle, NOTE_TAP = solenoid tap
const Trigger triggers[] = {
  {0, 48, 110}, {1, 55, 110}, {2, 60, 110}, {3, 64, 110}, {4, 67, 110}, {5, 72, 110},
  {6, NOTE_TAP, 0},   // B6 solenoid tap
  {7, -1, 0},         // B7 mute toggle
};
const uint8_t  NUM_TRIGGERS     = sizeof(triggers) / sizeof(triggers[0]);
const uint8_t  TRIG_BOARD       = 0;
const uint32_t TRIG_POLL_US     = 2000;
const uint32_t TRIG_DEBOUNCE_MS = 15;
bool     trigStable[NUM_TRIGGERS];
uint32_t trigChangedAt[NUM_TRIGGERS];

// ---------------- globals ----------------
Adafruit_MCP23X17 mcp[NUM_BOARDS];
bool boardOk[NUM_BOARDS];
HardwareSerial MidiSerial(1);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

struct NoteMsg { uint8_t note; uint8_t vel; };
struct Btn { int x, w; };    // screen button (here so Arduino's generated prototypes can see it)
QueueHandle_t netToChimes;   // notes / taps from the page, MQTT and the screen -> core 1
QueueHandle_t chimesToNet;   // played notes -> core 0 for publishing

uint32_t onAt[NUM_CHIMES], offAt[NUM_CHIMES];
bool     firing[NUM_CHIMES];
volatile bool muted = false;
volatile uint32_t played = 0;
volatile uint16_t lastPlayed = 0;        // note << 8 | vel
volatile uint32_t lastPlayedMs = 0;

struct Pending { uint8_t note; uint8_t vel; uint32_t at; };
Pending pend[PENDING_SIZE];
uint8_t pendHead = 0, pendCount = 0;

// solenoid tester (pin and polarity fixed at boot; pulse length live)
int  solGpio = SOL_DEFAULT;              // -1 = off
bool solInv  = false;
volatile int solMs = SOL_MS_DEF;
volatile bool solFiring = false;
volatile uint32_t solTaps = 0, solLastMs = 0;
uint32_t solOnAt = 0, solOffAt = 0;

volatile uint8_t uiVel = 90;             // screen strength: 45 soft / 90 med / 127 hard
bool updating = false;

// ======================= CORE 1: instrument =======================
inline void setChime(uint8_t idx, bool on) {
  uint8_t b = idx / 8;
  if (!boardOk[b]) return;
  mcp[b].digitalWrite(solPin[idx % 8], on ? HIGH : LOW);
}
inline void solWrite(bool on) { if (solGpio >= 0) digitalWrite(solGpio, (on ^ solInv) ? HIGH : LOW); }

void allOff() {
  solWrite(false); solFiring = false;
  for (uint8_t i = 0; i < NUM_CHIMES; i++) { setChime(i, false); firing[i] = false; }
}

uint8_t activeCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_CHIMES; i++) if (firing[i]) n++;
  return n;
}
inline bool canFire(int idx, uint32_t now) { return !firing[idx] && (now - onAt[idx]) >= REARM_US; }

void fire(uint8_t idx, uint8_t note, uint8_t vel, uint32_t now) {
  float v = vel / 127.0f;
  v = v * v * 0.5f + v * 0.5f;
  uint32_t pulse = PULSE_MIN_US + (uint32_t)((PULSE_MAX_US - PULSE_MIN_US) * v * trim[idx]);
  if (pulse > PULSE_CAP_US) pulse = PULSE_CAP_US;
  setChime(idx, true);
  firing[idx] = true; onAt[idx] = now; offAt[idx] = now + pulse;
  played++; lastPlayed = (uint16_t)note << 8 | vel; lastPlayedMs = millis();
  NoteMsg m = {note, vel};
  xQueueSend(chimesToNet, &m, 0);
}

void solTap() {
  if (muted || updating || solGpio < 0 || solFiring) return;
  uint32_t now = micros();
  if (solTaps && now - solOnAt < SOL_REARM_US) return;
  uint32_t pulse = (uint32_t)constrain((int)solMs, SOL_MS_MIN, SOL_MS_MAX) * 1000;
  solWrite(true);
  solFiring = true; solOnAt = now; solOffAt = now + pulse;
  solTaps++; solLastMs = millis();
}

inline void serviceSol() {
  if (solFiring && (int32_t)(micros() - solOffAt) >= 0) { solWrite(false); solFiring = false; }
}

void strike(uint8_t note, uint8_t vel) {
  if (note == NOTE_TAP) { solTap(); return; }
  if (muted || vel == 0) return;
  int idx = (int)note - LOW_NOTE;
  if (idx < 0 || idx >= NUM_CHIMES) return;
  uint32_t now = micros();
  if (!canFire(idx, now)) return;
  if (pendCount == 0 && activeCount() < MAX_ACTIVE) { fire(idx, note, vel, now); return; }
  if (pendCount == PENDING_SIZE) { pendHead = (pendHead + 1) % PENDING_SIZE; pendCount--; }
  pend[(pendHead + pendCount) % PENDING_SIZE] = {note, vel, now};
  pendCount++;
}

void servicePending() {
  uint32_t now = micros();
  while (pendCount && activeCount() < MAX_ACTIVE) {
    Pending p = pend[pendHead];
    pendHead = (pendHead + 1) % PENDING_SIZE; pendCount--;
    if (now - p.at > PENDING_MAX_US) continue;
    int idx = (int)p.note - LOW_NOTE;
    if (!canFire(idx, now)) continue;
    fire(idx, p.note, p.vel, now);
  }
}

// DIN MIDI: note-on only, any channel, running status, realtime bytes ignored
uint8_t midiStatus = 0, midiData[2], midiCount = 0;
void midiByte(uint8_t b) {
  if (b >= 0xF8) return;
  if (b & 0x80) { midiStatus = (b < 0xF0) ? b : 0; midiCount = 0; return; }
  if (!midiStatus) return;
  uint8_t type = midiStatus & 0xF0;
  uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
  midiData[midiCount++] = b;
  if (midiCount < need) return;
  midiCount = 0;
  if (type == 0x90 && midiData[1] > 0) strike(midiData[0], midiData[1]);
}

void serviceTriggers() {
  static uint32_t lastPoll = 0;
  uint32_t us = micros();
  if (us - lastPoll < TRIG_POLL_US || !boardOk[TRIG_BOARD]) return;
  lastPoll = us;
  uint8_t g = mcp[TRIG_BOARD].readGPIOB();
  uint32_t ms = millis();
  for (uint8_t i = 0; i < NUM_TRIGGERS; i++) {
    bool low = !((g >> triggers[i].bit) & 1);
    if (low == trigStable[i]) { trigChangedAt[i] = ms; continue; }
    if (ms - trigChangedAt[i] < TRIG_DEBOUNCE_MS) continue;
    trigStable[i] = low;
    if (!low) continue;
    if (triggers[i].note < 0) muted = !muted;
    else strike((uint8_t)triggers[i].note, triggers[i].vel);
  }
}

void serviceBootButton() {             // BOOT = solenoid tap, like the tester
  static bool stable = false; static uint32_t changed = 0;
  bool low = digitalRead(BOOT_BTN) == LOW;
  uint32_t ms = millis();
  if (low == stable) { changed = ms; return; }
  if (ms - changed < 25) return;
  stable = low;
  if (low) solTap();
}

// ======================= CORE 0: network =======================
Preferences prefs;
WebServer server(80);
DNSServer dns;

String staSsid, staPass;
String mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;
uint8_t staWhy = 0;
uint32_t staTry = 0, staNext = 0;

void staBegin() { staWhy = 0; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str()); staTry = millis() | 1; }
void staService() {
  if (!staSsid.length()) return;
  if (WiFi.status() == WL_CONNECTED) { staTry = 0; staNext = millis() + 45000; return; }
  if (staTry && millis() - staTry > 15000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }
  else if (!staTry && (int32_t)(millis() - staNext) >= 0) staBegin();
}

void loadCfg() {
  prefs.begin("chime", true);
  staSsid  = prefs.getString("ssid", "");  staPass  = prefs.getString("wpass", "");
  mqttHost = prefs.getString("mhost", ""); mqttPort = prefs.getUShort("mport", 1883);
  mqttUser = prefs.getString("muser", ""); mqttPass = prefs.getString("mpass", "");
  solGpio  = prefs.getInt("spin", SOL_DEFAULT);
  solInv   = prefs.getBool("sinv", false);
  solMs    = constrain(prefs.getInt("sms", SOL_MS_DEF), SOL_MS_MIN, SOL_MS_MAX);
  prefs.end();
  bool ok = solGpio < 0;
  for (int p : SOL_PINS) if (p == solGpio) ok = true;
  if (!ok) solGpio = -1;
}

void saveWifi(const String& s, const String& p) {
  staSsid = s; staPass = p;
  prefs.begin("chime", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
  if (staSsid.length()) staBegin(); else { staTry = 0; WiFi.disconnect(false, true); }
}

void queueNote(uint8_t note, uint8_t vel) { NoteMsg m = {note, vel}; xQueueSend(netToChimes, &m, 0); }

void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[32];
  len = min(len, (unsigned int)sizeof(msg) - 1);
  memcpy(msg, payload, len); msg[len] = '\0';
  String t(topic);
  if (t.endsWith("/note")) {
    int note = 0, vel = 100;
    sscanf(msg, "%d,%d", &note, &vel);
    queueNote((uint8_t)constrain(note, 0, 127), (uint8_t)constrain(vel, 1, 127));
  } else if (t.endsWith("/mute")) muted = (msg[0] == '1');
  else if (t.endsWith("/tap")) queueNote(NOTE_TAP, 0);
}

String esc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return s; }

String stateJson() {
  bool up = WiFi.status() == WL_CONNECTED;
  String boards = "[";
  for (uint8_t b = 0; b < NUM_BOARDS; b++) boards += String(b ? "," : "") + (boardOk[b] ? "1" : "0");
  boards += "]";
  char b[640];
  uint16_t lp = lastPlayed;
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"box\":1,\"boards\":%s,\"muted\":%d,\"played\":%lu,\"last\":[%u,%u],\"low\":%u,\"n\":%u,\"vel\":%u,"
    "\"sol\":{\"pin\":%d,\"inv\":%d,\"ms\":%d,\"taps\":%lu,\"min\":%d,\"max\":%d},"
    "\"net\":{\"ssid\":\"%s\",\"up\":%d,\"ip\":\"%s\",\"why\":%d},"
    "\"mqtt\":{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"up\":%d}}",
    FW_VERSION, boards.c_str(), (int)muted, (unsigned long)played, lp >> 8, lp & 255, LOW_NOTE, NUM_CHIMES, uiVel,
    solGpio, (int)solInv, (int)solMs, (unsigned long)solTaps, SOL_MS_MIN, SOL_MS_MAX,
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
input[type=range]{width:100%}label input,label select{width:160px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)}
label input[type=checkbox]{width:auto}
#keys{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-top:8px}#keys button{padding:10px 0;font-size:14px}
#keys button.s{background:#2a2a2a;color:var(--dim)}#keys button.on{background:var(--acc);color:#111}
.mute{background:var(--bad);color:#111;font-weight:700}#st,#sst{font:13px ui-monospace,monospace;color:var(--dim)}
#tap{background:var(--acc);color:#111;font-weight:700;font-size:20px;padding:18px}
</style></head><body><main>
<h1>Hall chimes <small id="fw"></small></h1>
<div class="card"><div id="st">…</div>
<label>velocity <b id="vv">100</b></label><input type="range" id="vel" min="1" max="127" value="100">
<div id="keys"></div><div class="row"><button id="mute">Mute</button></div></div>
<div class="card"><b>Solenoid</b> <small id="sp"></small><div id="sst"></div>
<div class="row"><button id="tap">TAP</button></div>
<label>pulse <b id="smv">12 ms</b></label><input type="range" id="sms" min="3" max="30" value="12">
<label>GPIO <select id="spin"></select></label><label>active low <input type="checkbox" id="sinv"></label>
<div class="row"><button id="ssave">Save pin (restarts)</button></div>
<p style="color:var(--dim);font-size:13px">Short taps only: strike, never hold, 30 ms at most. Also BOOT button, the screen's TAP and trigger B6.</p></div>
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
<p style="color:var(--dim);font-size:13px">Needs internet on this phone or computer (mobile data on, page open in Chrome, not the WiFi sign-in pop-up). The chimes are muted while it writes. This is the ESP32-S3-BOX build: never upload the plain chime.bin here, it uses the screen's pins.</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HOME=')HTML" HOME_SSID R"HTML(',PINS=[-1,9,10,11,12,13,14,21,38,39,42];
const $=id=>document.getElementById(id),go=u=>fetch(u).then(r=>r.json()).then(show).catch(()=>{});
const NAMES=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'],nm=n=>NAMES[n%12]+(Math.floor(n/12)-1);
let c={},built=0;
const WHY={2:'auth expired',15:'wrong password',201:'not found (2.4 GHz only)',202:'wrong password',203:'refused',204:'wrong password',205:'refused'};
PINS.forEach(p=>$('spin').append(new Option(p<0?'off':'GPIO '+p,p)));
function keys(j){if(built)return;built=1;for(let i=0;i<j.n;i++){const n=j.low+i,b=document.createElement('button');
 b.textContent=nm(n);b.dataset.n=n;if(NAMES[n%12].length>1)b.className='s';
 b.onclick=()=>go(`/n?note=${n}&v=${$('vel').value}`);$('keys').append(b)}}
function show(j){if(!j){$('st').textContent='no reply';return}c=j;keys(j);const n=j.net||{},m=j.mqtt||{},s=j.sol||{};
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · hall-chimes.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set — own WiFi only';
 $('mq').textContent=m.host?(m.up?'connected':'not connected'):'off';
 $('fw').textContent=j.fw;
 const found=j.boards.map((b,i)=>b?'0x'+(32+i).toString(16):null).filter(Boolean);
 $('st').textContent=`drivers ${found.length?found.join(' '):'none found'} · played ${j.played}${j.played?' · last '+nm(j.last[0])+' v'+j.last[1]:''}`;
 $('sp').textContent=s.pin<0?'off':`GPIO ${s.pin}${s.inv?' (active low)':''}`;
 $('sst').textContent=`taps ${s.taps} · pulse ${s.ms} ms`;
 document.querySelectorAll('#keys button').forEach(b=>b.classList.toggle('on',j.played&&+b.dataset.n===j.last[0]));
 $('mute').textContent=j.muted?'Muted — tap to unmute':'Mute';$('mute').className=j.muted?'mute':''}
function fill(j){show(j);$('ssid').value=(j.net&&j.net.ssid)||HOME;$('fwv').textContent='running '+j.fw;
 $('mhost').value=j.mqtt.host;$('mport').value=j.mqtt.port;$('muser').value=j.mqtt.user;
 $('sms').value=j.sol.ms;$('smv').textContent=j.sol.ms+' ms';$('spin').value=j.sol.pin;$('sinv').checked=!!j.sol.inv}
$('vel').oninput=()=>$('vv').textContent=$('vel').value;
$('mute').onclick=()=>go('/mute?on='+(c.muted?0:1));
$('tap').onclick=()=>go('/tap');
$('sms').oninput=()=>$('smv').textContent=$('sms').value+' ms';
$('sms').onchange=()=>go('/sol?ms='+$('sms').value);
$('ssave').onclick=()=>{if(!confirm('Save the solenoid pin and restart the board?'))return;
 fetch(`/solpin?pin=${$('spin').value}&inv=${$('sinv').checked?1:0}`).then(()=>setTimeout(()=>location.reload(),5000))};
$('scan').onclick=()=>{$('scan').textContent='scanning…';fetch('/scan').then(r=>r.json()).then(a=>{$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s)));$('scan').textContent=a.length+' found';if(a.length&&!$('ssid').value)$('ssid').value=a[0]}).catch(()=>$('scan').textContent='Scan')};
$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
$('join').onclick=()=>{if(!$('ssid').value)return;const q=new URLSearchParams({ssid:$('ssid').value,pass:$('wpass').value});go('/wifi?'+q)};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('msave').onclick=()=>{const q=new URLSearchParams({host:$('mhost').value,port:$('mport').value||1883,user:$('muser').value});
 if($('mpass').value)q.set('pass',$('mpass').value);go('/mqtt?'+q);$('mpass').value=''};
function send(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'chime_box.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),6000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)send(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');let m;try{
 b.textContent='checking…';m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json()}catch(e){b.textContent='no internet — mobile data on, open in Chrome';return}
 if(!m.box){b.textContent='that is not a BOX build — not installed';return}
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 try{b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'chime_box.bin',{cache:'no-store'});if(!r.ok)throw 0;
 send(await r.blob(),b)}catch(e){b.textContent='download failed — try again'}};
fetch('/state').then(r=>r.json()).then(fill);setInterval(()=>go('/state'),700);
</script></main></body></html>)HTML";

void sendState() { server.send(200, "application/json", stateJson()); }
int arg(const char* k, int d) { return server.hasArg(k) ? server.arg(k).toInt() : d; }

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/state", sendState);
  server.on("/n", [] {
    queueNote((uint8_t)constrain(arg("note", 60), 0, 127), (uint8_t)constrain(arg("v", 100), 1, 127));
    sendState();
  });
  server.on("/tap", [] { queueNote(NOTE_TAP, 0); sendState(); });
  server.on("/sol", [] {
    solMs = constrain(arg("ms", solMs), SOL_MS_MIN, SOL_MS_MAX);
    prefs.begin("chime", false); prefs.putInt("sms", solMs); prefs.end();
    sendState();
  });
  server.on("/solpin", [] {                 // pin / polarity only change at boot, so a change can't fire the coil
    int p = arg("pin", -1); bool ok = p < 0;
    for (int q : SOL_PINS) if (q == p) ok = true;
    if (!ok) { server.send(400, "text/plain", "pin not allowed"); return; }
    muted = true;
    prefs.begin("chime", false); prefs.putInt("spin", p); prefs.putBool("sinv", arg("inv", 0) != 0); prefs.end();
    server.send(200, "text/plain", "saved, restarting");
    delay(300); ESP.restart();
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
    if (server.hasArg("forget")) saveWifi("", "");
    else saveWifi(server.arg("ssid"), server.arg("pass"));
    sendState();
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
    mqtt.setServer(mqttHost.c_str(), mqttPort);
    sendState();
  });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "ok" : "update failed");
    if (ok) { delay(300); ESP.restart(); }
    updating = false;
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { updating = true; muted = true; Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) Update.end(true);
    else if (u.status == UPLOAD_FILE_ABORTED) { Update.abort(); updating = false; }
  });
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
}

// USB serial: "wifi<TAB>ssid<TAB>pass", "tap", "n <note> [vel]", "mute 0|1", "?"
void serviceSerial() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.length() < 200) line += ch; continue; }
    String l = line; line = "";
    if (l.startsWith("wifi\t")) {
      int t = l.indexOf('\t', 5);
      String s = t > 0 ? l.substring(5, t) : l.substring(5), p = t > 0 ? l.substring(t + 1) : "";
      saveWifi(s, p);
      Serial.printf("hall WiFi set to \"%s\", joining\n", s.c_str());
    } else if (l == "tap") queueNote(NOTE_TAP, 0);
    else if (l.startsWith("n ")) { int n = 60, v = 100; sscanf(l.c_str() + 2, "%d %d", &n, &v); queueNote(constrain(n, 0, 127), constrain(v, 1, 127)); }
    else if (l.startsWith("mute ")) muted = l[5] == '1';
    else if (l.length()) Serial.println(stateJson());
  }
}

void netTask(void*) {
  uint32_t lastMqttTry = 0;
  char topic[96];

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t i) { staWhy = i.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.printf("hall WiFi \"%s\": http://%s  http://%s.local\n", staSsid.c_str(), WiFi.localIP().toString().c_str(), DEVICE_NAME);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
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
    serviceSerial();

    bool up = WiFi.status() == WL_CONNECTED;
    if (!up || !mqttHost.length() || updating) {
      if (mqtt.connected()) mqtt.disconnect();
      NoteMsg m; while (xQueueReceive(chimesToNet, &m, 0) == pdTRUE) {}
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
          snprintf(topic, sizeof(topic), "%s/tap", TOPIC_BASE);  mqtt.subscribe(topic);
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

// ======================= CORE 0: screen + touch =======================
class LGFX_Box : public lgfx::LGFX_Device {     // ESP32-S3-BOX: same settings as LovyanGFX's own autodetect
  lgfx::Panel_ILI9342 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_TT21xxx _touch;
public:
  LGFX_Box() {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0; c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 7; c.pin_mosi = 6; c.pin_miso = -1; c.pin_dc = 4;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 5; c.pin_rst = 48; c.pin_busy = -1; c.offset_rotation = 1; c.readable = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 45; c.invert = false; c.freq = 12000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    { auto c = _touch.config();
      c.pin_int = 3; c.pin_sda = 8; c.pin_scl = 18; c.i2c_addr = 0x24; c.i2c_port = I2C_NUM_1; c.freq = 400000;
      c.x_min = 0; c.x_max = 319; c.y_min = 0; c.y_max = 239; c.bus_shared = false;
      _touch.config(c); _panel.setTouch(&_touch);
      float affine[6] = {1, 0, 0, 0, -1, 240};
      _panel.setCalibrateAffine(affine); }
    setPanel(&_panel);
  }
};
LGFX_Box lcd;

// layout (320 x 240)
const int KX = 4, KY = 26, KW = 60, KH = 26, KPX = 63, KPY = 28;   // 5 x 5 keys
const int BY = 184, BH = 52;                                       // bottom buttons
const Btn B_MUTE = {4, 92}, B_VEL = {100, 96}, B_TAP = {200, 116};

uint16_t C_BG, C_BAR, C_OAK, C_SHARP, C_TXT, C_DIM, C_BRASS, C_BAD, C_OK;
const char* NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void drawKey(int i, bool lit, bool pressed) {
  int n = LOW_NOTE + i, x = KX + (i % 5) * KPX, y = KY + (i / 5) * KPY;
  bool sharp = strlen(NAMES[n % 12]) > 1;
  uint16_t bg = lit ? C_BRASS : pressed ? C_DIM : sharp ? C_SHARP : C_OAK;
  lcd.fillRoundRect(x, y, KW, KH, 5, bg);
  lcd.setTextColor(lit || pressed ? C_BG : sharp ? C_DIM : C_TXT, bg);
  lcd.setTextDatum(middle_center); lcd.setFont(&fonts::Font2);
  char s[6]; snprintf(s, sizeof s, "%s%d", NAMES[n % 12], n / 12 - 1);
  lcd.drawString(s, x + KW / 2, y + KH / 2 + 1);
}

void drawBtn(const Btn& b, const char* top, const char* bottom, uint16_t bg, uint16_t fg) {
  lcd.fillRoundRect(b.x, BY, b.w, BH, 8, bg);
  lcd.setTextColor(fg, bg); lcd.setTextDatum(middle_center);
  if (bottom && *bottom) {
    lcd.setFont(&fonts::Font2); lcd.drawString(top, b.x + b.w / 2, BY + 16);
    lcd.setFont(&fonts::Font2); lcd.drawString(bottom, b.x + b.w / 2, BY + 36);
  } else { lcd.setFont(&fonts::Font4); lcd.drawString(top, b.x + b.w / 2, BY + BH / 2 + 1); }
}
void drawMute(bool m) { drawBtn(B_MUTE, m ? "MUTED" : "Mute", m ? "tap to unmute" : "", m ? C_BAD : C_OAK, m ? C_BG : C_TXT); }
void drawVel(uint8_t v) { drawBtn(B_VEL, "strength", v <= 45 ? "Soft" : v <= 90 ? "Medium" : "Hard", C_OAK, C_TXT); }
void drawTap(bool lit) {
  char sub[24];
  if (solGpio < 0) snprintf(sub, sizeof sub, "set GPIO on page"); else snprintf(sub, sizeof sub, "GPIO %d  %d ms", solGpio, (int)solMs);
  drawBtn(B_TAP, "TAP", sub, lit ? C_TXT : C_BRASS, C_BG);
}

void drawBar(const String& s) {
  lcd.fillRect(0, 0, 320, 22, C_BAR);
  lcd.setFont(&fonts::Font2); lcd.setTextDatum(middle_left); lcd.setTextColor(C_BRASS, C_BAR);
  lcd.drawString("Hall chimes", 6, 11);
  lcd.setTextDatum(middle_right); lcd.setTextColor(C_TXT, C_BAR);
  lcd.drawString(s, 314, 11);
}
void drawInfo(const String& s, bool bad) {
  lcd.fillRect(0, 167, 320, 16, C_BG);
  lcd.setFont(&fonts::Font0); lcd.setTextDatum(middle_center); lcd.setTextColor(bad ? C_BAD : C_DIM, C_BG);
  lcd.drawString(s, 160, 175);
}

int hitKey(int x, int y) {
  if (y < KY || y >= KY + 5 * KPY || x < KX) return -1;
  int c = (x - KX) / KPX, r = (y - KY) / KPY;
  if (c > 4 || (x - KX) % KPX >= KW || (y - KY) % KPY >= KH) return -1;
  return r * 5 + c;
}
bool inBtn(const Btn& b, int x, int y) { return y >= BY && y < BY + BH && x >= b.x && x < b.x + b.w; }

void uiTask(void*) {
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(160);
  C_BG = lcd.color565(18, 15, 12);   C_BAR = lcd.color565(40, 30, 20);  C_OAK = lcd.color565(74, 56, 38);
  C_SHARP = lcd.color565(42, 33, 25); C_TXT = lcd.color565(240, 230, 210); C_DIM = lcd.color565(150, 140, 125);
  C_BRASS = lcd.color565(232, 163, 61); C_BAD = lcd.color565(224, 90, 79); C_OK = lcd.color565(120, 200, 120);
  lcd.fillScreen(C_BG);

  int litKey = -1, pressKey = -1;
  bool lastMute = !muted, tapLit = false; uint8_t lastVel = 0; int lastSolMs = -1;
  String lastBar, lastInfo;
  bool wasDown = false;
  for (int i = 0; i < NUM_CHIMES; i++) drawKey(i, false, false);

  for (;;) {
    uint32_t ms = millis();
    // ---- touch: act on the press, not while held
    int32_t tx, ty;
    bool down = lcd.getTouch(&tx, &ty) > 0;
    if (down && !wasDown) {
      int k = hitKey(tx, ty);
      if (k >= 0) { queueNote(LOW_NOTE + k, uiVel); pressKey = k; drawKey(k, false, true); }
      else if (inBtn(B_MUTE, tx, ty)) muted = !muted;
      else if (inBtn(B_VEL, tx, ty)) uiVel = uiVel <= 45 ? 90 : uiVel <= 90 ? 127 : 45;
      else if (inBtn(B_TAP, tx, ty)) queueNote(NOTE_TAP, 0);
    }
    if (!down && wasDown && pressKey >= 0) { drawKey(pressKey, pressKey == litKey, false); pressKey = -1; }
    wasDown = down;

    // ---- last struck tube glows for 180 ms
    int lit = (played && ms - lastPlayedMs < 180) ? (int)(lastPlayed >> 8) - LOW_NOTE : -1;
    if (lit < 0 || lit >= NUM_CHIMES) lit = -1;
    if (lit != litKey) {
      if (litKey >= 0) drawKey(litKey, false, litKey == pressKey);
      if (lit >= 0) drawKey(lit, true, false);
      litKey = lit;
    }

    if (muted != lastMute) { lastMute = muted; drawMute(lastMute); }
    if (uiVel != lastVel) { lastVel = uiVel; drawVel(lastVel); }
    bool tl = solTaps && ms - solLastMs < 150;
    if (tl != tapLit || solMs != lastSolMs) { tapLit = tl; lastSolMs = solMs; drawTap(tapLit); }

    if (ms % 500 < 20) {                 // slow-changing text, a couple of times a second
      String bar;
      if (updating) bar = "updating...";
      else if (WiFi.status() == WL_CONNECTED) bar = staSsid + "  " + WiFi.localIP().toString();
      else if (staSsid.length()) bar = "joining " + staSsid + "...";
      else bar = String("WiFi \"") + AP_SSID + "\" 192.168.4.1";
      if (bar != lastBar) { lastBar = bar; drawBar(bar); }

      int nb = 0; String found;
      for (uint8_t b = 0; b < NUM_BOARDS; b++) if (boardOk[b]) { nb++; found += " 0x" + String(boardAddr[b], HEX); }
      String info = nb ? "drivers" + found : String("no tube drivers found on PMOD1");
      info += "  .  played " + String((unsigned long)played) + "  .  taps " + String((unsigned long)solTaps);
      if (mqttHost.length()) info += mqtt.connected() ? "  .  MQTT" : "  .  MQTT down";
      if (info != lastInfo) { lastInfo = info; drawInfo(info, nb == 0); }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  loadCfg();
  if (solGpio >= 0) { digitalWrite(solGpio, solInv ? HIGH : LOW); pinMode(solGpio, OUTPUT); solWrite(false); }
  pinMode(BOOT_BTN, INPUT_PULLUP);

  netToChimes = xQueueCreate(32, sizeof(NoteMsg));
  chimesToNet = xQueueCreate(32, sizeof(NoteMsg));

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Wire.setTimeOut(10);
  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    boardOk[b] = mcp[b].begin_I2C(boardAddr[b], &Wire);
    Serial.printf("Driver 0x%02X: %s\n", boardAddr[b], boardOk[b] ? "OK" : "NOT FOUND");
    if (!boardOk[b]) continue;
    for (uint8_t p = 0; p < 8; p++) { mcp[b].pinMode(solPin[p], OUTPUT); mcp[b].digitalWrite(solPin[p], LOW); }
  }
  if (boardOk[TRIG_BOARD]) {
    for (uint8_t p = 8; p < 16; p++) mcp[TRIG_BOARD].pinMode(p, INPUT_PULLUP);
    uint8_t g = mcp[TRIG_BOARD].readGPIOB();
    for (uint8_t i = 0; i < NUM_TRIGGERS; i++) { trigStable[i] = !((g >> triggers[i].bit) & 1); trigChangedAt[i] = millis(); }
  }

  for (uint8_t i = 0; i < NUM_CHIMES; i++) { trim[i] = 1.0f; firing[i] = false; onAt[i] = 0; }
  for (uint8_t i = 0; i < 5; i++) trim[i] = 1.15f;   // lowest tubes get a bit more

  MidiSerial.begin(31250, SERIAL_8N1, MIDI_RX_PIN, -1);

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(uiTask,  "ui",  8192,  nullptr, 1, nullptr, 0);
}

void loop() {
  serviceSol();
  while (MidiSerial.available()) midiByte((uint8_t)MidiSerial.read());
  serviceTriggers();
  serviceBootButton();

  NoteMsg m;
  while (xQueueReceive(netToChimes, &m, 0) == pdTRUE) { strike(m.note, m.vel); serviceSol(); }

  static bool wasMuted = false;
  if ((muted || updating) && !wasMuted) { allOff(); pendCount = 0; }
  wasMuted = muted || updating;

  serviceSol();
  uint32_t now = micros();
  for (uint8_t i = 0; i < NUM_CHIMES; i++) {
    if (firing[i] && (int32_t)(now - offAt[i]) >= 0) { setChime(i, false); firing[i] = false; }
  }
  servicePending();
  serviceSol();
}
