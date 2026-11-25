// The window lights' phone page, in a header so Arduino's prototype scan never reads the JavaScript as C.
#pragma once
// ---------- web ----------
// Page = the Ragamuffin Hall logo (brand/BRAND.md): white paper, black ink outlines, flat glass colours, the rose window.
// Fonts come from Google when the phone has internet; on the board's own WiFi it falls back to system faces.
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#ffffff"><title>Window lights</title>
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
<header><svg viewBox="-102 -102 204 204" class="rose"><circle r="101" fill="#ffffff"/><circle class="dash-b" r="95.5" fill="none" stroke="#60b6e5" stroke-width="6"/><circle class="dash-r" r="95.5" fill="none" stroke="#c62b29" stroke-width="6" stroke-dasharray="9.38 9.38"/><circle r="92.3" fill="none" stroke="#060709" stroke-width="1.5"/><circle r="98.7" fill="none" stroke="#060709" stroke-width="1.5"/><circle r="87.5" fill="none" stroke="#060709" stroke-width="2.4"/><g class="petals" stroke="#060709" stroke-width="2.4" stroke-linejoin="round"><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(11.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(33.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(56.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(78.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(101.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(123.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(146.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(168.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(191.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(213.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(236.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(258.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#fdcd09" transform="rotate(281.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#8dad3e" transform="rotate(303.75)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#60b6e5" transform="rotate(326.25)"/><path class="petal" d="M -5.79 -36.54 L -10.79 -68.15 A 4.3 4.3 0 0 1 -5.07 -74.5 A 6.1 6.1 0 1 1 5.07 -74.5 A 4.3 4.3 0 0 1 10.79 -68.15 L 5.79 -36.54 A 37 37 0 0 0 -5.79 -36.54 Z" fill="#c62b29" transform="rotate(348.75)"/></g><circle r="34.5" fill="#ffffff" stroke="#060709" stroke-width="2.4"/><g class="hub"><path d="M 0 0 L -2.4 -22.87 A 23 23 0 0 1 21.01 -9.35 Z" fill="#fdcd09"/><path d="M 0 0 L 21.01 -9.35 A 23 23 0 0 1 15.39 17.09 Z" fill="#8dad3e"/><path d="M 0 0 L 15.39 17.09 A 23 23 0 0 1 -11.5 19.92 Z" fill="#60b6e5"/><path d="M 0 0 L -11.5 19.92 A 23 23 0 0 1 -22.5 -4.78 Z" fill="#c62b29"/><path d="M 0 0 L -22.5 -4.78 A 23 23 0 0 1 -2.4 -22.87 Z" fill="#60b6e5"/><path d="M -2.4 -22.87 L 4.75 -8.23 L 21.01 -9.35 L 9.29 1.98 L 15.39 17.09 L 0.99 9.45 L -11.5 19.92 L -8.68 3.86 L -22.5 -4.78 L -6.36 -7.06 Z" fill="#ffffff" stroke="#060709" stroke-width="1.7" stroke-linejoin="miter"/><path d="M -0.68 -6.46 L 1.35 -2.34 L 5.94 -2.64 L 2.64 0.56 L 4.35 4.83 L 0.28 2.69 L -3.25 5.63 L -2.47 1.1 L -6.36 -1.35 L -1.81 -2.01 Z" fill="#060709"/><circle r="23" fill="none" stroke="#060709" stroke-width="2.4"/></g></svg><h1 class="word">RAGAMUFFIN</h1><p class="sub"><span class="rule"></span>WINDOW LIGHTS<span class="rule r"></span></p><p id="fw"></p></header>
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
<label>rings <input id="rings" placeholder="e.g. 40, 150, 230r, 420"></label>
<label>sync rings in Test <select id="sync"><option value="1">on</option><option value="0">off</option></select></label>
<div class="row"><button id="show">Show rings (20 s)</button></div>
<label>pixels on GPIO 5 <input type="number" id="nB" min="0" max="4000" placeholder="0 = off"></label>
<label>colour order <select id="ord"></select></label>
<label>GPIO 5 level % <input type="number" id="trim" min="1" max="100"></label>
<label>fade ms <input type="number" id="fade" min="0" max="5000"></label>
<label>supply limit mA <input type="number" id="lim" min="0" max="200000" placeholder="0 = none"></label>
<label>mA per pixel, full white <input type="number" id="pma" min="1" max="500"></label>
<div class="row"><button id="scfg">Save</button></div>
<p class="hint">Rings = pixels per ring in wiring order (the feed spoke counts as one). Tap Show rings: each ring lights red / yellow / blue / green with a white pixel at its start — adjust until colours change exactly at the joints. Add r to a ring that runs the other way. Whatever is left over is one more ring.
Pixels = chips, not LEDs: WS2811 neon often has 3 LEDs per chip. Changing a pixel count restarts the board.
Colour order: pick Test, then tap Red — if the ring shows green or blue, try the next order.
mA per pixel = reel W/m ÷ volts ÷ pixels per metre × 1000 (WS2812B 5 V ≈ 60).</p>
<p class="hint" id="saved"></p></div>
<div class="card" id="wifi"><h2>Hall WiFi <small id="nets"></small></h2>
<label>network <input id="ssid" list="aps"></label><datalist id="aps"></datalist>
<label>password <input id="wpass" type="password"></label>
<div class="row"><button id="hall">STARLINK</button><button id="clark">ClarkShop</button><button id="scan">Scan</button></div>
<div class="row"><button id="join">Join</button><button id="forget">Forget</button></div></div>
<div class="card"><h2>MQTT <small id="mq"></small></h2>
<label>broker <input id="mhost" placeholder="blank = off"></label><label>port <input id="mport" type="number"></label>
<label>user <input id="muser"></label><label>password <input id="mpass" type="password" placeholder="unchanged"></label>
<div class="row"><button id="msave">Save</button></div>
<p class="hint">Send to <b>ragamuffin/hall/window/set</b>: <code>c=ff9329&amp;b=30</code> · <code>b=0</code> · <code>test=1</code>. State is on <b>…/state</b>.</p></div>
<div class="card"><h2>Firmware <small id="fwv"></small></h2>
<div class="row"><button id="latest">Install latest</button></div>
<p class="hint">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up).</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<footer>Ragamuffin Studios, McCoysville, Pennsylvania</footer>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HALL=')HTML" HALL_SSID R"HTML(',HOME=')HTML" HOME_SSID R"HTML(';
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
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · hall-window.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set, own WiFi only';
 $('mq').textContent=m.host?(m.up?'connected':'connecting…'):'off'}
function fill(j){if(!(j.net&&j.net.up)&&!fill.done){const w=$('wifi');document.querySelector('.dash').after(w)}fill.done=1;show(j);['nA','nB','ord','trim','fade','lim','pma','rings','sync'].forEach(k=>$(k).value=j[k]);$('ssid').value=(j.net&&j.net.ssid)||HALL;
 $('mhost').value=j.mqtt.host;$('mport').value=j.mqtt.port;$('muser').value=j.mqtt.user}
let tq=0,q={};const send=o=>{Object.assign(q,o);busy=Date.now();clearTimeout(tq);tq=setTimeout(()=>{const s=new URLSearchParams(q);q={};go('/set?'+s)},60)};
const setC=h=>{$('col').value='#'+h;$('hex').textContent='#'+h;$('sw').style.background='#'+h;send({c:h})};
$('col').oninput=()=>setC($('col').value.slice(1));
const setB=v=>{$('bri').value=v;$('bv').textContent=v+' %';send({b:v})};
$('bri').oninput=()=>setB($('bri').value);
document.querySelectorAll('[data-b]').forEach(b=>b.onclick=()=>setB(b.dataset.b));
$('test').onclick=()=>send({test:c.test?0:1});
$('show').onclick=()=>fetch('/cfg?'+new URLSearchParams({rings:$('rings').value,show:1})).then(j2).then(show);
$('scfg').onclick=()=>{const s=new URLSearchParams();['nA','nB','ord','trim','fade','lim','pma'].forEach(k=>s.set(k,$(k).value||0));s.set('rings',$('rings').value);s.set('sync',$('sync').value);
 fetch('/cfg?'+s).then(j2).then(j=>{if(j.err){$('saved').textContent=j.err;return}
 $('saved').textContent=j.restart?'Saved. Restarting with the new pixel count…':'Saved ✓';if(j.restart)setTimeout(()=>location.reload(),6000);else fill(j)})};
$('scan').onclick=()=>{$('scan').textContent='scanning…';fetch('/scan').then(j2).then(a=>{$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s)));$('scan').textContent=a.length+' found'}).catch(()=>$('scan').textContent='Scan')};
$('hall').onclick=()=>{$('ssid').value=HALL;$('wpass').focus()};
$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
$('join').onclick=()=>{if(!$('ssid').value)return;go('/wifi?'+new URLSearchParams({ssid:$('ssid').value,pass:$('wpass').value}))};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('msave').onclick=()=>{const s=new URLSearchParams({host:$('mhost').value,port:$('mport').value||1883,user:$('muser').value});
 if($('mpass').value)s.set('pass',$('mpass').value);go('/mqtt?'+s);$('mpass').value=''};
function up(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'window.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),6000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)up(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');try{
 b.textContent='checking…';const m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json();
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'window.bin',{cache:'no-store'});if(!r.ok)throw 0;
 up(await r.blob(),b)}catch(e){b.textContent='no internet — mobile data on, open in Chrome'}};
fetch('/state').then(j2).then(fill);setInterval(()=>{if(Date.now()-busy>1500)busy=0;go('/state')},1500);
</script></main></body></html>)HTML";
