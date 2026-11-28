// The stair lights' phone page. Talks to the board's own API: /state /set /cfg /scan /wifi /mqtt /update.
import { useEffect, useRef, useState } from 'react'
import rose from './rose.svg?raw'

const LATEST = 'https://jobot101.github.io/Ragamuffin-Flasher/stairs/'   // = LATEST_URL in stairs.ino
const HALL = 'STARLINK', HOME = 'ClarkShop'                              // = HALL_SSID / HOME_SSID in stairs.ino; never a password here
const ORD = ['GRB', 'RGB', 'BRG', 'RBG', 'GBR', 'BGR']
const COLOURS = [['Warm', 'ff9329'], ['Candle', 'ff6a10'], ['White', 'ffd9b0'], ['Day', 'ffffff'], ['Red', 'c62b29'], ['Yellow', 'fdcd09'],
  ['Green', '8dad3e'], ['Blue', '60b6e5'], ['Show', '0a2a8a'], ['Purple', '7a2cff'], ['Pink', 'ff3c8a'], ['Teal', '00c0a0']]
const MODES = [['Solid', '■'], ['Pulse + colour', '◐'], ['Breathe', '○'], ['Spin', '◎'], ['Ripple', '≋'], ['Glass', '✦'], ['Candle', '♨'],
  ['Christmas', '🎄'], ['Candy cane', '🍬'], ['Snowfall', '❄'], ['Twinkle', '✧'], ['Runner', '▥'], ['Walk up', '⤴'], ['Measure GPIO 4', '📏'], ['Measure GPIO 5', '📐']]
const WHY = { 2: 'auth expired', 15: 'wrong password', 201: 'not found (2.4 GHz only)', 202: 'wrong password', 203: 'refused', 204: 'wrong password', 205: 'refused' }
const STRIP = ['nA', 'nB', 'ord', 'trim', 'fade', 'lim', 'limB', 'pma', 'name', 'bc', 'bw', 'mw']

const getJ = u => fetch(u).then(r => r.json())
const qs = o => new URLSearchParams(o).toString()
const speed = v => (v / 50).toFixed(v < 50 ? 2 : 1).replace(/\.?0+$/, '') + '×'
const pick = (o, keys) => Object.fromEntries(keys.map(k => [k, o[k] ?? '']))

