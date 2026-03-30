#ifndef TTS_PLAYBACK_STREAM_H
#define TTS_PLAYBACK_STREAM_H

/*
 * TtsAccumulator — Simple PSRAM buffer wrapped as Arduino Stream.
 *
 * Simplified buffer — no concurrent playback, just accumulate then play.
 * Previous versions tried to play audio during HTTP download using
 * timeout=0 i2s_write. This caused stuttering (unreliable DMA fill)
 * and truncation (buffer too small for long responses).
 *
 * Now this is just a dumb buffer. writeToStream() fills it,
 * then the caller plays from it after download completes using
 * the proven portMAX_DELAY playback path.
 *
 * Tick callback is still called for animation during download.
 */

#include <Arduino.h>
#include "Config.h"

class TtsAccumulator : public Stream {
public:
    TtsAccumulator(uint8_t* psramBuf, size_t capacity, TickCallback tick)
      : _buf(psramBuf), _cap(capacity), _writePos(0), _tick(tick) {}

    // ── Stream write interface (called by HTTPClient::writeToStream) ──
    size_t write(uint8_t b) override {
        if (_writePos >= _cap) return 0;
        _buf[_writePos++] = b;
        return 1;
    }

    size_t write(const uint8_t* data, size_t len) override {
        if (!data || len == 0) return 0;

        size_t toWrite = len;
        if (_writePos + toWrite > _cap) toWrite = _cap - _writePos;
        if (toWrite == 0) return 0;

        memcpy(_buf + _writePos, data, toWrite);
        _writePos += toWrite;

        // Keep animation cycling during download
        if (_tick) _tick();

        return toWrite;
    }

    // ── Stream read interface (not used, required by Stream) ──
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

    // ── Accessors ──
    size_t getBytesWritten() const { return _writePos; }

    // Number of complete 16-bit samples
    size_t getSampleCount() const { return (_writePos & ~1) / sizeof(int16_t); }

private:
    uint8_t*      _buf;
    size_t        _cap;
    size_t        _writePos;
    TickCallback  _tick;
};

#endif // TTS_PLAYBACK_STREAM_H