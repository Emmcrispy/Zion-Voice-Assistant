/*
 *  AudioEngine.cpp — Zion
 *
 *  DMA drain:
 *    When the last audio chunk is written to I2S, i2s_write() returns
 *    immediately — but up to 16 DMA buffers (~340 ms) of audio are
 *    still queued in hardware.  The old code called i2s_zero_dma_buffer()
 *    right away, killing the ending of every response.
 *
 *    Now we enter a "draining" state: isSpeaking() stays true for 400 ms
 *    after EOF so nothing interrupts the tail end of playback.  Only then
 *    do we zero the buffers and release resources.  16 buffers × 256
 *    samples ÷ 24 kHz ≈ 170 ms, so 400 ms gives plenty of margin.
 *
 *  Speaker DMA sizing:
 *    16 buffers × 256 samples ÷ 24 kHz ≈ 170 ms of runway.
 */

#include "AudioEngine.h"
#include <esp_heap_caps.h>

// ── lifecycle ───────────────────────────────────────────────

AudioEngine::AudioEngine()
  : ringBuffer(nullptr),
    recordBuffer(nullptr), recordBufferSize(0),
    recordedSamples(0), recording(false),
    _speechDetected(false), _speechAboveCount(0), _silenceStartMs(0), _silenceTimerActive(false),
    _recordStartMs(0),
    playbackBuffer(nullptr), playbackSize(0), playbackIndex(0),
    filePlayback(false), _speaking(false),
    _draining(false), _drainStartMs(0),
    stagingBuffer(nullptr), stagingAvail(0), stagingRead(0),
    _volumeScale(0.8f),
    _playbackAmplitude(0.0f)
{}

AudioEngine::~AudioEngine() {
    if (ringBuffer)     delete ringBuffer;
    if (recordBuffer)   heap_caps_free(recordBuffer);
    if (playbackBuffer) heap_caps_free(playbackBuffer);
    if (stagingBuffer)  heap_caps_free(stagingBuffer);
    if (playbackFile)   playbackFile.close();
}

void AudioEngine::begin() {
    ringBuffer = new RingBuffer(SAMPLE_RATE * 3);

    stagingBuffer = (int16_t*)heap_caps_malloc(
        STAGING_CAPACITY * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!stagingBuffer) {
        Serial.println("AudioEngine: PSRAM staging alloc FAILED");
    }

    setupMicI2S();
    setupSpeakerI2S();

    i2s_zero_dma_buffer(SPK_I2S_PORT);   // silence on power-up
    Serial.println("AudioEngine: OK");
}

void AudioEngine::setVolume(uint8_t scale) {
    _volumeScale = (float)constrain((int)scale, 0, 100) / 100.0f;
    Serial.printf("AudioEngine: volume %d%%\n", scale);
}

// ── silence / mic helpers ───────────────────────────────────

void AudioEngine::feedSilence() {
    size_t bw;
    int32_t silence[64] = {0};
    i2s_write(SPK_I2S_PORT, silence, sizeof(silence), &bw, 0);
}

void AudioEngine::resetMicBuffer() {
    if (ringBuffer) ringBuffer->reset();
    size_t br; int32_t trash[256];
    i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, 0);
    i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, 0);
}

void AudioEngine::restartMicI2S() {
    i2s_driver_uninstall(MIC_I2S_PORT);
    delay(10);
    setupMicI2S();
    delay(50);    // ICS-43434 datasheet: 50ms to lock clocks
    size_t br; int32_t trash[256];
    for (int i = 0; i < 16; i++)
        i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, 0);
    if (ringBuffer) ringBuffer->reset();
}

// ── I2S hardware setup ──────────────────────────────────────

void AudioEngine::setupMicI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 16,
        .dma_buf_len          = 256,
        .use_apll             = false
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = MIC_SCK,
        .ws_io_num    = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SD
    };
    i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(MIC_I2S_PORT, &pins);
    Serial.printf("AudioEngine: mic @ %d Hz  gain 8x\n", SAMPLE_RATE);
}

void AudioEngine::setupSpeakerI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = TTS_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 16,
        .dma_buf_len          = 256,
        .use_apll             = true
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = SPK_BCLK,
        .ws_io_num    = SPK_LRC,
        .data_out_num = SPK_DIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(SPK_I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(SPK_I2S_PORT, &pins);
    Serial.printf("AudioEngine: speaker @ %d Hz  APLL  DMA 16x256\n", TTS_SAMPLE_RATE);
}

// ── RMS calculation ─────────────────────────────────────────

int16_t AudioEngine::computeRMS(const int16_t* samples, size_t count) {
    if (count == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum += s * s;
    }
    return (int16_t)sqrt((double)(sum / (int64_t)count));
}

// ── mic buffer + recording ──────────────────────────────────

