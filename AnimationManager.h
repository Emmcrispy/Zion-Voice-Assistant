/*
 *  AnimationManager.h — PSRAM-cached frame animation for ILI9341
 *
 *  All animation frames live on the SD card as raw RGB565 .bin files.
 *  On each state change, the relevant frames are bulk-loaded into
 *  PSRAM so that update() never touches the SD card. This keeps
 *  the SPI bus free for audio and network traffic during playback.
 *
 *  Frame format: 320x240 little-endian uint16 RGB565 (153,600 bytes)
 */

#pragma once
#include <TFT_eSPI.h>
#include <SD.h>
#include "Config.h"

#define MAX_ANIM_FRAMES  10
#define FRAME_W          320
#define FRAME_H          240
#define FRAME_BYTES      (FRAME_W * FRAME_H * 2)  // 153,600

class AnimationManager {
public:
  AnimationManager(TFT_eSPI& tft);
  void begin();
  void showBoot1();
  void showBoot2();
  void setAnimation(AnimType state);
  void update();
  void freeCache();

private:
  TFT_eSPI& _tft;
  AnimType  _currentAnim;
  int       _currentFrame;
  int       _startFrame;
  int       _totalFrames;
  unsigned long _lastFrameTime;

  int _frameDelays[MAX_ANIM_FRAMES];

  uint16_t* _cache[MAX_ANIM_FRAMES];
  int       _cachedCount;

  void pushFrame(int idx);
  bool loadCache(const String& path, int count);
  int  countFrames(const String& path);
};
