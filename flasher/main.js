// Ragamuffin Flasher: one-tap ESP32-S3 flasher for phones (https://jobot101.github.io/Ragamuffin-Flasher/)
//   arm brain = Waveshare ESP32-S3-AUDIO-Board  (native USB-Serial-JTAG, 303A:1001, 16 MB flash)  } built by
//   camera    = Hiwonder ESP32-S3 vision module (4 MB flash; its USB may be native or a CH340 bridge) } jobot101/xarm,
//                                                       read from its Pages site (CORS *)
//   solenoid  = solenoid tester for any ESP32-S3 board (4 MB image, runs on any flash size; solenoid/ here)
//   chime     = hall chimes: MIDI in + 4 MCP23017 solenoid drivers on any ESP32-S3 (4 MB image; chime/ here)
// Android Chrome can't reach these through Web Serial, so we open them over WebUSB:
//   303A (native)  -> Google's web-serial-polyfill (CDC-ACM)
//   1A86 (CH340)   -> esptool-js's own WebUSBSerialPort
import { ESPLoader, Transport, WebUSBSerialPort } from 'esptool-js'
import { SerialPort as CdcPort } from 'web-serial-polyfill'
import './style.css'

const ESPRESSIF = 0x303a, WCH = 0x1a86
const XARM = 'https://jobot101.github.io/xarm/'   // Muffin's builds stay in the xarm repo
const BOARDS = {
  arm: { base: XARM + 'fw/', label: 'arm brain', expectMB: 16, appOffset: 0x10000 },
  cam: { base: XARM + 'cam/', label: 'camera', expectMB: 4 },
  sol: { base: './sol/', label: 'solenoid tester', expectMB: 0 },   // 0 = any flash size
  chime: { base: './chime/', label: 'hall chimes', expectMB: 0 },
}

const $ = (id) => document.getElementById(id)
const logEl = $('log'), statusEl = $('status'), barEl = $('bar'), flashBtn = $('flash'), monBtn = $('monitor')
const log = (s = '') => { logEl.textContent += s; logEl.scrollTop = logEl.scrollHeight }
const status = (s, cls = '') => { statusEl.textContent = s; statusEl.className = 'status ' + cls }
const progress = (f) => { barEl.style.width = `${Math.round(f * 100)}%`; roseFill(f) }

// The Ragamuffin Hall rose window: 16 scalloped petals of stained glass around a five-pointed star.
// It doubles as the progress meter: while flashing, the petals light one by one clockwise from the top.
const PETALS = 16, GLASS = ['#2a4f9e', '#a3213a'], ROUNDEL = ['#e3a531', '#2f8f5f']   // sapphire/ruby petals, amber/emerald roundels
const petals = []
function drawRose() {
  const svg = $('rose'), NS = 'http://www.w3.org/2000/svg', r1 = 40, r2 = 84, half = (Math.PI / PETALS) * 0.86
  const pt = (r, a) => `${(r * Math.cos(a)).toFixed(2)} ${(r * Math.sin(a)).toFixed(2)}`
  const el = (tag, attrs) => { const e = document.createElementNS(NS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); svg.append(e); return e }
  svg.innerHTML = `<defs><radialGradient id="shine" cx="38%" cy="30%" r="75%">
    <stop offset="0" stop-color="#fff6dc" stop-opacity=".32"/><stop offset=".55" stop-color="#fff6dc" stop-opacity="0"/>
    <stop offset="1" stop-color="#000" stop-opacity=".35"/></radialGradient></defs>`
  el('circle', { r: 95, fill: '#1c120c' })
  for (let i = 0; i < PETALS; i++) {
    const a = (i / PETALS) * 2 * Math.PI - Math.PI / 2, l = a - half, r = a + half
    const chord = 2 * r2 * Math.sin(half), scallop = (chord / 2) * 1.08
    const g = el('g', { class: 'petal' })
    const path = document.createElementNS(NS, 'path'), ro = document.createElementNS(NS, 'circle')
    path.setAttribute('fill', GLASS[i % 2])
    path.setAttribute('d', `M ${pt(r1, l)} L ${pt(r2, l)} A ${scallop} ${scallop} 0 0 1 ${pt(r2, r)} L ${pt(r1, r)} A ${r1} ${r1} 0 0 0 ${pt(r1, l)} Z`)
    const [cx, cy] = pt(r2 - 13, a).split(' ')
    Object.entries({ cx, cy, r: 5.6, fill: ROUNDEL[Math.floor(i / 2) % 2] }).forEach(([k, v]) => ro.setAttribute(k, v))
    g.append(path, ro); petals.push(g)
  }
  el('circle', { class: 'ring in', r: r1 - 3 })
  el('circle', { class: 'hub', r: r1 - 5 })
  const star = Array.from({ length: 10 }, (_, k) => pt(k % 2 ? 12.5 : 30, (k / 10) * 2 * Math.PI - Math.PI / 2)).join(' L ')
  el('path', { class: 'star', d: `M ${star} Z` })
  el('circle', { class: 'ring', r: 95 })
  el('circle', { class: 'shine', r: 95, fill: 'url(#shine)' })
}
function roseFill(f) { const n = Math.round(f * PETALS); petals.forEach((p, i) => p.classList.toggle('lit', i < n)) }
drawRose()
const terminal = { clean() { logEl.textContent = '' }, writeLine: (s) => log(s + '\n'), write: (s) => log(s) }

