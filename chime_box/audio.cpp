// audio.cpp: Verna's spoken prompts on the ESP32-S3-BOX's own speaker (core 0).
// ES8311 codec (I2C 0x18 on the BOX's internal bus 8/18, I2S MCLK 2, BCLK 17, LRCK 47, DOUT 15), speaker amp enable 46.
// The internal I2C bus is LovyanGFX's (the touch controller shares it), so the codec is set up from uiTask through
// lgfx's own I2C calls; this file only ever writes I2S. Volume is applied digitally to the samples.
#include "chimes.h"
#include <ESP_I2S.h>
#include <LovyanGFX.hpp>

static const int PIN_MCLK = 2, PIN_BCLK = 17, PIN_LRCK = 47, PIN_DOUT = 15, PIN_PA = 46;
static const uint8_t ES8311 = 0x18;
static const int I2C_PORT_INT = 1;          // I2C_NUM_1: the same port LGFX_Box gives the touch controller

static I2SClass i2s;
static QueueHandle_t sayQ = nullptr;
static volatile bool codecOk = false;
static volatile uint32_t lastSaid[V_COUNT];
static volatile uint32_t lastAny = 0;
volatile int vernaPlaying = -1;             // line playing now, -1 = quiet
volatile uint32_t vernaCount = 0;

static bool cw(uint8_t reg, uint8_t val) {
  return (bool)lgfx::i2c::writeRegister8(I2C_PORT_INT, ES8311, reg, val, 0, 100000);
}

// Called once from uiTask after lcd.init() (the touch driver has the internal I2C port up by then).
// ES8311 as I2S slave, MCLK 4.096 MHz from the ESP32 (256 x 16 kHz), 16-bit I2S, DAC on, ADC unused.
void audioInitCodec() {
  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, LOW);   // amp off until something plays
  bool ok = cw(0x00, 0x1F);                             // reset
  delay(20);
  ok = ok && cw(0x00, 0x00) && cw(0x00, 0x80);          // power-on, slave mode
  ok = ok && cw(0x01, 0x3F);                            // all clocks on, MCLK from the MCLK pin
  ok = ok && cw(0x02, 0x00) && cw(0x05, 0x00);          // pre-div 1, x1; ADC/DAC div 1
  ok = ok && cw(0x03, 0x10) && cw(0x04, 0x10);          // single-speed, OSR 16
  ok = ok && cw(0x07, 0x00) && cw(0x08, 0xFF) && cw(0x06, 0x03);   // LRCK = MCLK/256, BCLK div 4
  ok = ok && cw(0x09, 0x0C) && cw(0x0A, 0x0C);          // serial port in/out: I2S, 16-bit
  ok = ok && cw(0x0D, 0x01) && cw(0x0E, 0x02);          // analog power up
  ok = ok && cw(0x12, 0x00) && cw(0x13, 0x10);          // DAC on, output driver on
  ok = ok && cw(0x1C, 0x6A) && cw(0x37, 0x08);          // EQs bypassed
  ok = ok && cw(0x32, 0xBF) && cw(0x31, 0x00);          // DAC volume 0 dB, unmuted (level set in software)
  codecOk = ok;
  evlog(ok ? "speaker ready (Verna's prompts)" : "speaker: codec did not answer, prompts off");
}

// Queue one prompt. Any core, never blocks. The same prompt at most every 20 s (no-power every 12 s),
// any prompt at most every 3 s, so a hand hammering a key can't turn Verna into a loop.
void vernaSay(uint8_t line) {
  if (line >= V_COUNT || !sayQ || !cfg.voice) return;
  uint32_t ms = millis();
  uint32_t gap = line == V_NO_POWER ? 12000 : 20000;
  if (lastSaid[line] && ms - lastSaid[line] < gap) return;
  if (lastAny && ms - lastAny < 3000) return;
  lastSaid[line] = ms | 1; lastAny = ms | 1;
  xQueueSend(sayQ, &line, 0);
}

void audioTask(void*) {
  sayQ = xQueueCreate(4, 1);
  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT, -1, PIN_MCLK);
  bool i2sOk = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!i2sOk) evlog("speaker: I2S did not start, prompts off");
  static int16_t buf[512];                  // 256 stereo frames
  uint32_t quietSince = millis();
  bool paOn = false;
  for (;;) {
    uint8_t line;
    if (xQueueReceive(sayQ, &line, pdMS_TO_TICKS(100)) != pdTRUE) {
      if (paOn && millis() - quietSince > 400) { digitalWrite(PIN_PA, LOW); paOn = false; }
      continue;
    }
    if (!i2sOk || !codecOk || line >= V_COUNT) continue;
    const VernaClip& c = VERNA[line];
    static int16_t ulaw[256];                 // G.711 mu-law -> 16-bit, built once
    static bool ulawReady = false;
    if (!ulawReady) {
      for (int i = 0; i < 256; i++) {
        uint8_t u = ~i; int t = ((u & 0x0F) << 3) + 0x84; t <<= (u & 0x70) >> 4;
        ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
      }
      ulawReady = true;
    }
    int vol = constrain(cfg.voiceVol, 0, 100);
    if (!paOn) { digitalWrite(PIN_PA, HIGH); paOn = true; delay(30); }
    vernaPlaying = line; vernaCount = vernaCount + 1;   // not ++: gnu++2b warns on ++ of a volatile
    for (uint32_t i = 0; i < c.n; i += 256) {
      uint32_t n = min((uint32_t)256, c.n - i);
      for (uint32_t k = 0; k < n; k++) { int16_t s = (int16_t)((int32_t)ulaw[c.pcm[i + k]] * vol / 100); buf[2 * k] = s; buf[2 * k + 1] = s; }
      i2s.write((uint8_t*)buf, n * 4);
    }
    memset(buf, 0, sizeof buf);
    for (int k = 0; k < 4; k++) i2s.write((uint8_t*)buf, sizeof buf);   // ~64 ms of silence: no click at the end
    vernaPlaying = -1;
    quietSince = millis();
  }
}
