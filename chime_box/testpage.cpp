// testpage.cpp: the "Test all solenoids" page, served at /test. One button cycles ONCE through every connected
// output (enabled, on a driver board that answers, allowed by solo) using the walk in cal.cpp, and ticks each one off
// as its strike counter moves. Ragamuffin Workshop look (BRAND-UI.md). Works offline on the BOX's own WiFi.
// Uses only existing endpoints: /status, /outs, /walk[?v=&gap=], /calstop, /panic, /mute.
#include "chimes.h"

const char TEST_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Test all solenoids</title><style>
:root{--bg:#0c0b0a;--paper:#151311;--paper2:#1d1a17;--line:#2c2823;--ink:#efe7d6;--muted:#a89f8e;--gold:#c49a45;
--goldsoft:#2a2213;--goldink:#17120a;--ok:#8fbf7a;--warn:#e0b45c;--bad:#e0705c;--badsoft:#2b1512;--blue:#60b6e5;
--serif:"Cormorant Garamond",Georgia,"Times New Roman",serif;--sans:"DM Sans",system-ui,"Segoe UI",sans-serif;
--mono:ui-monospace,"Cascadia Mono",Consolas,monospace}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px var(--sans)}
header{position:sticky;top:0;z-index:5;background:var(--bg);border-bottom:1px solid var(--line);padding:10px 16px}
.top{display:flex;gap:10px;align-items:center;max-width:520px;margin:0 auto}
.brand{flex:1}.brand small{display:block;font:600 11px var(--serif);letter-spacing:.22em;color:var(--muted)}
.brand b{font:700 22px var(--serif)}
main{max-width:520px;margin:0 auto;padding:12px 16px 40px}
.panel{background:var(--paper);border:1px solid var(--line);border-radius:6px;padding:14px;margin-bottom:12px}
.panel h2{margin:0 0 10px;font:600 12px var(--serif);letter-spacing:.2em;text-transform:uppercase;color:var(--muted)}
button{font:inherit;min-height:44px;border-radius:6px;border:1px solid var(--line);background:var(--paper2);color:var(--ink);
 padding:10px 14px;touch-action:manipulation}
button.pri{background:var(--gold);color:var(--goldink);border-color:var(--gold);font-weight:700}
button:disabled{opacity:.45}
#panic{background:var(--bad);border-color:var(--bad);color:#17120a;font:800 18px var(--sans);letter-spacing:.12em;padding:12px 18px}
#go{width:100%;font-size:20px;padding:16px}
.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
label{display:flex;justify-content:space-between;align-items:center;gap:10px;margin:8px 0;color:var(--muted)}
input[type=range]{flex:1}
.mono,#msg,#sum{font:13px var(--mono);font-variant-numeric:tabular-nums}
#msg{color:var(--muted);margin-top:10px;min-height:1.3em}
.warn{background:var(--badsoft);border:1px solid var(--bad);color:var(--ink);border-radius:6px;padding:10px;margin-bottom:12px;display:none}
.bar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:10px}.bar i{display:block;height:100%;width:0;background:var(--gold)}
ul{list-style:none;margin:0;padding:0}
li{display:flex;align-items:center;gap:10px;padding:8px 2px;border-top:1px solid var(--line);font-size:15px}
li:first-child{border-top:0}
.lamp{width:10px;height:10px;border-radius:50%;background:var(--line);flex:none}
.n{font:13px var(--mono);color:var(--muted);width:2.4em;text-align:right}
.nm{flex:1}.st{font:12px var(--mono);color:var(--muted)}
li.wait .lamp{background:var(--line)} li.now .lamp{background:var(--gold);box-shadow:0 0 0 3px var(--goldsoft)}
li.ok .lamp{background:var(--ok)} li.ok .st{color:var(--ok)}
li.miss .lamp{background:var(--bad)} li.miss .st{color:var(--bad)}
li.skip{opacity:.5}
a{color:var(--gold)}
</style></head><body>
<header><div class="top"><div class="brand"><small>RAGAMUFFIN WORKSHOP</small><b>Test all solenoids</b></div>
<button id="panic">PANIC</button></div></header>
<main>
<div class="warn" id="warn"></div>
<div class="panel"><h2>Cycle once</h2>
<p style="margin:0 0 10px;color:var(--muted);font-size:14px">Strikes every connected solenoid one at a time, lowest number
first, then stops. Listen for each one, and watch it tick green here.</p>
<label>strength <b class="mono" id="vv">100</b><input type="range" id="vel" min="20" max="127" value="100"></label>
<label>gap between <b class="mono" id="gv">450 ms</b><input type="range" id="gap" min="250" max="1500" step="50" value="450"></label>
<button class="pri" id="go">Cycle once</button>
<div class="row"><button id="stop">Stop</button><button id="unmute" style="display:none">Unmute</button></div>
<div class="bar"><i id="prog"></i></div><div id="msg"></div></div>
<div class="panel"><h2>Connected outputs</h2><div id="sum" style="margin-bottom:8px"></div><ul id="list"></ul></div>
<p style="font-size:13px;color:var(--muted)"><a href="/">Back to the Hall chimes page</a></p>
</main>
<script>
const $=id=>document.getElementById(id);
let outs=[],st=null,base=null,running=false,expect=[],done={},lastSt={};
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const KIND=['off','tube','small solenoid','other'];
const NAMES=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'],nm=n=>n?NAMES[n%12]+(Math.floor(n/12)-1):'';
function usable(i){if(!st)return false;const o=outs[i];if(!o||!o.k)return false;const b=st.boards[Math.floor(i/8)];
 if(!b||b.s!==1)return false;const solo=st.cfg&&st.cfg.solo;return !solo||solo===i+1}