let board = 'arm'
let manifests = {}          // arm: manifest, cam: { qspi, opi }, sol: manifest, chime: manifest
let busy = false

const getJSON = async (u) => (await fetch(u, { cache: 'no-store' })).json()

async function loadManifests() {
  try { manifests.arm = await getJSON(BOARDS.arm.base + 'manifest.json') } catch {}
  try {
    manifests.cam = { qspi: await getJSON(BOARDS.cam.base + 'qspi/manifest.json'), opi: await getJSON(BOARDS.cam.base + 'opi/manifest.json') }
  } catch {}
  try { manifests.sol = await getJSON(BOARDS.sol.base + 'manifest.json') } catch {}
  try { manifests.chime = await getJSON(BOARDS.chime.base + 'manifest.json') } catch {}
  render(true)
}

function render(hint = false) {
  document.body.dataset.board = board
  document.querySelectorAll('.board').forEach(b => { b.classList.toggle('on', b.dataset.board === board); b.setAttribute('aria-checked', b.dataset.board === board) })
  $('modes').style.display = board === 'arm' ? '' : 'none'
  const m = board === 'cam' ? manifests.cam?.qspi : manifests[board]
  $('fw').textContent = m ? `build ${m.version}` : 'files missing'
  flashBtn.disabled = !m || busy
  flashBtn.textContent = { arm: 'Flash the arm brain', cam: 'Flash the camera', sol: 'Flash the solenoid', chime: 'Flash the hall chimes' }[board]
  if (hint) status({
    arm: 'Plug the Waveshare board (round, with the speaker) into this phone.',
    cam: 'Plug the camera module into this phone by its own USB-C. Unplug it from the arm first.',
    sol: 'Plug the ESP32-S3 board into this phone. Two USB ports? Either works. Unplug the solenoid supply first.',
    chime: 'Plug the chimes ESP32-S3 into this phone. Two USB ports? Either works. Switch the 24 V coil supply off first.',
  }[board])
}

document.querySelectorAll('.board').forEach(b => b.addEventListener('click', () => { if (!busy) { board = b.dataset.board; render(true) } }))

