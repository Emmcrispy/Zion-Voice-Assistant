#ifndef WAKE_WORD_ENGINE_H
#define WAKE_WORD_ENGINE_H

#include "Config.h"

class WakeWordEngine {
private:
  bool _initialized;
  float _threshold;
  
public:
  WakeWordEngine();
  bool begin();
  
  // No-op for non-continuous classifier (stateless, nothing to reset)
  void reset();
  
  // Run full non-continuous inference on 16000 samples (1 second).
  // Returns confidence for "Hey Zion" (0.0 - 1.0), or -1.0 on error.
  float processAudio(int16_t* audioData, size_t sampleCount);
  
  bool isReady();
};

#endif