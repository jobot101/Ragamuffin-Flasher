// ui.cpp: the BOX screen, kept simple for the owner. Core 0, a 10 KB task, about 25 passes a second.
// Portrait 240 x 320. Top bar, then one of four tabs: HOME | SOLENOIDS | PATTERNS | PANIC.
//   HOME = one big state word, one plain sentence, the address, and one big button (TEST ALL / STOP TEST / UNMUTE).
//   SOLENOIDS = a coloured square per output. PATTERNS = play or stop the four saved patterns.
//   PANIC = panic and mute at once, then HOME. All editing and settings are on the phone page.
// The screen never switches a coil. It only sends Msg, calls requestPanic()/requestUnmute(), and starts or stops
// the cal.cpp walk. Touch acts on the press only (never while held), except PANIC, which panics on any touch sample.
// Everything is redrawn only when it changed.
#include "chimes.h"
#include <LovyanGFX.hpp>

// ---------------- the BOX's screen + touch (copied from the working one-solenoid firmware) ----------------
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
static LGFX_Box lcd;

// 0 = portrait (240 x 320, the owner's choice: one quarter-turn counter-clockwise from the BOX's landscape 1).
static const uint8_t SCREEN_ROT = 0;

namespace {
struct UiBtn { int16_t x, y, w, h; };
}

// ---------------- colours: the Ragamuffin Workshop tokens (BRAND-UI.md), set in initColours() ----------------
static uint16_t C_BG, C_PAPER, C_PAPER2, C_LINE, C_RULE, C_INK, C_MUTED, C_GOLD, C_GOLDINK,
                C_OK, C_WARN, C_BAD;
static uint16_t L_RED, L_YEL, L_BLU, L_GRN;          // lamps and the emblem: the four glass colours, flat

static void initColours() {
  C_BG      = lcd.color565(0x0c, 0x0b, 0x0a);  // bg
  C_PAPER   = lcd.color565(0x15, 0x13, 0x11);  // panels
  C_PAPER2  = lcd.color565(0x1d, 0x1a, 0x17);  // raised controls
  C_LINE    = lcd.color565(0x2c, 0x28, 0x23);  // hairlines, an off lamp
  C_RULE    = lcd.color565(0x4a, 0x42, 0x38);  // a dead arrow
  C_INK     = lcd.color565(0xef, 0xe7, 0xd6);  // main text (cream)
  C_MUTED   = lcd.color565(0xa8, 0x9f, 0x8e);  // labels
  C_GOLD    = lcd.color565(0xc4, 0x9a, 0x45);  // the accent
  C_GOLDINK = lcd.color565(0x17, 0x12, 0x0a);  // text on gold
  C_OK      = lcd.color565(0x8f, 0xbf, 0x7a);
  C_WARN    = lcd.color565(0xe0, 0xb4, 0x5c);
  C_BAD     = lcd.color565(0xe0, 0x70, 0x5c);
  L_RED = lcd.color565(0xc6, 0x2b, 0x29); L_YEL = lcd.color565(0xfd, 0xcd, 0x09);
  L_BLU = lcd.color565(0x60, 0xb6, 0xe5); L_GRN = lcd.color565(0x8d, 0xad, 0x3e);
}

// ---------------- layout (worked out from the screen's size in layout()) ----------------
static const int BAR_H = 22, TAB_H = 48;
static int scrW = 240, scrH = 320;
static int CT = 23, CB = 271, TY = 272;             // content top, content bottom (exclusive), tab bar top

enum { T_HOME, T_SOL, T_PAT, T_PANIC, N_TABS };
static const char* const TAB_NAMES[N_TABS] = {"HOME", "COILS", "PLAY", "PANIC"};   // short, so PANIC is the widest
static int tab = T_HOME;
static int tabX[N_TABS + 1];                         // tab t spans tabX[t] .. tabX[t + 1]
static int tabFont = 2;

// HOME geometry
static UiBtn bBig;                                   // TEST ALL / STOP TEST / UNMUTE
static int H_WORD_Y, H_SENT_Y, H_IP_Y, H_PROG_Y;
static const int H_WORD_H = 50, H_LINE_H = 24, H_SENT_N = 3;

// SOLENOIDS geometry: 5 columns of squares
static const int S_COLS = 5;
static int S_PITCH, S_SIZE, S_X0, S_Y0, S_LEG_Y;   // pitch, square size, grid origin, legend line ("SS" is a pin macro)
static UiBtn bPrev, bNext;                           // only while there are more outputs than fit

