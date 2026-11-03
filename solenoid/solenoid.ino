// Solenoid tester: one MOSFET, one solenoid, any ESP32-S3 board.
// Flashed from the Ragamuffin Flasher (board "Solenoid"). Then join WiFi "Solenoid" (pw ragamuffin) -> 192.168.4.1
//
// Drive: GPIO -> logic-level MOSFET module gate (or a bare logic-level N-FET: 100 R series, 10k gate->GND).
//        Flyback diode across the coil (cathode to +V) is required: the hold mode PWMs the coil.
// Modes: strike = pulse at strike power %, length set by velocity (min..max ms), then off
//        hold   = full-power kick (0 = none), then PWM at hold % until released (auto-off after mhold s, 0 = never)
//        live   = output held at any duty 0-100 % from the page slider (same auto-off)
// Test bench: every setting is open (pulses to 2000 ms, duty 0-100 %, PWM 50 Hz-40 kHz) and applies the moment it
// changes. The heat limiter (400 ms burst, 300 ms/s average of strike/kick time) is a switch, on by default.
// Also: BOOT button = strike, USB serial commands (type ? for help), WiFi firmware update at /update.
// Network: always its own "Solenoid" AP (192.168.4.1); join the hall WiFi from the page -> http://solenoid.local,
//          http://solenoid or its IP on that network. Credentials live in NVS.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_timer.h"
#include "profile.h"          // struct Step (in a header so Arduino's auto-prototypes see it)

#if __has_include("version.h")
#include "version.h"                // CI writes FW_VERSION = the short commit SHA (same as sol/manifest.json)
#endif
#ifndef FW_VERSION
#define FW_VERSION  "dev"
#endif
#define LATEST_URL  "https://jobot101.github.io/Ragamuffin-Flasher/sol/"
#define AP_SSID     "Solenoid"
#define AP_PASS     "ragamuffin"
#define HOSTNAME    "solenoid"
#define HOME_SSID   "ClarkShop"   // preset network; its password is typed once on the page (NVS), never in the repo
#define BOOT_BTN    0
#define RES_BITS    10
#define FULL        1023          // ledcWrite turns max into 100 % on
#define BUCKET_MS   400.0f        // full-power ms that can go out back to back (24 V on a 12 V coil)
#define REFILL_MS_S 300.0f        // full-power ms per second on average (30 % duty at 24 V ~ 1.2x a 12 V coil's rating)
                                  // at 12 V (= coil rating) the limiter is off and hold can go to 100 %

struct Cfg {
  int pin = 4;
  bool inv = false;               // active-low driver
  int minMs = 3, maxMs = 25;      // strike pulse at velocity 1 / 127
  int kickMs = 30;                // hold: full-power pull-in
  int holdPct = 35;               // hold: PWM duty after the kick (35 % of 24 V ~ 8.4 V avg)
  int maxHoldS = 8;               // hold auto-release
  int hz = 20000;                 // PWM freq (opto-isolated modules: 1000)
  bool lim = true;                // heat limiter on strikes/kicks (for 12 V coils run at 24 V)
  int spPct = 100;                // strike power: PWM duty during a strike pulse
} cfg;

enum State { IDLE, STRIKE, KICK, HOLD, LIVE, PROFILE };
volatile State state = IDLE;
esp_timer_handle_t tmr;
Preferences prefs;
WebServer server(80);
DNSServer dns;

String staSsid, staPass;
uint8_t staWhy = 0;               // last disconnect reason
uint32_t staTry = 0;              // when the current join attempt started (0 = not trying)
uint32_t staNext = 0;             // next attempt
// paced joins: a station that retries nonstop hops channels and keeps knocking phones off the Solenoid AP
void staBegin() { staWhy = 0; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str()); staTry = millis() | 1; }
void staService() {
  if (!staSsid.length()) return;
  if (WiFi.status() == WL_CONNECTED) { staTry = 0; staNext = millis() + 45000; return; }
  if (staTry && millis() - staTry > 15000) { WiFi.disconnect(); staTry = 0; staNext = millis() + 45000; }  // give up for now
  else if (!staTry && (int32_t)(millis() - staNext) >= 0) staBegin();
}

