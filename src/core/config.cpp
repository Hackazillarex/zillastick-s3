#include "config.h"
#include "../globals.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

PQConfig g_cfg;

void config_init() {
    if (!LittleFS.begin(true)) {
        Serial.println("[CFG] SPIFFS mount failed — formatting");
        LittleFS.format();
        LittleFS.begin(true);
    }
    // Ensure required directories exist
    if (!LittleFS.exists(PAYLOAD_DIR))  LittleFS.mkdir(PAYLOAD_DIR);
    if (!LittleFS.exists(CAPTURES_DIR)) LittleFS.mkdir(CAPTURES_DIR);
    config_load();
}

void config_load() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        config_save();  // write defaults
        return;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.println("[CFG] Parse error — using defaults"); return; }

    g_cfg.rf_freq       = doc["rf_freq"]       | 433.92f;
    g_cfg.rf_modulation = doc["rf_mod"]        | 0;
    g_cfg.rf_bw         = doc["rf_bw"]         | 0;
    g_cfg.rf_preset     = doc["rf_preset"]     | 0;
    g_cfg.wifi_channel  = doc["wifi_ch"]       | 1;
    g_cfg.wifi_hide_oui = doc["wifi_hide_oui"] | false;
    g_cfg.ble_scan_time = doc["ble_scan_time"] | 5;
    g_cfg.brightness    = doc["brightness"]    | 80;
    g_cfg.rotate_180    = doc["rotate_180"]    | false;
    g_cfg.badusb_delay  = doc["badusb_delay"]  | 50;
    g_cfg.sound         = doc["sound"]         | false;

    Serial.println("[CFG] Loaded");
}

void config_save() {
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) { Serial.println("[CFG] Save failed"); return; }

    JsonDocument doc;
    doc["rf_freq"]       = g_cfg.rf_freq;
    doc["rf_mod"]        = g_cfg.rf_modulation;
    doc["rf_bw"]         = g_cfg.rf_bw;
    doc["rf_preset"]     = g_cfg.rf_preset;
    doc["wifi_ch"]       = g_cfg.wifi_channel;
    doc["wifi_hide_oui"] = g_cfg.wifi_hide_oui;
    doc["ble_scan_time"] = g_cfg.ble_scan_time;
    doc["brightness"]    = g_cfg.brightness;
    doc["rotate_180"]    = g_cfg.rotate_180;
    doc["badusb_delay"]  = g_cfg.badusb_delay;
    doc["sound"]         = g_cfg.sound;

    serializeJson(doc, f);
    f.close();
    Serial.println("[CFG] Saved");
}

void config_reset() {
    g_cfg = PQConfig();
    if (LittleFS.exists(CONFIG_FILE)) LittleFS.remove(CONFIG_FILE);
    config_save();
}