void AudioEngine::updateMicBuffer() {
    size_t bytes_read;
    int32_t rawSamples[128];

    esp_err_t err = i2s_read(MIC_I2S_PORT, rawSamples, sizeof(rawSamples),
                             &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return;

    int count = (int)(bytes_read / sizeof(int32_t));
    int16_t processed[128];

    for (int i = 0; i < count; i++) {
        int32_t s = (rawSamples[i] >> 16) << 3;  // 8x gain
        if (s >  32000) s =  32000;
        if (s < -32000) s = -32000;
        processed[i] = (int16_t)s;
    }

    ringBuffer->write(processed, (size_t)count);

    // if we're not recording, that's all we need
    if (!recording || !recordBuffer || recordedSamples >= recordBufferSize) return;

    size_t space  = recordBufferSize - recordedSamples;
    size_t toCopy = ((size_t)count < space) ? (size_t)count : space;
    memcpy(&recordBuffer[recordedSamples], processed, toCopy * sizeof(int16_t));
    recordedSamples += toCopy;

    // silence gate: detect speech start, then wait for silence after speech
    int16_t rms = computeRMS(processed, (size_t)count);

    if (!_speechDetected) {
        if (rms > SPEECH_RMS_THRESHOLD) {
            _speechAboveCount++;
            if (_speechAboveCount >= 2) {   // 2 consecutive reads (~32ms)
                _speechDetected     = true;
                _silenceTimerActive = false;
                Serial.printf("AudioEngine: speech start (RMS=%d)\n", (int)rms);
            }
        } else {
            _speechAboveCount = 0;          // reset on any dip
            if (millis() - _recordStartMs > NO_SPEECH_TIMEOUT_MS) {
                Serial.println("AudioEngine: no-speech timeout");
                recording = false;
                recordedSamples = 0;
            }
        }
    } else {
        if (rms > SILENCE_RMS_THRESHOLD) {
            _silenceTimerActive = false;
        } else {
            if (!_silenceTimerActive) {
                _silenceTimerActive = true;
                _silenceStartMs     = millis();
            } else if (millis() - _silenceStartMs >= SILENCE_AFTER_SPEECH_MS) {
                Serial.printf("AudioEngine: silence gate (%u samples)\n",
                              (unsigned)recordedSamples);
                recording = false;
            }
        }
    }

    if (recordedSamples >= recordBufferSize) {
        Serial.println("AudioEngine: record buffer full");
        recording = false;
    }
}

void AudioEngine::startRecording(int maxSeconds) {
    if (recordBuffer) { heap_caps_free(recordBuffer); recordBuffer = nullptr; }

    recordBufferSize = (size_t)(SAMPLE_RATE * maxSeconds);
    recordBuffer = (int16_t*)heap_caps_malloc(
        recordBufferSize * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!recordBuffer) {
        Serial.println("AudioEngine: record PSRAM alloc FAILED");
        return;
    }
    recordedSamples     = 0;
    recording           = true;
    _speechDetected     = false;
    _speechAboveCount   = 0;
    _silenceTimerActive = false;
    _silenceStartMs     = 0;
    _recordStartMs      = millis();

    // flush stale mic DMA
    size_t br; int32_t trash[256];
    i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, 0);
    i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, 0);

    Serial.printf("AudioEngine: recording (max %ds)\n", maxSeconds);
}

bool AudioEngine::isRecordingComplete() { return !recording; }

void AudioEngine::getRecordedAudio(int16_t** buffer, size_t* count) {
    *buffer = recordBuffer;
    *count  = recordedSamples;
}

void AudioEngine::freeRecordBuffer() {
    if (recordBuffer) { heap_caps_free(recordBuffer); recordBuffer = nullptr; }
    recordBufferSize = 0;
    recordedSamples  = 0;
}

// ── playback: RAM copy (short clips) ────────────────────────

void AudioEngine::startPlayback(int16_t* audioData, size_t sampleCount) {
    stopPlayback();
    playbackBuffer = (int16_t*)heap_caps_malloc(
        sampleCount * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!playbackBuffer) {
        Serial.println("AudioEngine: clip alloc FAILED");
        return;
    }
    memcpy(playbackBuffer, audioData, sampleCount * sizeof(int16_t));
    playbackSize  = sampleCount;
    playbackIndex = 0;
    filePlayback  = false;
    _speaking     = true;
}

// ── playback: zero-copy from PSRAM (TTS) ────────────────────
//    Takes ownership — we'll free the buffer on stop.

void AudioEngine::startPlaybackZeroCopy(int16_t* buf, size_t sampleCount) {
    stopPlayback();
    playbackBuffer = buf;
    playbackSize   = sampleCount;
    playbackIndex  = 0;
    filePlayback   = false;
    _speaking      = true;
    Serial.printf("AudioEngine: TTS playback %u samples (%.1fs)\n",
                  (unsigned)sampleCount,
                  (float)sampleCount / (float)TTS_SAMPLE_RATE);
}

// ── playback: file via staging buffer ───────────────────────

