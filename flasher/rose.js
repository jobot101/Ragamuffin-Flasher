// The Ragamuffin Hall rose window, drawn to match the logo (brand/ragamuffin-hall-logo.png):
// 16 cusped petals clockwise from just right of 12 o'clock: yellow, green, blue, red; a white ring; a hub of five
// glass wedges behind a white star with a small black star; a black ring; a red/blue dashed outer ring.
// One source for both the flasher page (live, petals as the progress meter) and brand/ragamuffin-hall-window.svg.
export const BRAND = { red: '#c62b29', yellow: '#fdcd09', blue: '#60b6e5', green: '#8dad3e', ink: '#060709', paper: '#ffffff' }

const { red, yellow, blue, green, ink, paper } = BRAND
const f = (n) => +n.toFixed(2)
const P = (r, a) => [f(r * Math.cos(a)), f(r * Math.sin(a))]
const deg = Math.PI / 180

function star(R, r, tilt) {
  const pts = []
  for (let k = 0; k < 10; k++) pts.push(P(k % 2 ? r : R, (-90 + tilt + k * 36) * deg).join(' '))
  return `M ${pts.join(' L ')} Z`
}

// one petal pointing straight up (local coords), rotated into place
function petalPath() {
  const h = (Math.PI / 16) * 0.8, r1 = 37, re = 69
  const il = [-r1 * Math.sin(h), -r1 * Math.cos(h)], ol = [-re * Math.sin(h), -re * Math.cos(h)]
  const xs = Math.abs(ol[0]) * 0.47, ny = -(re + 5.5), lobe = 4.3, dome = 6.1
  return [
    `M ${f(il[0])} ${f(il[1])}`, `L ${f(ol[0])} ${f(ol[1])}`,
    `A ${lobe} ${lobe} 0 0 1 ${f(-xs)} ${f(ny)}`,            // left shoulder
    `A ${dome} ${dome} 0 1 1 ${f(xs)} ${f(ny)}`,              // dome
    `A ${lobe} ${lobe} 0 0 1 ${f(-ol[0])} ${f(ol[1])}`,     // right shoulder
    `L ${f(-il[0])} ${f(il[1])}`, `A ${r1} ${r1} 0 0 0 ${f(il[0])} ${f(il[1])} Z`,
  ].join(' ')
}

export const PETAL_COLORS = [yellow, green, blue, red]

export function roseSVG({ standalone = false } = {}) {
  const sw = 2.4, petal = petalPath(), C = 2 * Math.PI * 95.5, dash = C / 64
  const petals = Array.from({ length: 16 }, (_, i) =>
    `<path class="petal" d="${petal}" fill="${PETAL_COLORS[i % 4]}" transform="rotate(${11.25 + i * 22.5})"/>`).join('')
  const tilt = -6, hub = [yellow, green, blue, red, blue]
  const wedges = hub.map((c, k) => {
    const a0 = (-90 + tilt + k * 72) * deg, a1 = a0 + 72 * deg, [x0, y0] = P(23, a0), [x1, y1] = P(23, a1)
    return `<path d="M 0 0 L ${x0} ${y0} A 23 23 0 0 1 ${x1} ${y1} Z" fill="${c}"/>`
  }).join('')
  const body = `
  <circle r="101" fill="${paper}"/>
  <circle class="dash-b" r="95.5" fill="none" stroke="${blue}" stroke-width="6"/>
  <circle class="dash-r" r="95.5" fill="none" stroke="${red}" stroke-width="6" stroke-dasharray="${f(dash)} ${f(dash)}"/>
  <circle r="92.3" fill="none" stroke="${ink}" stroke-width="1.5"/><circle r="98.7" fill="none" stroke="${ink}" stroke-width="1.5"/>
  <circle r="87.5" fill="none" stroke="${ink}" stroke-width="${sw}"/>
  <g class="petals" stroke="${ink}" stroke-width="${sw}" stroke-linejoin="round">${petals}</g>
  <circle r="34.5" fill="${paper}" stroke="${ink}" stroke-width="${sw}"/>
  <g class="hub">${wedges}<path d="${star(23, 9.5, tilt)}" fill="${paper}" stroke="${ink}" stroke-width="1.7" stroke-linejoin="miter"/>
  <path d="${star(6.5, 2.7, tilt)}" fill="${ink}"/><circle r="23" fill="none" stroke="${ink}" stroke-width="${sw}"/></g>`
  return standalone
    ? `<svg xmlns="http://www.w3.org/2000/svg" viewBox="-102 -102 204 204" width="1024" height="1024">${body}\n</svg>\n`
    : body
}