// PATTERNS geometry
static UiBtn bSlot[SEQ_SLOTS];
static int P_INFO_Y;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static bool inB(const UiBtn& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static bool before(uint32_t ms, uint32_t until) { return (int32_t)(until - ms) > 0; }

// ---------------- fonts ----------------
// 0 Font0 (6 x 8, tiny), 2 Font2 (UI words), 4 Font4 (big UI words), the rest GFX fonts
enum { F_TINY = 0, F_UI = 2, F_BIG = 4, F_MONO = 9, F_SERIF = 18,
       F_SANS12 = 30, F_SANSB12 = 31, F_SANSB18 = 32, F_SANSB24 = 33 };
static void useFont(int f) {
  switch (f) {
    case F_TINY:    lcd.setFont(&fonts::Font0); break;
    case F_BIG:     lcd.setFont(&fonts::Font4); break;
    case F_MONO:    lcd.setFont(&fonts::FreeMono9pt7b); break;           // the address
    case F_SERIF:   lcd.setFont(&fonts::FreeSerifBold9pt7b); break;      // the bar's name
    case F_SANS12:  lcd.setFont(&fonts::FreeSans12pt7b); break;          // HOME's sentence
    case F_SANSB12: lcd.setFont(&fonts::FreeSansBold12pt7b); break;      // solenoid numbers
    case F_SANSB18: lcd.setFont(&fonts::FreeSansBold18pt7b); break;      // the big button
    case F_SANSB24: lcd.setFont(&fonts::FreeSansBold24pt7b); break;      // HOME's state word
    default:        lcd.setFont(&fonts::Font2); break;
  }
}

// the first font of the list (ending in -1) that fits s in maxPx; the last one otherwise
static int fitFont(const char* s, int maxPx, const int* fl) {
  int f = fl[0];
  for (int k = 0; fl[k] >= 0; k++) {
    f = fl[k]; useFont(f);
    if (lcd.textWidth(s) <= maxPx) return f;
  }
  return f;
}

// cut a string until it fits maxPx in the current font
static void fitText(char* s, int maxPx) {
  size_t n = strlen(s);
  while (n && lcd.textWidth(s) > maxPx) s[--n] = 0;
}

// every fill comes first, so text is drawn without a background box (GFX fonts would overrun the row)
static void drawBtn(const UiBtn& b, const char* s, uint16_t bg, uint16_t fg, int font, int border = -1) {
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
  if (border >= 0) lcd.drawRoundRect(b.x, b.y, b.w, b.h, 6, (uint16_t)border);
  if (!s || !*s) return;
  useFont(font); lcd.setTextDatum(middle_center); lcd.setTextColor(fg);
  lcd.drawString(s, b.x + b.w / 2, b.y + b.h / 2 + 1);
}

// dir: 2 up, 3 down
static void drawArrow(const UiBtn& b, int dir, bool live) {
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 6, live ? C_PAPER2 : C_PAPER);
  lcd.drawRoundRect(b.x, b.y, b.w, b.h, 6, C_LINE);
  uint16_t fg = live ? C_INK : C_RULE;
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2, r = (b.h < b.w ? b.h : b.w) / 4;
  if (dir == 2) lcd.fillTriangle(cx, cy - r, cx - r - 2, cy + r, cx + r + 2, cy + r, fg);
  else          lcd.fillTriangle(cx, cy + r, cx - r - 2, cy - r, cx + r + 2, cy - r, fg);
}

// one cached text line: redrawn only when its text or colour changed. align 0 left, 1 centre. Cut to fit.
static const int NLINES = 12;
static char     lnTxt[NLINES][48];
static uint16_t lnCol[NLINES];
static bool     lnOk[NLINES];
static void invalidateLines() { for (int k = 0; k < NLINES; k++) lnOk[k] = false; }
static void txtLineAt(int k, int x, int y, int w, int h, int font, uint16_t fg, uint16_t bg, const char* s, int align) {
  if (k < 0 || k >= NLINES) return;
  char t[48];
  strlcpy(t, s, sizeof t);
  if (lnOk[k] && lnCol[k] == fg && !strcmp(lnTxt[k], t)) return;
  memcpy(lnTxt[k], t, sizeof t); lnCol[k] = fg; lnOk[k] = true;
  lcd.fillRect(x, y, w, h, bg);
  useFont(font); lcd.setTextColor(fg);
  fitText(t, w - 8);
  int cy = y + h / 2;
  if (align == 1) { lcd.setTextDatum(middle_center); lcd.drawString(t, x + w / 2, cy); }
  else            { lcd.setTextDatum(middle_left);   lcd.drawString(t, x + 4, cy); }
}

static void clearContent() { lcd.fillRect(0, CT, scrW, CB - CT, C_BG); }

