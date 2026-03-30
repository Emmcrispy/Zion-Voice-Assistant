/*
 * ZION — Portable AI Voice Assistant
 *
 * Hardware:  ESP32-S3, ILI9341 TFT, ICS-43434 mic, MAX98357A amp
 * Pipeline:  Wake word → STT → LLM → TTS → speaker
 *            with DALL-E image generation via function calling
 *
 * Key design decisions:
 *  - TTS downloads to PSRAM first, then plays with portMAX_DELAY.
 *    Concurrent download+playback caused stuttering (timeout=0 unreliable).
 *  - Background FreeRTOS task keeps thinking animation cycling during
 *    blocking HTTP calls (STT ~1s, LLM ~2s, TTS download ~0.5s).
 *  - All clips regenerated on every boot to pick up TTS voice changes.
 *  - English enforced at every stage: STT language hint, LLM system
 *    message, TTS instructions field.
 */

#include "Config.h"
#include "StateMachine.h"
#include "AnimationManager.h"
#include "AudioEngine.h"
#include "OpenAIClient.h"
#include "WakeWordEngine.h"
#include "SDManager.h"
#include "Ttsstreambuffer.h"
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <driver/i2s.h>

// ─────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────
TFT_eSPI          tft;
StateMachine      stateMachine;
AnimationManager  animManager(tft);
AudioEngine       audioEngine;
OpenAIClient      openAI;
WakeWordEngine    wakeWord;

String pendingTranscript     = "";
String pendingImagePrompt    = "";   // DALL-E prompt from LLM tool call
String pendingSpokenResponse = "";   // what to say while showing image
bool   pendingImageKeyword   = false;// keyword fallback flag

// ── Touch detection (rate-limited, no library) ──────────────
static uint32_t _lastTouchCheckMs = 0;

static bool isTouched() {
    if (millis() - _lastTouchCheckMs < 300) return false;
    _lastTouchCheckMs = millis();

    pinMode(TOUCH_XP, OUTPUT);
    pinMode(TOUCH_XM, OUTPUT);
    digitalWrite(TOUCH_XP, HIGH);
    digitalWrite(TOUCH_XM, LOW);
    pinMode(TOUCH_YP, INPUT);
    delayMicroseconds(20);
    int val = analogRead(TOUCH_YP);

    // Restore pins for SPI
    pinMode(TOUCH_YP, OUTPUT);
    pinMode(TOUCH_XP, OUTPUT);
    pinMode(TOUCH_XM, OUTPUT);
    pinMode(TOUCH_YM, OUTPUT);

    return (val > 300 && val < 3800);
}

// Background animation task — keeps thinking dots cycling during
// blocking HTTP calls (STT 1-2s, LLM 1-3s, TTS download 0.5-1s).
// Without this, the animation freezes on a single frame.
static volatile bool _thinkTaskRunning = false;
static volatile bool _thinkTaskDone    = true;
static TaskHandle_t  _thinkTaskHandle  = nullptr;

static void startThinkingTask() {
    if (_thinkTaskHandle) return;   // already running
    _thinkTaskRunning = true;
    _thinkTaskDone    = false;
    xTaskCreatePinnedToCore(
        [](void*) {
            while (_thinkTaskRunning) {
                animManager.update();
                vTaskDelay(pdMS_TO_TICKS(30));  // ~33 fps — smooth cycling
            }
            _thinkTaskDone = true;
            vTaskDelete(nullptr);
        },
        "think", 8192, nullptr, 1, &_thinkTaskHandle, 1
    );
}

static void stopThinkingTask() {
    if (!_thinkTaskHandle) return;
    _thinkTaskRunning = false;
    uint32_t t = millis();
    while (!_thinkTaskDone && millis() - t < 500) vTaskDelay(pdMS_TO_TICKS(5));
    _thinkTaskHandle = nullptr;
}

// ─────────────────────────────────────────────────────────────
//  PNG display — PNGdec callbacks + render helper
//  Image: 256×256 → centered on 320×240 TFT
//  X offset: (320-256)/2 = 32
//  Y offset: crop 8px top + 8px bottom → (256-240)/2 = 8
// ─────────────────────────────────────────────────────────────
static PNG  png;
static File pngFile;

