# ZION - Desktop AI Voice Assistant

A voice assistant built from scratch on an ESP32-S3 microcontroller. Wake word detection, speech-to-text, conversational AI, text-to-speech, and image generation running on a battery-powered device designed for desktop use.

**Portfolio:** [embeddedmind.dev/project/zion-voice-assistant](https://embeddedmind.dev/project/zion-voice-assistant)

---

## What It Does

Zion is a fully self-contained voice assistant with a custom wake word engine, animated character face, multi-turn conversation memory, and on-device image generation. All audio capture, playback, animation, and state management happen on the ESP32-S3. The voice pipeline uses OpenAI's cloud APIs for speech-to-text, language modeling, and text-to-speech.

- **Wake word detection** - "Hey Zion" triggers via an Edge Impulse neural network running locally on the MCU using peak detection at a 0.40 confidence threshold. No cloud dependency for activation.
- **Voice conversation** - Zion transcribes speech via Whisper, processes the question through GPT-4.1-nano, and responds through a speaker using gpt-4o-mini-tts with the nova voice.
- **Active listening** - After responding, Zion listens for 5 seconds without requiring the wake word again. Multi-turn conversations flow naturally.
- **Image generation** - Ask Zion to draw or create something and it routes the request to DALL-E 2 via GPT-4.1-nano function calling, downloads the 256x256 PNG, and displays it on the TFT.
- **Animated face** - Idle blinking, listening pulse, talking mouth animation, and thinking dots rendered from PSRAM-cached RGB565 frames on an ILI9341 TFT.
- **Touch interaction** - Tap the screen to wake Zion without the wake word, or tap during a response to cancel playback.
- **Configurable personality** - System prompt loaded from SD card defines Zion's voice, boundaries, and conversational style.

---

## Hardware

| Component | Part | Role | Cost |
|-----------|------|------|------|
| MCU | Adafruit Feather ESP32-S3 | Dual-core 240 MHz, 2 MB PSRAM, WiFi, dual I2S | $17.50 |
| Microphone | ICS-43434 (I2S MEMS) | 24-bit audio capture at 16 kHz, left channel | $4.95 |
| Amplifier | MAX98357A (I2S Class D) | 3W speaker driver, 24 kHz PCM playback | $5.95 |
| Display | ILI9341 2.8" TFT (320x240) | Animation rendering, image display, boot sequence | $29.95 |
| GPIO Expander | PCF8574 (I2C, addr 0x20) | TFT reset line | $4.95 |
| Power | 3.7V LiPo + USB-C | Battery operation via Feather charging circuit | $14.95 |
| Speaker | 3W 4 Ohm enclosed | All audio output | $7.50 |
| Breakout | Terminal Block FeatherWing (ID: 2926) | Wiring interface and on/off switch | $14.95 |
| USB | Panel mount USB-C extension cable | External charging and serial monitor without opening enclosure | $9.95 |
| **Total** | | | **$110.65** |

MicroSD (FAT32) is attached to the ILI9341 TFT breakout. Resistive 4-wire touch is built into the TFT panel.

Enclosure designed in OnShape and 3D printed in PLA.

---

## Wiring

| Signal | Pin | Notes |
|--------|-----|-------|
| Mic BCLK | D12 | I2S Port 0 |
| Mic LRCLK | D13 | I2S Port 0 |
| Mic DOUT | A0 | I2S Port 0 data in |
| Mic SEL | GND | Left channel select |
| Spk BCLK | A2 | I2S Port 1 |
| Spk LRC | A3 | I2S Port 1 |
| Spk DIN | A1 | I2S Port 1 data out |
| TFT SCK | GPIO36 | Shared SPI |
| TFT MISO | GPIO37 | Shared SPI |
| TFT MOSI | GPIO35 | Shared SPI |
| TFT CS | D5 | |
| SD CS | D9 | |
| TFT DC | D6 | |
| TFT RST | PCF8574 P1 | Via I2C expander |
| Touch Y+ | A4 | |
| Touch X+ | D11 | |
| Touch Y- | D10 | |
| Touch X- | A5 | |
| PCF8574 SDA | SDA (GPIO3) | I2C bus |
| PCF8574 SCL | SCL (GPIO4) | I2C bus |
| TFT LITE | 3.3V direct | See note below |

**Wiring constraint:** The TFT backlight LITE pin must connect directly to 3.3V, never to an analog-capable GPIO. Routing it to an ADC pin allows backlight PWM noise to couple through the ESP32-S3's shared ADC peripheral into the I2S mic data path, producing broadband static on every recording.

---

## Firmware Architecture

The firmware runs a 16-state finite state machine managing the full voice interaction lifecycle:

```
IDLE → WAKE_DETECTED → PLAYING_ACK → RECORDING → PROCESSING_STT
→ THINKING_LLM → SPEAKING_TTS → ACTIVE_LISTENING → (loop or IDLE)
```

With branches for image generation (GENERATING_IMAGE → SHOWING_IMAGE) and follow-up prompts (FOLLOWUP_PROMPT → FOLLOWUP_LISTENING → PROCESSING_FOLLOWUP).

### Key Design Decisions

**TTS downloads fully to PSRAM before playback.** Streaming was attempted early in development but caused audio stuttering. Non-blocking I2S writes were unreliable under CPU load from simultaneous WiFi and display operations. The tradeoff is 2 to 5 seconds of silence while the buffer fills, but playback is clean once it starts.

**Speaker I2S bus stays completely silent during recording.** Even writing zeros to the speaker generates clock transitions on BCLK (768 kHz) and LRC (24 kHz) that radiate electromagnetic noise into the nearby mic signal wires inside the enclosure. The firmware enforces mutual exclusion: speaker DMA only runs during idle where noise is tolerable for wake word detection, never during speech capture.

**Full I2S driver reinstall after every speaker session.** Calling `i2s_stop` and `i2s_start` alone can resume the word-select line out of phase, producing bit-shifted audio on the next recording. A full uninstall, reinstall, and pin reconfiguration forces clean clock regeneration every time.

**Peak detection for wake word instead of smoothed averaging.** The original 3-frame moving average destroyed real detections. Edge Impulse on a MEMS mic produces one strong inference frame surrounded by weak ones. Averaging them together pushed scores below any useful threshold. A single raw score above 0.40 now triggers immediately, with a 3-second warmup and 2-second cooldown to handle false positives.

**TTS retry on stale connections.** The HTTP keep-alive session between the LLM and TTS calls occasionally goes stale. The first TTS attempt reuses the existing TLS session. If it fails, a second attempt opens a fresh connection. This eliminated intermittent silent failures where Zion had a valid response but never spoke it.

**Animation frames freed before TTS buffer allocation.** The thinking animation's four frames (150KB each, 600KB total) fragment PSRAM when allocated in separate blocks. Freeing them before the 768KB contiguous TTS allocation ensures the heap has enough continuous space.

---

## Configuration

Create `config.json` on the SD card root:

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_pass": "YourPassword",
  "openai_key": "sk-...",
  "volume": 80
}
```

Create `system_prompt.txt` on the SD card root with Zion's personality and instructions.

**Neither file is included in this repository.**

---

## SD Card Structure

```
/
├── config.json
├── system_prompt.txt
├── anims/
│   ├── 1stboot/        frame_0.bin ... frame_N.bin
│   ├── 2ndboot/
│   ├── idle/           3 frames
│   ├── listening/      3 frames
│   ├── talking/        4 frames
│   └── thinking/       4 frames
└── clips/              generated at boot
    ├── ack.pcm
    ├── listening.pcm
    ├── followup.pcm
    └── goodbye.pcm