static void layout() {
  scrW = lcd.width(); scrH = lcd.height();
  CT = BAR_H + 1; TY = scrH - TAB_H; CB = TY - 1;

  // tabs: each as wide as its word plus a margin; the spare room shared out, PANIC taking a double share
  int tw[N_TABS], sum = 0;
  tabFont = F_UI;
  for (int pass = 0; pass < 2; pass++) {
    useFont(tabFont); sum = 0;
    for (int t = 0; t < N_TABS; t++) { tw[t] = lcd.textWidth(TAB_NAMES[t]) + 10; sum += tw[t]; }
    if (sum <= scrW) break;
    tabFont = F_TINY;
  }
  int spare = scrW - sum > 0 ? scrW - sum : 0, x = 0;
  for (int t = 0; t < N_TABS; t++) {
    tabX[t] = x;
    x += tw[t] + spare * (t == T_PANIC ? 2 : 1) / (N_TABS + 1);
  }
  tabX[N_TABS] = scrW;                               // rounding goes to PANIC

  // HOME: state word, up to 3 sentence lines, the address, the big button, its progress line
  H_WORD_Y = CT + 8;
  H_SENT_Y = H_WORD_Y + H_WORD_H + 2;
  H_IP_Y   = H_SENT_Y + H_SENT_N * H_LINE_H + 2;
  bBig     = {8, (int16_t)(H_IP_Y + 22), (int16_t)(scrW - 16), 64};
  H_PROG_Y = bBig.y + bBig.h + 4;

  // SOLENOIDS: 5 x 5 squares, a legend line under them
  S_PITCH = (scrW - 8) / S_COLS; S_SIZE = S_PITCH - 4;
  S_X0 = (scrW - S_COLS * S_PITCH) / 2 + 2; S_Y0 = CT + 4;
  S_LEG_Y = CB - 16;
  int navY = S_Y0 + 4 * S_PITCH;                           // paged: 4 rows of squares, then the page row
  bPrev = {4, (int16_t)navY, 72, (int16_t)(S_PITCH - 6)};
  bNext = {(int16_t)(scrW - 76), (int16_t)navY, 72, (int16_t)(S_PITCH - 6)};

  // PATTERNS: four big buttons, then two lines
  for (int s = 0; s < SEQ_SLOTS; s++) bSlot[s] = {4, (int16_t)(CT + 4 + s * 52), (int16_t)(scrW - 8), 48};
  P_INFO_Y = CT + 4 + SEQ_SLOTS * 52;
}

// ---------------- top bar and tabs ----------------
static void netText(char* s, size_t n) {
  if (updating) snprintf(s, n, "updating");
  else if (WiFi.status() == WL_CONNECTED) snprintf(s, n, "%s", WiFi.localIP().toString().c_str());
  else if (apOn) snprintf(s, n, "own WiFi");
  else snprintf(s, n, "no WiFi");
}

// the Ragamuffin Workshop rose, about 18 px: 16 petals in yellow, green, blue, red (x4) round a light hub
// with a dark spindle dot, inside a ring of red and blue dashes
static void drawEmblem(int cx, int cy) {
  const uint16_t pc[4] = {L_YEL, L_GRN, L_BLU, L_RED};
  lcd.fillCircle(cx, cy, 9, C_PAPER);
  for (int k = 0; k < 16; k++) {
    float a = 258.75f + k * 22.5f;                   // petal 0 sits at the top; angles run clockwise
    lcd.fillArc(cx, cy, 3, 6, a, a + 20.0f, pc[k & 3]);
    float d = k * 22.5f;
    lcd.fillArc(cx, cy, 8, 9, d, d + 14.0f, (k & 1) ? L_BLU : L_RED);
  }
  lcd.fillCircle(cx, cy, 2, C_INK);
  lcd.drawPixel(cx, cy, C_BG);
}

// Verna is talking: a tiny speaker glyph in blue (info), its left edge at x
static void drawSpeaker(int x, int cy, uint16_t bg) {
  lcd.fillRect(x, cy - 5, 10, 11, bg);
  lcd.fillRect(x, cy - 2, 3, 5, L_BLU);
  lcd.fillTriangle(x + 2, cy, x + 6, cy - 4, x + 6, cy + 4, L_BLU);
  lcd.drawFastVLine(x + 8, cy - 2, 5, L_BLU);
}