float bucket = BUCKET_MS;
uint32_t lastRefill = 0, strikes = 0, clipped = 0;
int repLeft = 0, repV = 100;
bool repProf = false;             // repeat plays the profile instead of a strike

// Profiles: a hit is a list of steps "duty:ms" played back to back, e.g. "100:16,0:35,100:12" (a flam).
// "duty:h" holds that duty until STOP / auto-off (strike-and-damp). Max 32 steps, ms 1-5000.
Step prof[32]; int profN = 0, profI = 0;
String profStr = "100:20";
uint32_t repNext = 0, repGap = 250;

// ---------- output ----------
static inline void out(uint32_t d) { ledcWrite(cfg.pin, cfg.inv ? FULL - d : d); }
static inline void off() { out(0); }
static inline uint32_t pct(int p) { return (uint32_t)FULL * constrain(p, 0, 100) / 100; }
static inline uint32_t holdDuty() { return pct(cfg.holdPct); }
int livePct = 0;
static inline void autoOff() { if (cfg.maxHoldS > 0) esp_timer_start_once(tmr, (uint64_t)cfg.maxHoldS * 1000000ULL); }

void stepNow() {
  if (profI >= profN) { off(); state = IDLE; return; }
  const Step& st = prof[profI++];
  out(pct(st.d));
  if (st.hold) { state = HOLD; autoOff(); return; }
  esp_timer_start_once(tmr, (uint64_t)st.ms * 1000ULL);
}

void IRAM_ATTR onTimer(void*) {
  if (state == PROFILE) { stepNow(); return; }
  if (state == KICK) {            // pull-in done -> hold at reduced duty, arm the auto-release
    out(holdDuty()); state = HOLD;
    autoOff();
  } else { off(); state = IDLE; livePct = 0; } // strike over, or hold/live timed out
}

void refill() {
  uint32_t now = millis();
  if (!cfg.lim) { bucket = BUCKET_MS; lastRefill = now; return; }   // limiter off
  bucket = min(BUCKET_MS, bucket + (now - lastRefill) * REFILL_MS_S / 1000.0f);
  lastRefill = now;
}

int msForVel(int v) {
  v = constrain(v, 1, 127);
  return cfg.minMs + (cfg.maxMs - cfg.minMs) * (v - 1) / 126;
}

// returns ms actually fired (0 = skipped: too hot)
int strike(int vel) {
  refill();
  int ms = msForVel(vel);
  if (cfg.lim) {
    if (bucket < ms) { clipped++; ms = (int)bucket; if (ms < cfg.minMs) return 0; }
    bucket -= ms;
  }
  esp_timer_stop(tmr);
  state = STRIKE; out(pct(cfg.spPct)); strikes++;
  esp_timer_start_once(tmr, (uint64_t)ms * 1000ULL);
  return ms;
}

bool holdOn() {
  esp_timer_stop(tmr);
  if (cfg.kickMs <= 0) { out(holdDuty()); state = HOLD; autoOff(); return true; }   // no kick: straight to hold
  refill();
  if (cfg.lim) { if (bucket < cfg.kickMs) { clipped++; return false; } bucket -= cfg.kickMs; }
  state = KICK; out(FULL);
  esp_timer_start_once(tmr, (uint64_t)cfg.kickMs * 1000ULL);
  return true;
}

// live: hold the output at any duty until changed, STOP, or auto-off
void live(int p) {
  esp_timer_stop(tmr);
  livePct = constrain(p, 0, 100);
  if (!livePct) { off(); state = IDLE; return; }
  out(pct(livePct)); state = LIVE; autoOff();
}