bool AudioEngine::startPlaybackFromFile(const String& path) {
    stopPlayback();

    playbackFile = SD.open(path.c_str(), FILE_READ);
    if (!playbackFile) {
        Serial.printf("AudioEngine: can't open %s\n", path.c_str());
        return false;
    }

    uint32_t fileSize = playbackFile.size();
    if (fileSize == 0) {
        playbackFile.close();
        return false;
    }
    if (fileSize & 1) {
        Serial.printf("AudioEngine: WARNING — odd file size %u\n", fileSize);
    }

    stagingAvail = 0;
    stagingRead  = 0;
    if (!refillStaging()) {
        playbackFile.close();
        return false;
    }

    filePlayback = true;
    _speaking    = true;
    Serial.printf("AudioEngine: playing %s (%.1fs)\n",
                  path.c_str(), (float)fileSize / (TTS_SAMPLE_RATE * 2.0f));
    return true;
}

bool AudioEngine::refillStaging() {
    if (!playbackFile || !stagingBuffer) return false;
    int bytesRead = playbackFile.read(
        (uint8_t*)stagingBuffer, STAGING_CAPACITY * sizeof(int16_t));
    if (bytesRead <= 0) { stagingAvail = 0; stagingRead = 0; return false; }
    stagingAvail = (size_t)bytesRead / sizeof(int16_t);
    stagingRead  = 0;
    return true;
}

// ── stop ────────────────────────────────────────────────────

void AudioEngine::stopPlayback() {
    _speaking     = false;
    _draining     = false;
    _drainStartMs = 0;
    filePlayback  = false;
    playbackSize  = 0;
    playbackIndex = 0;
    stagingAvail  = 0;
    stagingRead   = 0;
    _playbackAmplitude = 0.0f;

    if (playbackFile)   playbackFile.close();
    if (playbackBuffer) { heap_caps_free(playbackBuffer); playbackBuffer = nullptr; }

    i2s_zero_dma_buffer(SPK_I2S_PORT);
}

// ── main playback pump — call from loop() ───────────────────
//
// When the last audio sample enters the DMA pipeline, we don't
// zero the buffers right away.  Instead we set _draining = true
// and wait 400 ms for the hardware to finish.  isSpeaking()
// returns true during this window so the state machine won't
// cut us off early.

void AudioEngine::updateSpeakerPlayback() {

    // DMA drain: wait for pipeline to empty, then clean up
    if (_draining) {
        if (millis() - _drainStartMs >= 400) {
            _draining = false;
            stopPlayback();
        }
        return;
    }

    if (!_speaking) return;

    const size_t CHUNK = 256;

    // ── file path: SD → staging → I2S ───────────────────────
    if (filePlayback) {
        if (stagingRead >= stagingAvail) {
            if (!refillStaging()) {
                // all data sent — start draining
                _speaking     = false;
                _draining     = true;
                _drainStartMs = millis();
                if (playbackFile) playbackFile.close();
                return;
            }
        }

        size_t avail  = stagingAvail - stagingRead;
        size_t frames = (avail > CHUNK) ? CHUNK : avail;

        static int32_t ampBuf[CHUNK * 2];
        float chunkPeak = 0.0f;
        for (size_t i = 0; i < frames; i++) {
            float   scaled = (float)stagingBuffer[stagingRead + i] * _volumeScale;
            int32_t s32    = (int32_t)((int16_t)constrain((int)scaled, -32768, 32767)) << 16;
            ampBuf[i * 2]     = s32;
            ampBuf[i * 2 + 1] = s32;
            float absVal = fabsf(scaled / 32768.0f);
            if (absVal > chunkPeak) chunkPeak = absVal;
        }
        _playbackAmplitude = chunkPeak;

        size_t bw = 0;
        i2s_write(SPK_I2S_PORT, ampBuf, frames * 2 * sizeof(int32_t),
                  &bw, portMAX_DELAY);
        stagingRead += bw / (2 * sizeof(int32_t));
        return;
    }

    // ── RAM / zero-copy path: PSRAM → I2S ───────────────────
    if (!playbackBuffer || playbackIndex >= playbackSize) {
        if (playbackIndex > 0) {
            _speaking     = false;
            _draining     = true;
            _drainStartMs = millis();
            return;     // don't free yet — drain first
        }
        stopPlayback();
        return;
    }

    size_t remaining = playbackSize - playbackIndex;
    size_t frames    = (remaining > CHUNK) ? CHUNK : remaining;

    static int32_t ampBuf[CHUNK * 2];
    float chunkPeak = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        float   scaled = (float)playbackBuffer[playbackIndex + i] * _volumeScale;
        int32_t s32    = (int32_t)((int16_t)constrain((int)scaled, -32768, 32767)) << 16;
        ampBuf[i * 2]     = s32;
        ampBuf[i * 2 + 1] = s32;
        float absVal = fabsf(scaled / 32768.0f);
        if (absVal > chunkPeak) chunkPeak = absVal;
    }
    _playbackAmplitude = chunkPeak;

    size_t bw = 0;
    i2s_write(SPK_I2S_PORT, ampBuf, frames * 2 * sizeof(int32_t),
              &bw, portMAX_DELAY);
    playbackIndex += bw / (2 * sizeof(int32_t));
}