// While pwrState == 0 the whole top bar becomes the red "no 24 V" banner, on every tab. It takes the bar's
// own 22 px, so no tab's layout or touch areas move.
static void drawBar(bool force) {
  static char last[24];
  static int lastM = -1, lastP = -1, lastV = -1;
  char s[24];
  netText(s, sizeof s);
  int m = muted ? 1 : 0, pw = pwrState == 0 ? 1 : 0, vt = vernaPlaying >= 0 ? 1 : 0;
  if (!force && m == lastM && pw == lastP && vt == lastV && (pw || !strcmp(s, last))) return;
  strlcpy(last, s, sizeof last); lastM = m; lastP = pw; lastV = vt;
  const int tx = 25;
  if (pw) {
    lcd.fillRect(0, 0, scrW, BAR_H, C_BAD);
    lcd.drawFastHLine(0, BAR_H, scrW, C_LINE);
    drawEmblem(11, BAR_H / 2);
    int right = scrW - 4 - (vt ? 13 : 0);
    if (vt) drawSpeaker(scrW - 13, BAR_H / 2, C_BAD);
    const char* msg = "NO 24 V - PLUG IN THE SUPPLY";
    useFont(F_UI);
    if (lcd.textWidth(msg) > right - tx) msg = "NO 24 V - PLUG IN SUPPLY";
    if (lcd.textWidth(msg) > right - tx) { msg = "NO 24 V - PLUG IN THE SUPPLY"; useFont(F_TINY); }
    lcd.setTextDatum(middle_center); lcd.setTextColor(C_BG);
    lcd.drawString(msg, (tx + right) / 2, BAR_H / 2 + 1);
    return;
  }
  lcd.fillRect(0, 0, scrW, BAR_H, C_PAPER);
  lcd.drawFastHLine(0, BAR_H, scrW, C_LINE);
  drawEmblem(11, BAR_H / 2);
  // the address on the right in Font0 (the speaker lamp just left of it), then the name in serif caps
  // if it still fits, else Font2
  useFont(F_TINY);
  fitText(s, scrW - 5 - tx - 82 - 6 - 13);           // always leaves room for the name in Font2 and the lamp
  int iw = lcd.textWidth(s);
  lcd.setTextDatum(middle_right); lcd.setTextColor(C_INK);
  lcd.drawString(s, scrW - 5, BAR_H / 2);
  if (vt) { drawSpeaker(scrW - 5 - iw - 13, BAR_H / 2, C_PAPER); iw += 13; }
  const char* name = m ? "MUTED" : "HALL CHIMES";
  useFont(F_SERIF);
  if (tx + lcd.textWidth(name) > scrW - 5 - iw - 6) useFont(F_UI);
  lcd.setTextDatum(middle_left); lcd.setTextColor(m ? C_BAD : C_INK);
  lcd.drawString(name, tx, BAR_H / 2 + 1);
}

static int tabAt(int x) {
  for (int t = 0; t < N_TABS; t++) if (x < tabX[t + 1]) return t;
  return N_TABS - 1;
}

// tabs: paper with a line border; the open tab gets a gold top rule and gold text; PANIC is always solid bad
static void drawTabs() {
  lcd.fillRect(0, TY, scrW, TAB_H, C_BG);
  lcd.drawFastHLine(0, TY, scrW, C_LINE);
  useFont(tabFont); lcd.setTextDatum(middle_center);
  for (int t = 0; t < N_TABS; t++) {
    int bx = tabX[t] + 2, by = TY + 4, bw = tabX[t + 1] - tabX[t] - 4, bh = TAB_H - 7;
    uint16_t fg;
    if (t == T_PANIC) { lcd.fillRoundRect(bx, by, bw, bh, 5, C_BAD); fg = C_BG; }
    else {
      lcd.fillRect(bx, by, bw, bh, C_PAPER);
      lcd.drawRect(bx, by, bw, bh, C_LINE);
      if (t == tab) { lcd.fillRect(bx, by, bw, 3, C_GOLD); fg = C_GOLD; }
      else fg = C_MUTED;
    }
    lcd.setTextColor(fg);
    lcd.drawString(TAB_NAMES[t], bx + bw / 2, by + bh / 2 + 2);
  }
}

// ---------------- live tracking: a strike flashes its square even if the coil was on between two passes ----
// flashUntil[i] = 0: not flashing. Armed values are odd (| 1), and disarmed once they pass.
static uint32_t seenStrikes[MAX_OUT], flashUntil[MAX_OUT];
static void trackStrikes(uint32_t ms) {
  for (int i = 0; i < MAX_OUT; i++) {
    uint32_t n = strikes[i];
    if (n != seenStrikes[i]) { seenStrikes[i] = n; flashUntil[i] = (ms + 160) | 1; }
    else if (flashUntil[i] && !before(ms, flashUntil[i])) flashUntil[i] = 0;
  }
}

static bool isHot(int i, uint32_t ms) {
  uint32_t r = refusedAt[i];
  if (r && (int32_t)(ms - r) < 3000) return true;   // signed: r = millis()|1 can be 1 ms ahead
  return heat[i] + outCap(i) / 1000.0f > HEAT_CAP_MS;
}

// =============================== HOME ===============================
static uint32_t panicAtMs = 0;
static char hWord[16], hSent[48];
static uint16_t hWordC = 0;
static int hBtn = -1;

enum { HB_TEST, HB_STOPTEST, HB_STOP, HB_UNMUTE, HB_WAIT_UPD, HB_WAIT_PWR };
static int homeBtnState() {
  if (calState == CAL_WALK || calCmd == CAL_WALK) return HB_STOPTEST;
  if (calBusy()) return HB_STOP;                     // a sweep or tune started from the page: this stops it
  if (updating) return HB_WAIT_UPD;
  if (muted) return HB_UNMUTE;
  if (pwrState == 0) return HB_WAIT_PWR;             // strikes are refused without the supply
  return HB_TEST;
}

static void homeFresh() { hWord[0] = hSent[0] = 0; hWordC = 0; hBtn = -1; }