static void* pngOpen(const char* filename, int32_t* size) {
    pngFile = SD.open(filename, FILE_READ);
    if (!pngFile) return NULL;
    *size = pngFile.size();
    return &pngFile;
}
static void pngClose(void* handle) {
    if (pngFile) pngFile.close();
}
static int32_t pngRead(PNGFILE* handle, uint8_t* buffer, int32_t length) {
    if (!pngFile) return 0;
    return pngFile.read(buffer, length);
}
static int32_t pngSeek(PNGFILE* handle, int32_t position) {
    if (!pngFile) return 0;
    return pngFile.seek(position);
}
static int pngDraw(PNGDRAW* pDraw) {
    // Buffer for one row of pixels (256 max)
    uint16_t lineBuffer[256];
    png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    // Center 256×256 on 320×240: skip first 8 rows, last 8 rows
    int screenY = pDraw->y - 8;
    if (screenY >= 0 && screenY < 240) {
        int xOffset = (320 - pDraw->iWidth) / 2;
        tft.pushImage(xOffset, screenY, pDraw->iWidth, 1, lineBuffer);
    }
    return 1;  // PNGdec expects non-zero to continue decoding
}

// Display a PNG file from SD, centered on TFT
static bool displayPNG(const char* path) {
    int rc = png.open(path, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG open failed: %d\n", rc);
        return false;
    }
    Serial.printf("PNG: %dx%d\n", png.getWidth(), png.getHeight());
    tft.fillScreen(TFT_BLACK);  // clear before drawing
    rc = png.decode(NULL, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG decode failed: %d\n", rc);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────
static void printMem(const char* label) {
    Serial.printf("[MEM] %s -- Heap: %u  PSRAM: %u\n",
                  label, ESP.getFreeHeap(), ESP.getFreePsram());
}

static void speakerDecay() {
    audioEngine.stopPlayback();
    delay(100);   // let speaker cone settle
    audioEngine.restartMicI2S();
    Serial.println(">> Mic open");
}

static void goIdle() {
    audioEngine.stopPlayback();
    audioEngine.resetMicBuffer();
    wakeWord.reset();
    stateMachine.setState(STATE_IDLE_LISTENING);
    animManager.setAnimation(ANIM_IDLE);
}

// Thorough mic recovery after a long blocking operation (image gen).
// The I2S DMA overflows during 15-30s of blocking, leaving stale data.
// resetMicBuffer() only drains 2 reads — we need to fully flush the DMA
// and give it a moment to start producing fresh samples.
static void flushMicAfterBlock() {
    audioEngine.restartMicI2S();
}

// ─────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ZION Booting ===");
    printMem("boot");

    Wire.begin();
    if (!g_pcf.begin(PCF_I2C_ADDR, &Wire)) {
        Serial.println("FATAL: PCF8574 not found"); while (1) delay(1000);
    }
    g_pcf.pinMode(PCF_TFT_RST, OUTPUT);
    g_pcf.digitalWrite(PCF_TFT_RST, HIGH);
    Serial.println("PCF8574 OK");

    // TFT reset pulse — minimized to reduce white screen time
    g_pcf.digitalWrite(PCF_TFT_RST, LOW);  delay(10);
    g_pcf.digitalWrite(PCF_TFT_RST, HIGH); delay(50);
    tft.init(); tft.setRotation(3); tft.fillScreen(TFT_BLACK);
    Serial.println("TFT OK");

    if (!SDManager_begin()) { Serial.println("FATAL: SD"); while (1) delay(1000); }
    if (!loadConfig())      { Serial.println("FATAL: config.json"); while (1) delay(1000); }
    Serial.printf("Key: %.8s...  Volume: %d%%\n", OPENAI_KEY, CONFIG_VOLUME);

    // ── Boot1 animation plays IMMEDIATELY after SD loads ──
    // This covers perceived startup time — user sees animation
    // instead of a black screen during WiFi/NTP/audio init.
    animManager.begin();
    animManager.showBoot1();

    // ── Heavy init (happens after boot1 finishes) ──
    openAI.begin();
    if (!openAI.connectWiFi()) { Serial.println("FATAL: WiFi"); while (1) delay(1000); }

    configTime(-5 * 3600, 3600, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    if (getLocalTime(&t, 5000)) {
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &t);
        Serial.printf("NTP: %s\n", buf);
    }

    // ── Boot2 animation ──
    animManager.showBoot2();

    // ── Audio + wake word + clips ──
    audioEngine.begin();
    audioEngine.setVolume(CONFIG_VOLUME);

    wakeWord.begin();

    // Set up animation tick callback — called during blocking HTTP
    // so thinking animation + TTS audio keep running.
    openAI.setTickCallback([]() {
        animManager.update();
        audioEngine.updateSpeakerPlayback();
    });

    Serial.println("Generating clip cache...");
    openAI.cacheClips();

    // ── Clear stale mic data from boot, go straight to idle ──
    audioEngine.resetMicBuffer();
    stateMachine.setState(STATE_IDLE_LISTENING);
    animManager.setAnimation(ANIM_IDLE);
    printMem("ready");
    Serial.println("=== ZION ready — say 'Hey Zion' ===\n");
}

// ─────────────────────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────────────────────
void loop() {
    // Animation is handled by a background task during processing states,
    // and by PNG decode during image states. Only tick from loop() otherwise.
    State state = stateMachine.getState();
    bool bgAnimActive = (state == STATE_PROCESSING_STT ||
                         state == STATE_THINKING_LLM   ||
                         state == STATE_SHOWING_IMAGE   ||
                         state == STATE_GENERATING_IMAGE);
    if (!bgAnimActive) {
        animManager.update();
    }

    switch (state) {

        // ── IDLE ───────────────────────────────────────────────
        // Every WAKE_INFERENCE_INTERVAL ms, grab the latest 1 second
        // (16000 samples) and run the FULL non-continuous classifier.
        //
        // Peak detection: trigger on any single raw score above threshold.
        // 1.5s warmup prevents false triggers from stale boot audio.
        // 2s cooldown after each trigger prevents double-fires.
        case STATE_IDLE_LISTENING: {
            audioEngine.feedSilence();
            audioEngine.updateMicBuffer();

            // Tap to wake
            if (isTouched()) {
                stateMachine.setState(STATE_WAKE_DETECTED);
                animManager.setAnimation(ANIM_LISTENING);
                break;
            }

            static uint32_t lastInferMs = 0;
            static uint32_t lastTriggerMs = 0;

            // 3s warmup: skip detection when first entering IDLE
            if (stateMachine.getStateTime() < 3000) break;

            // 2s cooldown after last trigger to prevent double-fires
            if (millis() - lastTriggerMs < 2000 && lastTriggerMs > 0) break;

            if (audioEngine.getAvailableSamples() >= 16000 &&
                millis() - lastInferMs >= WAKE_INFERENCE_INTERVAL) {
                lastInferMs = millis();
                int16_t* buf = (int16_t*)heap_caps_malloc(
                    16000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (buf) {
                    audioEngine.peekSamples(buf, 16000);
                    float conf = wakeWord.processAudio(buf, 16000);
                    heap_caps_free(buf);

                    // Debug: print every score so we can tune threshold
                    if (conf > 0.10f) {
                        Serial.printf("  [wake score: %.2f]\n", conf);
                    }

                    // Peak detection: trigger on any single strong frame
                    if (conf >= WAKE_THRESHOLD) {
                        Serial.printf("\n>> [WAKE: HEY ZION] (raw=%.2f)\n", conf);
                        lastTriggerMs = millis();
                        stateMachine.setState(STATE_WAKE_DETECTED);
                        animManager.setAnimation(ANIM_LISTENING);
                    }
                }
            }
            break;
        }

        // ── WAKE DETECTED — play "Yes?" ack clip ──────────────
        case STATE_WAKE_DETECTED: {
            if (audioEngine.startPlaybackFromFile(CLIP_ACK_PATH)) {
                stateMachine.setState(STATE_PLAYING_ACK);
            } else {
                speakerDecay();
                audioEngine.startRecording(RECORD_SECONDS);
                stateMachine.setState(STATE_RECORDING);
                animManager.setAnimation(ANIM_LISTENING);
                Serial.println(">> [LISTENING]");
            }
            break;
        }

        // ── PLAYING ACK ───────────────────────────────────────
        case STATE_PLAYING_ACK: {
            for (int i = 0; i < 4; i++) audioEngine.updateSpeakerPlayback();
            if (!audioEngine.isSpeaking() || stateMachine.getStateTime() > 4000) {
                speakerDecay();
                audioEngine.startRecording(RECORD_SECONDS);
                stateMachine.setState(STATE_RECORDING);
                animManager.setAnimation(ANIM_LISTENING);
                Serial.println(">> [LISTENING — speak your question]");
            }
            break;
        }

        // ── RECORDING ─────────────────────────────────────────
        case STATE_RECORDING: {
            audioEngine.updateMicBuffer();

            if (audioEngine.isRecordingComplete()) {
                stateMachine.setState(STATE_PROCESSING_STT);
                animManager.setAnimation(ANIM_THINK);
                break;
            }
            if (stateMachine.getStateTime() > (uint32_t)((RECORD_SECONDS + 2) * 1000)) {
                Serial.println("Recording: hard timeout");
                stateMachine.setState(STATE_PROCESSING_STT);
                animManager.setAnimation(ANIM_THINK);
            }
            break;
        }

        // ── STT ───────────────────────────────────────────────
        case STATE_PROCESSING_STT: {
            int16_t* audioBuf = nullptr; size_t sampleCnt = 0;
            audioEngine.getRecordedAudio(&audioBuf, &sampleCnt);

            if (sampleCnt == 0) {
                Serial.println(">> [No audio captured — idle]"); goIdle(); break;
            }

            uint32_t pipelineStart = millis();  // track total pipeline time

            // Start background task so thinking dots keep cycling
            // through the entire STT → LLM → TTS download pipeline.
            startThinkingTask();

            String transcript = openAI.sendSTT(audioBuf, sampleCnt);
            Serial.printf(">> [YOU]: \"%s\"\n", transcript.c_str());

            if (transcript.length() == 0) {
                Serial.println(">> [STT empty — idle]");
                stopThinkingTask(); goIdle(); break;
            }

            pendingTranscript    = transcript;
            pendingImageKeyword  = OpenAIClient::isImageRequest(transcript);
            stateMachine.setState(STATE_THINKING_LLM);
            break;
        }

        // ── LLM (with function calling) ──────────────────────
        // Background animation task is already running from STT state.
        // The LLM can either:
        //   A) Return a text response → normal TTS flow
        //   B) Call generate_image tool → image generation flow
        //   C) Return text but keywords detected → force image
        case STATE_THINKING_LLM: {
            Serial.println(">> [Thinking...]");

            LLMResult llmResp = openAI.sendLLM(pendingTranscript);

            bool doImage = llmResp.generateImage;

            // Keyword fallback: if keywords detected but LLM gave text
            if (!doImage && pendingImageKeyword && llmResp.text.length() > 0) {
                Serial.println(">> [Keyword fallback -> forcing image generation]");
                doImage = true;
                llmResp.imagePrompt    = pendingTranscript;
                llmResp.spokenResponse = "Here's what I created for you!";
            }

            pendingTranscript = "";
            pendingImageKeyword = false;

            if (doImage) {
                stopThinkingTask();
                pendingImagePrompt    = llmResp.imagePrompt;
                pendingSpokenResponse = llmResp.spokenResponse;
                Serial.printf(">> [Image prompt]: \"%s\"\n", pendingImagePrompt.c_str());
                stateMachine.setState(STATE_GENERATING_IMAGE);
                break;
            }

            // ── NORMAL TEXT PATH — download to PSRAM, then play ──
            if (llmResp.text.length() == 0) {
                Serial.println(">> [LLM empty — idle]");
                stopThinkingTask(); goIdle(); break;
            }
            Serial.printf(">> [ZION]: \"%s\"\n", llmResp.text.c_str());

            // Free record buffer — STT already copied the data.
            audioEngine.freeRecordBuffer();     // ~256KB back

            // Free thinking animation BEFORE TTS alloc — the 4 think
            // frames (600KB) fragment PSRAM and block the 768KB contiguous alloc.
            stopThinkingTask();
            animManager.freeCache();

            {
                const size_t TTS_BUF_CAP = 768 * 1024;
                uint8_t* ttsBuf = (uint8_t*)heap_caps_malloc(TTS_BUF_CAP, MALLOC_CAP_SPIRAM);

                if (ttsBuf) {
                    Serial.println(">> [Downloading TTS to PSRAM...]");
                    TtsAccumulator accum(ttsBuf, TTS_BUF_CAP, nullptr);
                    uint32_t t0 = millis();
                    bool ok = openAI.downloadTTSToStream(llmResp.text, &accum);
                    Serial.printf(">> [TTS: %ums, %u/%u bytes (%.0f%%)]\n",
                                  (unsigned)(millis() - t0),
                                  (unsigned)accum.getBytesWritten(),
                                  (unsigned)TTS_BUF_CAP,
                                  100.0f * accum.getBytesWritten() / TTS_BUF_CAP);

                    size_t samples = accum.getSampleCount();
                    if (!ok || samples == 0) {
                        heap_caps_free(ttsBuf);
                        Serial.println(">> [TTS failed — idle]");
                        goIdle(); break;
                    }

                    animManager.setAnimation(ANIM_TALK);
                    audioEngine.startPlaybackZeroCopy((int16_t*)ttsBuf, samples);
                } else {
                    Serial.println(">> [PSRAM alloc failed — SD fallback]");
                    printMem("alloc failed");
                    Serial.println(">> [Downloading TTS...]");
                    String ttsPath;
                    if (!openAI.streamTTS(llmResp.text, ttsPath)) {
                        Serial.println(">> [TTS failed — idle]"); goIdle(); break;
                    }
                    animManager.setAnimation(ANIM_TALK);
                    if (!audioEngine.startPlaybackFromFile(ttsPath)) {
                        Serial.println(">> [Playback failed — idle]"); goIdle(); break;
                    }
                }
            }

            stateMachine.setState(STATE_SPEAKING_TTS);
            Serial.println(">> [SPEAKING]");
            break;
        }

        // ── GENERATING IMAGE ──────────────────────────────────
        // animation runs during DALL-E API wait.
        // Split into: TTS → animate during requestImageURL →
        // stop animation → download image to SD → display.
        case STATE_GENERATING_IMAGE: {
            Serial.println(">> [Image request — speaking first...]");
            printMem("before image gen");

            // 1. Download TTS for spoken response
            String ttsPath = "";
            if (pendingSpokenResponse.length() > 0) {
                animManager.setAnimation(ANIM_THINK);
                openAI.streamTTS(pendingSpokenResponse, ttsPath);
            }

            // 2. Play TTS to completion — user hears response
            if (ttsPath.length() > 0) {
                audioEngine.startPlaybackFromFile(ttsPath);
                animManager.setAnimation(ANIM_TALK);
                while (audioEngine.isSpeaking()) {
                    audioEngine.updateSpeakerPlayback();
                    animManager.update();
                    delay(1);
                }
            }

            // 3. Start thinking animation in a background task.
            //    requestImageURL only uses WiFi (no SD access),
            //    so animation can safely read BMP frames from SD.
            animManager.setAnimation(ANIM_THINK);
            animManager.update();  // show first frame immediately

            static volatile bool _animTaskRunning = false;
            static volatile bool _animTaskDone    = false;
            _animTaskRunning = true;
            _animTaskDone    = false;
            TaskHandle_t animTaskHandle = nullptr;

            xTaskCreatePinnedToCore(
                [](void* param) {
                    while (_animTaskRunning) {
                        animManager.update();
                        vTaskDelay(pdMS_TO_TICKS(50));   // fast tick for smooth thinking dots
                    }
                    _animTaskDone = true;
                    vTaskDelete(nullptr);  // self-delete
                },
                "imgAnim",    // task name
                8192,         // stack — pushFrame needs room for SPI DMA
                nullptr,      // param
                1,            // priority (low — yield to main)
                &animTaskHandle,
                1             // Core 1 (same as Arduino loop)
            );

            Serial.println(">> [Generating image (animation running)...]");

            // 4. DALL-E API call — blocks 5-15 seconds.
            //    Animation task keeps cycling on Core 1 during this.
            String imageUrl = openAI.requestImageURL(pendingImagePrompt);

            // 5. STOP animation task before SD access.
            //    Signal exit, then wait for task to finish its
            //    current SPI operation and self-delete.
            _animTaskRunning = false;
            uint32_t waitStart = millis();
            while (!_animTaskDone && (millis() - waitStart) < 2000) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            animTaskHandle = nullptr;
            Serial.println(">> [Animation task stopped]");

            pendingImagePrompt    = "";
            pendingSpokenResponse = "";

            if (imageUrl.length() == 0) {
                Serial.println(">> [DALL-E API failed — idle]");
                tft.fillScreen(TFT_BLACK);
                flushMicAfterBlock();
                goIdle();
                break;
            }

            // 6. Download image to SD (no animation — SPI bus safety)
            Serial.println(">> [Downloading image...]");
            bool imgOk = openAI.downloadImageToSD(imageUrl, IMAGE_TMP_PATH);
            imageUrl = "";  // free the long URL string

            if (!imgOk) {
                Serial.println(">> [Image download failed — idle]");
                tft.fillScreen(TFT_BLACK);
                flushMicAfterBlock();
                goIdle();
                break;
            }

            // 7. Display PNG on TFT
            if (!displayPNG(IMAGE_TMP_PATH)) {
                Serial.println(">> [PNG display failed — idle]");
                SD.remove(IMAGE_TMP_PATH);
                tft.fillScreen(TFT_BLACK);
                flushMicAfterBlock();
                goIdle();
                break;
            }

            stateMachine.setState(STATE_SHOWING_IMAGE);
            printMem("after image gen");
            Serial.println(">> [SHOWING IMAGE]");
            break;
        }

        // ── SHOWING IMAGE ────────────────────────────────────
        // Image is on TFT, TTS is playing. Wait for:
        //   - TTS playback to finish
        //   - Then hold image for IMAGE_DISPLAY_MS total
        //   - Clear screen, recover mic, return to idle
        case STATE_SHOWING_IMAGE: {
            for (int i = 0; i < 4; i++) audioEngine.updateSpeakerPlayback();

            bool ttsDone = !audioEngine.isSpeaking();
            bool timeUp  = stateMachine.getStateTime() > IMAGE_DISPLAY_MS;

            if (ttsDone && timeUp) {
                Serial.println(">> [Image done — cleaning up]");
                SD.remove(IMAGE_TMP_PATH);
                // Clear TFT so image doesn't linger behind animation
                tft.fillScreen(TFT_BLACK);
                // Thorough mic recovery after long blocking period
                flushMicAfterBlock();
                goIdle();
                break;
            }

            // Safety timeout (60s)
            if (stateMachine.getStateTime() > 60000) {
                Serial.println(">> [Image display timeout]");
                SD.remove(IMAGE_TMP_PATH);
                tft.fillScreen(TFT_BLACK);
                flushMicAfterBlock();
                goIdle();
            }
            break;
        }

        // ══════════════════════════════════════════════════════
        //  NORMAL CONVERSATION STATES (completely unchanged)
        // ══════════════════════════════════════════════════════

        // ── SPEAKING TTS ──────────────────────────────────────
        case STATE_SPEAKING_TTS: {
            // 6 pumps = 64ms of audio queued per loop.
            // pushFrame takes ~45ms SPI. DMA drains ~50ms per loop.
            // Net surplus: +14ms per loop — DMA never starves.
            for (int i = 0; i < 6; i++) {
                audioEngine.updateSpeakerPlayback();
            }

            // Tap to cancel response
            if (isTouched()) {
                goIdle();
                break;
            }

            if (!audioEngine.isSpeaking()) {
                printMem("after TTS");
                speakerDecay();
                audioEngine.startRecording(ACTIVE_LISTEN_SECONDS);
                stateMachine.setState(STATE_ACTIVE_LISTENING);
                animManager.setAnimation(ANIM_LISTENING);
                Serial.println(">> [ACTIVE LISTENING]");
                break;
            }

            if (stateMachine.getStateTime() > 120000) {
                Serial.println("SPEAKING: 120s timeout");
                openAI.clearHistory(); goIdle();
            }
            break;
        }

        // ── FOLLOWUP PROMPT ───────────────────────────────────
        case STATE_FOLLOWUP_PROMPT: {
            for (int i = 0; i < 4; i++) audioEngine.updateSpeakerPlayback();
            if (!audioEngine.isSpeaking() || stateMachine.getStateTime() > 8000) {
                speakerDecay();
                audioEngine.startRecording(FOLLOWUP_RECORD_SECONDS);
                stateMachine.setState(STATE_FOLLOWUP_LISTENING);
                animManager.setAnimation(ANIM_LISTENING);
                Serial.println(">> [LISTENING for yes/no]");
            }
            break;
        }

        // ── FOLLOWUP LISTENING ────────────────────────────────
        case STATE_FOLLOWUP_LISTENING: {
            audioEngine.updateMicBuffer();

            if (audioEngine.isRecordingComplete()) {
                stateMachine.setState(STATE_PROCESSING_FOLLOWUP);
                animManager.setAnimation(ANIM_THINK);
                break;
            }
            if (stateMachine.getStateTime() >
                    (uint32_t)((FOLLOWUP_RECORD_SECONDS + 2) * 1000)) {
                Serial.println(">> [No response — goodbye]");
                if (audioEngine.startPlaybackFromFile(CLIP_GOODBYE_PATH)) {
                    stateMachine.setState(STATE_PLAYING_GOODBYE);
                    animManager.setAnimation(ANIM_TALK);
                } else {
                    openAI.clearHistory(); goIdle();
                }
            }
            break;
        }

        // ── PROCESSING FOLLOWUP ───────────────────────────────
        case STATE_PROCESSING_FOLLOWUP: {
            int16_t* audioBuf = nullptr; size_t sampleCnt = 0;
            audioEngine.getRecordedAudio(&audioBuf, &sampleCnt);

            animManager.update();  // show thinking frame
            String reply = "";
            if (sampleCnt >= 4000) {
                reply = openAI.sendSTT(audioBuf, sampleCnt);
                Serial.printf(">> [Follow-up]: \"%s\"\n", reply.c_str());
            } else {
                Serial.println(">> [No follow-up speech]");
            }

            if (openAI.isYesResponse(reply)) {
                Serial.println(">> [YES — continue conversation]");
                if (audioEngine.startPlaybackFromFile(CLIP_LISTENING_PATH)) {
                    stateMachine.setState(STATE_PLAYING_LISTENING);
                    animManager.setAnimation(ANIM_TALK);
                    Serial.println(">> [ZION: 'I'm listening!']");
                } else {
                    speakerDecay();
                    audioEngine.startRecording(RECORD_SECONDS);
                    stateMachine.setState(STATE_RECORDING);
                    animManager.setAnimation(ANIM_LISTENING);
                    Serial.println(">> [LISTENING — speak your question]");
                }
            } else {
                Serial.println(">> [NO — ending conversation]");
                if (audioEngine.startPlaybackFromFile(CLIP_GOODBYE_PATH)) {
                    stateMachine.setState(STATE_PLAYING_GOODBYE);
                    animManager.setAnimation(ANIM_TALK);
                    Serial.println(">> [ZION: 'I'll be here if you need me!']");
                } else {
                    openAI.clearHistory(); printMem("end"); goIdle();
                }
            }
            break;
        }

        // ── PLAYING "I'm listening!" → start recording ────────
        case STATE_PLAYING_LISTENING: {
            for (int i = 0; i < 4; i++) audioEngine.updateSpeakerPlayback();
            if (!audioEngine.isSpeaking() || stateMachine.getStateTime() > 5000) {
                speakerDecay();
                audioEngine.startRecording(RECORD_SECONDS);
                stateMachine.setState(STATE_RECORDING);
                animManager.setAnimation(ANIM_LISTENING);
                Serial.println(">> [LISTENING — speak your question]");
            }
            break;
        }

        // ── PLAYING goodbye → go idle ─────────────────────────
        case STATE_PLAYING_GOODBYE: {
            for (int i = 0; i < 4; i++) audioEngine.updateSpeakerPlayback();
            if (!audioEngine.isSpeaking() || stateMachine.getStateTime() > 5000) {
                openAI.clearHistory();
                printMem("end");
                goIdle();
            }
            break;
        }

        // ── ACTIVE LISTENING (no wake word needed) ───────────
        case STATE_ACTIVE_LISTENING: {
            audioEngine.updateMicBuffer();

            if (audioEngine.isRecordingComplete()) {
                int16_t* buf = nullptr; size_t cnt = 0;
                audioEngine.getRecordedAudio(&buf, &cnt);

                if (cnt >= 4000) {
                    stateMachine.setState(STATE_PROCESSING_STT);
                    animManager.setAnimation(ANIM_THINK);
                } else {
                    openAI.clearHistory();
                    goIdle();
                }
                break;
            }

            if (stateMachine.getStateTime() > (uint32_t)((ACTIVE_LISTEN_SECONDS + 2) * 1000)) {
                openAI.clearHistory();
                goIdle();
            }
            break;
        }

        // ── ERROR ─────────────────────────────────────────────
        case STATE_ERROR: {
            audioEngine.stopPlayback();
            animManager.setAnimation(ANIM_ERROR);
            Serial.println("ERROR — recovering in 5s");
            delay(5000); openAI.clearHistory(); goIdle();
            break;
        }
    }
}