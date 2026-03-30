#include "SDManager.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// PCF8574 global instance
Adafruit_PCF8574 g_pcf;

// Config volume — default 80, overridden by config.json
uint8_t CONFIG_VOLUME = 80;

// ─────────────────────────────────────────────────────────────
void SDManager_select()   { digitalWrite(SD_CS, LOW);  }
void SDManager_deselect() { digitalWrite(SD_CS, HIGH); }

// ─────────────────────────────────────────────────────────────
static bool initSD() {
    SPIClass& sharedSPI = tft.getSPIinstance();
    // Physical SPI pins: SCK=36, MISO=37, MOSI=35
    sharedSPI.begin(36, 37, 35, SD_CS);

    // Start at 4 MHz for reliability, then bump to 20 MHz after init
    if (!SD.begin(SD_CS, sharedSPI, 4000000)) {
        Serial.println("SD.begin() FAILED at 4MHz");
        return false;
    }
    // Reinit at higher speed — ESP32 handles 20 MHz fine on short traces
    SD.end();
    if (!SD.begin(SD_CS, sharedSPI, 20000000)) {
        Serial.println("SD.begin() FAILED at 20MHz — falling back to 4MHz");
        if (!SD.begin(SD_CS, sharedSPI, 4000000)) {
            Serial.println("SD.begin() FAILED at fallback 4MHz");
            return false;
        }
    }

    uint64_t cardBytes = SD.cardSize();
    Serial.printf("SD OK — %.1f GB | Type: %d\n",
                  (float)cardBytes / (1024.0f * 1024.0f * 1024.0f),
                  (int)SD.cardType());
    return true;
}

// ─────────────────────────────────────────────────────────────
bool SDManager_begin() {
    pinMode(SD_CS, OUTPUT);
    SDManager_deselect();
    return initSD();
}

// ─────────────────────────────────────────────────────────────
bool SDManager_appendTestLine(const char* line) {
    File f = SD.open("/test.txt", FILE_APPEND);
    if (!f) {
        Serial.println("SDManager: cannot open /test.txt");
        return false;
    }
    f.println(line);
    f.close();
    return true;
}

// ─────────────────────────────────────────────────────────────
bool loadConfig() {
    File f = SD.open("/config.json");
    if (!f) {
        Serial.println("loadConfig: /config.json not found");
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("loadConfig: JSON parse error: %s\n", err.c_str());
        return false;
    }

    // WiFi
    strncpy(WIFI_SSID, doc["wifi_ssid"] | "", sizeof(WIFI_SSID) - 1);
    WIFI_SSID[sizeof(WIFI_SSID) - 1] = '\0';

    strncpy(WIFI_PASS, doc["wifi_pass"] | "", sizeof(WIFI_PASS) - 1);
    WIFI_PASS[sizeof(WIFI_PASS) - 1] = '\0';

    // OpenAI key
    strncpy(OPENAI_KEY, doc["openai_key"] | "", sizeof(OPENAI_KEY) - 1);
    OPENAI_KEY[sizeof(OPENAI_KEY) - 1] = '\0';

    // Volume (0-100)
    CONFIG_VOLUME = (uint8_t)constrain((int)(doc["volume"] | 80), 0, 100);

    // System prompt from /system_prompt.txt
    File prompt = SD.open("/system_prompt.txt");
    if (prompt) {
        SYSTEM_PROMPT = "";
        while (prompt.available()) {
            SYSTEM_PROMPT += (char)prompt.read();
        }
        prompt.close();
        SYSTEM_PROMPT.trim();
        Serial.printf("System prompt loaded: %d chars\n", SYSTEM_PROMPT.length());
    } else {
        // Sensible default if file is missing
        SYSTEM_PROMPT = "You are Zion, a helpful and concise voice assistant. "
                        "Always respond in English regardless of the input language. "
                        "Keep responses under 3 sentences unless detail is requested. "
                        "Be conversational and friendly.";
        Serial.println("system_prompt.txt not found — using default");
    }

    Serial.printf("loadConfig OK | Volume: %d\n", CONFIG_VOLUME);
    return true;
}