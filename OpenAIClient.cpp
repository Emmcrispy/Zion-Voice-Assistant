#include "OpenAIClient.h"
#include <esp_heap_caps.h>
#include <esp_wifi.h>

char    WIFI_SSID[64]   = "";
char    WIFI_PASS[64]   = "";
char    OPENAI_KEY[192] = "";
String  SYSTEM_PROMPT   = "";

OpenAIClient::OpenAIClient()
  : _historyCount(0)
{}

void OpenAIClient::begin() {
    clearHistory();
}

void OpenAIClient::clearHistory() {
    _historyCount = 0;
    for (int i = 0; i < MAX_HISTORY_TURNS * 2; i++) {
        _history[i].role    = "";
        _history[i].content = "";
    }
}

// ─────────────────────────────────────────────────────────────
//  connectWiFi — disable power save for low latency
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::connectWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        // disable WiFi power save ──────────────
        // Default modem sleep adds 100-300ms wake-up latency on
        // each network operation. Voice assistant needs low latency
        // over power efficiency.
        WiFi.setSleep(false);

        // max transmit power for range ──────────────
        WiFi.setTxPower(WIFI_POWER_19_5dBm);

        Serial.printf("\nWiFi connected! IP: %s  RSSI: %d dBm  (power save OFF)\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }
    Serial.println("\nWiFi FAILED");
    return false;
}

bool OpenAIClient::ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("WiFi dropped — reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print(".");
    }
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok) WiFi.setSleep(false);  // re-apply after reconnect
    Serial.printf(ok ? "\nWiFi OK\n" : "\nWiFi FAILED\n");
    return ok;
}

