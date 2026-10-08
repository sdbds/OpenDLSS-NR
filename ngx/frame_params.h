#pragma once
#include <stdint.h>

struct NrFrameParams {
  uint32_t width, height, fullWidth, fullHeight;
  uint32_t seed, historyValid, enabled, autoMask;
  uint32_t style, colorX, colorY, colorWidth;
  uint32_t colorHeight, motionX, motionY, motionWidth;
  uint32_t motionHeight, outputX, outputY, reserved;
  float intensity, localTone, localStructure, skinStructure;
  float motionScaleX, motionScaleY, blendScale, padding;
};
static_assert(sizeof(NrFrameParams) == 112);