bool parseProfile(const String& in, Step* o, int& n) {
  n = 0; int i = 0, L = in.length();
  while (i < L && n < 32) {
    int c = in.indexOf(':', i); if (c < 0) return false;
    int e = in.indexOf(',', c); if (e < 0) e = L;
    String d = in.substring(i, c), t = in.substring(c + 1, e); d.trim(); t.trim();
    if (!d.length() || !t.length()) return false;
    o[n].d = constrain(d.toInt(), 0, 100);
    o[n].hold = (t[0] == 'h' || t[0] == 'H');
    o[n].ms = o[n].hold ? 0 : constrain(t.toInt(), 1, 5000);
    if (!o[n].hold && t.toInt() < 1) return false;
    n++; i = e + 1;
    if (o[n - 1].hold) break;     // nothing plays after a hold
  }
  return n > 0;
}

bool setProfile(const String& in) {
  Step tmp[32]; int n;
  if (!parseProfile(in, tmp, n)) return false;
  memcpy(prof, tmp, sizeof tmp); profN = n; profStr = in;
  prefs.begin("sol", false); prefs.putString("prof", profStr); prefs.end();
  return true;
}

// returns false if the heat limiter says no
bool playProfile() {
  refill();
  float e = 0; for (int k = 0; k < profN; k++) if (!prof[k].hold) e += prof[k].d * prof[k].ms / 100.0f;
  if (cfg.lim) { if (e > bucket) { clipped++; return false; } bucket -= e; }
  esp_timer_stop(tmr);
  profI = 0; state = PROFILE; strikes++;
  stepNow();
  return true;
}

void release() { esp_timer_stop(tmr); off(); state = IDLE; livePct = 0; }
void stopAll() { repLeft = 0; release(); }

bool pinOk(int p) {   // S3: no strapping (0,3,45,46), USB (19,20), flash/PSRAM (26-37)
  return (p >= 1 && p <= 18 && p != 3) || p == 21 || (p >= 38 && p <= 44) || p == 47 || p == 48;
}

void attachPin(int oldPin) {
  if (oldPin >= 0) { ledcDetach(oldPin); pinMode(oldPin, INPUT_PULLDOWN); }
  if (!ledcAttach(cfg.pin, cfg.hz, RES_BITS)) {           // that frequency can't run at 10 bits: fall back
    Serial.printf("PWM %d Hz not possible, using 1000\n", cfg.hz); cfg.hz = 1000; ledcAttach(cfg.pin, cfg.hz, RES_BITS);
  }
  off();
}

// ---------- config ----------
void loadCfg() {
  prefs.begin("sol", true);
  cfg.pin = prefs.getInt("pin", cfg.pin);       cfg.inv = prefs.getBool("inv", cfg.inv);
  cfg.minMs = prefs.getInt("min", cfg.minMs);   cfg.maxMs = prefs.getInt("max", cfg.maxMs);
  cfg.kickMs = prefs.getInt("kick", cfg.kickMs); cfg.holdPct = prefs.getInt("hold", cfg.holdPct);
  cfg.maxHoldS = prefs.getInt("mhold", cfg.maxHoldS); cfg.hz = prefs.getInt("hz", cfg.hz);
  cfg.lim = prefs.getBool("lim", cfg.lim); cfg.spPct = prefs.getInt("sp", cfg.spPct);
  prefs.end();
  if (!pinOk(cfg.pin)) cfg.pin = 4;
}

void saveCfg() {
  prefs.begin("sol", false);
  prefs.putInt("pin", cfg.pin);     prefs.putBool("inv", cfg.inv);
  prefs.putInt("min", cfg.minMs);   prefs.putInt("max", cfg.maxMs);
  prefs.putInt("kick", cfg.kickMs); prefs.putInt("hold", cfg.holdPct);
  prefs.putInt("mhold", cfg.maxHoldS); prefs.putInt("hz", cfg.hz);
  prefs.putBool("lim", cfg.lim); prefs.putInt("sp", cfg.spPct);
  prefs.end();
}

