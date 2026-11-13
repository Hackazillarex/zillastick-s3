// ─────────────────────────────────────────────────────────────
//  PINGEQUA Firmware v1.0.0
//  M5Stack Stick S3 + Pingequa RF Pack S3 v1
//  Custom, original firmware — not a fork.
//  (c) hackazillarex / zillastick.com
// ─────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <M5Unified.h>
#include <LittleFS.h>
#include <SPI.h>

#include "globals.h"
#include "core/display.h"
#include "core/menu.h"
#include "core/config.h"
#include "modules/wifi/wifi_module.h"
#include "modules/ble/ble_module.h"
#include "modules/rf/rf_cc1101.h"
#include "modules/nrf24/nrf24_module.h"
#include "modules/badusb/badusb.h"
#include "modules/files/file_manager.h"
#include "modules/remote/remote_ctrl.h"

// ─── Global variable definitions ────────────────────────────────
// PQConfig g_cfg is defined in config.cpp
AppState  g_state       = STATE_SPLASH;
bool      g_rf_ready    = false;
bool      g_nrf_ready   = false;
bool      g_nrf_detected = false;  // latches true on first confirmed NRF24 init

// ─── Settings screen ──────────────────────────────────────────
// TX power levels in 20% increments (esp_wifi units = dBm × 4)
static const uint8_t TX_PWR_VALS[5] = { 16, 34, 52, 68, 82 };
static const char*   TX_PWR_LABELS[5] = { "20%  4.0dBm", "40%  8.5dBm",
                                           "60% 13.0dBm", "80% 17.0dBm",
                                          "100% 20.5dBm" };
static uint8_t _tx_pwr_idx() {
    for (uint8_t i = 0; i < 5; i++)
        if (TX_PWR_VALS[i] >= g_cfg.wifi_tx_power) return i;
    return 4;
}

static void settings_run() {
    Menu m("Settings");
    m.add("RF Frequency",  "R");
    m.add("BadUSB Delay",  "B");
    m.add("Display Bright","D");
    m.add("WiFi TX Power", "T");
    m.add("About",         "?");
    m.add("< Back",        " ");
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) m.scroll_down();
        if (M5.BtnB.wasPressed()) {
            switch(m.selected()) {
                case 0: {
                    // RF freq — quick nudge up/down
                    display_clear();
                    draw_header("RF Frequency");
                    float freq = g_cfg.rf_freq;
                    while (true) {
                        M5.update();
                        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
                        M5.Lcd.setTextSize(2);
                        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                        M5.Lcd.setCursor(20, CONTENT_Y + 20);
                        M5.Lcd.printf("%.2f MHz", freq);
                        draw_footer("A:+0.1", "B:save");
                        if (M5.BtnA.wasPressed()) {
                            freq += 0.1f;
                            if (freq > 928.0f) freq = 300.0f;
                        }
                        if (M5.BtnB.wasPressed()) {
                            g_cfg.rf_freq = freq;
                            config_save();
                            show_success("Saved!");
                            delay(800);
                            break;
                        }
                        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
                        delay(10);
                    }
                    break;
                }
                case 1: {
                    // BadUSB delay
                    display_clear();
                    draw_header("BadUSB Delay");
                    uint16_t dly = g_cfg.badusb_delay;
                    while (true) {
                        M5.update();
                        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
                        M5.Lcd.setTextSize(2);
                        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                        M5.Lcd.setCursor(30, CONTENT_Y + 20);
                        M5.Lcd.printf("%d ms", dly);
                        draw_footer("A:+10ms", "B:save");
                        if (M5.BtnA.wasPressed()) {
                            dly += 10;
                            if (dly > 500) dly = 10;
                        }
                        if (M5.BtnB.wasPressed()) {
                            g_cfg.badusb_delay = dly;
                            config_save();
                            show_success("Saved!");
                            delay(800);
                            break;
                        }
                        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
                        delay(10);
                    }
                    break;
                }
                case 2: {
                    // Brightness
                    display_clear();
                    draw_header("Brightness");
                    uint8_t bright = g_cfg.brightness;
                    while (true) {
                        M5.update();
                        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
                        M5.Lcd.setTextSize(2);
                        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                        M5.Lcd.setCursor(50, CONTENT_Y + 20);
                        M5.Lcd.printf("%d%%", (bright * 100) / 255);
                        draw_footer("A:+10", "B:save");
                        M5.Lcd.setBrightness(bright);
                        if (M5.BtnA.wasPressed()) {
                            bright = (bright + 25 > 255) ? 30 : bright + 25;
                            M5.Lcd.setBrightness(bright);
                        }
                        if (M5.BtnB.wasPressed()) {
                            g_cfg.brightness = bright;
                            config_save();
                            show_success("Saved!");
                            delay(800);
                            break;
                        }
                        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
                        delay(10);
                    }
                    break;
                }
                case 3: {
                    // WiFi TX Power
                    display_clear();
                    draw_header("WiFi TX Power");
                    uint8_t pwr_idx = _tx_pwr_idx();
                    while (true) {
                        M5.update();
                        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
                        M5.Lcd.setTextSize(2);
                        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                        M5.Lcd.setCursor(14, CONTENT_Y + 10);
                        M5.Lcd.printf("%s", TX_PWR_LABELS[pwr_idx]);
                        M5.Lcd.setTextSize(1);
                        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                        M5.Lcd.setCursor(4, CONTENT_Y + 40);
                        M5.Lcd.print("Lower = cooler device");
                        M5.Lcd.setCursor(4, CONTENT_Y + 54);
                        M5.Lcd.print("Higher = more range");
                        draw_footer("A:cycle", "B:save");
                        if (M5.BtnA.wasPressed()) {
                            pwr_idx = (pwr_idx + 1) % 5;
                        }
                        if (M5.BtnB.wasPressed()) {
                            g_cfg.wifi_tx_power = TX_PWR_VALS[pwr_idx];
                            config_save();
                            show_success("Saved!");
                            delay(800);
                            break;
                        }
                        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
                        delay(10);
                    }
                    break;
                }
                case 4: {
                    // About
                    display_clear();
                    draw_header("About");
                    M5.Lcd.setTextSize(1);

                    M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 4);
                    M5.Lcd.printf("%s %s", FW_NAME, FW_VERSION);

                    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 18);
                    M5.Lcd.print(FW_SUBTITLE);

                    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 32);
                    M5.Lcd.print(FW_AUTHOR);

                    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 50);
                    M5.Lcd.printf("RF Ready:  %s", g_rf_ready  ? "YES" : "NO");
                    M5.Lcd.setCursor(4, CONTENT_Y + 62);
                    M5.Lcd.printf("NRF Ready: %s", g_nrf_detected ? "YES" : "NO");

                    // Heap info
                    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 78);
                    M5.Lcd.printf("Free heap: %luK", ESP.getFreeHeap()/1024);

                    draw_footer("", "Back");
                    while (true) {
                        M5.update();
                        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
                        delay(10);
                    }
                    break;
                }
                case 5: return;
            }
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}

