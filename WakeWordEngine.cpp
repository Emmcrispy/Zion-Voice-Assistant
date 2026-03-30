#include "WakeWordEngine.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Hey_Zion_inferencing.h>

WakeWordEngine::WakeWordEngine() : _initialized(false), _threshold(WAKE_THRESHOLD) {}

bool WakeWordEngine::begin() {
  _initialized = true;

  Serial.printf("WakeWordEngine ready | Threshold: %.2f (non-continuous)\n", _threshold);
  Serial.printf("Model expects frequency: %d Hz\n", EI_CLASSIFIER_FREQUENCY);
  Serial.printf("Raw sample count: %d | Slice size: %d | Slices/window: %d\n",
                EI_CLASSIFIER_RAW_SAMPLE_COUNT,
                EI_CLASSIFIER_SLICE_SIZE,
                EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

  return true;
}

bool WakeWordEngine::isReady() {
  return _initialized;
}

void WakeWordEngine::reset() {
  // No internal state to reset with non-continuous classifier.
  // run_classifier() is stateless — each call is independent.
}

/*
 * processAudio() — NON-CONTINUOUS classification.
 *
 * Takes a full 1-second window (16000 samples at 16kHz) and runs
 * the complete inference pipeline: MFCC feature extraction → neural
 * network → classification result.
 *
 * Returns the raw confidence for "hey-zion" (0.0 - 1.0).
 * Returns -1.0 on error.
 *
 * The caller is responsible for smoothing/thresholding — the .ino
 * maintains a moving average over the last few frames to filter
 * out random noise spikes while catching real sustained detections.
 */
float WakeWordEngine::processAudio(int16_t* audioData, size_t sampleCount) {
  if (!_initialized) return -1.0f;

  // Need a full window
  if (sampleCount < (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT) return -1.0f;

  // Use the LAST RAW_SAMPLE_COUNT samples (most recent 1 second)
  size_t offset = sampleCount - (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT;
  const size_t windowSize = (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT;

  // Allocate float buffer in PSRAM
  float* features = (float*)heap_caps_malloc(
      windowSize * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!features) return -1.0f;

  // Convert int16 -> float [-1.0, 1.0]
  for (size_t i = 0; i < windowSize; i++) {
    features[i] = (float)audioData[offset + i] / 32768.0f;
  }

  // Create signal from the entire 1-second window
  signal_t signal;
  numpy::signal_from_buffer(features, windowSize, &signal);

  ei_impulse_result_t result;

  // NON-CONTINUOUS: process the full window at once
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

  heap_caps_free(features);

  if (err != EI_IMPULSE_OK) {
    Serial.printf("WakeWord: classifier error %d\n", (int)err);
    return -1.0f;
  }

  // Find and return hey-zion confidence
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (String(result.classification[i].label) == "hey-zion") {
      return result.classification[i].value;
    }
  }

  return 0.0f;
}