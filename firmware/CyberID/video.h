#pragma once
#include <stdint.h>

// One looping clip stored as back-to-back JPEG frames in flash.
struct Video {
  const char *name;
  uint16_t fps;
  uint16_t frameCount;
  const uint32_t *offsets;  // frameCount + 1 entries: frame i is data[offsets[i] .. offsets[i+1])
  const uint8_t *data;
};
