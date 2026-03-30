/*
 *  AnimationManager.cpp — Zion
 *
 *  How it works:
 *    1. setAnimation() reads every frame from SD into PSRAM (once)
 *    2. update()       pushes the next cached frame to the TFT (fast)
 *    3. No SD I/O happens during animation playback — ever.
 *
 *  Frame timing (per animation):
 *    idle      3 frames: 2 s open → 60 ms half-shut → 60 ms closed
 *    listening 3 frames @ 150 ms  (pulsing glow)
 *    talking   4 frames @ 250 ms  (mouth shapes, skip frame 0)
 *    thinking  4 frames @ 200 ms  (thought-bubble dots)
 *    boot      1stboot (5 frames @ 600 ms) → 2ndboot (1 frame, hold)
 */

#include "AnimationManager.h"
#include <esp_heap_caps.h>

AnimationManager::AnimationManager(TFT_eSPI& tft)
  : _tft(tft),
    _currentAnim(ANIM_IDLE), _currentFrame(0), _startFrame(0),
    _totalFrames(0), _lastFrameTime(0), _cachedCount(0)
{
  memset(_frameDelays, 0, sizeof(_frameDelays));
  memset(_cache, 0, sizeof(_cache));
}

void AnimationManager::begin() {
  Serial.println("AnimationManager: ready (320x240, PSRAM cache)");
}

// ── PSRAM cache management ──────────────────────────────────

void AnimationManager::freeCache() {
  for (int i = 0; i < _cachedCount; i++) {
    if (_cache[i]) { free(_cache[i]); _cache[i] = nullptr; }
  }
  _cachedCount = 0;
}

bool AnimationManager::loadCache(const String& path, int count) {
  freeCache();
  if (count > MAX_ANIM_FRAMES) count = MAX_ANIM_FRAMES;

  unsigned long t0 = millis();
  for (int i = 0; i < count; i++) {
    _cache[i] = (uint16_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!_cache[i]) {
      Serial.printf("  cache: PSRAM alloc failed on frame %d\n", i);
      freeCache();
      return false;
    }

    String fp = path + "/frame_" + String(i) + ".bin";
    File f = SD.open(fp);
    if (!f) {
      Serial.printf("  cache: can't open %s\n", fp.c_str());
      freeCache();
      return false;
    }
    f.read((uint8_t*)_cache[i], FRAME_BYTES);
    f.close();
    _cachedCount++;
  }

  Serial.printf("  cache: %d frames in %lu ms (%d KB)\n",
                count, millis() - t0, count * FRAME_BYTES / 1024);
  return true;
}

// ── display ─────────────────────────────────────────────────

void AnimationManager::pushFrame(int idx) {
  if (idx < 0 || idx >= _cachedCount || !_cache[idx]) return;
  _tft.setSwapBytes(true);
  _tft.pushImage(0, 0, FRAME_W, FRAME_H, _cache[idx]);
}

// ── boot sequence ───────────────────────────────────────────

void AnimationManager::showBoot1() {
  int n1 = countFrames("/anims/1stboot");
  if (n1 > 0 && loadCache("/anims/1stboot", n1)) {
    for (int i = 0; i < n1; i++) { pushFrame(i); delay(600); }
  }
  freeCache();
}

void AnimationManager::showBoot2() {
  int n2 = countFrames("/anims/2ndboot");
  if (n2 > 0 && loadCache("/anims/2ndboot", n2)) {
    for (int i = 0; i < n2; i++) { pushFrame(i); delay(600); }
  }
  freeCache();
}

// ── frame counting ──────────────────────────────────────────

int AnimationManager::countFrames(const String& path) {
  int n = 0;
  while (n < MAX_ANIM_FRAMES &&
         SD.exists(path + "/frame_" + String(n) + ".bin"))
    n++;
  return n;
}

// ── setAnimation — load frames + set timing ─────────────────

void AnimationManager::setAnimation(AnimType state) {
  _currentAnim   = state;
  _startFrame    = 0;
  _currentFrame  = 0;
  _lastFrameTime = millis();
  memset(_frameDelays, 0, sizeof(_frameDelays));

  String path;
  bool reload = true;

  switch (state) {
    case ANIM_IDLE:
      path = "/anims/idle";
      _frameDelays[0] = 2000;
      _frameDelays[1] = 60;
      _frameDelays[2] = 60;
      break;

    case ANIM_LISTENING:
      path = "/anims/listening";
      _frameDelays[0] = 150;
      _frameDelays[1] = 150;
      _frameDelays[2] = 150;
      break;

    case ANIM_TALK:
      path = "/anims/talking";
      _startFrame = 1;    // skip frame_0 (mouth closed)
      for (int i = 0; i < MAX_ANIM_FRAMES; i++) _frameDelays[i] = 250;
      break;

    case ANIM_THINK:
      path = "/anims/thinking";
      for (int i = 0; i < MAX_ANIM_FRAMES; i++) _frameDelays[i] = 200;
      break;

    case ANIM_ERROR:
      path = "/anims/idle";
      _frameDelays[0] = 1000;
      break;
  }

  if (reload) {
    _totalFrames = countFrames(path);
    if (_totalFrames > 0) loadCache(path, _totalFrames);
    Serial.printf("anim: %s (%d frames)\n", path.c_str(), _totalFrames);
  }

  if (_cachedCount > 0) {
    _currentFrame = _startFrame;
    pushFrame(_currentFrame);
  }
}

// ── update — call every loop() iteration ────────────────────

void AnimationManager::update() {
  if (_totalFrames == 0 || _cachedCount == 0) return;

  int dly = _frameDelays[_currentFrame % MAX_ANIM_FRAMES];
  if (dly == 0) dly = 300;

  if (millis() - _lastFrameTime >= (unsigned long)dly) {
    _lastFrameTime = millis();
    _currentFrame++;
    if (_currentFrame >= _totalFrames) _currentFrame = _startFrame;
    pushFrame(_currentFrame);
  }
}