// the sentence, centred, wrapped at spaces onto up to H_SENT_N lines
static void drawSentence(const char* s, uint16_t fg) {
  lcd.fillRect(0, H_SENT_Y, scrW, H_SENT_N * H_LINE_H, C_BG);
  useFont(F_SANS12); lcd.setTextDatum(middle_center); lcd.setTextColor(fg);
  const int w = scrW - 16;
  char line[48];
  const char* p = s;
  for (int ln = 0; ln < H_SENT_N && *p; ln++) {
    int len = 0;                                     // the longest run of whole words that fits
    for (const char* e = p;;) {
      while (*e && *e != ' ') e++;
      int cand = clampi((int)(e - p), 0, (int)sizeof line - 1);
      memcpy(line, p, cand); line[cand] = 0;
      if (len && lcd.textWidth(line) > w) break;
      len = cand;
      if (!*e) break;
      e++;
    }
    memcpy(line, p, len); line[len] = 0;
    fitText(line, w);
    lcd.drawString(line, scrW / 2, H_SENT_Y + ln * H_LINE_H + H_LINE_H / 2);
    p += len;
    while (*p == ' ') p++;
  }
}

static void drawHome() {
  char s[48];
  int okN = 0, faultB = -1;
  for (int b = 0; b < MAX_BOARDS; b++) {
    uint8_t st = bState[b];
    if (st == B_OK) okN++;
    if (st == B_FAULT && faultB < 0) faultB = b;
  }

  // the state word and its sentence
  const char* word; uint16_t c;
  if (stuckMask)             { word = "COIL STUCK?"; c = C_BAD; snprintf(s, sizeof s, "Cut the 24 V supply now."); }
  else if (pwrState == 0)    { word = "NO 24 V"; c = C_BAD; snprintf(s, sizeof s, "Plug in the power supply."); }
  else if (updating)         { word = "UPDATING"; c = C_GOLD; snprintf(s, sizeof s, "New firmware is going in."); }
  else if (muted)            { word = "MUTED"; c = C_BAD; snprintf(s, sizeof s, "Tap UNMUTE to play again."); }
  else if (faultB >= 0)      { word = "FAULT"; c = C_BAD;
                               snprintf(s, sizeof s, "Check driver board 0x%02X.", BOARD_BASE + faultB); }
  else if (!okN)             { word = "NO DRIVERS"; c = C_BAD; snprintf(s, sizeof s, "Check the cable to the driver boards."); }
  else                       { word = "READY"; c = C_OK; snprintf(s, sizeof s, "Ready to play."); }
  if (strcmp(word, hWord) || c != hWordC) {
    strlcpy(hWord, word, sizeof hWord); hWordC = c;
    lcd.fillRect(0, H_WORD_Y, scrW, H_WORD_H, C_BG);
    static const int WF[] = {F_SANSB24, F_SANSB18, F_BIG, -1};
    fitFont(word, scrW - 16, WF);
    lcd.setTextDatum(middle_center); lcd.setTextColor(c);
    lcd.drawString(word, scrW / 2, H_WORD_Y + H_WORD_H / 2);
  }
  if (strcmp(s, hSent)) { strlcpy(hSent, s, sizeof hSent); drawSentence(s, C_INK); }

  // the address of the phone page
  if (WiFi.status() == WL_CONNECTED) snprintf(s, sizeof s, "Phone page: %s", WiFi.localIP().toString().c_str());
  else if (apOn) snprintf(s, sizeof s, "WiFi %s, 192.168.4.1", AP_SSID);
  else snprintf(s, sizeof s, "No WiFi");
  txtLineAt(0, 0, H_IP_Y, scrW, 18, F_UI, C_MUTED, C_BG, s, 1);

  // the one big button
  int hb = homeBtnState();
  if (hb != hBtn) {
    hBtn = hb;
    static const int BF[] = {F_SANSB18, F_BIG, -1};
    const char* t = hb == HB_STOPTEST ? "STOP TEST" : hb == HB_STOP ? "STOP" : hb == HB_UNMUTE ? "UNMUTE" :
                    hb == HB_WAIT_UPD ? "UPDATING" : "TEST ALL";
    int f = fitFont(t, bBig.w - 12, BF);
    if (hb == HB_STOPTEST || hb == HB_STOP) drawBtn(bBig, t, C_BAD, C_BG, f);
    else if (hb == HB_TEST || hb == HB_UNMUTE) drawBtn(bBig, t, C_GOLD, C_GOLDINK, f);
    else drawBtn(bBig, t, C_PAPER, C_RULE, f, C_LINE);   // can't run now: dim
  }

  // progress under it: "Testing 3 of 25" from calMsg, or how the last test ended
  char m[64];
  memcpy(m, calMsg, sizeof m); m[63] = 0;           // a copy: cal.cpp may be writing it
  int n = 0, tot = 0;
  const char* par = strrchr(m, '(');
  uint16_t pc = C_MUTED;
  if (hb == HB_STOPTEST) {
    pc = C_GOLD;
    if (par && sscanf(par, "(%d of %d)", &n, &tot) == 2) snprintf(s, sizeof s, "Testing %d of %d", n, tot);
    else snprintf(s, sizeof s, "Starting the test...");
  }
  else if (hb == HB_STOP) snprintf(s, sizeof s, "Calibrating from the phone.");
  else if (hb != HB_TEST) s[0] = 0;
  else if (sscanf(m, "Walk done: %d", &n) == 1) snprintf(s, sizeof s, "Tested %d solenoids.", n);
  else if (!strncmp(m, "Walk ", 5) && strstr(m, " after ") &&
           sscanf(strstr(m, " after "), " after %d of %d", &n, &tot) == 2)
    snprintf(s, sizeof s, "Stopped after %d of %d.", n, tot);
  else if (!strcmp(m, "Walk: no outputs ready")) snprintf(s, sizeof s, "No solenoids ready to test.");
  else snprintf(s, sizeof s, "Taps each solenoid once, in turn.");
  txtLineAt(1, 0, H_PROG_Y, scrW, 18, F_UI, pc, C_BG, s, 1);
}