export default function App() {
  const [st, setSt] = useState(null)          // last /state from the board
  const [err, setErr] = useState('')
  const [look, setLook] = useState({ c: 'ff9329', b: 40, s: 50 })   // what the colour / sliders show
  const busy = useRef(0), queue = useRef({}), timer = useRef(0), wifiFirst = useRef(null)

  const show = j => {
    if (!j || j.err) return setErr(j ? j.err : 'no reply')
    setErr(''); setSt(j)
    if (Date.now() - busy.current > 1500) setLook({ c: j.c, b: j.b, s: j.s })   // don't fight a finger on a slider
  }
  const go = u => getJ(u).then(show).catch(() => setErr('no reply'))

  useEffect(() => { go('/state'); const t = setInterval(() => go('/state'), 1500); return () => clearInterval(t) }, [])
  useEffect(() => { if (st) document.title = st.name[0].toUpperCase() + st.name.slice(1) + ' lights' }, [st?.name])

  // batch quick changes (slider drags) into one /set every 60 ms
  const send = o => {
    Object.assign(queue.current, o); busy.current = Date.now(); clearTimeout(timer.current)
    timer.current = setTimeout(() => { const q = qs(queue.current); queue.current = {}; go('/set?' + q) }, 60)
  }
  const setC = c => { setLook(l => ({ ...l, c })); send({ c }) }
  const setB = b => { setLook(l => ({ ...l, b })); send({ b }) }
  const setS = s => { setLook(l => ({ ...l, s })); send({ s }) }

  if (st && wifiFirst.current === null) wifiFirst.current = !st.net?.up   // not on the hall WiFi yet: put WiFi at the top
  const wifi = st && <Wifi st={st} go={go} />

  return <main>
    <header>
      <span dangerouslySetInnerHTML={{ __html: rose }} />
      <h1 className="word">RAGAMUFFIN</h1>
      <p className="sub"><span className="rule" />{(st?.name || 'stairs').toUpperCase()} LIGHTS<span className="rule r" /></p>
      <p id="fw">{st && 'build ' + st.fw}</p>
    </header>
    <div className="dash" />
    {wifiFirst.current && wifi}

    <div className="card"><h2>Light</h2>
      <div className="sw" style={{ background: '#' + look.c }}>
        <input type="color" value={'#' + look.c} onChange={e => setC(e.target.value.slice(1))} /><span>#{look.c}</span>
      </div>
      <div className="chips">{COLOURS.map(([n, h]) =>
        <button key={h} onClick={() => setC(h)}><i style={{ background: '#' + h }} />{n}</button>)}</div>
      <div className="bri"><span>Brightness</span><b>{look.b} %</b></div>
      <input type="range" min="0" max="100" value={look.b} onChange={e => setB(+e.target.value)} />
      <div className="row">{[0, 10, 30, 60, 100].map(v =>
        <button key={v} onClick={() => setB(v)}>{v ? v + ' %' : 'Off'}</button>)}</div>

      <h2 style={{ marginTop: 14 }}>Mode</h2>
      <div className="chips">{MODES.map(([n, g], i) =>
        <button key={i} className={st && !st.test && st.m === i ? 'on' : ''} onClick={() => send({ m: i })}>
          <i style={{ border: 0, fontSize: 22, lineHeight: '26px' }}>{g}</i>{n}</button>)}</div>
      <div className="bri"><span>Speed</span><b>{speed(look.s)}</b></div>
      <input type="range" min="1" max="100" value={look.s} onChange={e => setS(+e.target.value)} />
      <div className="row"><button className={'test' + (st?.test ? ' on' : '')} onClick={() => send({ test: st?.test ? 0 : 1 })}>
        {st?.test ? 'Test running — tap a colour to stop' : 'Test: every colour'}</button></div>
      <div id="st">{err || (st ? <>
        {st.test ? 'test · ' + st.fx : (MODES[st.m] || ['?'])[0].toLowerCase() + ' · #' + st.c} · {st.b} % · {st.nA}{st.nB ? ' + ' + st.nB : ''} px · {ORD[st.ord]} · about {st.limB ? `${st.maA} + ${st.maB}` : st.ma} mA
        {st.cap ? <span className="warn"> · 100 % = the most the supplies allow</span> : null}</> : '…')}</div>
    </div>

    {st && <Steps st={st} show={show} />}
    {st && <Strip st={st} show={show} />}
    {!wifiFirst.current && wifi}
    {st && <Mqtt st={st} go={go} />}
    {st && <Firmware st={st} />}
    <footer>Ragamuffin Studios, McCoysville, Pennsylvania</footer>
  </main>
}

function Field({ label, children }) { return <label>{label} {children}</label> }