// ─── Main menu definition ─────────────────────────────────────
struct MainEntry {
    const char* label;
    const char* icon;
    AppState    state;
    bool        req_rf;
    bool        req_nrf;
};

static const MainEntry MAIN_MENU[] = {
    { "WiFi",       "W", STATE_WIFI,     false, false },
    { "BLE",        "B", STATE_BLE,      false, false },
    { "Sub-GHz RF", "R", STATE_RF,       false, false },
    { "2.4GHz NRF", "N", STATE_NRF24,    false, false },
    { "Bad USB",    "U", STATE_BADUSB,   false, false },
    { "Files",      "F", STATE_FILES,       false, false },
    { "Settings",   "S", STATE_SETTINGS,   false, false },
    { "Remote Ctrl","C", STATE_REMOTE_CTRL,false, false },
    { "Power Off",  "P", STATE_POWER_OFF,  false, false },
};
#define MAIN_MENU_COUNT  (sizeof(MAIN_MENU)/sizeof(MAIN_MENU[0]))

// ─── Draw main menu ───────────────────────────────────────────
static Menu* s_main_menu = nullptr;

static void build_main_menu() {
    if (!s_main_menu) {
        s_main_menu = new Menu("ZILLASTICK S3");
    }
    s_main_menu->clear();
    for (size_t i = 0; i < MAIN_MENU_COUNT; i++) {
        const MainEntry& e = MAIN_MENU[i];
        bool ena = true;
        if (e.req_rf  && !g_rf_ready)  ena = false;
        if (e.req_nrf && !g_nrf_ready) ena = false;
        s_main_menu->add(e.label, e.icon, ena);
    }
}