static void touchHome(int x, int y, uint32_t ms) {
  if (!inB(bBig, x, y)) return;
  switch (homeBtnState()) {
    case HB_UNMUTE:                                  // a new press, and not the finger that just hit PANIC
      if (ms - panicAtMs >= 1000) requestUnmute("screen");   // never fires anything
      break;
    case HB_STOPTEST:
    case HB_STOP:
      calAbort = true;                               // the walk (or sweep) stops within ~10 ms
      break;
    case HB_TEST:
      if (!calBusy()) {
        calOut = 0;
        calAbort = false;                            // as net.cpp calStart(): clear a stale stop before the command
        calCmd = CAL_WALK;
      }
      break;
    default: break;                                  // updating or no supply: nothing to do
  }
}

// =============================== SOLENOIDS ===============================
static uint8_t sList[MAX_OUT];
static int sN = 0, sPage = 0;
static uint16_t sKey[S_COLS * S_COLS];
static int sNavKey = -1;

enum { SS_OFF = 1, SS_READY, SS_FIRE, SS_HOT };
static uint8_t solState(int i, uint32_t ms) {
  uint8_t bs = bState[i >> 3];
  if (outs[i].kind == K_OFF || bs == B_MISSING) return SS_OFF;
  if (bs == B_FAULT) return SS_HOT;
  if (outOn[i] || (flashUntil[i] && before(ms, flashUntil[i]))) return SS_FIRE;
  if (!outUsable(i)) return SS_OFF;                 // solo blocks it
  if (isHot(i, ms)) return SS_HOT;
  return SS_READY;
}

static void drawLegend() {
  // coloured squares with a word each; "hot/fault" shortens to "hot" if the line would not fit
  const uint16_t lc[5] = {C_OK, C_GOLD, C_BAD, C_PAPER2, C_WARN};
  const char* lw[5] = {"ready", "firing", "hot/fault", "off", "check"};
  useFont(F_TINY);
  int tot = 0;
  for (int k = 0; k < 5; k++) tot += 8 + 3 + lcd.textWidth(lw[k]) + (k < 4 ? 8 : 0);
  if (tot > scrW - 8) { tot -= lcd.textWidth(lw[2]); lw[2] = "hot"; tot += lcd.textWidth(lw[2]); }
  lcd.fillRect(0, S_LEG_Y, scrW, 16, C_BG);
  int x = (scrW - tot) / 2, cy = S_LEG_Y + 8;
  lcd.setTextDatum(middle_left);
  for (int k = 0; k < 5; k++) {
    if (k == 4) { lcd.fillCircle(x + 4, cy, 4, C_WARN); lcd.setTextColor(C_BG); lcd.drawString("!", x + 2, cy); }
    else {
      lcd.fillRect(x, cy - 4, 8, 8, lc[k]);
      if (k == 3) lcd.drawRect(x, cy - 4, 8, 8, C_RULE);
    }
    lcd.setTextColor(C_MUTED);
    lcd.drawString(lw[k], x + 11, cy);
    x += 8 + 3 + lcd.textWidth(lw[k]) + 8;
  }
}

static void solFresh() {
  clearContent();
  for (auto& k : sKey) k = 0xFFFF;
  sNavKey = -1;
  drawLegend();
}

