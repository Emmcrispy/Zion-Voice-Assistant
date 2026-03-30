#pragma once

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <Adafruit_PCF8574.h>
#include "Config.h"

// Global PCF8574 instance — used by main .ino and SDManager
extern Adafruit_PCF8574 g_pcf;

bool SDManager_begin();
bool loadConfig();
bool SDManager_appendTestLine(const char* line);
void SDManager_select();
void SDManager_deselect();