#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  File Manager — SPIFFS browser for /payloads and /captures
//  View · Delete · Info for files stored on-device
//  WiFi File Server — AsyncWebServer AP for remote access
// ─────────────────────────────────────────────────────────────

void file_manager_run();          // Main entry: on-device browser + WiFi server option
void file_wifi_server_run();      // WiFi AP web file manager (download / upload / delete)