bool OpenAIClient::isYesResponse(const String& transcript) {
    if (transcript.length() < 1) return false;
    String t = transcript; t.toLowerCase(); t.trim();
    if (t.indexOf("yes")      >= 0) return true;
    if (t.indexOf("yeah")     >= 0) return true;
    if (t.indexOf("yep")      >= 0) return true;
    if (t.indexOf("sure")     >= 0) return true;
    if (t.indexOf("please")   >= 0) return true;
    if (t.indexOf("another")  >= 0) return true;
    if (t.indexOf("more")     >= 0) return true;
    if (t.indexOf("continue") >= 0) return true;
    if (t.indexOf("go on")    >= 0) return true;
    if (t.indexOf("go ahead") >= 0) return true;
    if (t.indexOf("thank")    >= 0) return true;
    if (t.indexOf("okay")     >= 0) return true;
    if (t.indexOf("ok")       >= 0) return true;
    if (t.indexOf("uh huh")   >= 0) return true;
    if (t.indexOf("mhm")      >= 0) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────
//  isImageRequest — keyword fallback for image detection
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::isImageRequest(const String& transcript) {
    String t = transcript; t.toLowerCase();
    return (t.indexOf("draw")        >= 0 ||
            t.indexOf("show me")     >= 0 ||
            t.indexOf("visualize")   >= 0 ||
            t.indexOf("picture")     >= 0 ||
            t.indexOf("image")       >= 0 ||
            t.indexOf("paint")       >= 0 ||
            t.indexOf("illustrate")  >= 0 ||
            t.indexOf("generate")    >= 0 ||
            t.indexOf("sketch")      >= 0 ||
            t.indexOf("create a")    >= 0 ||
            t.indexOf("what does")   >= 0 ||
            t.indexOf("what would")  >= 0);
}

// ─────────────────────────────────────────────────────────────
//  sendSTT — build WAV directly in PSRAM, POST via reused TLS
//
// optimizations:
//  1. Eliminated SD write/read roundtrip — WAV built in PSRAM.
//     Old: build WAV → write SD → read SD → build body → POST
//     New: build WAV+body in one PSRAM buffer → POST
//     Saves ~200-400ms of SD I/O per call.
//
//  2. HTTP setReuse(true) — if a TLS connection to
//     api.openai.com is already open, skip the 2-4s handshake.
// ─────────────────────────────────────────────────────────────
String OpenAIClient::sendSTT(int16_t* audioData, size_t sampleCount) {
    if (!ensureWiFi() || !audioData || sampleCount == 0) return "";

    // ── Trim silence — smaller WAV = faster upload ──────────
    const int16_t TRIM_THRESH = 400;
    size_t trimStart = 0, trimEnd = sampleCount;

    for (size_t i = 0; i < sampleCount; i++) {
        if (abs(audioData[i]) > TRIM_THRESH) {
            trimStart = (i > 800) ? i - 800 : 0;
            break;
        }
    }
    for (size_t i = sampleCount; i > trimStart; i--) {
        if (abs(audioData[i - 1]) > TRIM_THRESH) {
            trimEnd = (i + 800 < sampleCount) ? i + 800 : sampleCount;
            break;
        }
    }

    int16_t* trimmedAudio = audioData + trimStart;
    size_t   trimmedCount = trimEnd - trimStart;
    if (trimmedCount < 1600) {
        trimmedAudio = audioData;
        trimmedCount = sampleCount;
    }

    Serial.printf("STT: trimmed %u→%u samples (%.1fs saved)\n",
                  (unsigned)sampleCount, (unsigned)trimmedCount,
                  (float)(sampleCount - trimmedCount) / SAMPLE_RATE);

    // ── Build WAV + multipart body directly in PSRAM ─────────
    // No SD write/read — saves ~200-400ms of I/O.
    uint32_t dataSize = trimmedCount * 2;
    uint32_t wavSize  = 44 + dataSize;

    String boundary = "----ZionBoundaryXkT7rZu0gW";
    String hdr = "--" + boundary + "\r\n"
                 "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                 "Content-Type: audio/wav\r\n\r\n";
    String ftr = "\r\n--" + boundary + "\r\n"
                 "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
                 "--" + boundary + "\r\n"
                 "Content-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
                 "--" + boundary + "--\r\n";

    size_t totalBody = hdr.length() + wavSize + ftr.length();
    uint8_t* body = (uint8_t*)heap_caps_malloc(totalBody, MALLOC_CAP_SPIRAM);
    if (!body) { Serial.println("sendSTT: PSRAM alloc failed"); return ""; }

    size_t pos = 0;

    // 1. Multipart header
    memcpy(body + pos, hdr.c_str(), hdr.length());
    pos += hdr.length();

    // 2. WAV file header (44 bytes) — built in-place
    uint32_t chunkSize  = 36 + dataSize;
    uint32_t sr         = SAMPLE_RATE;
    uint32_t byteRate   = SAMPLE_RATE * 2;
    uint32_t fmtSize    = 16;
    uint16_t audioFmt   = 1;       // PCM
    uint16_t numCh      = 1;       // mono
    uint16_t blockAlign = 2;
    uint16_t bitsPS     = 16;

    memcpy(body + pos, "RIFF", 4);         pos += 4;
    memcpy(body + pos, &chunkSize, 4);     pos += 4;
    memcpy(body + pos, "WAVE", 4);         pos += 4;
    memcpy(body + pos, "fmt ", 4);         pos += 4;
    memcpy(body + pos, &fmtSize, 4);       pos += 4;
    memcpy(body + pos, &audioFmt, 2);      pos += 2;
    memcpy(body + pos, &numCh, 2);         pos += 2;
    memcpy(body + pos, &sr, 4);            pos += 4;
    memcpy(body + pos, &byteRate, 4);      pos += 4;
    memcpy(body + pos, &blockAlign, 2);    pos += 2;
    memcpy(body + pos, &bitsPS, 2);        pos += 2;
    memcpy(body + pos, "data", 4);         pos += 4;
    memcpy(body + pos, &dataSize, 4);      pos += 4;

    // 3. Audio sample data
    memcpy(body + pos, (uint8_t*)trimmedAudio, dataSize);
    pos += dataSize;

    // 4. Multipart footer
    memcpy(body + pos, ftr.c_str(), ftr.length());

    Serial.printf("STT: WAV built in PSRAM (%u bytes, skipped SD)\n", wavSize);

    // ── setReuse(true) — reuse TLS from previous call ────────
    _http.setReuse(true);
    _http.begin("https://api.openai.com/v1/audio/transcriptions");
    _http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
    _http.addHeader("Content-Type",  "multipart/form-data; boundary=" + boundary);
    _http.setTimeout(15000);

    uint32_t t0 = millis();
    int    httpCode = _http.POST(body, totalBody);
    String response = _http.getString();
    heap_caps_free(body);
    Serial.printf("STT httpCode: %d (%.0fms)\n", httpCode, (float)(millis() - t0));

    String transcript = "";
    if (httpCode == 200) {
        StaticJsonDocument<1024> doc;
        if (!deserializeJson(doc, response)) {
            transcript = doc["text"].as<String>();
            transcript.trim();
        }
    } else {
        Serial.println(response);
    }
    _http.end();
    return transcript;
}

// ─────────────────────────────────────────────────────────────
//  sendLLM — chat completion with function calling (generate_image)
//  Reuses TLS from the STT call (saves ~2-4s handshake).
// ─────────────────────────────────────────────────────────────
LLMResult OpenAIClient::sendLLM(const String& userMessage) {
    LLMResult result;
    result.generateImage = false;

    if (!ensureWiFi()) return result;

    _http.setReuse(true);
    _http.begin("https://api.openai.com/v1/chat/completions");
    _http.addHeader("Content-Type",  "application/json");
    _http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
    _http.setTimeout(15000);

    // Build request with function calling tool
    DynamicJsonDocument doc(8192);
    doc["model"]       = "gpt-4.1-nano";
    doc["max_tokens"]  = LLM_MAX_TOKENS;
    doc["temperature"] = 0.7;
    doc["tool_choice"] = "auto";

    // ── Tool definition: generate_image ──────────────────────
    JsonArray tools = doc.createNestedArray("tools");
    JsonObject tool = tools.createNestedObject();
    tool["type"] = "function";
    JsonObject func = tool.createNestedObject("function");
    func["name"]        = "generate_image";
    func["description"] = "Generate an image when the user asks to see, draw, "
                          "visualize, create, paint, sketch, or imagine something "
                          "visual. Use for any request involving pictures or illustrations.";
    JsonObject params = func.createNestedObject("parameters");
    params["type"] = "object";
    JsonObject props = params.createNestedObject("properties");

    JsonObject promptProp = props.createNestedObject("prompt");
    promptProp["type"]        = "string";
    promptProp["description"] = "Detailed DALL-E prompt describing the image to generate";

    JsonObject spokenProp = props.createNestedObject("spoken_response");
    spokenProp["type"]        = "string";
    spokenProp["description"] = "A short friendly sentence to speak aloud while the image is displayed";

    JsonArray required = params.createNestedArray("required");
    required.add("prompt");
    required.add("spoken_response");

    // ── Messages ─────────────────────────────────────────────
    JsonArray messages = doc.createNestedArray("messages");

    {
        JsonObject sys = messages.createNestedObject();
        sys["role"] = "system";
        struct tm timeinfo;
        char dateBuf[32] = "";
        if (getLocalTime(&timeinfo, 100)) {
            strftime(dateBuf, sizeof(dateBuf), "%B %d, %Y", &timeinfo);
        }
        String sysContent = "";
        if (dateBuf[0]) {
            sysContent += "Today's date is " + String(dateBuf) + ". ";
        }
        if (SYSTEM_PROMPT.length() > 0) {
            sysContent += SYSTEM_PROMPT;
        } else {
            sysContent += "You are Zion, a helpful voice assistant. "
                         "Respond in English. Be concise and friendly.";
        }
        sys["content"] = sysContent;
    }
    for (int i = 0; i < _historyCount; i++) {
        JsonObject msg = messages.createNestedObject();
        msg["role"] = _history[i].role; msg["content"] = _history[i].content;
    }
    JsonObject user = messages.createNestedObject();
    user["role"] = "user"; user["content"] = userMessage;

    String payload; serializeJson(doc, payload);

    uint32_t t0 = millis();
    int httpCode = _http.POST(payload);

    if (httpCode == 200) {
        String response = _http.getString();
        Serial.printf("LLM: %d (%ums)\n", httpCode, (unsigned)(millis() - t0));
        DynamicJsonDocument rd(8192);
        if (!deserializeJson(rd, response)) {
            JsonObject choice = rd["choices"][0];
            String finishReason = choice["finish_reason"].as<String>();

            if (finishReason == "tool_calls") {
                JsonObject toolCall = choice["message"]["tool_calls"][0];
                String funcName = toolCall["function"]["name"].as<String>();
                String argsStr  = toolCall["function"]["arguments"].as<String>();

                if (funcName == "generate_image") {
                    StaticJsonDocument<1024> argsDoc;
                    if (!deserializeJson(argsDoc, argsStr)) {
                        result.generateImage  = true;
                        result.imagePrompt    = argsDoc["prompt"].as<String>();
                        result.spokenResponse = argsDoc["spoken_response"].as<String>();
                        Serial.printf("LLM tool call: generate_image\n");
                        Serial.printf("  prompt: %s\n", result.imagePrompt.c_str());
                        Serial.printf("  spoken: %s\n", result.spokenResponse.c_str());
                    }
                }
            } else {
                result.text = choice["message"]["content"].as<String>();
                result.text.trim();
            }
        }
    } else {
        Serial.printf("LLM error: %d (%ums)\n", httpCode, (unsigned)(millis() - t0));
    }
    _http.end();

    // Only add to history for normal text responses
    if (!result.generateImage && result.text.length() > 0) {
        int maxSlots = MAX_HISTORY_TURNS * 2;
        if (_historyCount >= maxSlots) {
            for (int i = 0; i < maxSlots - 2; i++) _history[i] = _history[i + 2];
            _historyCount = maxSlots - 2;
        }
        _history[_historyCount].role    = "user";
        _history[_historyCount].content = userMessage; _historyCount++;
        _history[_historyCount].role    = "assistant";
        _history[_historyCount].content = result.text; _historyCount++;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
//  requestImageURL — Call DALL-E 2 API, return image URL
//
//  This is the slow part (~5-15 seconds). It only uses the
//  network — no SD card access — so the caller can safely
//  run animation (which reads BMP from SD) concurrently.
//
// split from generateImage to allow animation during
//  the DALL-E API wait.
// ─────────────────────────────────────────────────────────────
String OpenAIClient::requestImageURL(const String& prompt) {
    if (!ensureWiFi()) return "";

    Serial.println(">> [Calling DALL-E 2...]");
    Serial.printf("[MEM] pre-DALLE Heap: %u  PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    _http.setReuse(true);
    _http.begin("https://api.openai.com/v1/images/generations");
    _http.addHeader("Content-Type", "application/json");
    _http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
    _http.setTimeout(60000);

    StaticJsonDocument<512> reqDoc;
    reqDoc["model"]           = "dall-e-2";
    reqDoc["prompt"]          = prompt;
    reqDoc["n"]               = 1;
    reqDoc["size"]            = "256x256";
    reqDoc["response_format"] = "url";
    String payload; serializeJson(reqDoc, payload);

    uint32_t t0 = millis();
    int httpCode = _http.POST(payload);
    payload = "";
    Serial.printf("DALL-E httpCode: %d (%.0fms)\n",
                  httpCode, (float)(millis() - t0));

    if (httpCode != 200) {
        Serial.printf("DALL-E error: %d\n", httpCode);
        if (httpCode > 0) Serial.println(_http.getString());
        _http.end();
        return "";
    }

    // Parse image URL from response
    String imageUrl = "";
    {
        String response = _http.getString();
        _http.end();

        StaticJsonDocument<256> filter;
        filter["data"][0]["url"] = true;
        DynamicJsonDocument respDoc(2048);
        if (deserializeJson(respDoc, response, DeserializationOption::Filter(filter))) {
            Serial.println("DALL-E: JSON parse failed");
            return "";
        }
        imageUrl = respDoc["data"][0]["url"].as<String>();
    }

    if (imageUrl.length() == 0) {
        Serial.println("DALL-E: no URL in response");
        return "";
    }
    Serial.printf("DALL-E: got URL (%d chars)\n", imageUrl.length());
    return imageUrl;
}

// ─────────────────────────────────────────────────────────────
//  downloadImageToSD — Download image from CDN URL to SD card
//
//  This DOES access SD card. Caller must NOT run animation
//  concurrently (SPI bus conflict). Typically takes 1-2s.
//
// split from generateImage.
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::downloadImageToSD(const String& imageUrl, const char* sdPath) {
    if (!ensureWiFi() || imageUrl.length() == 0) return false;

    Serial.printf("[MEM] pre-download Heap: %u  PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    _http.setReuse(false);
    _http.begin(imageUrl);
    _http.setTimeout(30000);

    int imgCode = _http.GET();
    if (imgCode != 200) {
        Serial.printf("Image download error: %d\n", imgCode);
        _http.end();
        return false;
    }

    if (SD.exists(sdPath)) SD.remove(sdPath);
    File f = SD.open(sdPath, FILE_WRITE);
    if (!f) {
        Serial.println("Image: cannot open SD file");
        _http.end();
        return false;
    }

    int bytesWritten = _http.writeToStream(&f);
    f.flush(); f.close();
    _http.end();

    if (bytesWritten <= 0) {
        Serial.printf("Image download failed: %d\n", bytesWritten);
        SD.remove(sdPath);
        return false;
    }

    Serial.printf("Image: %d bytes -> %s\n", bytesWritten, sdPath);
    Serial.printf("[MEM] post-download Heap: %u  PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    return true;
}

// ─────────────────────────────────────────────────────────────
//  generateImage — Combined call (kept for clip cache / simple use)
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::generateImage(const String& prompt, const char* sdPath) {
    String url = requestImageURL(prompt);
    if (url.length() == 0) return false;
    return downloadImageToSD(url, sdPath);
}

// ─────────────────────────────────────────────────────────────
//  downloadTTSToStream — Stream TTS PCM to any Arduino Stream
//
//  Posts to OpenAI TTS API, then uses writeToStream() to pipe
//  decoded PCM data to the provided Stream. writeToStream handles
//  chunked transfer encoding internally.
//
// used with TtsAccumulator to download to PSRAM buffer.
//  Playback happens AFTER download completes (sequential, not
//  concurrent) for reliable audio via portMAX_DELAY.
//
//  Uses _http with setReuse(true) for TLS session reuse.
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::downloadTTSToStream(const String& text, Stream* output) {
    if (!ensureWiFi() || !output) return false;

    // Build JSON payload once — reused across attempts
    StaticJsonDocument<768> doc;
    doc["model"]           = "gpt-4o-mini-tts";
    doc["voice"]           = "nova";
    doc["instructions"]    = "Speak in English. Use a warm, friendly, natural tone.";
    doc["input"]           = text;
    doc["response_format"] = "pcm";
    String payload;
    serializeJson(doc, payload);

    // Try up to 2 attempts: first reuses TLS, retry uses fresh connection
    for (int attempt = 0; attempt < 2; attempt++) {

        if (attempt == 0) {
            _http.setReuse(true);
        } else {
            Serial.println("TTS: retrying with fresh connection...");
            _http.setReuse(false);
        }

        _http.begin("https://api.openai.com/v1/audio/speech");
        _http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
        _http.addHeader("Content-Type",  "application/json");
        _http.setTimeout(30000);

        uint32_t t0 = millis();
        int httpCode = _http.POST(payload);
        Serial.printf("TTS httpCode: %d (%.0fms to response) attempt %d\n",
                      httpCode, (float)(millis() - t0), attempt + 1);

        if (httpCode != 200) {
            Serial.printf("TTS error: %d\n", httpCode);
            if (httpCode > 0) Serial.println(_http.getString());
            _http.end();
            if (attempt == 0) continue;   // retry once
            return false;
        }

        int bytesWritten = _http.writeToStream(output);
        _http.end();

        if (bytesWritten <= 0) {
            Serial.printf("TTS stream download failed: %d\n", bytesWritten);
            if (attempt == 0) continue;   // retry once
            return false;
        }

        bool even = ((bytesWritten & 1) == 0);
        Serial.printf("TTS stream: %d bytes (%.1fs @ 24kHz) %s\n",
                      bytesWritten,
                      (float)bytesWritten / (24000.0f * 2.0f),
                      even ? "OK" : "WARNING: ODD");
        payload = "";
        return true;
    }

    payload = "";
    return false;
}

// ─────────────────────────────────────────────────────────────
//  _downloadToSD — TTS download using _http with connection reuse
//
// reuses TLS connection from prior calls (same host)
//  for each TTS call — that's a ~2-4s TLS handshake EVERY time.
//  Now uses _http with setReuse(true), piggybacking on the TLS
//  session from the preceding LLM call. writeToStream handles
//  chunked transfer encoding internally.
//
//  Model: gpt-4o-mini-tts
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::_downloadToSD(const String& text, const char* path) {
    if (!ensureWiFi()) return false;

    // Ensure directory exists
    String dir = String(path);
    int slash = dir.lastIndexOf('/');
    if (slash > 0) {
        String dp = dir.substring(0, slash);
        if (!SD.exists(dp.c_str())) SD.mkdir(dp.c_str());
    }

    // Build JSON payload
    StaticJsonDocument<768> doc;
    doc["model"]           = "gpt-4o-mini-tts";
    doc["voice"]           = "nova";
    doc["instructions"]    = "Speak in English. Use a warm, friendly, natural tone.";
    doc["input"]           = text;
    doc["response_format"] = "pcm";
    String payload;
    serializeJson(doc, payload);

    // reuse TLS from prior call ─────────
    _http.setReuse(true);
    _http.begin("https://api.openai.com/v1/audio/speech");
    _http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
    _http.addHeader("Content-Type",  "application/json");
    _http.setTimeout(30000);

    uint32_t t0 = millis();
    int httpCode = _http.POST(payload);
    payload = "";
    Serial.printf("TTS httpCode: %d (%.0fms to response)\n",
                  httpCode, (float)(millis() - t0));

    if (httpCode != 200) {
        Serial.printf("TTS error: %d\n", httpCode);
        if (httpCode > 0) Serial.println(_http.getString());
        _http.end();
        return false;
    }

    // Open SD file for writing
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("TTS: cannot open SD file");
        _http.end();
        return false;
    }

    // writeToStream handles chunked + content-length responses.
    // Same proven method used for image downloads.
    int bytesWritten = _http.writeToStream(&f);
    f.flush();
    f.close();
    _http.end();

    if (bytesWritten <= 0) {
        Serial.printf("TTS download failed: %d\n", bytesWritten);
        SD.remove(path);
        return false;
    }

    bool even = ((bytesWritten & 1) == 0);
    Serial.printf("TTS: %d bytes -> %s (%.1fs @ 24kHz) %s\n",
                  bytesWritten, path,
                  (float)bytesWritten / (24000.0f * 2.0f),
                  even ? "OK" : "WARNING: ODD BYTE COUNT");
    return true;
}

// ─────────────────────────────────────────────────────────────
//  streamTTS — blocking full download to SD, then return.
// ─────────────────────────────────────────────────────────────
bool OpenAIClient::streamTTS(const String& text, String& outFilePath) {
    static uint8_t idx = 0;
    char path[64];
    snprintf(path, sizeof(path), "/prompts/prompt_%02u.pcm", idx);
    idx = (idx + 1) % 4;
    outFilePath = String(path);

    if (!SD.exists("/prompts")) SD.mkdir("/prompts");

    Serial.println(">> [Downloading TTS...]");
    return _downloadToSD(text, path);
}

// ─────────────────────────────────────────────────────────────
//  cacheClips — generate all cached clips on SD if missing
// benefits from connection reuse — subsequent clips
//  piggyback on the first clip's TLS session.
// ─────────────────────────────────────────────────────────────
void OpenAIClient::cacheClips() {
    // Regenerate clips when TTS voice/language settings change.
    // The marker file tracks the current voice config — if missing
    // or different, we re-generate all clips.
    bool needRegen = !SD.exists("/clips/.v_en");

    if (needRegen) {
        Serial.println("cacheClips: regenerating (new voice config)");
        if (SD.exists("/clips")) {
            SD.remove(CLIP_ACK_PATH);
            SD.remove(CLIP_FOLLOWUP_PATH);
            SD.remove(CLIP_LISTENING_PATH);
            SD.remove(CLIP_GOODBYE_PATH);
            SD.remove("/clips/.v_en");
        }
    }

    if (!SD.exists("/clips")) SD.mkdir("/clips");

    struct ClipDef { const char* path; const char* text; };
    ClipDef clips[] = {
        { CLIP_ACK_PATH,       "Yes?" },
        { CLIP_FOLLOWUP_PATH,  "Do you have another question?" },
        { CLIP_LISTENING_PATH, "Okay, I'm listening!" },
        { CLIP_GOODBYE_PATH,   "Alright, I'll be here if you need me!" },
    };

    for (auto& clip : clips) {
        if (!SD.exists(clip.path)) {
            Serial.printf("Generating clip: %s\n", clip.path);
            _downloadToSD(clip.text, clip.path)
                ? Serial.printf("  OK: %s\n", clip.path)
                : Serial.printf("  FAILED: %s\n", clip.path);
        } else {
            Serial.printf("Clip cached: %s\n", clip.path);
        }
    }

    // Write version marker so we don't regenerate next boot
    if (!SD.exists("/clips/.v_en")) {
        File m = SD.open("/clips/.v_en", FILE_WRITE);
        if (m) { m.print("en"); m.close(); }
    }
}