#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────
//  I2S Microphone — ICS-43434  (I2S_NUM_0)
// ─────────────────────────────────────────────────────────────
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_SCK         12          // BCLK → D12
#define MIC_WS          13          // LRCL → D13
#define MIC_SD          A0          // DOUT → A0
// SEL wired to GND → left channel only

// ─────────────────────────────────────────────────────────────
//  I2S Speaker — MAX98357A  (I2S_NUM_1)
// ─────────────────────────────────────────────────────────────
#define SPK_I2S_PORT    I2S_NUM_1
#define SPK_LRC         A3
#define SPK_BCLK        A2
#define SPK_DIN         A1
// GAIN floating = 9 dB | SD floating = always on

// ─────────────────────────────────────────────────────────────
//  SPI — TFT ILI9341 + SD share bus
//  ESP32-S3 Feather hardware SPI: SCK=36 MISO=37 MOSI=35
// ─────────────────────────────────────────────────────────────
#define TFT_CS          5
#define TFT_DC          6
#define TFT_RST         -1          // driven by PCF8574 P1
#define SD_CS           9

// ─────────────────────────────────────────────────────────────
//  Touch (resistive 4-wire)
// ─────────────────────────────────────────────────────────────
#define TOUCH_YP        A4
#define TOUCH_XM        A5
#define TOUCH_YM        10
#define TOUCH_XP        11

// ─────────────────────────────────────────────────────────────
//  PCF8574 I2C GPIO expander — STEMMA QT
// ─────────────────────────────────────────────────────────────
#define PCF_I2C_ADDR    0x20
#define PCF_TFT_RST     1           // P1 → TFT RST

// ─────────────────────────────────────────────────────────────
//  Audio
// ─────────────────────────────────────────────────────────────
#define SAMPLE_RATE             16000   // mic / wake word model
#define TTS_SAMPLE_RATE         24000   // OpenAI TTS PCM output
#define SAMPLE_BITS             16

// Confirmed in echo prototype: (raw >> 16) << 5 = clean, crisp audio
#define MIC_GAIN                5       // 1<<5 = 32x gain

// ─────────────────────────────────────────────────────────────
//  Recording
// ─────────────────────────────────────────────────────────────
#define RECORD_SECONDS          5       // max recording after wake word
#define FOLLOWUP_RECORD_SECONDS 5       // followup capture
#define ACTIVE_LISTEN_SECONDS   5       // post-response listen window

// Calibrated against observed ambient noise floor of 280-650 RMS.
#define SPEECH_RMS_THRESHOLD    600
#define SILENCE_RMS_THRESHOLD   300
#define SILENCE_AFTER_SPEECH_MS 1000
#define NO_SPEECH_TIMEOUT_MS    5000

// ─────────────────────────────────────────────────────────────
//  Wake word
// ─────────────────────────────────────────────────────────────
#define WAKE_THRESHOLD          0.40f   // peak detection: based on raw scores 0.39-0.61 from testing
#define WAKE_INFERENCE_INTERVAL 300     // ms between inference runs
#define WAKE_SMOOTH_FRAMES      3       // moving average over last N frames

// ─────────────────────────────────────────────────────────────
//  Cached audio clips on SD
// ─────────────────────────────────────────────────────────────
#define CLIP_ACK_PATH           "/clips/ack.pcm"
#define CLIP_FOLLOWUP_PATH      "/clips/followup.pcm"
#define CLIP_LISTENING_PATH     "/clips/listening.pcm"
#define CLIP_GOODBYE_PATH       "/clips/goodbye.pcm"

// ─────────────────────────────────────────────────────────────
//  Image generation (DALL-E)
// ─────────────────────────────────────────────────────────────
#define IMAGE_TMP_PATH          "/tmp_img.png"
#define IMAGE_DISPLAY_MS        8000    // show image for at least 8 seconds

// ─────────────────────────────────────────────────────────────
//  Conversation history
// ─────────────────────────────────────────────────────────────
#define MAX_HISTORY_TURNS       3
#define LLM_MAX_TOKENS          150

// ─────────────────────────────────────────────────────────────
//  State machine
// ─────────────────────────────────────────────────────────────
enum State {
    STATE_IDLE_LISTENING,
    STATE_WAKE_DETECTED,
    STATE_PLAYING_ACK,
    STATE_RECORDING,
    STATE_PROCESSING_STT,
    STATE_THINKING_LLM,
    STATE_SPEAKING_TTS,
    STATE_FOLLOWUP_PROMPT,
    STATE_FOLLOWUP_LISTENING,
    STATE_PROCESSING_FOLLOWUP,
    STATE_PLAYING_LISTENING,
    STATE_PLAYING_GOODBYE,
    STATE_GENERATING_IMAGE,     // DALL-E API + download PNG
    STATE_SHOWING_IMAGE,        // display PNG + play TTS
    STATE_ACTIVE_LISTENING,     // post-response listen without wake word
    STATE_ERROR
};

enum AnimType {
    ANIM_IDLE,
    ANIM_LISTENING,
    ANIM_THINK,
    ANIM_TALK,
    ANIM_ERROR
};

// ─────────────────────────────────────────────────────────────
//  Tick callback — called by OpenAIClient during blocking HTTP
//  operations so animation + audio playback stay alive.
// ─────────────────────────────────────────────────────────────
typedef void (*TickCallback)();

extern char     WIFI_SSID[64];
extern char     WIFI_PASS[64];
extern char     OPENAI_KEY[192];
extern String   SYSTEM_PROMPT;
extern uint8_t  CONFIG_VOLUME;

#endif // CONFIG_H