#pragma once
#include <Arduino.h>
#include <SPI.h>

// ─────────────────────────────────────────────────────────────
//  PINGEQUA Firmware Globals
// ─────────────────────────────────────────────────────────────

// ── Firmware Info ─────────────────────────────────────────────
#define FW_NAME      "ZILLASTICK S3"
#define FW_VERSION   "1.0.0"
#define FW_SUBTITLE  "Wireless Auditing"
#define FW_AUTHOR    "By Hackazillarex"

// ── Display (M5StickC S3, landscape 240x135) ──────────────────
#define DISP_W       240
#define DISP_H       135
#define HEADER_H     20
#define FOOTER_H     14
#define CONTENT_Y    (HEADER_H + 2)
#define CONTENT_H    (DISP_H - HEADER_H - FOOTER_H - 4)

// ── Pingequa RF Pack S3 v1 — Pin Mapping ─────────────────────
// Source: Bruce firmware table for M5StickC S3 + RF Pack S3 v1
// Verified against: community.home-assistant.io/t/cc1101-pins-on-m5-sticks3/1015931
#define RF_SPI_SCK   5
#define RF_SPI_MISO  4
#define RF_SPI_MOSI  6

// CC1101 Sub-GHz (433 MHz default)
#define CC1101_CS    2
#define CC1101_GDO0  3
#define CC1101_GDO2  -1   // Not connected on RF Pack S3 v1

// NRF24L01 2.4 GHz
#define NRF24_CE     1
#define NRF24_CS     8

// ── Storage paths ─────────────────────────────────────────────
#define CONFIG_FILE    "/pq_config.json"
#define PAYLOAD_DIR    "/payloads"
#define CAPTURES_DIR   "/captures"
#define WIFI_DIR       "/wifi"

// ── Timing ────────────────────────────────────────────────────
#define SPLASH_MS      2800
#define DEBOUNCE_MS    180
#define LONG_PRESS_MS  700

// ── RGB565 Color Theme (ZILLASTICK Red) ───────────────────────
// Helper: RGB888 -> RGB565
#define RGB565(r,g,b) ((uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)))

#define CLR_BG           RGB565( 12,  0,  0)   // Near-black red tint
#define CLR_PRIMARY      RGB565(220,  0,  0)   // Main red
#define CLR_BRIGHT       RGB565(255, 40, 40)   // Bright red / highlight
#define CLR_DARK         RGB565( 90,  0,  0)   // Dark red
#define CLR_ACCENT       RGB565(255,100,  0)   // Orange-red accent
#define CLR_TEXT         RGB565(255,255,255)   // White text
#define CLR_TEXT_DIM     RGB565(160,160,160)   // Dim text
#define CLR_SEL_BG       RGB565(180,  0,  0)   // Selected item bg
#define CLR_SEL_FG       RGB565(255,255,255)   // Selected item fg
#define CLR_HEADER_BG    RGB565( 70,  0,  0)   // Header bar
#define CLR_BORDER       RGB565( 60,  0,  0)   // Divider lines
#define CLR_SUCCESS      RGB565(  0,200, 80)   // Green — OK
#define CLR_WARN         RGB565(255,200,  0)   // Yellow — warning
#define CLR_ERR          RGB565(255, 40, 40)   // Bright red — error
#define CLR_BLACK        0x0000
#define CLR_WHITE        0xFFFF

// ── App State Machine ─────────────────────────────────────────
enum AppState {
    STATE_SPLASH = 0,
    STATE_MAIN_MENU,
    STATE_WIFI,
    STATE_BLE,
    STATE_RF,
    STATE_NRF24,
    STATE_BADUSB,
    STATE_FILES,
    STATE_SETTINGS,
    STATE_ABOUT,
    STATE_REMOTE_CTRL,
    STATE_POWER_OFF
};

extern AppState   g_state;
extern bool       g_rf_ready;
extern bool       g_nrf_ready;
extern bool       g_nrf_detected;  // latches true on first confirmed NRF24 init — never cleared by deinit
// Note: both CC1101 (ELECHOUSE) and NRF24 (RF24) share the global SPI object.
// ELECHOUSE initialises it via setSpiPin(); RF24 receives &SPI in begin().
// Do NOT create a second SPIClass on SPI2_HOST — ESP-IDF refuses a second
// spi_bus_initialize() call on the same host and the second handle is dead.