String cfgJson() {
  refill();
  static const char* names[] = {"idle", "strike", "kick", "hold", "live", "profile"};
  char b[900];
  bool up = WiFi.status() == WL_CONNECTED;
  String ip = up ? WiFi.localIP().toString() : "";
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"pin\":%d,\"inv\":%d,\"min\":%d,\"max\":%d,\"kick\":%d,\"hold\":%d,\"mhold\":%d,\"hz\":%d,\"lim\":%d,\"sp\":%d,\"live\":%d,"
    "\"state\":\"%s\",\"heat\":%d,\"strikes\":%lu,\"clipped\":%lu,\"rep\":%d,\"prof\":\"%s\","
    "\"net\":{\"ssid\":\"%s\",\"up\":%d,\"ip\":\"%s\",\"why\":%d}}",
    FW_VERSION, cfg.pin, cfg.inv, cfg.minMs, cfg.maxMs, cfg.kickMs, cfg.holdPct, cfg.maxHoldS, cfg.hz, cfg.lim, cfg.spPct, livePct,
    names[state], (int)(100 - bucket * 100 / BUCKET_MS), (unsigned long)strikes, (unsigned long)clipped, repLeft, profStr.c_str(),
    staSsid.c_str(), up, ip.c_str(), up ? 0 : staWhy);
  return b;
}

String applyCfg(std::function<bool(const char*)> has, std::function<int(const char*)> get) {
  int oldPin = cfg.pin, oldHz = cfg.hz; bool oldInv = cfg.inv;
  if (has("pin")) { int p = get("pin"); if (!pinOk(p)) return "pin not allowed"; cfg.pin = p; }
  if (has("inv"))   cfg.inv = get("inv");
  if (has("min"))   cfg.minMs = constrain(get("min"), 1, 2000);
  if (has("max"))   cfg.maxMs = constrain(get("max"), 1, 2000);
  if (has("kick"))  cfg.kickMs = constrain(get("kick"), 0, 2000);
  if (has("lim"))   cfg.lim = get("lim");
  if (has("sp"))    cfg.spPct = constrain(get("sp"), 1, 100);
  if (has("hold"))  cfg.holdPct = get("hold");
  cfg.holdPct = constrain(cfg.holdPct, 0, 100);
  if (has("mhold")) cfg.maxHoldS = constrain(get("mhold"), 0, 600);
  if (has("hz"))    cfg.hz = constrain(get("hz"), 50, 40000);
  if (cfg.maxMs < cfg.minMs) cfg.maxMs = cfg.minMs;
  if (cfg.pin != oldPin || cfg.hz != oldHz || cfg.inv != oldInv) { stopAll(); attachPin(oldPin); }
  saveCfg();
  return "";
}

