#pragma once
#include <stdint.h>
// one step of a hit profile: duty %, time ms, or hold until STOP / auto-off
struct Step { uint8_t d; uint16_t ms; bool hold; };