// How this browser reaches the board:
//   Android Chrome         -> WebUSB (its Web Serial can't open the S3's native USB)
//   desktop Chrome / Edge  -> Web Serial (the OS driver owns CDC, so WebUSB can't claim it)
//   iPhone / iPad / Safari / Firefox -> none: WiFi updates only
const ANDROID = /Android/i.test(navigator.userAgent)
const VIA = ANDROID && 'usb' in navigator ? 'webusb' : 'serial' in navigator ? 'serial' : 'usb' in navigator ? 'webusb' : null

async function pickPort() {
  if (VIA === 'serial') {
    const filters = [{ usbVendorId: ESPRESSIF }, { usbVendorId: WCH }]
    const known = (await navigator.serial.getPorts()).filter(p => filters.some(f => f.usbVendorId === p.getInfo().usbVendorId))
    return known[0] || navigator.serial.requestPort({ filters })
  }
  if (VIA !== 'webusb') throw new Error("This browser can't reach USB. Use Chrome on Android, or Chrome/Edge on a computer.")
  return pickUsb()
}

// pick a board over WebUSB and wrap it as a Web-Serial-style port esptool-js understands
async function pickUsb() {
  const known = (await navigator.usb.getDevices()).filter(d => d.vendorId === ESPRESSIF || d.vendorId === WCH)
  const dev = known[0] || await navigator.usb.requestDevice({ filters: [{ vendorId: ESPRESSIF }, { vendorId: WCH }] })
  if (dev.vendorId === WCH) return new WebUSBSerialPort(dev).asSerialPort()
  return new CdcPort(dev)
}