// ---------- web ----------
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Solenoid</title><style>
:root{--bg:#121212;--card:#1d1d1d;--fg:#eee;--dim:#999;--acc:#e8a33d;--bad:#e05a4f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 12px}h1 small{color:var(--dim);font-weight:400}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px}
button{font:inherit;border:0;border-radius:10px;padding:12px;background:#333;color:var(--fg);width:100%;touch-action:none;user-select:none;-webkit-user-select:none}
.big{font-size:22px;padding:26px;background:var(--acc);color:#111;font-weight:700}
.big:active,.held{filter:brightness(1.3)}.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}.pre{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px}.pre button{padding:10px 4px;font-size:14px}.pre .on{background:var(--acc);color:#111}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;color:var(--dim)}
input[type=range]{width:100%}input[type=number],select{width:90px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)}
#st{font:13px ui-monospace,monospace;color:var(--dim)}.heat{height:6px;background:#333;border-radius:3px;margin-top:6px}.heat div{height:100%;background:var(--bad);border-radius:3px;width:0}
</style></head><body><main>
<h1>Solenoid <small id="fw"></small></h1>
<div class="card"><button class="big" id="strike">STRIKE</button>
<label>velocity <b id="vv">100</b></label><input type="range" id="vel" min="1" max="127" value="100">
<div class="row"><button id="hold">HOLD (press)</button><button id="stop">STOP</button></div>
<div class="heat"><div id="heat"></div></div><div id="st">…</div></div>
<div class="card"><b>Live power</b> <small style="color:var(--dim)">drag to drive the coil at any duty; STOP or 0 = off</small>
<label>duty <b id="lv">0 %</b></label><input type="range" id="live" min="0" max="100" value="0">
<div class="row"><button id="l25">25 %</button><button id="l50">50 %</button><button id="l100">100 %</button><button id="l0">Off</button></div></div>
<div class="card"><b>Hit profiles</b> <small style="color:var(--dim)">tap one to load and fire it</small>
<div class="pre" id="pre"></div>
<p id="pdesc" style="color:var(--dim);font-size:13px;margin:8px 0 0;min-height:2.6em"></p>
<label>profile <input id="prof" style="width:200px;font:13px ui-monospace,monospace;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)"></label>
<label>stretch time % <input type="number" id="str" value="100" min="10" max="500"></label>
<div class="row"><button class="big" id="play" style="padding:16px;font-size:18px">PLAY</button></div>
<p style="color:var(--dim);font-size:12px;margin-top:8px">duty:ms steps, comma separated. 0:ms = gap. duty:h = hold until STOP. Stretch scales every step's time (shorter for 24 V, longer for weak coils).</p></div>
<div class="card"><b>Repeat</b>
<label>count <input type="number" id="n" value="16"></label><label>bpm <input type="number" id="bpm" value="240"></label>
<label>play the profile instead of a strike <input type="checkbox" id="rprof"></label>
<button id="rep">Go</button></div>
<div class="card"><b>Settings</b> <small id="saved" style="color:var(--dim)">changes apply right away</small>
<label>heat limiter (for 12 V coils on 24 V) <input type="checkbox" id="lim"></label>
<label>GPIO <select id="pin"></select></label>
<label>active low <input type="checkbox" id="inv"></label>
<label>strike min ms (1–2000) <input type="number" id="min" min="1" max="2000"></label><label>strike max ms (1–2000) <input type="number" id="max" min="1" max="2000"></label>
<label>strike power % <input type="number" id="sp" min="1" max="100"></label>
<label>hold kick ms (0 = none) <input type="number" id="kick" min="0" max="2000"></label><label>hold % (0–100) <input type="number" id="hold_" min="0" max="100"></label>
<label>auto-off s (0 = never) <input type="number" id="mhold" min="0" max="600"></label><label>PWM Hz (50–40000) <input type="number" id="hz" min="50" max="40000"></label>
<button id="save">Save all</button></div>
<div class="card"><b>Hall WiFi</b> <small id="nets"></small>
<label>network <input id="ssid" list="aps" style="width:160px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)"></label><datalist id="aps"></datalist>
<label>password <input id="wpass" type="password" style="width:160px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)"></label>
<div class="row"><button id="clark">ClarkShop</button><button id="scan">Scan</button></div>
<div class="row"><button id="join">Join</button><button id="forget">Forget</button></div></div>
<div class="card"><b>Firmware</b> <small id="fwv"></small>
<div class="row"><button id="latest">Install latest</button></div>
<p style="color:var(--dim);font-size:13px">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up).</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HOME=')HTML" HOME_SSID R"HTML(';
const $=id=>document.getElementById(id),go=u=>fetch(u).then(r=>r.json()).then(show).catch(()=>{});
const PINS=[1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,43,44,47,48];
PINS.forEach(p=>$('pin').add(new Option('GPIO '+p,p)));
let c={};
const WHY={2:'auth expired',15:'wrong password',201:'not found (2.4 GHz only)',202:'wrong password',203:'refused',204:'wrong password',205:'refused'};
function show(j){if(!j||j.err){$('st').textContent=j?j.err:'no reply';return}c=j;const n=j.net||{};
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · solenoid.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set — own WiFi only';
 $('fw').textContent=j.fw;$('heat').style.width=j.heat+'%';
 $('st').textContent=`GPIO ${j.pin} · ${j.state} · ${Math.round(j.min+(j.max-j.min)*($('vel').value-1)/126)} ms · strikes ${j.strikes} · heat-clipped ${j.clipped}${j.rep?' · repeat '+j.rep:''}`;
 $('rep').textContent=j.rep?'Stop':'Go'}
function fill(j){show(j);if(!$('prof').value)$('prof').value=j.prof;$('ssid').value=(j.net&&j.net.ssid)||HOME;$('fwv').textContent='running '+j.fw;$('lim').checked=!!j.lim;$('pin').value=j.pin;$('inv').checked=!!j.inv;
 ['min','max','kick','mhold','hz','sp'].forEach(k=>$(k).value=j[k]);$('hold_').value=j.hold}
const F={pin:'pin',inv:'inv',lim:'lim',min:'min',max:'max',sp:'sp',kick:'kick',hold_:'hold',mhold:'mhold',hz:'hz'};
const val=id=>$(id).type==='checkbox'?($(id).checked?1:0):$(id).value;
Object.keys(F).forEach(id=>$(id).addEventListener('change',()=>fetch(`/cfg?${F[id]}=${val(id)}`).then(r=>r.json()).then(j=>{
 if(j.err){$('saved').textContent=j.err;return}fill(j);$('saved').textContent='saved ✓';setTimeout(()=>$('saved').textContent='changes apply right away',1500)})));
let lt=0;const setLive=p=>{$('live').value=p;$('lv').textContent=p+' %';clearTimeout(lt);lt=setTimeout(()=>go('/d?p='+p),40)};
$('live').oninput=()=>setLive($('live').value);
[['l25',25],['l50',50],['l100',100],['l0',0]].forEach(([id,p])=>$(id).onclick=()=>setLive(p));
$('vel').oninput=()=>{$('vv').textContent=$('vel').value;show(c)};
$('strike').addEventListener('pointerdown',e=>{e.preventDefault();go('/s?v='+$('vel').value)});
const h=$('hold');
h.addEventListener('pointerdown',e=>{e.preventDefault();h.setPointerCapture(e.pointerId);h.classList.add('held');go('/h?on=1')});
['pointerup','pointercancel','lostpointercapture'].forEach(t=>h.addEventListener(t,()=>{if(h.classList.contains('held')){h.classList.remove('held');go('/h?on=0')}}));
$('stop').onclick=()=>{$('live').value=0;$('lv').textContent='0 %';go('/stop')};
$('scan').onclick=()=>{$('scan').textContent='scanning…';fetch('/scan').then(r=>r.json()).then(a=>{$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s)));$('scan').textContent=a.length+' found';if(a.length&&!$('ssid').value)$('ssid').value=a[0]}).catch(()=>$('scan').textContent='Scan')};
$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
$('join').onclick=()=>{if(!$('ssid').value)return;const q=new URLSearchParams({ssid:$('ssid').value,pass:$('wpass').value});go('/wifi?'+q)};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('rep').onclick=()=>go(c.rep?'/stop':`/r?n=${$('n').value}&bpm=${$('bpm').value}&v=${$('vel').value}&prof=${$('rprof').checked?1:0}`);
// presets: what a spring-return push-pull can do (times for a JF-0530B on 12 V; use stretch for other coils/voltages)
const PRESETS=[
 ['Tap','40:12','Soft touch: low power, short. Quiet notes and checking the strike point.'],
 ['Strike','100:20','A normal full hit, then off so the spring pulls the plunger back.'],
 ['Hammer','100:45','Long full-power hit. Loudest, but the plunger rests on the tube a moment, so it rings shorter.'],
 ['Ring','100:12','Fly-off hit: power cuts before impact so the plunger coasts, bounces off and the tube rings free.'],
 ['Flam','100:16,0:35,100:12','Two quick hits, like a drummer’s flam.'],
 ['Triple','100:14,0:30,100:12,0:30,100:10','Three hits, a little softer each time.'],
 ['Roll','100:10,0:35,100:10,0:35,100:10,0:35,100:10,0:35,100:10,0:35,100:10,0:35','Fast roll: six short hits. Raise stretch to slow it.'],
 ['Soft return','100:20,0:10,30:30,15:30','Hit, then a little pull during the return to cushion the spring slap. Quieter mechanism.'],
 ['Ramp','25:15,50:15,75:15,100:20','Soft-start push that builds up. Gentle contact, less clack.'],
 ['Strike + damp','100:20,0:400,100:30,40:h','Hit, let it ring 0.4 s, then pull back in and hold: the plunger mutes the tube. STOP releases.'],
 ['Buzz','100:6,0:8,100:6,0:8,100:6,0:8,100:6,0:8,100:6,0:8,100:6,0:8,100:6,0:8,100:6,0:8','Rapid flutter that rattles the tube. Tremolo / bell buzz.'],
 ['Full on','100:h','Straight full power until STOP or auto-off. For checking pull force and heat.'],
];
const stretch=(str,k)=>str.split(',').map(t=>{const[d,m]=t.split(':');return /^h/i.test(m)?t:d+':'+Math.max(1,Math.round(m*k/100))}).join(',');
const playP=(str)=>fetch('/play?s='+encodeURIComponent(stretch(str,$('str').value))).then(r=>r.json()).then(j=>{if(j.err){$('st').textContent=j.err;return}show(j)});
PRESETS.forEach(([n,str,d])=>{const b=document.createElement('button');b.textContent=n;b.onclick=()=>{
 document.querySelectorAll('.pre button').forEach(x=>x.classList.toggle('on',x===b));$('prof').value=str;$('pdesc').textContent=d;playP(str)};$('pre').append(b)});