function Strip({ st, show }) {
  const [f, setF] = useState(() => pick(st, STRIP))   // filled once; the poll doesn't overwrite typing
  const [msg, setMsg] = useState('')
  const set = k => e => setF({ ...f, [k]: e.target.value })
  const num = (k, min, max, ph) => <input type="number" min={min} max={max} placeholder={ph} value={f[k]} onChange={set(k)} />

  const save = () => {
    const o = { ...f }; ['nA', 'nB', 'ord', 'trim', 'fade', 'lim', 'limB', 'pma', 'bw', 'mw'].forEach(k => o[k] = o[k] || 0)
    getJ('/cfg?' + qs(o)).then(j => {
      if (j.err) return setMsg(j.err)
      if (!j.restart) { setMsg('Saved ✓'); setF(pick(j, STRIP)); return show(j) }
      setMsg(j.name ? 'Saved. Restarting as hall-' + j.name + '.local…' : 'Saved. Restarting with the new pixel count…')
      setTimeout(() => location.href = j.name ? 'http://hall-' + j.name + '.local/' : location.href, 7000)
    }).catch(() => setMsg('no reply'))
  }

  return <div className="card"><h2>Strip <small>set once per install</small></h2>
    <Field label="pixels on GPIO 4">{num('nA', 1, 4000)}</Field>
    <Field label="board name"><input placeholder="stairs" value={f.name} onChange={set('name')} /></Field>
    <Field label="pixels on GPIO 5">{num('nB', 0, 4000, '0 = off')}</Field>
    <Field label="colour order"><select value={f.ord} onChange={set('ord')}>{ORD.map((o, i) => <option key={o} value={i}>{o}</option>)}</select></Field>
    <Field label="GPIO 5 level %">{num('trim', 1, 100)}</Field>
    <Field label="fade ms">{num('fade', 0, 5000)}</Field>
    <Field label="supply limit mA (GPIO 4)">{num('lim', 0, 200000, '0 = none')}</Field>
    <Field label="GPIO 5 supply limit mA">{num('limB', 0, 200000, '0 = shares the one above')}</Field>
    <Field label="mA per pixel, full white">{num('pma', 1, 500)}</Field>
    <h2 style={{ marginTop: 14 }}>Runner <small>for Runner and Walk up</small></h2>
    <Field label="border colour"><input type="color" value={'#' + f.bc} onChange={e => setF({ ...f, bc: e.target.value.slice(1) })} /></Field>
    <Field label="border pixels each side">{num('bw', 0, 500)}</Field>
    <Field label="middle stripe pixels">{num('mw', 0, 2000, '0 = fill')}</Field>
    <div className="row"><button onClick={save}>Save</button></div>
    <p className="hint">Runner and Walk up use the Light colour for the middle stripe. Step edges are set in Steps above.</p>
    <p className="hint">Board name → hall-NAME.local and its own WiFi; changing it restarts the board.
      Pixels = chips, not LEDs: WS2811 neon often has 3 LEDs per chip. Changing a pixel count restarts the board.
      Colour order: pick Test, then tap Red — if the strip shows green or blue, try the next order.
      mA per pixel = reel W/m ÷ volts ÷ pixels per metre × 1000 (WS2812B 5 V ≈ 60).</p>
    <p className="hint">{msg}</p>
  </div>
}

// Steps: tap a step and the board lights only it (green = its first pixel, red = its last, neighbours dim blue).
// Slide its start, size and borders and watch the stairs; nothing is kept until Save.
const OUTS = { A: { label: 'Upper flight (GPIO 4)', n: 'nA', key: 'stA' }, B: { label: 'Landing + lower flight (GPIO 5)', n: 'nB', key: 'stB' } }
// map string: "start-end/startBorder/endBorder" per step (end included), top first; a plain "start" runs to the
// next step; no borders = the global border width (-1 here)
const toList = (str, n) => {
  const t = (str || '').split(',').map(x => x.trim()).filter(x => /^\d/.test(x)).map(x => {
    const [range, bs, be] = x.split('/'), [s, e] = range.split('-').map(Number)
    return { s, e, bs: bs ? +bs : -1, be: be ? +be : -1 }
  })
  return t.map((x, i) => ({ ...x, e: x.e >= 0 ? x.e : (i + 1 < t.length ? t[i + 1].s : n) - 1 }))
}
const toStr = l => l.map(x => x.s + '-' + x.e + (x.bs >= 0 || x.be >= 0 ? '/' + x.bs + '/' + x.be : '')).join(',')