```

Animation frames are 320x240 raw RGB565 little-endian `.bin` files (153,600 bytes each).

Audio clips in `/clips/` are regenerated from TTS on every boot to pick up any voice or prompt changes.

---

## Building

1. Open `PortableAI_Assistant.ino` in Arduino IDE
2. Board: **Adafruit Feather ESP32-S3 (2MB PSRAM)**
3. Partition scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)**
4. Install libraries: TFT_eSPI, ArduinoJson, PNGdec, Adafruit PCF8574
5. Configure TFT_eSPI `User_Setup.h` for ILI9341 with the pin assignments listed above
6. Add your Edge Impulse wake word model library to Arduino's libraries folder
7. Prepare SD card with animation frames, config.json, and system_prompt.txt
8. Compile and upload

---

## Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Mic gain | 8x (<<3) | 32x clipped speech above 80 dB SPL inside enclosure |
| Wake threshold | 0.40 | Based on observed raw scores of 0.39 to 0.61 |
| Wake warmup | 3 seconds | Prevents false trigger from stale audio after entering idle |
| Wake cooldown | 2 seconds | Prevents double triggers on a single utterance |
| Speech RMS | 600 | Rejects ambient room noise at 2 to 3 foot range |
| Consecutive reads | 2 (~32ms) | Rejects brief transients like door closes |
| Silence RMS | 300 | End-of-speech detection |
| Record limit | 5 seconds | All three recording states |
| Max tokens | 150 | Keeps TTS output under 768KB buffer limit |
| TTS buffer | 768 KB | Approximately 15 seconds of 24 kHz mono PCM |
| Talk animation | 250ms per frame | Skips frame 0 (mouth closed), cycles frames 1 through 3 |
| Mic stabilization | 50ms | Per ICS-43434 datasheet clock lock time |
| Conversation history | 3 turns | Balances context quality with request payload size |

---

## Source Files

| File | Purpose |
|------|---------|
| `PortableAI_Assistant.ino` | Main firmware: setup, loop, 16-state machine, touch input, PNG display |
| `Config.h` | Pin definitions, thresholds, state enum, API configuration |
| `AudioEngine.cpp / .h` | I2S mic and speaker management, DMA, recording, playback, gain, ring buffer |
| `OpenAIClient.cpp / .h` | WiFi, Whisper STT, GPT-4.1-nano LLM, TTS with retry, DALL-E, conversation history |
| `AnimationManager.cpp / .h` | PSRAM frame cache, boot sequence, per-animation timing |
| `WakeWordEngine.cpp / .h` | Edge Impulse inference wrapper |
| `StateMachine.cpp / .h` | State tracking, timing, named state logging |
| `RingBuffer.cpp / .h` | Thread-safe circular buffer for wake word audio window |
| `SDManager.cpp / .h` | SD card initialization, config.json and system_prompt.txt parsing |
| `Ttsstreambuffer.h` | PSRAM accumulator stream for TTS download |

---

## Known Limitations

- WiFi required for all inference beyond wake word. No offline conversation fallback.
- Wake word accuracy depends on your Edge Impulse model and training data. You will need to train your own model at edgeimpulse.com with your chosen wake phrase and export it as an Arduino library. For best results, record your training samples through the actual ICS-43434 microphone on the device, not through a phone or laptop mic. The frequency response and noise floor differ significantly between microphones, and a model trained on laptop audio will underperform on embedded hardware.
- 2.5 to 7 second processing latency between question and response start.
- TTS responses capped at roughly 15 seconds (768KB buffer at 24 kHz).
- Omnidirectional mic picks up room noise. Sustained background audio can trigger false recordings.

---

## Author

**Emmanuel Crispin** - [embeddedmind.dev](https://embeddedmind.dev)

Sole developer. Hardware, firmware, cloud integration, and enclosure design.

Built January 7 to March 27, 2026.
