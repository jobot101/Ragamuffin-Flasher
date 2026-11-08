// Ragamuffin Hall chimes: control firmware for the Espressif ESP32-S3-BOX (original) on its dock.
// 25 brass tubes C3..C5 plus any smaller solenoids / attachments, on Adafruit MCP23017 solenoid drivers 0x20..0x27.
// MIDI in, phone page (hall-chimes.local, or own WiFi "HallChimes" -> 192.168.4.1), BOX screen with a status panel
// for every output and a step sequencer for chimes and beats. Spec: DESIGN.md. Shared contract: chimes.h.
//
// Safety in one line: short taps only (30 ms hard cap, no hold), everything off at boot and on any panic, a supply
// budget and a heat guard per coil, and only core 1 (core.cpp) ever switches a coil.
//
// Build: arduino-cli, esp32 core 3.x,
//   FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
//   Libraries: LovyanGFX, PubSubClient. CI: .github/workflows/chimebox.yml.

#include "chimes.h"

QueueHandle_t netToChimes = nullptr;   // core 0 -> core 1
QueueHandle_t chimesToNet = nullptr;   // played notes -> core 0 (MQTT)

void setup() {
  driversOffNow();   // FIRST: the MCP23017s keep their latches across an ESP reset, so zero them before anything else
  Serial.begin(115200);
  netToChimes = xQueueCreate(64, sizeof(Msg));
  chimesToNet = xQueueCreate(32, sizeof(NoteMsg));
  loadCfg();      // NVS read only (no flash write): settings, outputs, patterns, pins
  coreSetup();    // everything off again: tester pin, every driver found (off-first init); then inputs and MIDI
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(uiTask,  "ui",  10240, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(calTask, "cal", 8192,  nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 0);   // Verna's prompts (codec set up by uiTask)
}

void loop() { coreLoop(); }   // core 1: MIDI, buttons, scheduler, every coil write
