#ifndef OPENAI_CLIENT_H
#define OPENAI_CLIENT_H

#include "Config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>

// ── LLM result — normal text OR image tool call ─────────────
struct LLMResult {
    String text;            // normal response (empty if tool call)
    bool   generateImage;   // true if generate_image tool was called
    String imagePrompt;     // DALL-E prompt from tool call
    String spokenResponse;  // what to say while showing image
};

class OpenAIClient {
public:
    OpenAIClient();
    void begin();
    bool connectWiFi();

    // Tick callback — called during blocking HTTP waits so
    // animation + audio playback keep running.
    void setTickCallback(TickCallback cb) { _tickCb = cb; }

    // STT — send WAV audio, get transcript back
    String sendSTT(int16_t* audioData, size_t sampleCount);

    // LLM — send text with function calling, returns text or image tool call
    LLMResult sendLLM(const String& userMessage);

    // stream TTS audio to PSRAM buffer ──────────────
    // Posts to TTS API, pipes decoded PCM via writeToStream().
    // Used with TtsAccumulator to download to PSRAM buffer.
    bool downloadTTSToStream(const String& text, Stream* output);

    // ── Blocking TTS to SD file (used for image path + clip cache) ──
    bool streamTTS(const String& text, String& outFilePath);

    // ── Image generation ───────────────────────────────────────
    // two-step image gen: URL request + download (allows animation between)
    // requestImageURL: DALL-E API call only (no SD, safe to animate)
    // downloadImageToSD: CDN download (uses SD, NO animation)
    String requestImageURL(const String& prompt);
    bool   downloadImageToSD(const String& imageUrl, const char* sdPath);
    // Combined call (for simple use / clip cache):
    bool generateImage(const String& prompt, const char* sdPath);

    // ── Keyword fallback — detects image-related requests ──────
    static bool isImageRequest(const String& transcript);

    // ── Clip cache ─────────────────────────────────────────────
    void cacheClips();

    // ── Conversation history ───────────────────────────────────
    void clearHistory();

    // ── Follow-up classification ───────────────────────────────
    bool isYesResponse(const String& transcript);

private:
    HTTPClient _http;

    // Conversation history
    struct HistoryMsg { String role; String content; };
    HistoryMsg _history[MAX_HISTORY_TURNS * 2];
    int        _historyCount;

    // Helpers
    bool ensureWiFi();
    bool _downloadToSD(const String& text, const char* path);

    TickCallback _tickCb = nullptr;
    void _tick() { if (_tickCb) _tickCb(); }
};

#endif