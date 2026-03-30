/*
 *  AudioEngine.h — mic capture + speaker playback for Zion
 *
 *  Mic:     ICS-43434  on I2S_NUM_0, 16 kHz mono
 *  Speaker: MAX98357A  on I2S_NUM_1, 24 kHz stereo
 *
 *  Three playback paths, all using portMAX_DELAY for gapless audio:
 *    1. startPlayback()          — copies data to PSRAM (short clips)
 *    2. startPlaybackZeroCopy()  — takes ownership of a PSRAM buffer (TTS)
 *    3. startPlaybackFromFile()  — streams from SD via a staging buffer
 *
 *  DMA drain: when the last audio chunk enters the I2S pipeline,
 *  we wait 400 ms for the hardware to finish playing before zeroing
 *  the DMA buffers.  isSpeaking() stays true during this drain so
 *  nothing else tramples the output.
 */

#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include "Config.h"
#include "RingBuffer.h"
#include <driver/i2s.h>
#include <SD.h>

// how many int16 samples to buffer between SD reads and I2S writes.
// 16 384 samples @ 24 kHz ≈ 682 ms of runway — plenty of headroom
// even when animation pushes a frame (~30 ms SPI block).
#define STAGING_CAPACITY  16384

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    void begin();
    void setVolume(uint8_t pct);

    // mic ring buffer
    size_t getAvailableSamples() { return ringBuffer ? ringBuffer->available() : 0; }
    void   readSamples(int16_t* dst, size_t n)  { if (ringBuffer) ringBuffer->read(dst, n); }
    void   peekSamples(int16_t* dst, size_t n)  { if (ringBuffer) ringBuffer->peekLast(dst, n); }
    void   updateMicBuffer();
    void   resetMicBuffer();
    void   restartMicI2S();

    // recording with silence-gate
    void startRecording(int maxSec = RECORD_SECONDS);
    bool isRecordingComplete();
    void getRecordedAudio(int16_t** buf, size_t* count);
    void freeRecordBuffer();        // reclaim PSRAM after STT is done

    // playback
    void startPlayback(int16_t* data, size_t samples);           // copy
    void startPlaybackZeroCopy(int16_t* buf, size_t samples);    // take ownership
    bool startPlaybackFromFile(const String& path);              // SD stream
    void updateSpeakerPlayback();
    void stopPlayback();
    bool isSpeaking() { return _speaking || _draining; }

    // feed zeros to keep MAX98357A alive when idle
    void feedSilence();

    // Playback amplitude for mouth animation
    float getPlaybackAmplitude() { return _playbackAmplitude; }

private:
    RingBuffer* ringBuffer;

    // recording state
    int16_t* recordBuffer;
    size_t   recordBufferSize;
    size_t   recordedSamples;
    bool     recording;
    bool     _speechDetected;
    int      _speechAboveCount;
    uint32_t _silenceStartMs;
    bool     _silenceTimerActive;
    uint32_t _recordStartMs;

    // playback state (RAM / zero-copy)
    int16_t* playbackBuffer;
    size_t   playbackSize;
    size_t   playbackIndex;

    // playback state (file)
    File     playbackFile;
    bool     filePlayback;
    bool     _speaking;

    // DMA drain — lets the last ~340 ms of audio finish playing
    bool          _draining;
    unsigned long _drainStartMs;

    // SD → I2S staging buffer (lives in PSRAM)
    int16_t* stagingBuffer;
    size_t   stagingAvail;
    size_t   stagingRead;

    float    _volumeScale;
    float    _playbackAmplitude;    // current chunk RMS for mouth animation

    void    setupMicI2S();
    void    setupSpeakerI2S();
    bool    refillStaging();
    int16_t computeRMS(const int16_t* samples, size_t count);
};

#endif