static void drawSolCell(int slot, int idx, uint8_t st, bool chk) {
  int x = S_X0 + (slot % S_COLS) * S_PITCH, y = S_Y0 + (slot / S_COLS) * S_PITCH;
  lcd.fillRect(x, y, S_SIZE, S_SIZE, C_BG);
  if (idx < 0) return;
  uint16_t fill = st == SS_READY ? C_OK : st == SS_FIRE ? C_GOLD : st == SS_HOT ? C_BAD : C_PAPER2;
  uint16_t fg = st == SS_OFF ? C_MUTED : C_BG;
  lcd.fillRoundRect(x, y, S_SIZE, S_SIZE, 5, fill);
  if (st == SS_OFF) lcd.drawRoundRect(x, y, S_SIZE, S_SIZE, 5, C_LINE);
  char s[4];
  snprintf(s, sizeof s, "%d", idx + 1);
  useFont(F_SANSB12); lcd.setTextDatum(middle_center); lcd.setTextColor(fg);
  lcd.drawString(s, x + S_SIZE / 2, y + S_SIZE / 2 + 1);
  if (chk) {                                         // past its wear threshold: a "!" in the corner
    int cx = x + S_SIZE - 7, cy = y + 7;
    lcd.fillCircle(cx, cy, 6, C_BG);
    lcd.fillCircle(cx, cy, 5, C_WARN);
    useFont(F_TINY); lcd.setTextColor(C_BG);
    lcd.drawString("!", cx + 1, cy + 1);
  }
}

static void drawSol(uint32_t ms) {
  sN = 0;
  for (int i = 0; i < MAX_OUT; i++)
    if (outs[i].kind != K_OFF || bState[i >> 3] != B_MISSING) sList[sN++] = (uint8_t)i;
  const int full = S_COLS * S_COLS;
  bool paged = sN > full;
  int per = paged ? S_COLS * 4 : full;
  int pages = sN ? (sN + per - 1) / per : 1;
  if (sPage >= pages) sPage = pages - 1;

  // the list's shape changed (count, page, paging on/off): clear the whole grid, then redraw every square
  int nav = (paged ? 1 : 0) | (sPage << 1) | (sN << 8);
  bool navNew = nav != sNavKey;
  if (navNew) {
    sNavKey = nav;
    lcd.fillRect(0, S_Y0, scrW, S_LEG_Y - S_Y0, C_BG);
    for (auto& k : sKey) k = 0xFFFF;
    if (paged) {                                     // the page row takes the fifth row of squares
      drawArrow(bPrev, 2, sPage > 0);
      drawArrow(bNext, 3, sPage + 1 < pages);
      char s[24];
      snprintf(s, sizeof s, "%d-%d of %d", sPage * per + 1, clampi((sPage + 1) * per, 1, sN), sN);
      useFont(F_UI); lcd.setTextDatum(middle_center); lcd.setTextColor(C_INK);
      lcd.drawString(s, scrW / 2, bPrev.y + bPrev.h / 2);
    }
  }

  for (int k = 0; k < per; k++) {
    int j = sPage * per + k;
    int idx = j < sN ? sList[j] : -1;
    uint8_t st = idx >= 0 ? solState(idx, ms) : 0;
    bool chk = idx >= 0 && wearCheck(idx);
    uint16_t key = idx < 0 ? 0 : (uint16_t)((idx + 1) << 4 | st << 1 | (chk ? 1 : 0));
    if (key == sKey[k]) continue;
    sKey[k] = key;
    drawSolCell(k, idx, st, chk);
  }

  if (navNew && !sN) {
    useFont(F_UI); lcd.setTextDatum(middle_center); lcd.setTextColor(C_MUTED);
    lcd.drawString("No solenoids found.", scrW / 2, S_Y0 + 2 * S_PITCH);
  }
}

static void touchSol(int x, int y) {               // page buttons only: tapping a square never strikes
  if (sN <= S_COLS * S_COLS) return;
  int pages = (sN + S_COLS * 4 - 1) / (S_COLS * 4);
  if (inB(bPrev, x, y) && sPage > 0) sPage--;
  else if (inB(bNext, x, y) && sPage + 1 < pages) sPage++;
}

// =============================== PATTERNS ===============================
namespace { struct PatC { bool ok; uint8_t playing; char nm[12]; }; }
static PatC patC[SEQ_SLOTS];

static void patFresh() {
  clearContent(); invalidateLines();
  for (auto& c : patC) c.ok = false;
}