function draw(){const L=$('list');if(!st||!outs.length){L.innerHTML='<li>loading...</li>';return}
 const rows=[];let n=0;
 for(let i=0;i<outs.length;i++){const o=outs[i];const b=st.boards[Math.floor(i/8)];
  if(!o.k&&!(b&&b.s===1&&i<25))continue;               // show enabled outputs, plus the tubes on boards that answer
  const u=usable(i);if(u)n++;
  let cls=u?'wait':'skip',txt=u?'ready':(!o.k?'off':(!b||b.s!==1?'no board':'solo blocks'));
  if(done[i]==='ok'){cls='ok';txt='struck'}else if(done[i]==='miss'){cls='miss';txt='no strike'}
  if(running&&st.live.on[i])cls='now';
  rows.push(`<li class="${cls}"><span class="lamp"></span><span class="n">${i+1}</span><span class="nm">${esc(o.n||('out '+(i+1)))}
   <span class="st">${KIND[o.k]||''} ${nm(o.no)} 0x${(32+Math.floor(i/8)).toString(16)}.${i%8}</span></span><span class="st">${txt}</span></li>`)}
 L.innerHTML=rows.join('')||'<li>no outputs yet: set them up on the Outputs tab</li>';
 const ok=Object.values(done).filter(v=>v==='ok').length,miss=Object.values(done).filter(v=>v==='miss').length;
 $('sum').textContent=`${n} connected · ${ok} struck${miss?' · '+miss+' no strike':''}`;
 $('prog').style.width=expect.length?Math.round(100*(ok+miss)/expect.length)+'%':'0'}
function warn(){const w=[];if(st.muted)w.push('The chimes are muted: unmute to test.');
 if(st.power&&st.power.ok===0)w.push('No 24 V: plug in the power supply.');
 if(st.cfg&&st.cfg.solo)w.push(`Solo is on: only output ${st.cfg.solo} can strike (Settings on the main page).`);
 if(!st.boards.some(b=>b.s===1))w.push('No driver board answers: check the STEMMA QT cable and 3.3 V.');
 $('warn').innerHTML=w.map(esc).join('<br>');$('warn').style.display=w.length?'block':'none';
 $('unmute').style.display=st.muted?'block':'none';$('go').disabled=running||st.muted||!expect.length&&!outs.some((_,i)=>usable(i))}
function poll(){fetch('/status').then(r=>r.json()).then(j=>{st=j;
  if(running){const s=j.live.strikes;
   for(const i of expect){if(!done[i]&&s[i]>base[i])done[i]='ok'}
   const walking=j.cal&&j.cal.state===4;
   if(!walking&&lastSt.walking){running=false;for(const i of expect)if(!done[i])done[i]='miss';
    const ok=expect.filter(i=>done[i]==='ok').length;$('msg').textContent=`Done: ${ok} of ${expect.length} struck.`+
     (ok<expect.length?' The red ones were sent but did not strike (too hot, board fault or no power). See Status on the main page.':'')}
   else if(walking)$('msg').textContent=j.cal.msg||'cycling...';
   lastSt.walking=walking}
  warn();draw()}).catch(()=>{$('msg').textContent='no reply from the chimes'})}
function loadOuts(){return fetch('/outs').then(r=>r.json()).then(j=>{outs=j.outs||[];draw()})}
$('vel').oninput=()=>$('vv').textContent=$('vel').value;
$('gap').oninput=()=>$('gv').textContent=$('gap').value+' ms';
$('go').onclick=()=>{if(!st)return;expect=[];done={};for(let i=0;i<outs.length;i++)if(usable(i))expect.push(i);
 if(!expect.length){$('msg').textContent='Nothing connected to test.';return}
 base=st.live.strikes.slice();running=true;lastSt.walking=true;$('msg').textContent=`Cycling through ${expect.length}...`;draw();
 fetch(`/walk?v=${$('vel').value}&gap=${$('gap').value}`).then(r=>{if(r.status===409){running=false;$('msg').textContent='Busy: a sweep, tune or walk is already running. Press Stop first.'}})
  .catch(()=>{running=false;done={};$('msg').textContent='Could not start: no reply from the chimes. Check the WiFi and try again.';draw()})};
$('stop').onclick=()=>fetch('/calstop').then(()=>{$('msg').textContent='Stopped.'});
$('panic').onclick=()=>{running=false;fetch('/panic')};
$('unmute').onclick=()=>fetch('/mute?on=0');
loadOuts().then(poll);setInterval(poll,300);setInterval(loadOuts,10000);
</script></body></html>)HTML";
