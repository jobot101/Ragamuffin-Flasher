# Ragamuffin Hall — brand

The reference is **`ragamuffin-hall-logo.png`**, the logo exactly as Joseph sent it on Nov 2 2026. Match it; don't reinterpret it.

## The mark
- **Rose window:** a circle drawn in black ink linework on white, with flat stained-glass colours (no gradients, no shading).
- **16 cusped petals,** clockwise from just right of 12 o'clock: **yellow, green, blue, red**, repeated four times.
  - Each petal has its own black outline, with a white gap between neighbours.
  - Each petal ends in two small rounded shoulders and a round dome on top.
- **Hub:**
  - a thick white ring with black lines on both sides;
  - inside it, a circle of five glass wedges, clockwise from the top: yellow, green, blue, red, blue;
  - a white five-pointed star with a black outline over the wedges, and a small solid black star in the middle.
- **Rings:** one black line around the petals, a white band, then an outer ring of **alternating red and blue dashes** between two thin black lines.
- **Wordmark:** **RAGAMUFFIN** in a black rock-gothic/western display face with spiky flared serifs. **HALL** sits underneath, smaller, between two black rules that taper to points at their outer ends.
  - The exact typeface is unknown. The web stand-in is **New Rocker** (Google Fonts / @fontsource/new-rocker).
  - If the real font file turns up, drop it in and swap it.

## Colours (sampled from the logo)
| | hex |
|---|---|
| red | `#c62b29` |
| yellow | `#fdcd09` |
| blue | `#60b6e5` |
| green | `#8dad3e` |
| ink | `#060709` |
| paper | `#ffffff` |

## Files
- `ragamuffin-hall-logo.png`: the original (a screenshot, 516×455; it has a Google UI scrap at the bottom left).
- `ragamuffin-hall-window.svg`: the window redrawn exactly as vector art, at any size. It's generated from `flasher/rose.js` (`roseSVG({ standalone: true })`), which is also what the flasher page draws.

## Using it on pages and screens
- White paper, black ink outlines (about 2.5 px at phone size), and the four glass colours flat. The rose window is the one loud element.
- Headings use the wordmark face. Body text is a plain humanist sans (Alegreya Sans).
- When a page needs one colour per thing (like the flasher's boards), take them from the four glass colours.
- The red/blue dashes of the outer ring work as a secondary motif (the flasher's progress bar).