static void drawPat() {
  int ss = seqSlot;
  char s[40];
  for (int k = 0; k < SEQ_SLOTS; k++) {
    const UiBtn& b = bSlot[k];
    PatC c; c.ok = true; c.playing = ss == k ? 1 : 0;
    memcpy(c.nm, pats[k].name, sizeof c.nm); c.nm[11] = 0;
    if (!c.nm[0]) snprintf(c.nm, sizeof c.nm, "Pattern %d", k + 1);
    uint16_t bg = c.playing ? C_GOLD : C_PAPER2, fg = c.playing ? C_GOLDINK : C_INK;
    if (!patC[k].ok || patC[k].playing != c.playing || strcmp(patC[k].nm, c.nm)) {
      patC[k] = c;
      if (c.playing) drawBtn(b, "", bg, fg, F_BIG); else drawBtn(b, "", bg, fg, F_BIG, C_LINE);
      useFont(F_BIG); lcd.setTextDatum(middle_center); lcd.setTextColor(fg);
      strlcpy(s, c.nm, sizeof s); fitText(s, b.w - 16);
      lcd.drawString(s, b.x + b.w / 2, b.y + 16);
      lnOk[2 + k] = false;                           // its sub line sits on the new fill
    }
    if (c.playing) {
      int steps = clampi(pats[k].steps, 1, SEQ_STEPS);
      snprintf(s, sizeof s, "Step %d of %d: tap to stop", clampi((int)seqStep + 1, 1, steps), steps);
    } else snprintf(s, sizeof s, "Pattern %d: tap to play", k + 1);
    txtLineAt(2 + k, b.x + 6, b.y + 30, b.w - 12, 16, F_UI, c.playing ? C_GOLDINK : C_MUTED, bg, s, 1);
  }
  if (muted) txtLineAt(0, 0, P_INFO_Y, scrW, 16, F_UI, C_BAD, C_BG, "Muted: unmute on HOME.", 1);
  else txtLineAt(0, 0, P_INFO_Y, scrW, 16, F_UI, C_MUTED, C_BG, "Edit patterns on the phone:", 1);
  snprintf(s, sizeof s, "%s.local", DEVICE_NAME);
  txtLineAt(1, 0, P_INFO_Y + 16, scrW, 16, F_UI, C_MUTED, C_BG, s, 1);
}

static void touchPat(int x, int y) {
  for (int k = 0; k < SEQ_SLOTS; k++) {
    if (!inB(bSlot[k], x, y)) continue;
    if (seqSlot == k) sendMsg(M_SEQ, 255, 0, 0);     // the playing one: stop
    else sendMsg(M_SEQ, (uint8_t)k, 0, 0);
    return;
  }
}

// =============================== tabs and touch ===============================
static void setTab(int t) {
  if (t == T_PANIC) t = T_HOME;                      // PANIC has no page of its own: it panics, then HOME
  tab = t;
  drawTabs();
  invalidateLines();
  if (t == T_SOL) solFresh();
  else if (t == T_PAT) patFresh();
  else { clearContent(); homeFresh(); }
}

// The PANIC tab bypasses the press-edge filter below: any touch sample on it panics, even a finger that slid
// there, landed within 150 ms of another press, or is still "held" after a touch dropout. Once per touch on it.
static bool panicHeld = false;
static void panicTouch(bool down, int x, int y, uint32_t ms) {
  bool in = down && y >= TY && tabAt(x) == T_PANIC;
  if (in && !panicHeld) { requestPanic(true, "screen"); panicAtMs = ms; }
  panicHeld = in;
}

// press edge only, ignoring a held finger and bounces: released after 2 quiet reads, 150 ms between presses
static bool touchEdge(bool down, int32_t tx, int32_t ty, int32_t& x, int32_t& y, uint32_t ms) {
  static bool held = false;
  static uint8_t quiet = 0;
  static uint32_t lastEdge = 0;
  if (down) {
    quiet = 0;
    if (!held) {
      held = true;
      if (ms - lastEdge >= 150) { lastEdge = ms; x = tx; y = ty; return true; }
    }
  } else if (held && ++quiet >= 2) held = false;
  return false;
}

static void onTouch(int x, int y, uint32_t ms) {
  if (y >= TY) {
    int t = tabAt(x);
    // PANIC itself was already requested by panicTouch(), on the first sample, before any drawing
    if (t == T_PANIC || t != tab) setTab(t);
    return;
  }
  if (y < CT || y >= CB) return;
  if (tab == T_HOME) touchHome(x, y, ms);
  else if (tab == T_SOL) touchSol(x, y);
  else if (tab == T_PAT) touchPat(x, y);
}

void uiTask(void*) {
  lcd.init();
  audioInitCodec();     // once, here: the ES8311 shares lgfx's internal I2C port with the touch controller
  lcd.setRotation(SCREEN_ROT);
  lcd.setBrightness(160);
  initColours();
  layout();
  lcd.fillScreen(C_BG);
  drawBar(true);
  setTab(T_HOME);

  uint32_t tBar = 0, tSlow = 0;
  for (;;) {
    uint32_t ms = millis();
    int32_t rx = 0, ry = 0, tx = 0, ty = 0;
    bool down = lcd.getTouch(&rx, &ry) > 0;
    panicTouch(down, (int)rx, (int)ry, ms);          // first: PANIC never waits for the press filter
    if (touchEdge(down, rx, ry, tx, ty, ms)) { onTouch((int)tx, (int)ty, ms); tSlow = 0; }
    trackStrikes(ms);
    if (ms - tBar >= 250) { tBar = ms; drawBar(false); }

    // SOLENOIDS and PATTERNS every pass (strike flash, step counter); HOME a few times a second
    if (tab == T_SOL) drawSol(ms);
    else if (tab == T_PAT) drawPat();
    else if (tSlow == 0 || ms - tSlow >= 250) { tSlow = ms | 1; drawHome(); }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}
