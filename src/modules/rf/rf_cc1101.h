#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  Sub-GHz RF Module — CC1101 via Pingequa RF Pack S3 v1
//  Scan · Capture · Replay · Frequency config
// ─────────────────────────────────────────────────────────────

#define RF_CAPTURE_MAX_SAMPLES  2048    // Raw sample buffer
#define RF_CAPTURE_MAX_FILES    10

struct RFCapture {
    float    freq;
    uint8_t  mod;
    uint16_t samples[RF_CAPTURE_MAX_SAMPLES];
    uint32_t count;
    char     filename[32];
};

void rf_module_run();         // Entry point
bool rf_init();               // Returns true if CC1101 found
void rf_deinit();

void rf_scan_run();           // Sweep & detect signals
void rf_capture_run();        // Record raw signal
void rf_replay_run();         // Retransmit captured signal
void rf_replay_file_run();    // Pick capture file to replay
void rf_config_run();         // Set freq/mod/bandwidth
void rf_presets_run();        // Common frequency presets
void rf_jam_detect_run();     // Sub-GHz jamming detection (315/433/868/915 MHz)
int  rf_rssi();               // Current RSSI in dBm (calls rf_init if needed)

// Preset frequencies
struct RFPreset {
    const char* name;
    float       freq;
    uint8_t     mod;      // 0=ASK/OOK
};
extern const RFPreset RF_PRESETS[];
extern const int      RF_PRESET_COUNT;
