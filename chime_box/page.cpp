// page.cpp: the phone page, the main way to play, set up and program the Hall chimes (the BOX sits in the control box).
// One PROGMEM page, no outside files, so it works on the own WiFi with no internet. It only calls the endpoints in
// DESIGN.md; every strike still goes through core 1 and its caps. HOME_SSID and LATEST_URL come from chimes.h.
#include "chimes.h"

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Hall chimes</title><style>
/* Ragamuffin Workshop tokens (see BRAND-UI.md); no web fonts, the stacks fall back offline */
:root{--bg:#0c0b0a;--paper:#151311;--paper-2:#1d1a17;--line:#2c2823;--rule:#4a4238;--ink:#efe7d6;--muted:#a89f8e;
--gold:#c49a45;--gold-ink:#17120a;--gold-soft:#2a2213;--ok:#8fbf7a;--warn:#e0b45c;--bad:#e0705c;--bad-soft:#2b1512;
--red:#c62b29;--yel:#fdcd09;--blu:#60b6e5;--grn:#8dad3e;--v1:#5c4724;--v2:#8f6f35;--v3:#d9ad55;
--f-serif:"Cormorant Garamond",Georgia,"Times New Roman",serif;--f-ui:"DM Sans",system-ui,"Segoe UI",sans-serif;
--f-mono:ui-monospace,"Cascadia Mono",Consolas,monospace;
--acc:var(--gold);--dim:var(--muted);--fg:var(--ink);--card:var(--paper);--ln:var(--line);color-scheme:dark}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.4 var(--f-ui);-webkit-tap-highlight-color:transparent}
main{max-width:520px;margin:0 auto;padding:0 12px 24px}
header{position:sticky;top:0;z-index:5;background:var(--bg);padding:8px 0 0;border-bottom:1px solid var(--line);margin-bottom:12px}
.brand{display:flex;align-items:center;gap:10px}#emb svg{width:36px;height:36px;display:block}
.ws{font:600 11px/1.2 var(--f-serif);letter-spacing:.24em;color:var(--muted)}
h1{font:600 25px/1.05 var(--f-serif);margin:0;letter-spacing:.02em}
h1 small{font:12px var(--f-mono);color:var(--muted);margin-left:6px;letter-spacing:0}
h2{font:600 13px/1.2 var(--f-serif);letter-spacing:.2em;text-transform:uppercase;color:var(--muted);margin:0 0 8px;padding-bottom:7px;border-bottom:1px solid var(--line)}
h2 small{letter-spacing:.04em;text-transform:none;font-family:var(--f-ui)}
.card{background:var(--paper);border:1px solid var(--line);border-radius:6px;padding:12px 14px;margin-bottom:12px}
button{font:inherit;border:1px solid var(--line);border-radius:6px;padding:10px 12px;background:var(--paper-2);color:var(--ink);width:100%;min-height:44px;touch-action:manipulation;cursor:pointer}
a.btn{display:flex;align-items:center;justify-content:center;border:1px solid var(--line);border-radius:6px;padding:10px 12px;background:var(--paper-2);color:var(--ink);min-height:44px;text-decoration:none;text-align:center}
button:active,a.btn:active{border-color:var(--gold)}button:disabled{opacity:.4}a.btn:focus-visible{outline:1px solid var(--gold);outline-offset:1px}
button:focus-visible,input:focus-visible,select:focus-visible{outline:1px solid var(--gold);outline-offset:1px}
.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
label{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:8px 0;color:var(--muted);font-size:15px}
label b{font:600 15px var(--f-mono);color:var(--ink)}
input[type=range]{width:100%;accent-color:var(--gold)}
label input,label select{width:160px;font:inherit;padding:8px;min-height:40px;border-radius:6px;border:1px solid var(--line);background:var(--paper-2);color:var(--ink)}
input[type=number]{font-family:var(--f-mono)}
label input[type=checkbox]{width:24px;height:24px;min-height:0;accent-color:var(--gold)}
input[type=file]{width:100%;margin-top:8px;color:var(--muted);font-size:14px}
.acc{background:var(--gold);border-color:var(--gold);color:var(--gold-ink);font-weight:700}
#panic{background:var(--bad);border-color:var(--bad);color:#1a0b08;font:800 24px var(--f-ui);letter-spacing:.16em;flex:2;min-height:58px}
#mute{background:transparent;border-color:var(--muted);display:flex;align-items:center;justify-content:center;gap:8px}
#mute::before{content:"";width:9px;height:9px;border-radius:50%;background:var(--line);flex:none}
#mute.mute{background:var(--bad-soft);border-color:var(--bad);color:var(--bad);font-weight:700}#mute.mute::before{background:var(--bad)}
.mono,#sum{font:13px/1.45 var(--f-mono);color:var(--muted)}
.mono,#sum,#tres,label b,.st,.oh b,table.t{font-variant-numeric:tabular-nums}
.stl{display:flex;align-items:flex-start;gap:8px;margin-top:8px}.stl>.lamp{margin-top:5px}.stl #sum{flex:1;min-width:0}.lm{flex:none;font-size:11px;letter-spacing:.1em;line-height:19px}
#msg{color:var(--gold);font-size:14px;min-height:0}
.lamp,.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--line);flex:none;vertical-align:middle}
.lamp.ok{background:var(--ok)}.lamp.gold{background:var(--gold)}.lamp.warn{background:var(--yel)}.lamp.bad{background:var(--bad)}.lamp.blue{background:var(--blu)}
.lrow{display:flex;align-items:center;gap:8px}td .lamp{margin-right:6px}
nav{display:flex;gap:4px;overflow-x:auto;margin-top:8px;scrollbar-width:none}
nav button{width:auto;flex:none;padding:8px 12px;min-height:44px;font-size:14px;color:var(--muted);border-radius:4px 4px 0 0;border-bottom:0;border-top:2px solid var(--line)}
nav button.sel{color:var(--gold);background:var(--gold-soft);border-top-color:var(--gold)}.tab{display:none}.tab.sel{display:block}
.note{color:var(--muted);font-size:13px}
#keys{display:grid;grid-template-columns:repeat(auto-fill,minmax(62px,1fr));gap:6px;margin-top:8px}
#keys button{padding:8px 2px;font-size:14px;line-height:1.2;overflow:hidden}#keys button.s{background:var(--bg)}
#keys button.on{background:var(--gold);border-color:var(--gold);color:var(--gold-ink)}
#keys small{display:block;color:var(--muted);font:12px var(--f-mono)}#keys .on small{color:var(--gold-ink)}
#songs button{margin-top:6px;text-align:left}
.o{border-top:1px solid var(--line);padding:4px 0}.oh{display:flex;align-items:center;gap:8px;min-height:44px;cursor:pointer}
.oh b{width:26px;flex:none;font:600 13px var(--f-mono);color:var(--muted);text-align:right}
.nm{flex:1;min-width:0}
.o .dot{background:var(--ok)}.o.off .dot,.o.nob .dot{background:var(--line)}.o.off{color:var(--muted)}.o.warm .dot{background:var(--yel)}
.o.fire .dot{background:var(--gold)}.o.hot .dot,.o.flt .dot{background:var(--bad)}
.bar{height:5px;width:54px;flex:none;overflow:hidden;background:repeating-linear-gradient(90deg,var(--line) 0 4px,transparent 4px 6px)}
.bar i{display:block;height:100%;width:0;background:repeating-linear-gradient(90deg,var(--gold) 0 4px,transparent 4px 6px)}
.o.hot .bar i{background:repeating-linear-gradient(90deg,var(--bad) 0 4px,transparent 4px 6px)}
.st{width:84px;text-align:right;flex:none;font-size:12px}.o.flt .st,.o.hot .st{color:var(--bad)}
.meter{height:6px;margin:8px 0 4px;overflow:hidden;background:repeating-linear-gradient(90deg,var(--line) 0 8px,transparent 8px 10px)}
.meter i{display:block;height:100%;width:0;background:repeating-linear-gradient(90deg,var(--red) 0 8px,transparent 8px 10px,var(--blu) 10px 18px,transparent 18px 20px)}
.ed{padding:4px 0 8px}
#grid{overflow-x:auto;margin-top:10px}.g{border-collapse:collapse}.g td{padding:0;border:1px solid var(--line)}.g td.b{border-left:2px solid var(--rule)}
.g th{position:sticky;left:0;background:var(--paper);z-index:1;padding:0 4px 0 0}
.g select{width:100px}
.c{display:block;width:30px;height:36px;min-height:0;padding:0;border:0;border-radius:0;background:var(--paper-2)}
.c.v1{background:var(--v1)}.c.v2{background:var(--v2)}.c.v3{background:var(--v3)}.c.cur{outline:2px solid var(--ink);outline-offset:-2px}
#steps{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px}#steps button{padding:8px 2px;font:13px var(--f-mono)}
#steps button i{display:block;height:4px;margin-top:4px;background:var(--gold)}
.pz{display:flex;align-items:center;gap:8px;margin-top:6px}.pz span{width:112px;font-size:12px}.pz .bar{flex:1;width:auto}.pz button{width:110px;flex:none;padding:8px}
table.t{width:100%;border-collapse:collapse}table.t td{padding:5px 2px;border-top:1px solid var(--line);color:var(--ink)}
#btab tr:first-child td{border-top:0;color:var(--muted);font-size:11px;letter-spacing:.1em;text-transform:uppercase}
.bad{color:var(--bad)}#cnt td:first-child{color:var(--muted)}#cnt td:last-child{text-align:right}
#log{white-space:pre-wrap;word-break:break-word;font-size:12px;background:var(--bg);border:1px solid var(--line);border-radius:4px;padding:8px}
#tres{font:28px var(--f-mono);color:var(--ink);text-align:center;margin:12px 0}
.ban{display:none;margin-top:8px;padding:9px 12px;border:1px solid var(--bad);border-radius:6px;background:var(--bad-soft);color:var(--bad);font-weight:700;text-align:center}.ban.on{display:block}
#spk{display:none;color:var(--ink)}#spk.on{display:inline-flex;align-items:center;gap:4px}#spk svg{width:14px;height:14px}
.n1{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.n2{display:block;white-space:normal;font:12px/1.35 var(--f-mono);color:var(--muted)}
.chk{display:none;font:700 10px/16px var(--f-mono);font-style:normal;letter-spacing:.1em;color:var(--warn);border:1px solid var(--warn);border-radius:3px;padding:0 4px;margin-right:6px;vertical-align:1px}.chk.on{display:inline}
.rec{display:flex;align-items:center;justify-content:center;gap:8px;background:transparent;border-color:var(--muted)}.rec::before{content:"";width:9px;height:9px;border-radius:50%;background:var(--line);flex:none}
.rec.on{background:var(--bad-soft);border-color:var(--bad);color:var(--bad);font-weight:700}.rec.on::before{background:var(--bad)}#precs{min-height:0;margin-top:4px;color:var(--bad)}
.g tr.sn td{border:0;font:10px/16px var(--f-mono);color:var(--rule);text-align:center}.g tr.sn td.d{color:var(--ink)}.g tr.sn td.b{border-left:2px solid transparent}
.g select.na,.rr .na{color:var(--warn)}.g select.na{border-color:var(--warn)}
summary{cursor:pointer;min-height:44px;display:flex;align-items:center;color:var(--muted);font-size:14px;border-top:1px solid var(--line);margin-top:10px}
.rr{display:flex;align-items:center;gap:8px;border-top:1px solid var(--line);min-height:48px}.rr b{width:48px;flex:none;font:12px var(--f-mono);color:var(--muted)}.rr span{flex:1;min-width:0;font-size:13px}
.g select,.rr select,.pg select{font-size:13px;padding:7px 2px;min-height:36px;border-radius:4px;border:1px solid var(--line);background:var(--paper-2);color:var(--ink)}.rr select{width:112px;min-height:40px}
#pmeta{margin-top:4px}#pdesc{margin-top:6px;min-height:2.8em}
#vt{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:6px;margin-bottom:8px}#vt button{font:12px var(--f-mono);padding:6px 4px;overflow-wrap:anywhere}
.pd{border-top:1px solid var(--line);padding:6px 0 10px}.pdh{display:flex;align-items:center;gap:8px}.pdh b{flex:1;font:600 13px var(--f-serif);letter-spacing:.16em;text-transform:uppercase;color:var(--muted)}.pdh button{width:auto;padding:6px 16px}
.pg{display:grid;grid-template-columns:1fr 1fr;gap:6px 8px}.pg label{flex-direction:column;align-items:stretch;margin:0;gap:3px;font-size:12px}.pg select{width:100%;min-height:40px}
</style></head><body><main>
<header><div class="brand"><span id="emb"></span><div><div class="ws">RAGAMUFFIN WORKSHOP</div><h1>Hall Chimes <small id="fw"></small></h1></div></div>
<div class="row"><button id="panic">PANIC</button><button id="mute">Mute</button></div>
<div class="stl"><i class="lamp" id="lsum"></i><div id="sum">…</div><span class="lm" id="spk" title="Verna is speaking"><svg viewBox="0 0 16 16" aria-hidden="true"><path fill="currentColor" d="M2 6h3l4-3v10l-4-3H2z"/><path d="M11 5.5q1.6 2.5 0 5M12.8 3.8q3 4.2 0 8.4" fill="none" stroke="currentColor" stroke-width="1.3"/></svg><i class="lamp gold"></i></span><span class="mono lm"><i class="lamp" id="lmidi"></i> MIDI</span></div>
<div class="ban" id="nov">No 24 V: plug in the power supply</div><div id="msg"></div><nav id="nav"></nav></header>

<section class="tab" id="t-play">
<div class="card"><label>strength <b id="vv">100</b></label><input type="range" id="vel" min="1" max="127" value="100">
<div id="keys"></div></div>
<div class="card"><h2>Tunes</h2><div id="songs"></div><div class="row"><button id="dstop">Stop</button><button id="walk">Walk all</button></div>
<div class="row"><a class="btn" href="/test">Test all solenoids</a></div></div>
<div class="card"><h2>Tester</h2><div id="tst" class="note"></div><div class="row"><button class="acc" id="tap">TAP</button></div></div>
</section>

<section class="tab" id="t-outs">
<div class="card"><h2>Every solenoid and attachment</h2><div id="bud" class="mono"></div><div class="meter" id="budm"><i></i></div>
<label>show all 64 <input type="checkbox" id="all"></label><div id="olist"></div>
<div class="row"><button id="rall">Reset all to defaults</button></div>
<p class="note">Tap a row to set it up. Output n is on board 0x20 + (n-1)/8, pin (n-1)%8. "wear" counts every strike for life; CHECK means it passed its check count: look it over, then Reset wear. Tube = the brass tubes; small solenoid = beaters, blocks and the like; off never fires. Lamps: green ready, gold firing, yellow warm, red hot or board fault, dark off or no board. The bar is coil heat; the red and blue bar above is the 24 V supply in use.</p></div>
</section>

<section class="tab" id="t-prog">
<div class="card"><h2>Program chimes or beats</h2>
<label>pattern <select id="slot"><option value="0">1</option><option value="1">2</option><option value="2">3</option><option value="3">4</option></select></label>
<label>name <input id="pn" maxlength="11"></label>
<label>time signature <select id="pts"></select></label>
<label>steps <input id="pst" type="number" min="1" max="32"></label>
<label>tempo (bpm) <input id="pbpm" type="number" min="30" max="240"></label>
<label>swing <b id="pswv">0 %</b></label><input type="range" id="psw" min="0" max="75" value="0">
<label>loop <input id="plp" type="checkbox"></label>
<div class="row"><button class="acc" id="pplay">Play</button><button id="pstop">Stop</button><button class="rec" id="prec">REC</button></div>
<div id="precs" class="mono"></div>
<div id="grid"></div>
<div class="row"><button id="psave">Save</button><button id="pclr">Clear</button></div>
<details><summary>Row instruments</summary><div id="rroles"></div></details>
<p class="note">Each row strikes one output; each column is one step, with a stronger rule at each beat group. Tap a cell: off, ghost (60), medium (100), accent (127), off. Right-click or long-press clears it. Space plays and stops. REC records MIDI notes into this pattern: while it plays they land on the nearest step; when stopped, the first note starts one pass. Edits play straight away; Save keeps them after a restart.</p></div>
<div class="card"><h2>Presets <small class="note">world rhythms</small></h2>
<label>preset <select id="ppre"><option>loading…</option></select></label>
<div id="pdesc" class="note"></div><div id="pmeta" class="mono"></div>
<div class="row"><button class="acc" id="ppl">Load into this slot</button></div>
<p class="note">Presets from the Tribe of Abraham drum sequencer (MIT). Each row goes to the first output set to that instrument (Outputs, instrument); a row with none shows "not assigned". Loading does not save: tap Save to keep it.</p></div>
</section>

<section class="tab" id="t-cal">
<div class="card"><h2>Calibrate</h2>
<label>output <select id="cout"></select></label>
<label>softest (ms) <input id="cmi" type="number" step="0.1" min="1" max="30"></label>
<label>hardest (ms) <input id="cfu" type="number" step="0.1" min="1" max="30"></label>
<div class="row"><button id="ctmi">Try softest</button><button id="ctfu">Try hardest</button></div>
<div class="row"><button class="acc" id="csv">Save</button><button id="cdf">Defaults</button></div>
<div class="row"><button id="ccp">Copy to all of this kind</button></div>
<p class="note">Softest: the shortest tap that still sounds. Hardest: where it stops getting louder. Nothing goes past the cap, 30 ms at most.</p></div>
<div class="card"><h2>Sweep</h2><p class="note">Strikes it from soft to its cap, about one a second: tubes from 3 ms in 1 ms steps; small solenoids (or a softest under 3 ms) from 1 ms, in 0.5 ms steps when the cap is short. Then tap a step to use it.</p>
<div class="row"><button class="acc" id="csw">Sweep</button><button id="cst">Stop</button></div>
<div class="row"><button id="csm">tap sets softest</button><button id="csf">tap sets hardest</button></div>
<div id="cmsg" class="mono"></div><div id="steps"></div><div id="sug" class="note"></div></div>
<div class="card"><h2>Even out loudness</h2><p class="note">Strikes every output in turn at the same strength, 450 ms apart. Listen for the ones that stand out and set them above.</p>
<div class="row"><button id="cwk">Walk all outputs</button></div></div>
</section>

<section class="tab" id="t-tune">
<div class="card"><h2>Tune</h2><label>output <select id="tout"></select></label>
<div class="row"><button class="acc" id="tgo">Strike and measure</button></div>
<div id="tres">–</div><div id="tmsg" class="mono"></div>
<p class="note">Needs the piezo, and the chimes unmuted. It names the note only (C, C#, …), not the octave: tubes ring with strong overtones.</p></div>
<div class="card"><h2>Find piezo pin</h2><p class="note">Tap Listen, then knock near the piezo for 3 seconds. The pin that jumps is the piezo.</p>
<div class="row"><button id="pzs">Listen 3 s</button></div><div id="pzr"></div></div>
</section>

<section class="tab" id="t-stat">
<div class="card"><h2>Driver boards</h2><table class="t mono" id="btab"></table><div class="row"><button id="rescan">Rescan</button><a class="btn" href="/test">Test all solenoids</a></div></div>
<div class="card"><h2>Counters</h2><table class="t mono" id="cnt"></table></div>
<div class="card"><h2>Log</h2><div id="log" class="mono"></div></div>
</section>

<section class="tab" id="t-set">
<div class="card"><h2>Coils and supply</h2>
<label>cap per strike (ms) <input id="scap" type="number" min="3" max="30" step="0.5"></label>
<label>24 V supply (mA) <input id="ssup" type="number" min="100" max="20000" step="50"></label>
<label>driver outputs on <select id="sport"><option value="0">port A</option><option value="1">port B</option></select></label>
<label>only output (0 = all) <input id="ssolo" type="number" min="0" max="64"></label>
<div class="row"><button class="acc" id="sv1">Save</button></div>
<p class="note">No setting goes past 30 ms: short taps only, never a hold. "Only output" is for bench bring-up.</p></div>
<div class="card"><h2>MIDI</h2>
<label>channel (0 = all) <input id="smch" type="number" min="0" max="16"></label>
<label>strength curve <input id="sgam" type="number" min="0.3" max="4" step="0.1"></label>
<label>fold notes into C3..C5 <input id="sfold" type="checkbox"></label>
<label>latency (ms) <input id="slat" type="number" min="0" max="30"></label>
<label>contact triggers <input id="strig" type="checkbox"></label>
<div class="row"><button class="acc" id="sv2">Save</button></div>
<label>sustain pedal (CC64) does <select id="scc64"></select></label>
<p class="note">Triggers are contacts to GND on board 0x20 port B (outputs on port A only). B0 to B5 strike outputs 1, 8, 13, 17, 20 and 25. B6 is panic (it mutes; hold it 1.5 s to unmute when already muted). Leave B7 unwired: it is held low.</p></div>
<div class="card"><h2>Pins <small class="note">saving restarts</small></h2>
<label>MIDI in <select id="pmidi"><option value="14">GPIO 14</option><option value="44">GPIO 44</option></select></label>
<label>panic button <select id="pbtn"></select></label>
<label>tester solenoid <select id="psol"></select></label>
<label>tester active low <input type="checkbox" id="pinv"></label>
<label>piezo <select id="ppz"></select></label>
<div class="row"><button class="acc" id="sv3">Save pins and restart</button></div>
<label>tester pulse <b id="smsv">12 ms</b></label><input type="range" id="ssms" min="3" max="30" value="12">
<p class="note">Each pin does one job. The tester only taps, 3 to 30 ms, then off.</p></div>
<div class="card"><h2>Verna <small class="note">the BOX speaker</small></h2><div class="lrow"><i class="lamp" id="lvo"></i><div id="vst" class="note mono"></div></div>
<label>speak <input type="checkbox" id="von"></label>
<label>volume <b id="vvv">70</b></label><input type="range" id="vvol" min="0" max="100" value="70">
<details><summary>Test each prompt</summary><div id="vt"></div></details>
<p class="note">Verna says what the chimes need: no power, muted, too hot, a board back, tune results and the like.</p></div>
<div class="card"><h2>Power sense <small class="note">saving restarts</small></h2><div class="lrow"><i class="lamp" id="lpw"></i><div id="pwst" class="note mono"></div></div>
<label>sense pin <select id="ppwr"></select></label>
<div class="row"><button class="acc" id="sv4">Save pin and restart</button></div>
<p class="note">Wiring: <span class="mono">24 V + -&gt; 100 kOhm -&gt; pin; pin -&gt; 15 kOhm -&gt; GND; about 3.1 V when on</span>. With no 24 V nothing strikes, and the page, screen and Verna say so.</p></div>
<div class="card"><h2>Foot pedals <small class="note">a pin change restarts</small></h2><div id="peds"></div>
<p class="note">A foot switch on a header pin. Auto polarity reads the released level at boot, so normally-open and normally-closed pedals both work. Strike fires the output picked; the other actions work on the pattern chosen on Program.</p></div>
<div class="card"><h2>Hall WiFi</h2><div class="lrow"><i class="lamp" id="lnet"></i><div id="nets" class="note mono"></div></div>
<label>network <input id="ssid" list="aps"></label><datalist id="aps"></datalist>
<label>password <input id="wpass" type="password"></label>
<div class="row"><button id="clark"></button><button id="scan">Scan</button></div>
<div class="row"><button class="acc" id="join">Join</button><button id="forget">Forget</button></div></div>
<div class="card"><h2>Own WiFi</h2><div class="lrow"><i class="lamp" id="lap"></i><div id="apst" class="note mono"></div></div>
<label>password <input id="appw" type="password" placeholder="8 or more"></label>
<div class="row"><button id="apsv">Save</button><button id="apdf">Back to default</button></div>
<p class="note">The fallback network, on whenever the hall WiFi is away. The BOX screen (SETUP) shows its password.</p></div>
<div class="card"><h2>MQTT</h2><div class="lrow"><i class="lamp" id="lmq"></i><div id="mq" class="note mono"></div></div>
<label>broker <input id="mhost" placeholder="blank = off"></label><label>port <input id="mport" type="number"></label>
<label>user <input id="muser"></label><label>password <input id="mpass" type="password" placeholder="unchanged"></label>
<div class="row"><button id="msave">Save</button></div></div>
<div class="card"><h2>Firmware</h2><div id="fwv" class="note mono"></div>
<div class="row"><button class="acc" id="latest">Install latest</button></div>
<p class="note">Needs internet on this phone or computer (mobile data on, page open in Chrome, not the WiFi sign-in pop-up). The chimes are muted while it writes. This is the ESP32-S3-BOX build: never upload the plain chime.bin here, it uses the screen's pins.</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
</section>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(',HOME=')HTML" HOME_SSID R"HTML(';
const HDR=[9,10,11,12,13,14,21,38,39,42,44],ADC=[9,10,11,12,13,14];
const $=id=>document.getElementById(id),esc=s=>String(s).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
const NN=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'],nm=n=>n>0?NN[n%12]+(Math.floor(n/12)-1):'';
const KIND=['off','tube','small solenoid','other'],KL=['off','T','S','O'],VC=[0,60,100,127];
// round 2 tables, in the order of chimes.h: TIME_SIGS (label, steps per beat group), ROLE_NAMES, PEDAL_ACT_NAMES, VernaLine keys
const TS=[['4/4',4],['3/4',4],['6/8',6],['5/4',4],['7/8',2],['9/8',6],['12/8',6]];
const ROLE=['','Kick','Snare','Hi-Hat','Open HH','Rim','Clap','Lo Tom','Hi Tom','Cowbell','Shaker'];
const PACT=['none','play/stop','next pattern','prev pattern','tap tempo','record','strike','panic'],PPOL=['auto','pressed = high','pressed = low'];
const VK=('ready no_power power_back muted unmuted panic no_driver too_hot not_set_up updating tune_on tune_touch_sharp tune_touch_flat tune_sharp tune_flat tune_no_signal cal_min cal_full cal_done pattern_saved '
 +[1,2,3,4,5,6,7,8].map(n=>'board_back_'+n).join(' ')+' wear_check rec_start rec_saved rec_stop').split(' ');
const kf=n=>n>=1e4?(n/1000).toFixed(n>=1e5?0:1)+'k':String(n||0);
const WHY={2:'auth expired',15:'wrong password',201:'not found (2.4 GHz only)',202:'wrong password',203:'refused',204:'wrong password',205:'refused'};
const ms=v=>v>100?v/1000:+v,f1=v=>String(Math.round(v*10)/10);   // ms, whether a value came as us or ms
let S={},O=[],P=null,slot=0,all=0,open=-1,cells=[],cur=-1,lastN=-1,timer=0,mt=0,tapMin=1,keySig='',oSig='',stSig='',songsDone=0,filled=0;
let oRev=-1,oBusy=0,pBusy=0,prevS=[];const cq={};   // revisions seen, fetches in flight, strike counts last poll, cell edits
let PR=null,prBusy=0,prTry=0,lastCur=-1;             // presets list, the board's working slot last seen
function J(u){return fetch(u,{cache:'no-store'}).then(r=>r.ok?r.json():r.status==409?r.json().then(j=>{show(j);throw new Error('busy: wait for the calibration to finish, or Stop it')}):r.text().then(t=>{throw new Error(t||('error '+r.status))}))}
function msg(t){$('msg').textContent=t;clearTimeout(mt);mt=setTimeout(()=>$('msg').textContent='',5000)}
const bad=e=>{msg(e&&e.message?e.message:'no reply');return 0};
const go=u=>J(u).then(j=>{show(j);return 1}).catch(bad);
const og=u=>J(u).then(j=>{setOuts(j);return 1}).catch(bad);
const sg=u=>J(u).then(j=>{setSeq(j);return 1}).catch(bad);
// for replies that may be status, /outs, /seq or plain text
const ask=u=>fetch(u,{cache:'no-store'}).then(r=>r.text().then(t=>{if(!r.ok)throw new Error(t||('error '+r.status));let j=0;try{j=JSON.parse(t)}catch(e){}
 if(j){if(j.outs)setOuts(j);else if(j.vel)setSeq(j);else show(j)}return 1})).catch(bad);
const qs=o=>new URLSearchParams(o).toString();
const nz=o=>{for(const k in o)if(o[k]==='')delete o[k];return o};   // drop blank number fields: the board keeps those
const bs=i=>S.boards&&S.boards[i>>3]?S.boards[i>>3].s:0;   // 0 missing, 1 ok, 2 fault
const lab=i=>O[i]&&O[i].n?O[i].n:'out '+(i+1);
const use=i=>O[i]&&O[i].k>0&&bs(i)==1&&!(S.cfg&&S.cfg.solo&&S.cfg.solo!=i+1);
const vel=()=>$('vel').value;

// brand emblem (looks only): the rose window, 16 petals in the four glass colours, spindle hub, red/blue dashed ring
let lampN=-1,lampM=-1;
(()=>{const G=['#fdcd09','#8dad3e','#60b6e5','#c62b29'],K='#060709',ring=(c,o)=>`<circle r="47" fill="none" stroke="${c}" stroke-width="3" pathLength="32" stroke-dasharray=".8 1.2" stroke-dashoffset="${o}"/>`;let p='';
 for(let k=0;k<16;k++)p+=`<path transform="rotate(${k*22.5})" fill="${G[k%4]}" d="M-2.4-15L-6.4-34Q-7.2-40.5-3-41L0-38.4L3-41Q7.2-40.5 6.4-34L2.4-15Z"/>`;
 $('emb').innerHTML=`<svg viewBox="-50 -50 100 100" aria-hidden="true"><circle r="49.5" fill="#fff"/>${ring('#c62b29',0)}${ring('#60b6e5',1)}<circle r="43.5" fill="none" stroke="${K}" stroke-width="1.6"/><g stroke="${K}" stroke-width="1.2" stroke-linejoin="round">${p}</g><circle r="15" fill="#fff" stroke="${K}" stroke-width="1.6"/><path fill="${K}" d="M-4.5-10h9v9h-9zM-2.2-1h4.4L0 5z"/><path stroke="${K}" stroke-width="1.6" d="M-8 8h16"/></svg>`})();

// tabs
const TABS=[['play','Play'],['outs','Outputs'],['prog','Program'],['cal','Calibrate'],['tune','Tune'],['stat','Status'],['set','Settings']];
function tab(t){TABS.forEach(([k])=>{$('t-'+k).classList.toggle('sel',k==t);$('b-'+k).classList.toggle('sel',k==t)});history.replaceState(null,'','#'+t)}
TABS.forEach(([k,n])=>{const b=document.createElement('button');b.id='b-'+k;b.textContent=n;b.onclick=()=>tab(k);$('nav').append(b)});
tab(TABS.some(([k])=>'#'+k==location.hash)?location.hash.slice(1):'play');

// status
function busy(){return (S.cal&&S.cal.state)||(S.seq&&S.seq.slot>=0)||S.demo>0||(S.rec&&S.rec.state>0)}
function poll(){clearTimeout(timer);J('/status').then(show).catch(()=>{$('sum').textContent='no reply from the chimes';$('lsum').className='lamp'})
 .finally(()=>timer=setTimeout(poll,busy()?400:1000))}
function show(j){if(!j||!j.boards)return;S=j;$('fw').textContent=j.fw;
 const m=$('mute');m.textContent=j.muted?'Muted · tap to unmute':'Mute';m.className=j.muted?'mute':'';
 // follow the working slot when the BOX screen or a pedal changes it (not over unsaved edits here)
 if(j.cur!=null&&+j.cur!=lastCur){const c=+j.cur;if((lastCur<0||!P||!P.dirty)&&c!=slot&&c>=0&&c<4){slot=c;P=null}lastCur=c}
 summary();keys();songs();outList();liveOuts();seqCursor();calShow();tuneShow();statTab();netShow();r2Show();sync();
 if(!filled){filled=1;fillCfg();fillNet()}}
function r2Show(){const j=S,pw=j.power||{},v=j.verna||{},R=j.rec||{},sp=v.playing!=null&&v.playing>=0;
 $('nov').classList.toggle('on',pw.ok===0);$('spk').classList.toggle('on',sp);
 $('vst').textContent=j.verna?`${v.on?'on':'off'} · volume ${v.vol} · said ${v.count}`+(sp?' · speaking: '+String(v.key||VK[v.playing]||v.playing).replace(/_/g,' '):''):'not in this firmware';
 $('lvo').className='lamp '+(sp?'gold':v.on?'ok':'');
 $('pwst').textContent=pw.pin==null?'not in this firmware':pw.pin<0?'no sense pin: strikes are not held back':`GPIO ${pw.pin} · ${pw.ok?'24 V present':'NO 24 V'} · ${pw.asks||0} strikes refused`;
 $('lpw').className='lamp '+(pw.pin==null||pw.pin<0?'':pw.ok?'ok':'bad');
 (j.pedals||[]).forEach((p,n)=>{const l=$('pl'+n);if(l)l.className='lamp '+(p.down?'gold':p.pin>=0?'ok':'')});
 const on=R.slot>=0&&(R.state==1||R.state==2),b=$('prec');b.classList.toggle('on',on);b.textContent=R.state==1?'ARMED':'REC';
 $('precs').textContent=R.slot>=0?R.state==1?`REC armed on pattern ${R.slot+1}: play a note to start`:R.state==2?`recording into pattern ${R.slot+1}`+(R.notes!=null?` · ${R.notes} notes`:'')+' · tap REC to stop':R.state==3?'saving the take…':'':''}
// keep /outs and /seq fresh: fetched (and retried every poll) until they arrive, and again whenever the board says
// they changed, from this page or from the BOX screen
function sync(){const r=S.rev;if(!r)return;
 if(!oBusy&&(!O.length||r.outs!==oRev)){oBusy=1;J('/outs').then(setOuts).catch(()=>0).finally(()=>oBusy=0)}
 if(!pBusy&&(!P||P.s!=slot||(r.pat&&r.pat[slot]!==P.rev))){pBusy=1;J('/seq?s='+slot).then(setSeq).catch(()=>0).finally(()=>pBusy=0)}
 if(P&&r.dirty&&P.s==slot&&!!r.dirty[slot]!=!!P.dirty){P.dirty=r.dirty[slot];saveLbl()}
 if(!PR&&!prBusy&&prTry<5){prBusy=1;prTry++;J('/presets').then(setPre).catch(()=>0).finally(()=>prBusy=0)}}
function summary(){const j=S,ok=j.boards.filter(b=>b.s==1).length,f=j.boards.filter(b=>b.s==2);
 const np=j.power&&j.power.ok===0;
 let t=j.stuck?'COIL MAY BE STUCK ON: cut the 24 V':j.updating?'UPDATING':np?'NO 24 V':j.muted?'MUTED':f.length?'FAULT '+f.map(b=>'0x'+b.a.toString(16)).join(' '):ok?'READY':'NO DRIVERS';
 t+=` · ${ok} driver${ok==1?'':'s'} · ${j.on}/${j.maxOn} on · ${j.maOn}/${j.supma} mA · played ${j.played}`;
 if(j.played&&j.last)t+=` · last ${lab(j.last[0])} v${j.last[1]}`;
 if(j.demo>0)t+=' · tune: '+((j.songs||[])[j.demo-1]||j.demo);
 if(j.seq&&j.seq.slot>=0)t+=` · pattern ${j.seq.slot+1} step ${j.seq.step+1}`;
 if(j.cal&&j.cal.state)t+=' · '+(['','sweeping','tuning','listening','walking'][j.cal.state]||'busy');
 $('sum').textContent=t;
 $('lsum').className='lamp '+(j.updating?'warn':j.stuck||np||j.muted||f.length?'bad':!ok?'':j.on>0||(lampN>=0&&j.played!=lampN)?'gold':'ok');
 $('lmidi').className='lamp'+(lampM>=0&&j.midi!=lampM?' blue':'');lampN=j.played;lampM=j.midi}
$('panic').onclick=()=>go('/panic').then(k=>k&&msg('ALL OFF, muted'));
$('mute').onclick=()=>go('/mute?on='+(S.muted?0:1));

// play
$('vel').oninput=()=>$('vv').textContent=vel();
function keys(){const u=[];for(let i=0;i<64;i++)if(use(i))u.push(i);
 const sig=u.map(i=>i+O[i].n+O[i].no).join(),k=$('keys');
 if(sig!=keySig){keySig=sig;k.innerHTML='';
  if(!u.length)k.innerHTML='<p class="note">Nothing to play yet: no output is on, or no driver board answers. See Outputs and Status.</p>';
  u.forEach(i=>{const b=document.createElement('button'),o=O[i];b.dataset.i=i;
   b.innerHTML=esc(lab(i))+'<small>'+esc(o.no?nm(o.no):KL[o.k])+'</small>';
   if(o.k==1&&o.no&&NN[o.no%12].length>1)b.className='s';
   b.onclick=()=>go(`/play?o=${i+1}&v=${vel()}`);k.append(b)})}
 const L=S.live||{},fl=S.played!=lastN&&S.last?S.last[0]:-1;lastN=S.played;
 k.querySelectorAll('button').forEach(b=>{const i=+b.dataset.i;b.classList.toggle('on',!!(L.on&&L.on[i])||i==fl)});
 const c=S.cfg||{};$('tst').textContent=c.solp<0?'Tester off: set its pin in Settings.':`Tester on GPIO ${c.solp}${c.sinv?' (active low)':''}, ${c.sms} ms tap. Also MQTT tap. (BOOT is a panic button.)`}
function songs(){const d=$('songs');if(!songsDone&&S.songs){songsDone=1;S.songs.forEach((n,x)=>{const b=document.createElement('button');
 b.textContent=n;b.onclick=()=>go('/demo?n='+(x+1));d.append(b)})}
 [...d.children].forEach((b,x)=>b.className=S.demo==x+1?'acc':'')}
$('dstop').onclick=()=>go('/demo?n=0');$('walk').onclick=()=>go('/walk');$('tap').onclick=()=>go('/tap');

// outputs: status panel and setup
function setOuts(j){if(!j||!j.outs)return;O=j.outs;if(j.rev!=null)oRev=j.rev;oSig='';keySig='';outList();liveOuts();keys();outSelects();pedOuts();if(P)grid()}
const vis=i=>all||(O[i]&&O[i].k>0)||bs(i)>0;
function outList(){if(!O.length)return;const v=[];for(let i=0;i<64;i++)if(vis(i))v.push(i);
 const sig=v.join()+'|'+open;if(sig==oSig)return;oSig=sig;const d=$('olist');d.innerHTML='';
 if(!v.length)d.innerHTML='<p class="note">No driver board answers and no output is on. Tick "show all 64" to set them up anyway.</p>';
 v.forEach(i=>{const o=O[i],e=document.createElement('div');e.className='o';e.id='o'+i;
  e.innerHTML=`<div class="oh"><span class="dot"></span><b>${i+1}</b><span class="nm"><span class="n1"><em class="chk">CHECK</em>${esc(lab(i))}</span><small class="n2">${KL[o.k]}${o.no?' '+nm(o.no):''}${o.r?' '+ROLE[o.r]:''} · 0x${(32+(i>>3)).toString(16)}/${i&7}<span class="wr"></span></small></span><span class="bar"><i></i></span><span class="st mono"></span></div>`;
  e.firstChild.onclick=()=>{open=open==i?-1:i;outList();liveOuts()};
  if(open==i)e.append(editor(i));d.append(e)})}
function liveOuts(){const L=S.live;if(!L||!L.on||!O.length)return;
 $('bud').textContent=`supply ${S.maOn} of ${S.supma} mA in use · up to ${S.maxOn} coils at once · ${S.on} on now`;
 $('budm').firstChild.style.width=(S.supma>0?Math.min(100,S.maOn*100/S.supma):0)+'%';
 // a coil is on for 30 ms at most, so "firing" also means: it struck since the last poll (as the BOX screen does)
 const fire=[];for(let i=0;i<64;i++){fire[i]=!!L.on[i]||(prevS[i]!=null&&L.strikes[i]>prevS[i]);prevS[i]=L.strikes[i]}
 for(let i=0;i<64;i++){const e=$('o'+i);if(!e)continue;const b=bs(i);
  e.className='o'+(O[i].k?'':' off')+(b==0?' nob':'')+((L.heat[i]||0)>=50?' warm':'')+(fire[i]?' fire':'')+(L.hot[i]?' hot':'')+(b==2?' flt':'');
  e.querySelector('.bar i').style.width=Math.min(100,L.heat[i]||0)+'%';
  e.querySelector('.st').textContent=(b==2?'FAULT':b==0?'no board':fire[i]?'firing':L.hot[i]?'hot':'')+' '+L.strikes[i];
  if(L.wear){e.querySelector('.wr').textContent=' · wear '+kf(L.wear[i]);const w=e.querySelector('.ewc');if(w)w.textContent=L.wear[i]}
  e.querySelector('.chk').classList.toggle('on',!!(L.check&&L.check[i]))}}
function editor(i){const o=O[i],d=document.createElement('div'),q=s=>d.querySelector(s);d.className='ed';
 let no='<option value="0">none</option>';for(let n=21;n<=108;n++)no+=`<option value="${n}">${nm(n)} (${n})</option>`;
 if(o.no&&(o.no<21||o.no>108))no+=`<option value="${o.no}">${o.no}</option>`;
 d.innerHTML=`<label>name <input class="en" maxlength="11"></label>
<label>kind <select class="ek">${KIND.map((k,x)=>`<option value="${x}">${k}</option>`).join('')}</select></label>
<label>MIDI note <select class="eno">${no}</select></label>
<label>softest (ms) <input class="emi" type="number" step="0.1" min="1" max="30"></label>
<label>hardest (ms) <input class="efu" type="number" step="0.1" min="1" max="30"></label>
<label>cap (ms) <input class="ec" type="number" step="0.1" min="1" max="30"></label>
<label>coil current (mA) <input class="ema" type="number" min="0" max="3000"></label>
<label>instrument (presets) <select class="erl">${ROLE.map((n,x)=>`<option value="${x}">${x?n:'none'}</option>`).join('')}</select></label>
<label>check at (k strikes, 0 off) <input class="ewk" type="number" min="0" max="65000"></label>
<label>lifetime strikes <b class="ewc">${S.live&&S.live.wear?S.live.wear[i]:'–'}</b></label>
<div class="row"><button class="acc esv">Save output</button><button class="ert">Reset</button><button class="epl">Strike</button></div>
<div class="row"><button class="ewr">Reset wear</button></div>`;
 q('.en').value=o.n;q('.ek').value=o.k;q('.eno').value=o.no;q('.emi').value=f1(o.mi/1000);q('.efu').value=f1(o.fu/1000);
 q('.ec').value=f1(o.c/1000);q('.ema').value=o.ma;q('.erl').value=o.r||0;q('.ewk').value=o.w!=null?o.w:'';
 q('.esv').onclick=()=>{if(['.emi','.efu','.ec','.ema'].some(s=>q(s).value===''))return msg('Fill in every number first');
  const p={o:i+1,name:q('.en').value,kind:q('.ek').value,note:q('.eno').value,min:q('.emi').value,
  full:q('.efu').value,cap:q('.ec').value,ma:q('.ema').value,role:q('.erl').value};if(q('.ewk').value!=='')p.wear=q('.ewk').value;
  og('/out?'+qs(p)).then(k=>k&&msg(`output ${i+1} saved`))};
 q('.ert').onclick=()=>{if(confirm(`Reset output ${i+1} to its defaults?`))og(`/outreset?o=${i+1}`)};
 q('.ewr').onclick=()=>{if(confirm(`Reset the wear count of output ${i+1}? Do it after you service or replace it.`))ask(`/wearreset?o=${i+1}`).then(k=>k&&msg(`output ${i+1} wear reset`))};
 q('.epl').onclick=()=>go(`/play?o=${i+1}&v=${vel()}`);return d}
$('all').onchange=()=>{all=$('all').checked;outList();liveOuts()};
$('rall').onclick=()=>{if(confirm('Reset all 64 outputs to the defaults? Names, notes and calibration go back to new.'))og('/outreset?o=0')};

// program: the step sequencer
const vcl=v=>v<=0?'':v<80?' v1':v<114?' v2':' v3';
// a late reply for another slot is ignored
function setSeq(j){if(!j||!j.vel||(j.s!=null&&+j.s!=slot))return;P=j;$('slot').value=slot;$('pn').value=j.name;$('pst').value=j.steps;
 $('pts').value=j.ts||0;$('pbpm').value=j.bpm;$('psw').value=j.swing;$('pswv').textContent=j.swing+' %';$('plp').checked=!!j.loop;
 $('slot').options[slot].text=(slot+1)+' '+j.name;grid()}
const rrOf=()=>P.role||P.rowRole||[];   // the instrument each row was made for (a label only)
function rowSel(r,ro){let h=`<option value="0">${ro?esc(ROLE[ro]||ro)+': not assigned':'none'}</option>`;
 for(let i=0;i<64;i++)if((O[i]&&O[i].k>0)||P.row[r]==i)h+=`<option value="${i+1}">${i+1} ${esc(lab(i))}</option>`;return h}
// beat groups follow the time signature: a stronger rule and a beat number at the start of each group
function grid(){const t=document.createElement('table'),sub=P.sub||(TS[P.ts]||TS[0])[1],rr=rrOf(),h=t.insertRow();t.className='g';cells=[];
 h.className='sn';h.append(document.createElement('th'));
 for(let c=0;c<P.steps;c++){const d=h.insertCell();if(c%sub)d.textContent='·';else{d.textContent=c/sub+1;d.className='d'+(c?' b':'')}}
 for(let r=0;r<12;r++){const tr=t.insertRow(),th=document.createElement('th'),s=document.createElement('select');
  s.innerHTML=rowSel(r,rr[r]);s.value=P.row[r]<64?P.row[r]+1:0;if(rr[r]&&!(P.row[r]<64))s.className='na';
  s.onchange=()=>{mark();sg(`/seqrow?s=${slot}&r=${r}&o=${s.value}`)};
  th.append(s);tr.append(th);cells[r]=[];
  for(let c=0;c<P.steps;c++){const td=tr.insertCell(),b=document.createElement('button');if(c&&c%sub==0)td.className='b';
   b.onclick=()=>cell(r,c);b.oncontextmenu=e=>{e.preventDefault();cell(r,c,0)};td.append(b);cells[r][c]=b}}
 const g=$('grid'),x=g.scrollLeft;g.innerHTML='';g.append(t);g.scrollLeft=x;paint();saveLbl();roles(rr)}
function roles(rr){const d=$('rroles');d.innerHTML='';
 for(let r=0;r<12;r++){const e=document.createElement('div'),s=document.createElement('select'),sp=document.createElement('span'),o=P.row[r];
  e.className='rr';e.innerHTML=`<b>row ${r+1}</b>`;ROLE.forEach((n,x)=>s.append(new Option(x?n:'no instrument',x)));s.value=rr[r]||0;
  s.onchange=()=>{mark();sg('/seqrow?'+qs({s:slot,r,role:s.value}))};   // the row keeps its output
  sp.textContent=o<64?(o+1)+' '+lab(o):rr[r]?ROLE[rr[r]]+': not assigned':'no output';if(!(o<64)&&rr[r])sp.className='na';
  e.append(s,sp);d.append(e)}}
function paint(){if(!P)return;cells.forEach((row,r)=>row.forEach((b,c)=>b.className='c'+vcl(P.vel[r][c])+(c==cur?' cur':'')))}
// "unsaved" comes from the board (patDirty), so the page and the BOX screen always agree
function saveLbl(){$('psave').textContent=P&&P.dirty?'Save *':'Save'}
function mark(){if(P)P.dirty=1;saveLbl()}
// a reply only updates the cell it was for, and only if no newer tap on that cell is on its way
function cell(r,c,v){if(v!=null&&P.vel[r][c]==v)return;const n=v!=null?v:VC.find(x=>x>P.vel[r][c])||0,k=r*32+c,q=(cq[k]||0)+1;cq[k]=q;P.vel[r][c]=n;mark();paint();
 J(`/seqcell?s=${slot}&r=${r}&c=${c}&v=${n}`).then(j=>{if(j&&j.vel&&j.s==slot&&P&&cq[k]==q){P.vel[r][c]=j.vel[r][c];P.rev=j.rev;P.dirty=j.dirty;paint();saveLbl()}}).catch(bad)}
function seqCursor(){const q=S.seq||{},n=q.slot==slot?q.step:-1;if(n!=cur){cur=n;paint()}
 $('pplay').textContent=q.slot==slot?'Playing':'Play'}
function meta(){mark();sg('/seqmeta?'+qs(Object.assign(nz({steps:$('pst').value,bpm:$('pbpm').value,swing:$('psw').value}),
 {s:slot,name:$('pn').value,loop:$('plp').checked?1:0})))}
['pn','pst','pbpm','psw','plp'].forEach(id=>$(id).onchange=meta);
$('psw').oninput=()=>$('pswv').textContent=$('psw').value+' %';
TS.forEach(([l],x)=>$('pts').append(new Option(l,x)));
$('pts').onchange=()=>{mark();sg(`/seqmeta?s=${slot}&ts=${$('pts').value}`)};   // the board sets steps to fit
$('slot').onchange=()=>{const s=+$('slot').value,old=slot;
 if(P&&P.dirty&&confirm(`Save pattern ${old+1} first? If not, it stays marked unsaved and is lost at the next restart.`))J('/seqsave?s='+old).catch(bad);
 slot=s;P=null;pBusy=1;sg('/seq?s='+s).finally(()=>pBusy=0);lastCur=s;fetch('/set?cur='+s).catch(()=>0)};   // the screen and pedals follow
const playStop=()=>go((S.seq||{}).slot==slot?'/seqstop':'/seqplay?s='+slot);
$('pplay').onclick=()=>go('/seqplay?s='+slot);$('pstop').onclick=()=>go('/seqstop');
$('prec').onclick=()=>{const R=S.rec||{};ask(R.slot>=0&&R.state>0&&R.state<3?'/rec?stop=1':'/rec?s='+slot)};
// Space plays and stops on the Program tab (not while typing in a field)
const spc=e=>(e.key==' '||e.code=='Space')&&$('t-prog').classList.contains('sel')&&!/INPUT|SELECT|TEXTAREA/.test(e.target.tagName);
addEventListener('keydown',e=>{if(!spc(e))return;e.preventDefault();if(!e.repeat)playStop()});
addEventListener('keyup',e=>{if(spc(e))e.preventDefault()});
// presets (Tribe of Abraham, MIT)
function setPre(a){if(!Array.isArray(a)||!a.length)return;PR=a;const s=$('ppre');s.innerHTML='';a.forEach((p,x)=>s.append(new Option(p.name,x)));preShow()}
function preShow(){const p=PR&&PR[$('ppre').value];if(!p)return;$('pdesc').textContent=p.desc||'';
 $('pmeta').textContent=`${(TS[p.ts]||TS[0])[0]} · ${p.steps} steps · ${p.bpm} bpm · swing ${p.swing} %`+((p.roles||[]).some(r=>r)?' · '+p.roles.filter(r=>r).map(r=>ROLE[r]||r).join(', '):'')}
$('ppre').onchange=preShow;
$('ppl').onclick=()=>{const x=$('ppre').value,p=PR&&PR[x];if(!p)return msg('No presets from the board yet');
 if(!confirm(`Load "${p.name}" into pattern ${slot+1}? Its cells, rows, tempo and name are replaced. Tap Save to keep it.`))return;
 mark();sg(`/preset?s=${slot}&p=${x}`).then(k=>k&&msg(`${p.name} is in pattern ${slot+1}: tap Save to keep it`))};
$('psave').onclick=()=>sg('/seqsave?s='+slot).then(k=>k&&msg(`pattern ${slot+1} saved`));
$('pclr').onclick=()=>{if(confirm(`Clear every cell of pattern ${slot+1}?`)){mark();sg('/seqclear?s='+slot)}};

// calibrate
function outSelects(){['cout','tout'].forEach(id=>{const s=$(id),v=s.value;s.innerHTML='';
 for(let i=0;i<64;i++)if(O[i]&&O[i].k>0)s.append(new Option(`${i+1} ${lab(i)}${O[i].no?' '+nm(O[i].no):''}`,i));
 if(!s.options.length)s.append(new Option('no output is on',-1));
 if([...s.options].some(o=>o.value==v))s.value=v});calFill()}
const ci=()=>{const i=+$('cout').value;if(i<0||!O[i]){msg('Pick an output first');return -1}return i};
function calFill(){const i=+$('cout').value,o=O[i];if(!o)return;$('cmi').value=f1(o.mi/1000);$('cfu').value=f1(o.fu/1000)}
$('cout').onchange=calFill;
$('ctmi').onclick=()=>{const i=ci();if(i>=0)go(`/try?o=${i+1}&ms=${$('cmi').value}`)};
$('ctfu').onclick=()=>{const i=ci();if(i>=0)go(`/try?o=${i+1}&ms=${$('cfu').value}`)};
$('csv').onclick=()=>{const i=ci();if(i>=0)og('/out?'+qs({o:i+1,min:$('cmi').value,full:$('cfu').value})).then(k=>{if(k){calFill();msg(`output ${i+1} saved`)}})};
$('cdf').onclick=()=>{const i=ci();if(i>=0&&confirm(`Put output ${i+1} back to its defaults?`))og(`/outreset?o=${i+1}`).then(calFill)};
$('ccp').onclick=()=>{const i=ci();if(i<0)return;const k=O[i].k,mi=$('cmi').value,fu=$('cfu').value;let n=0;
 if(mi===''||fu==='')return msg('Fill in softest and hardest first');
 for(let x=0;x<64;x++)if(O[x]&&O[x].k==k)n++;
 if(!confirm(`Set softest ${mi} ms and hardest ${fu} ms on all ${n} ${KIND[k]} outputs?`))return;
 og('/outcopy?'+qs({k,min:mi,full:fu})).then(r=>{if(r){calFill();msg(`copied to ${n} outputs`)}})};   // one save on the board
$('csw').onclick=()=>{const i=ci();if(i<0)return;if(S.muted){msg('Unmute first');return}go('/sweep?o='+(i+1))};
$('cst').onclick=()=>go('/calstop');$('cwk').onclick=()=>go('/walk');
function tapMode(m){tapMin=m;$('csm').className=m?'acc':'';$('csf').className=m?'':'acc'}
$('csm').onclick=()=>tapMode(1);$('csf').onclick=()=>tapMode(0);tapMode(1);
function calShow(){const c=S.cal;if(!c)return;
 $('cmsg').textContent=(c.state==1?`sweeping · now ${f1(c.cur/1000)} ms · `:'')+(c.msg||'');
 const st=c.steps||[],sig=JSON.stringify(st);if(sig==stSig)return;stSig=sig;
 const d=$('steps'),mx=Math.max(1,...st.map(s=>s[1]));d.innerHTML='';
 st.forEach(([us,pk])=>{const b=document.createElement('button');b.innerHTML=`${f1(us/1000)} ms`+(pk?` · ${pk}<i style="width:${Math.round(pk*100/mx)}%"></i>`:'');
  b.onclick=()=>{$(tapMin?'cmi':'cfu').value=f1(us/1000);msg((tapMin?'softest':'hardest')+' set to '+f1(us/1000)+' ms: tap Save to keep it')};d.append(b)});
 suggest(st)}
function suggest(st){const s=$('sug');s.innerHTML='';if(!st.length)return;const pk=st.map(x=>x[1]),mx=Math.max(...pk),mn=Math.min(...pk);
 if(mx<=0){s.textContent='No piezo readings: listen, then tap the first step you hear as softest and the loudest clean one as hardest.';return}
 if(mx-mn<20){s.textContent='The piezo barely hears this one. Move it closer, or go by ear.';return}
 const lo=st.find(x=>x[1]>=mn+(mx-mn)*.15),hi=st.find(x=>x[1]>=mn+(mx-mn)*.9);
 s.textContent=`From the piezo: softest about ${f1(lo[0]/1000)} ms, hardest about ${f1(hi[0]/1000)} ms. `;
 const b=document.createElement('button');b.textContent='Use these';b.style.marginTop='6px';
 b.onclick=()=>{$('cmi').value=f1(lo[0]/1000);$('cfu').value=f1(hi[0]/1000);msg('tap Save to keep them')};s.append(b)}

// tune and piezo
$('tgo').onclick=()=>{const i=+$('tout').value;if(i<0||!O[i])return msg('Pick an output first');
 if((S.cfg||{}).pzp<0)return msg('Set the piezo pin first (Find piezo pin, below)');if(S.muted)return msg('Unmute first');go('/tune?o='+(i+1))};
function tuneShow(){const t=S.tune,c=S.cal||{},pp=(S.cfg||{}).pzp;if(!t)return;const r=$('tres');
 if(c.state==2)r.textContent='listening…';
 else if(t.ok>0){const ce=+t.cents;r.textContent=`${t.note}  ${(+t.hz).toFixed(1)} Hz  ${ce>0?'+':''}${ce} cents`+(Math.abs(ce)<=10?'  in tune':'')}
 else r.textContent='–';$('tmsg').textContent=c.msg||'';
 const z=S.pz,d=$('pzr');if(!z)return;
 if(c.state==3){d.innerHTML='<p class="note">Listening… knock near the piezo.</p>';d.dataset.sig='';return}
 if(!z.done||!z.pins)return;const sig=JSON.stringify(z)+pp;if(d.dataset.sig==sig)return;d.dataset.sig=sig;
 const mx=Math.max(1,...z.pk);d.innerHTML='';
 z.pins.forEach((p,x)=>{if(p<0)return;const e=document.createElement('div');e.className='pz';
  e.innerHTML=`<span class="mono">GPIO ${p} · ${z.pk[x]}</span><span class="bar"><i style="width:${Math.round(z.pk[x]*100/mx)}%"></i></span>`;
  const b=document.createElement('button');b.textContent=p==pp?'in use':'use GPIO '+p;b.disabled=p==pp;
  b.onclick=()=>setPins({pz:p});e.append(b);d.append(e)})}
$('pzs').onclick=()=>go('/pzscan');

// status tab
function statTab(){const j=S;
 $('btab').innerHTML='<tr><td>board</td><td>outputs</td><td>state</td><td>errors</td></tr>'+j.boards.map((b,x)=>
  `<tr><td>0x${b.a.toString(16)}</td><td>${x*8+1}-${x*8+8}</td><td${b.s==2?' class="bad"':''}><i class="lamp ${['','ok','bad'][b.s]||''}"></i>${['not found','OK','FAULT'][b.s]||b.s}</td><td>${b.e}</td></tr>`).join('');
 const C=[['played',j.played],['coils on now',`${j.on} of ${j.maxOn} (${j.maOn} mA)`],['queue now / most',`${j.queue} / ${j.qpeak}`],
  ['dropped (late or full)',j.dropped],['refused, hot',j.heatRefused],['refused, board fault',j.faultRefused],['cut to the cap',j.clamped],
  ['panics',j.panics],['watchdog',j.watchdog],['MIDI notes',j.midi],['sustain pedal',j.sustain?'down':'up'],['hard cap',ms(j.hardCap)+' ms']];
 if(j.power)C.push(['refused, no 24 V',j.power.asks||0]);if(j.verna)C.push(['Verna prompts',j.verna.count]);
 if(j.pedals)C.push(['foot pedals down',j.pedals.map((p,n)=>p.down?n+1:'').filter(x=>x).join(' ')||'none']);
 $('cnt').innerHTML=C.map(([k,v])=>`<tr><td>${k}</td><td>${esc(v)}</td></tr>`).join('');
 $('log').textContent=(j.log||[]).join('\n')}
$('rescan').onclick=()=>go('/rescan');

// settings
function fillCfg(){const c=S.cfg;if(!c)return;$('scap').value=ms(c.cap);$('ssup').value=c.supma;$('sport').value=c.port;$('ssolo').value=c.solo;
 $('smch').value=c.mch;$('sgam').value=c.gam;$('sfold').checked=!!c.fold;$('slat').value=ms(c.lat);$('strig').checked=!!c.trig;
 $('pmidi').value=c.midip;$('pbtn').value=c.btnp;$('psol').value=c.solp;$('pinv').checked=!!c.sinv;$('ppz').value=c.pzp;
 $('ssms').value=c.sms;$('smsv').textContent=c.sms+' ms';
 const v=S.verna;if(v){$('von').checked=!!v.on;$('vvol').value=v.vol;$('vvv').textContent=v.vol}
 if(S.power&&S.power.pin!=null)$('ppwr').value=S.power.pin;
 const cc=c.cc64!=null?c.cc64:S.cc64;if(cc!=null)$('scc64').value=cc;
 (S.pedals||[]).forEach((p,n)=>{const e=$('pd'+n);if(!e)return;const q=s=>e.querySelector(s);
  q('.pp').value=q('.pp').dataset.v=p.pin;q('.pa').value=p.act;q('.pi').value=q('.pi').dataset.v=p.inv||0;q('.po').dataset.v=p.out;q('.po').value=p.out;q('.po').disabled=p.act!=6})}
function fillNet(){const n=S.net||{},m=S.mqtt||{};$('ssid').value=n.ssid||HOME;$('mhost').value=m.host||'';$('mport').value=m.port||1883;$('muser').value=m.user||''}
function netShow(){const n=S.net||{},m=S.mqtt||{};
 $('nets').textContent=n.up?`on ${n.ssid} · ${n.ip} · hall-chimes.local`:n.ssid?`joining ${n.ssid}…${n.why?' ('+(WHY[n.why]||'reason '+n.why)+')':''}`:'not set: own WiFi only';
 $('apst').textContent=`${n.apssid||'HallChimes'} · ${n.ap?'on, 192.168.4.1':'off while the hall WiFi is up'}`;
 $('mq').textContent=m.host?(m.up?'connected':'not connected'):'off';$('fwv').textContent='running '+S.fw;
 $('lnet').className='lamp '+(n.up?'ok':n.ssid?(n.why?'bad':'warn'):'');$('lap').className='lamp '+(n.ap?'ok':'');
 $('lmq').className='lamp '+(m.host?(m.up?'ok':'bad'):'')}
[['pbtn',HDR],['psol',HDR],['ppz',ADC]].forEach(([id,l])=>[-1,...l].forEach(p=>$(id).append(new Option(p<0?'off':'GPIO '+p,p))));
const saved=k=>{if(k){fillCfg();msg('saved')}};
// port and trig go only when they changed: each change switches everything off and re-inits the boards
$('sv1').onclick=()=>{const c=S.cfg||{},p=$('sport').value,q=nz({cap:$('scap').value,supma:$('ssup').value,solo:$('ssolo').value});
 if(p!=c.port){if(!confirm('Switch every driver to port '+'AB'[p]+'? Everything goes off first.'))return;q.port=p}
 go('/set?'+qs(q)).then(saved)};
$('sv2').onclick=()=>{const c=S.cfg||{},t=$('strig').checked?1:0,q=nz({mch:$('smch').value,gam:$('sgam').value,fold:$('sfold').checked?1:0,lat:$('slat').value});
 if(t!=(c.trig?1:0))q.trig=t;go('/set?'+qs(q)).then(saved)};
$('ssms').oninput=()=>$('smsv').textContent=$('ssms').value+' ms';
$('ssms').onchange=()=>go('/set?sms='+$('ssms').value).then(saved);
function setPins(o){const c=S.cfg||{},q=Object.assign({midi:c.midip,btn:c.btnp,sol:c.solp,inv:c.sinv,pz:c.pzp},S.power&&S.power.pin!=null?{pwr:S.power.pin}:{},o);
 if(!confirm('Save the pins and restart the board? Everything goes off and mutes first.'))return;
 fetch('/pins?'+qs(q)).then(r=>r.ok?(msg('saved, restarting…'),setTimeout(()=>location.reload(),7000)):r.text().then(t=>msg('not saved: '+t))).catch(()=>msg('no reply'))}
$('sv3').onclick=()=>setPins({midi:$('pmidi').value,btn:$('pbtn').value,sol:$('psol').value,inv:$('pinv').checked?1:0,pz:$('ppz').value});
// MIDI sustain pedal, Verna, power sense, foot pedals
PACT.forEach((t,x)=>$('scc64').append(new Option(t,x)));$('scc64').onchange=()=>go('/set?cc64='+$('scc64').value).then(saved);
VK.forEach(k=>{const b=document.createElement('button');b.textContent=k.replace(/_/g,' ');b.onclick=()=>ask('/say?l='+k);$('vt').append(b)});
const voice=()=>ask('/voice?'+qs({on:$('von').checked?1:0,vol:$('vvol').value}));
$('von').onchange=voice;$('vvol').oninput=()=>$('vvv').textContent=$('vvol').value;$('vvol').onchange=voice;
[-1,...HDR].forEach(p=>$('ppwr').append(new Option(p<0?'off':'GPIO '+p,p)));
$('sv4').onclick=()=>setPins({pwr:$('ppwr').value});
// pedal n = 0..3; out is 1-based. act and out are live; pin and polarity are boot-only (saved, then the board restarts)
function pedOuts(){for(let n=0;n<4;n++){const s=$('pd'+n).querySelector('.po'),v=s.dataset.v;s.innerHTML='';
 for(let i=0;i<64;i++)if(O[i]&&O[i].k>0||v==i+1)s.append(new Option(`${i+1} ${lab(i)}`,i+1));if(!s.options.length)s.append(new Option('no output is on',1));if(v!=null)s.value=v}}
for(let n=0;n<4;n++){const e=document.createElement('div'),q=s=>e.querySelector(s);e.className='pd';e.id='pd'+n;
 e.innerHTML=`<div class="pdh"><i class="lamp" id="pl${n}"></i><b>Pedal ${n+1}</b><button>Test</button></div><div class="pg">
<label>pin<select class="pp"></select></label><label>polarity<select class="pi"></select></label>
<label>action<select class="pa"></select></label><label>strikes<select class="po"></select></label></div>`;
 [-1,...HDR].forEach(p=>q('.pp').append(new Option(p<0?'off':'GPIO '+p,p)));PPOL.forEach((t,x)=>q('.pi').append(new Option(t,x)));
 PACT.forEach((t,x)=>q('.pa').append(new Option(t,x)));
 const live=()=>{q('.po').disabled=q('.pa').value!=6;q('.po').dataset.v=q('.po').value;ask('/pedal?'+qs({n,act:q('.pa').value,out:q('.po').value}))};
 q('.pa').onchange=q('.po').onchange=live;
 const boot=(c,k,t)=>{const p=q(c),back=()=>p.value=p.dataset.v;
  if(!confirm(`Set pedal ${n+1} ${t(p)} and restart the board? Everything goes off and mutes first.`))return back();
  fetch('/pedal?'+qs({n,[k]:p.value})).then(r=>r.ok?(msg('saved, restarting…'),setTimeout(()=>location.reload(),7000)):r.text().then(t=>{msg('not saved: '+t);back()})).catch(()=>{msg('no reply');back()})};
 q('.pp').onchange=()=>boot('.pp','pin',p=>p.value<0?'to no pin':'to GPIO '+p.value);
 q('.pi').onchange=()=>boot('.pi','inv',p=>'polarity to '+PPOL[p.value]);
 q('.pdh button').onclick=()=>ask(`/pedal?n=${n}&press=1`);$('peds').append(e)}
$('clark').textContent=HOME;$('clark').onclick=()=>{$('ssid').value=HOME;$('wpass').focus()};
// the board scans in the background (so its PANIC keeps working): start it, then ask until the list is ready
function scanPoll(u,n){J(u).then(a=>{if(a&&a.scanning){if(n<20)setTimeout(()=>scanPoll('/scan',n+1),700);else $('scan').textContent='Scan';return}
 if(!Array.isArray(a)){$('scan').textContent='scan failed';return}$('aps').innerHTML='';
 a.forEach(s=>$('aps').append(new Option(s.ssid||s)));$('scan').textContent=a.length+' found';if(a.length&&!$('ssid').value)$('ssid').value=a[0].ssid||a[0]}).catch(()=>$('scan').textContent='Scan')}
$('scan').onclick=()=>{$('scan').textContent='scanning…';scanPoll('/scan?start=1',0)};
$('join').onclick=()=>{if($('ssid').value)go('/wifi?'+qs({ssid:$('ssid').value,pass:$('wpass').value})).then(k=>k&&msg('joining…'))};
$('forget').onclick=()=>{if(confirm('Forget the hall WiFi?'))go('/wifi?forget=1')};
$('apsv').onclick=()=>{const p=$('appw').value;if(p.length<8)return msg('8 characters or more');go('/appass?'+qs({p})).then(k=>{if(k){$('appw').value='';msg('saved')}})};
$('apdf').onclick=()=>{if(confirm('Put the own WiFi password back to the default?'))go('/appass?p=').then(k=>k&&msg('back to the default'))};
$('msave').onclick=()=>{const q={host:$('mhost').value,port:$('mport').value||1883,user:$('muser').value};
 if($('mpass').value)q.pass=$('mpass').value;go('/mqtt?'+qs(q)).then(k=>k&&msg('saved'));$('mpass').value=''};
function send(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'chime_box.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' · rebooting';setTimeout(()=>location.reload(),6000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(!f)return;
 if(/^chime\.bin$/i.test(f.name)&&!confirm('chime.bin is the plain build and drives the screen pins. Upload it anyway?'))return;send(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');let m;try{
 b.textContent='checking…';m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json()}catch(e){b.textContent='no internet: mobile data on, open in Chrome';return}
 if(m.box!==true){b.textContent='not a BOX build, not installed';return}
 if(m.version===S.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 try{b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'chime_box.bin',{cache:'no-store'});if(!r.ok)throw 0;
 send(await r.blob(),b)}catch(e){b.textContent='download failed, try again'}};

poll();   // the first /status reply fetches /outs and /seq (sync), and keeps retrying them until they arrive
</script></main></body></html>)HTML";