// ─── Dispatch from main menu selection ───────────────────────
static void dispatch(int sel) {
    if (sel < 0 || sel >= (int)MAIN_MENU_COUNT) return;
    const MainEntry& e = MAIN_MENU[sel];

    if (e.req_rf && !g_rf_ready) {
        show_error("CC1101 not ready\nCheck RF Pack wiring");
        delay(2000);
        return;
    }
    if (e.req_nrf && !g_nrf_ready) {
        show_error("NRF24 not ready\nCheck RF Pack wiring");
        delay(2000);
        return;
    }

    g_state = e.state;

    switch(e.state) {
        case STATE_WIFI:     wifi_module_run();     break;
        case STATE_BLE:      ble_module_run();      break;
        case STATE_RF:       rf_module_run();       break;
        case STATE_NRF24:    nrf24_module_run();    break;
        case STATE_BADUSB:   badusb_module_run();   break;
        case STATE_FILES:       file_manager_run();   break;
        case STATE_SETTINGS:    settings_run();       break;
        case STATE_REMOTE_CTRL: remote_ctrl_run();    break;
        case STATE_POWER_OFF: {
            display_clear();
            draw_header("Power Off");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 20);
            M5.Lcd.print("Powering off...");
            M5.Lcd.setCursor(4, CONTENT_Y + 36);
            M5.Lcd.print("Hold button to wake");
            delay(1200);
            M5.Power.powerOff();
            break;  // unreachable, but good practice
        }
        default: break;
    }

    g_state = STATE_MAIN_MENU;
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
    // M5 init
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);

    Serial.println("\n\n[ZS] ZILLASTICK S3 firmware starting...");

    // AXP2101 charge current — set explicitly so the device charges properly
    // while running.  Default AXP2101 boot current is 100 mA (too low to keep
    // up with ESP32-S3 + RF draw).  500 mA is the AXP2101/USB-C limit on this
    // board.  M5Unified exposes the M5Stack-tuned setter directly.
    M5.Power.setChargeCurrent(500);   // mA — max safe for the M5Stick S3 USB-C port

    // Display init
    display_init();
    M5.Lcd.setBrightness(200);

    // Config / SPIFFS — config_init() also calls config_load() internally
    config_init();
    M5.Lcd.setBrightness(g_cfg.brightness);

    // Seed default BadUSB payloads on first boot (no uploadfs needed)
    badusb_ensure_defaults();

    // Splash screen — 2 second hold
    g_state = STATE_SPLASH;
    display_splash();
    delay(2000);

    // RF modules share the global SPI object (ELECHOUSE initialises it via
    // setSpiPin; RF24 receives &SPI).  Do NOT create a second SPIClass on the
    // same SPI2_HOST — ESP-IDF rejects a second spi_bus_initialize() call.

    // Auto-detect RF hardware
    Serial.println("[PQ] Probing CC1101...");
    display_clear();
    draw_header("Initializing");
    draw_status_bar("Probing CC1101...", CLR_PRIMARY);
    g_rf_ready = rf_init();
    Serial.printf("[PQ] CC1101: %s\n", g_rf_ready ? "FOUND" : "not found");

    // Settle between CC1101 and NRF24 probes.  ELECHOUSE leaves the SPI
    // peripheral configured with CC1101_CS as the hardware CS pin; nrf24_init()
    // deasserts it explicitly, but give the bus a moment to idle first.
    delay(250);

    draw_status_bar("Probing NRF24...", CLR_PRIMARY);
    g_nrf_ready = nrf24_init();
    nrf24_deinit();  // Power down until needed
    Serial.printf("[PQ] NRF24: %s\n", g_nrf_ready ? "FOUND" : "not found");

    // Status summary
    display_clear();
    draw_header("Hardware Ready");
    M5.Lcd.setTextSize(1);
    int y = CONTENT_Y + 6;
    auto _status = [&](const char* label, bool ok) {
        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
        M5.Lcd.setCursor(4, y);
        M5.Lcd.print(label);
        M5.Lcd.setTextColor(ok ? CLR_SUCCESS : CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(120, y);
        M5.Lcd.print(ok ? "OK" : "N/A");
        y += 14;
    };
    _status("WiFi",       true);
    _status("BLE",        true);
    _status("Sub-GHz RF", g_rf_ready);
    _status("NRF24",      g_nrf_detected);
    _status("Bad USB",    true);
    delay(1200);

    // ── BtnA held at boot → jump straight into Remote Ctrl ──
    M5.update();
    if (M5.BtnA.isPressed()) {
        display_clear();
        draw_header("Remote Ctrl");
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 20);
        M5.Lcd.print("Boot shortcut active");
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 34);
        M5.Lcd.print("Starting in 1s...");
        delay(1000);
        g_state = STATE_REMOTE_CTRL;
        remote_ctrl_run();
    }

    // Enter main menu
    g_state = STATE_MAIN_MENU;
    build_main_menu();
    s_main_menu->draw();
}

// ─── Loop ────────────────────────────────────────────────────
void loop() {
    M5.update();

    if (g_state != STATE_MAIN_MENU) return;

    if (M5.BtnA.wasPressed()) {
        s_main_menu->scroll_down();
    }

    if (M5.BtnB.wasPressed()) {
        int sel = s_main_menu->selected();
        dispatch(sel);
        // Redraw main menu on return
        build_main_menu();
        s_main_menu->draw();
    }

    // Long-press B on main menu: quick battery check
    if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
        display_clear();
        draw_header("Power");
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(30, CONTENT_Y + 20);
        int batt = battery_pct();
        M5.Lcd.printf("Bat: %d%%", batt);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 56);
        M5.Lcd.printf("Heap free: %luK", ESP.getFreeHeap()/1024);
        draw_footer("", "Back");
        delay(200);  // Debounce
        while (true) {
            M5.update();
            if (M5.BtnB.wasPressed()) break;
            delay(10);
        }
        build_main_menu();
        s_main_menu->draw();
    }

    delay(10);
}
