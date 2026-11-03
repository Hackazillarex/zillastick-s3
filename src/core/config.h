#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  Persistent config stored as JSON in SPIFFS
// ─────────────────────────────────────────────────────────────

struct PQConfig {
    // RF
    float    rf_freq       = 433.92f;    // MHz
    uint8_t  rf_modulation = 0;          // 0=ASK/OOK, 1=FSK, 2=GFSK, 3=MSK, 4=4FSK
    uint8_t  rf_bw         = 0;          // 0=203kHz, 1=101kHz, 2=58kHz
    uint8_t  rf_preset     = 0;          // 0=custom, 1=garage, 2=keyfob, 3=weather

    // WiFi
    uint8_t  wifi_channel  = 1;
    bool     wifi_hide_oui = false;      // Hide known-vendor OUI in scan
    uint8_t  wifi_tx_power = 52;         // esp_wifi_set_max_tx_power units (0.25 dBm each)
                                         //   16=4dBm(20%) 34=8.5dBm(40%) 52=13dBm(60%,def)
                                         //   68=17dBm(80%) 82=20.5dBm(100%)

    // BLE
    uint8_t  ble_scan_time = 5;          // seconds

    // Display
    uint8_t  brightness    = 20;         // 0-100
    bool     rotate_180    = false;

    // Bad USB
    uint16_t badusb_delay  = 50;         // ms between keystrokes

    // System
    bool     sound         = false;      // Future: buzzer
};

extern PQConfig g_cfg;

void config_init();
void config_load();
void config_save();
void config_reset();