async function fetchBin(base, path) {
  const r = await fetch(base + path, { cache: 'no-store' })
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`)
  return new Uint8Array(await r.arrayBuffer())
}

async function flash() {
  if (busy) return
  busy = true; render(); monBtn.disabled = true
  document.body.classList.remove('done'); document.body.classList.add('flashing')
  const B = BOARDS[board]
  const mode = document.querySelector('input[name=mode]:checked').value
  const full = board !== 'arm' || mode !== 'app'           // write every part
  const erase = board !== 'arm' || mode === 'full'          // and wipe the chip first ('keep' leaves nvs + store alone)
  let transport
  try {
    terminal.clean(); progress(0)
    status('Pick the board in the list and allow access…')
    const port = await pickPort()

    status('Connecting…')
    transport = new Transport(port, false)
    // USB-Serial-JTAG ignores baud rate, so stay at 115200: no mid-session port reopen
    const loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal })
    log(`connected: ${await loader.main()}\n`)

    // sanity: is this the board we think it is? (wrong firmware on the wrong board won't boot)
    const features = (await loader.chip.getChipFeatures(loader)).join(', ')
    const flashMB = parseInt(loader.DETECTED_FLASH_SIZES[(await loader.readFlashId()) >> 16 & 0xff] || '0')
    log(`features: ${features}\nflash: ${flashMB} MB\n`)
    if (flashMB && B.expectMB && flashMB !== B.expectMB &&
        !confirm(`This board has ${flashMB} MB flash — the ${B.label} should have ${B.expectMB} MB. Is it the right board? Flash anyway?`))
      throw new Error('cancelled — wrong board?')

    let man = manifests[board], base = B.base
    if (board === 'cam') {   // octal PSRAM (8 MB, 1.8 V) vs quad (2 MB) — pick the matching build
      const opi = /PSRAM 8MB|Octal|1v8/i.test(features)
      man = opi ? manifests.cam.opi : manifests.cam.qspi
      base = B.base + (opi ? 'opi/' : 'qspi/')
      log(`camera build: ${opi ? 'octal' : 'quad'} PSRAM\n`)
    }

    status('Downloading firmware…')
    const parts = full ? man.builds[0].parts : man.builds[0].parts.filter(p => p.offset === B.appOffset)
    const files = []
    for (const p of parts) {
      files.push({ data: await fetchBin(base, p.path), address: p.offset })
      log(`fetched ${p.path} (${files.at(-1).data.length} bytes) -> 0x${p.offset.toString(16)}\n`)
    }
    const total = files.reduce((n, f) => n + f.data.length, 0)
    const before = files.map((_, i) => files.slice(0, i).reduce((n, f) => n + f.data.length, 0))

    status(erase ? 'Erasing + writing (about 1–2 min)…' : full ? 'Writing everything (about 1–2 min)…' : 'Writing app…')
    await loader.writeFlash({
      fileArray: files, flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
      eraseAll: erase, compress: true,
      reportProgress: (i, written, size) => progress((before[i] + (written / size) * files[i].data.length) / total),
    })
    progress(1)
    status('Done. Rebooting…', 'ok')
    await loader.after('hard_reset')
    status({
      arm: 'Done ✓ — listen for “Ready to rumble!”, then join WiFi “Muffin” (pw ragamuffin) → 192.168.4.1',
      cam: 'Camera done ✓ — plug it back onto the arm (5V, GND, SDA, SCL). If nothing happens, tap its reset.',
      sol: 'Solenoid done ✓ — join WiFi “Solenoid” (pw ragamuffin) → 192.168.4.1. MOSFET gate on GPIO 4 (change it there). BOOT button = strike.',
      chime: 'Chimes done ✓ — join WiFi “Chimes” (pw ragamuffin) → 192.168.4.1: drivers found, test tubes, hall WiFi, MQTT, Install latest. MIDI in on GPIO 18, I2C SDA 8 / SCL 9.',
    }[board], 'ok')
    document.body.classList.add('done')
  } catch (e) {
    console.error(e)
    log(`\nERROR: ${e.message || e}\n`)
    const msg = String(e.message || e)
    if (/No device selected|requestDevice|No port selected|requestPort/.test(msg)) status('No board picked. Check the cable, or do BOOT+RESET and try again.', 'bad')
    else status('Failed: ' + msg + ' — try BOOT+RESET, then Flash again.', 'bad')
  } finally {
    try { await transport?.disconnect() } catch {}
    busy = false; render(); monBtn.disabled = false
    document.body.classList.remove('flashing')
  }
}

// read the board's boot log (115200)
let monitorPort = null
async function monitor() {
  if (monitorPort) { try { await monitorPort.close() } catch {} monitorPort = null; monBtn.textContent = 'Boot log'; return }
  try {
    const port = await pickPort()
    await port.open({ baudRate: 115200 })
    monitorPort = port; monBtn.textContent = 'Stop log'
    terminal.clean(); log('--- boot log (tap RESET on the board to see it from the start) ---\n')
    const reader = port.readable.getReader(), dec = new TextDecoder()
    while (monitorPort) {
      const { value, done } = await reader.read()
      if (done) break
      log(dec.decode(value, { stream: true }))
    }
    reader.releaseLock()
  } catch (e) {
    log(`\nlog stopped: ${e.message || e}\n`)
    monitorPort = null; monBtn.textContent = 'Boot log'
  }
}

flashBtn.addEventListener('click', flash)
monBtn.addEventListener('click', monitor)
if (!VIA) {
  // no USB at all (iPhone, iPad, Safari, Firefox): WiFi card first, USB tools out of the way
  document.body.classList.add('no-usb')
  $('nousb').hidden = false
  document.querySelector('.usb').hidden = true
  $('wifi').after($('nousb'))
}
// iPhones resolve muffin.local; many Androids don't, but the router usually answers http://muffin
const IOS = /iPhone|iPad|iPod/i.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)
if (!IOS) $('open-home').href = 'http://muffin/#update'
$('share').addEventListener('click', async () => {
  const url = location.href.split('#')[0]
  try { if (navigator.share) return await navigator.share({ title: 'Ragamuffin Flasher', url }) } catch { return }
  try { await navigator.clipboard.writeText(url); $('share').textContent = 'Link copied' } catch { prompt('Copy this link', url) }
})
loadManifests()