$('play').addEventListener('pointerdown',e=>{e.preventDefault();playP($('prof').value)});
$('prof').addEventListener('change',()=>{document.querySelectorAll('.pre button').forEach(x=>x.classList.remove('on'));$('pdesc').textContent='Custom profile.'});
$('save').onclick=()=>go(`/cfg?lim=${val('lim')}&sp=${$('sp').value}&pin=${$('pin').value}&inv=${$('inv').checked?1:0}&min=${$('min').value}&max=${$('max').value}&kick=${$('kick').value}&hold=${$('hold_').value}&mhold=${$('mhold').value}&hz=${$('hz').value}`).then(()=>fetch('/state').then(r=>r.json()).then(fill));
function send(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'solenoid.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),5000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)send(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');try{
 b.textContent='checking…';const m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json();
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'solenoid.bin',{cache:'no-store'});if(!r.ok)throw 0;
 send(await r.blob(),b)}catch(e){b.textContent='no internet — mobile data on, open in Chrome'}};
fetch('/state').then(r=>r.json()).then(fill);setInterval(()=>go('/state'),700);
</script></main></body></html>)HTML";

void sendState() { server.send(200, "application/json", cfgJson()); }
int arg(const char* k, int d) { return server.hasArg(k) ? server.arg(k).toInt() : d; }

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/state", sendState);
  server.on("/s", [] { strike(arg("v", 100)); sendState(); });
  server.on("/h", [] { if (arg("on", 0)) holdOn(); else release(); sendState(); });
  server.on("/d", [] { live(arg("p", 0)); sendState(); });
  server.on("/play", [] {           // /play?s=<profile> sets + plays; /play alone replays the current one
    if (server.hasArg("s") && !setProfile(server.arg("s"))) { server.send(400, "application/json", "{\"err\":\"bad profile: use duty:ms,duty:ms (duty 0-100, ms 1-5000, or duty:h to hold)\"}"); return; }
    if (!server.hasArg("set")) playProfile();
    sendState();
  });
  server.on("/stop", [] { stopAll(); sendState(); });
  server.on("/r", [] {
    repV = arg("v", 100); repGap = 60000 / constrain(arg("bpm", 240), 1, 3000);
    repProf = arg("prof", 0); repLeft = constrain(arg("n", 16), 1, 10000); repNext = millis(); sendState();
  });
  server.on("/cfg", [] {
    String e = applyCfg([](const char* k) { return server.hasArg(k); }, [](const char* k) { return (int)server.arg(k).toInt(); });
    if (e.length()) server.send(400, "application/json", "{\"err\":\"" + e + "\"}"); else sendState();
  });
  server.on("/scan", [] {
    int n = WiFi.scanNetworks();
    String j = "[";
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i); if (!s.length() || j.indexOf("\"" + s + "\"") >= 0) continue;
      s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
      if (j.length() > 1) j += ",";
      j += "\"" + s + "\"";
    }
    WiFi.scanDelete();
    server.send(200, "application/json", j + "]");
  });
  server.on("/wifi", [] {
    if (server.hasArg("forget")) { staSsid = staPass = ""; WiFi.disconnect(false, true); }
    else { staSsid = server.arg("ssid"); staPass = server.arg("pass"); }
    prefs.begin("sol", false); prefs.putString("ssid", staSsid); prefs.putString("wpass", staPass); prefs.end();
    sendState();
    if (staSsid.length()) staBegin(); else staTry = 0;
  });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "ok" : "update failed");
    if (ok) { delay(300); ESP.restart(); }
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { stopAll(); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) Update.end(true);
  });
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });  // captive portal
  server.begin();
}