function Steps({ st, show }) {
  const [map, setMap] = useState(() => ({ A: toList(st.stA, st.nA), B: toList(st.stB, st.nB) }))
  const [out, setOut] = useState('A')
  const [sel, setSel] = useState(null)
  const [msg, setMsg] = useState('')
  const timer = useRef(0)
  const list = map[out], n = st[OUTS[out].n]
  const name = i => out === 'B' && i === 0 ? 'Landing walls' : 'Step ' + (out === 'B' ? i : i + 1)

  const preview = (m, o, i) => {   // send the edited map + which step to light, a moment after the last tap
    clearTimeout(timer.current)
    timer.current = setTimeout(() => getJ('/step?' + qs({ stA: toStr(m.A), stB: toStr(m.B), out: o, i })).then(show).catch(() => {}), 120)
  }
  const pick = i => { setSel(i); preview(map, out, i) }
  // set one value of step i, kept inside its neighbours: start / size / border at the green end / border at the red end
  const bw = v => (v < 0 ? st.bw : v)
  const lo = i => (i ? list[i - 1].e + 1 : 0), hi = i => (i + 1 < list.length ? list[i + 1].s - 1 : n - 1)
  const setVal = (i, key, v) => {
    const l = list.map(x => ({ ...x })), x = l[i], clamp = (a, b) => Math.max(a, Math.min(b, v))
    if (key === 's') x.s = clamp(lo(i), x.e)
    else if (key === 'size') x.e = x.s + clamp(1, hi(i) - x.s + 1) - 1
    else x[key] = clamp(0, x.e - x.s + 1)
    const m = { ...map, [out]: l }; setMap(m); setMsg('not saved'); preview(m, out, i)
  }
  const save = () => getJ('/cfg?' + qs({ stA: toStr(map.A), stB: toStr(map.B) }))
    .then(j => { setMsg(j.err || 'Saved ✓'); show(j) }).catch(() => setMsg('no reply'))
  const done = () => { setSel(null); getJ('/step?done=1').then(show) }
  // a plain function, not a component, so React keeps the same slider while it's dragged
  const ctl = (i, k, label, v, min, max) => <div key={k} style={{ marginTop: 8 }}>
    <div className="bri" style={{ margin: 0 }}><span>{label}</span><b style={{ fontSize: 20 }}>{v}</b></div>
    <input type="range" min={min} max={max} value={v} onChange={e => setVal(i, k, +e.target.value)} />
    <div className="row" style={{ marginTop: 2 }}>{[-10, -1, 1, 10].map(d =>
      <button key={d} style={{ padding: '6px 2px' }} onClick={() => setVal(i, k, v + d)}>{d > 0 ? '+' + d : d}</button>)}</div>
  </div>
  const gap = i => i + 1 < list.length ? list[i + 1].s - list[i].e - 1 : 0

  return <div className="card"><h2>Steps <small>tap one, nudge its edges, Save</small></h2>
    <div className="row">{Object.keys(OUTS).map(o =>
      <button key={o} className={o === out ? 'on' : ''} onClick={() => { setOut(o); setSel(null) }}>{OUTS[o].label}</button>)}</div>
    {list.map((x, i) => <div key={i}>
      <div className="row"><button className={i === sel ? 'on' : ''} onClick={() => pick(i)} style={{ textAlign: 'left' }}>
        {name(i)} · {x.s}–{x.e} <small>({x.e - x.s + 1} px)</small></button></div>
      {i === sel && <>
        {ctl(i, 's', 'Start (green)', x.s, lo(i), x.e)}
        {ctl(i, 'size', `Size (red at ${x.e})`, x.e - x.s + 1, 1, hi(i) - x.s + 1)}
        {ctl(i, 'bs', 'Border at the green end', bw(x.bs), 0, Math.min(80, x.e - x.s + 1))}
        {ctl(i, 'be', 'Border at the red end', bw(x.be), 0, Math.min(80, x.e - x.s + 1))}
      </>}
      {gap(i) > 0 && <p className="hint" style={{ margin: '2px 0 0 10px' }}>↳ {gap(i)} px dark (loop / corner)</p>}
    </div>)}
    <div className="row"><button onClick={save}>Save</button><button onClick={done}>Done</button></div>
    <p className="hint">Top step first. The lit step shows its borders dim yellow. Move green onto the first pixel you want lit at one end of the step and red onto the last at the other end. Anything between one step's red and the next step's green stays dark, for a loop of strip round a corner. Done goes back to the lights; Save keeps the map.</p>
    <p className="hint">{msg}</p>
  </div>
}

