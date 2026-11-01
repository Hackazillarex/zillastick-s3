#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ─────────────────────────────────────────────────────────────
//  WiFi Module
//  Scan · Deauth · Evil Portal · Beacon Spam · Probe Sniff
// ─────────────────────────────────────────────────────────────

#define WIFI_MAX_NETS   40
#define DEAUTH_REASON   0x0007   // Class 3 frame from nonassociated STA

struct NetInfo {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;
};

void wifi_module_run();       // Entry point called from main menu

// Sub-features (also callable directly)
void wifi_scan_run();
void wifi_deauth_run();
void wifi_evil_portal_run();
void wifi_beacon_spam_run();
void wifi_probe_sniff_run();
void wifi_deauth_detect_run();  // Passive deauth/disassoc frame detection
void wifi_evil_twin_run();      // Clone AP + deauth real AP + captive portal
void wifi_network_scanner_run(); // Connect to WiFi, host discovery, port scan
void wifi_hs_passive_run();      // WPA2 4-way handshake capture (passive monitor)
void wifi_hs_active_run();       // WPA2 4-way handshake capture (deauth-forced)

// Utilities
const char* auth_str(uint8_t mode);
void bssid_str(const uint8_t* bssid, char* out);  // out must be 18 bytes