// ---------- serial ----------
void serialCmd() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim(); if (!line.length()) continue;
    char c = line[0]; int n = line.length() > 1 ? line.substring(1).toInt() : -1;
    if (c == 's') Serial.printf("strike %d ms\n", strike(n > 0 ? n : 100));
    else if (c == 'h') Serial.println(holdOn() ? "hold" : "too hot");
    else if (c == 'r') { release(); Serial.println("released"); }
    else if (c == 'x') { stopAll(); Serial.println("stopped"); }
    else if (c == 'P') { String a = line.substring(1); a.trim();
      if (a.length() && !setProfile(a)) Serial.println("bad profile"); else Serial.println(playProfile() ? "play " + profStr : "too hot"); }
    else if (c == 'd') { live(n < 0 ? 0 : n); Serial.printf("live %d %%\n", livePct); }
    else if (c == 'p') {
      String e = applyCfg([](const char* k) { return !strcmp(k, "pin"); }, [n](const char*) { return n; });
      Serial.println(e.length() ? e : "pin " + String(cfg.pin));
    } else Serial.println(cfgJson() + "\ns<vel> strike · h hold · r release · x stop · d<pct> live duty · P<profile> play · p<gpio> set pin · ? state");
    line = "";
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  loadCfg();
  prefs.begin("sol", true); String sp = prefs.getString("prof", profStr); prefs.end();
  if (!parseProfile(sp, prof, profN)) { profStr = "100:20"; parseProfile(profStr, prof, profN); } else profStr = sp;
  attachPin(-1);
  esp_timer_create_args_t ta = {}; ta.callback = onTimer; ta.name = "sol";
  esp_timer_create(&ta, &tmr);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  lastRefill = millis();

  prefs.begin("sol", true); staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("wpass", ""); prefs.end();
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t i) { staWhy = i.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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
  Serial.printf("\n%s  solenoid on GPIO %d  WiFi \"%s\" -> http://%s\n", FW_VERSION, cfg.pin, AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  serialCmd();
  staService();

  static bool was = true; static uint32_t tDown = 0;      // BOOT button: strike at velocity 100
  bool up = digitalRead(BOOT_BTN);
  if (!up && was && millis() - tDown > 120) { tDown = millis(); strike(100); }
  was = up;

  if (repLeft > 0 && (int32_t)(millis() - repNext) >= 0) { if (repProf) playProfile(); else strike(repV); repLeft--; repNext += repGap; }
  delay(1);
}