function Wifi({ st, go }) {
  const n = st.net || {}
  const [ssid, setSsid] = useState(n.ssid || HALL)
  const [pass, setPass] = useState('')
  const [aps, setAps] = useState([])
  const [scanTxt, setScanTxt] = useState('Scan')
  const passRef = useRef()
  const preset = s => { setSsid(s); passRef.current.focus() }
  const scan = () => { setScanTxt('scanning…'); getJ('/scan').then(a => { setAps(a); setScanTxt(a.length + ' found') }).catch(() => setScanTxt('Scan')) }

  return <div className="card"><h2>Hall WiFi <small>{n.up ? `on ${n.ssid} · ${n.ip} · ${st.host}.local`
    : n.ssid ? `joining ${n.ssid}…${n.why ? ' (' + (WHY[n.why] || 'reason ' + n.why) + ')' : ''}` : 'not set, own WiFi only'}</small></h2>
    <Field label="network"><input list="aps" value={ssid} onChange={e => setSsid(e.target.value)} /></Field>
    <datalist id="aps">{aps.map(a => <option key={a} value={a} />)}</datalist>
    <Field label="password"><input ref={passRef} type="password" value={pass} onChange={e => setPass(e.target.value)} /></Field>
    <div className="row"><button onClick={() => preset(HALL)}>{HALL}</button><button onClick={() => preset(HOME)}>{HOME}</button><button onClick={scan}>{scanTxt}</button></div>
    <div className="row"><button onClick={() => ssid && go('/wifi?' + qs({ ssid, pass }))}>Join</button>
      <button onClick={() => confirm('Forget the hall WiFi?') && go('/wifi?forget=1')}>Forget</button></div>
  </div>
}

function Mqtt({ st, go }) {
  const m = st.mqtt || {}
  const [f, setF] = useState({ host: m.host || '', port: m.port || 1883, user: m.user || '', pass: '' })
  const set = k => e => setF({ ...f, [k]: e.target.value })
  const save = () => { const o = { host: f.host, port: f.port || 1883, user: f.user }; if (f.pass) o.pass = f.pass; go('/mqtt?' + qs(o)); setF({ ...f, pass: '' }) }

  return <div className="card"><h2>MQTT <small>{m.host ? (m.up ? 'connected' : 'connecting…') : 'off'}</small></h2>
    <Field label="broker"><input placeholder="blank = off" value={f.host} onChange={set('host')} /></Field>
    <Field label="port"><input type="number" value={f.port} onChange={set('port')} /></Field>
    <Field label="user"><input value={f.user} onChange={set('user')} /></Field>
    <Field label="password"><input type="password" placeholder="unchanged" value={f.pass} onChange={set('pass')} /></Field>
    <div className="row"><button onClick={save}>Save</button></div>
    <p className="hint">Send to <b>ragamuffin/hall/{st.name}/set</b>: <code>c=ff9329&amp;b=30</code> · <code>m=1&amp;s=40</code> (mode 0–14, speed 1–100) · <code>b=0</code> · <code>test=1</code>. State is on <b>…/state</b>.</p>
  </div>
}

function Firmware({ st }) {
  const [latest, setLatest] = useState('Install latest')
  const [upTxt, setUpTxt] = useState('Upload file')
  const [file, setFile] = useState(null)

  const upload = (blob, say) => {
    const x = new XMLHttpRequest(), d = new FormData(); d.append('f', blob, 'stairs.bin')
    x.upload.onprogress = e => say('writing ' + Math.round(e.loaded * 100 / e.total) + '%')
    x.onload = () => { say(x.responseText + ' — rebooting'); setTimeout(() => location.reload(), 6000) }
    x.onerror = () => say('upload failed')
    x.open('POST', '/update'); x.send(d)
  }
  const installLatest = async () => {
    try {
      setLatest('checking…')
      const m = await (await fetch(LATEST + 'manifest.json', { cache: 'no-store' })).json()
      if (m.version === st.fw && !confirm('Already on ' + m.version + '. Reinstall?')) return setLatest('up to date (' + m.version + ')')
      setLatest('downloading ' + m.version + '…')
      const r = await fetch(LATEST + 'stairs.bin', { cache: 'no-store' }); if (!r.ok) throw 0
      upload(await r.blob(), setLatest)
    } catch { setLatest('no internet — mobile data on, open in Chrome') }
  }

  return <div className="card"><h2>Firmware <small>running {st.fw}</small></h2>
    <div className="row"><button onClick={installLatest}>{latest}</button></div>
    <p className="hint">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up).</p>
    <input type="file" accept=".bin" onChange={e => setFile(e.target.files[0])} />
    <div className="row"><button onClick={() => file && upload(file, setUpTxt)}>{upTxt}</button></div>
  </div>
}
