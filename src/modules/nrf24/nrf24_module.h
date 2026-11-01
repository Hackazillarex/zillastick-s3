#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  NRF24L01 2.4 GHz Module
//  Channel Scan · MouseJack · Packet Sniffer · Jam Detect
// ─────────────────────────────────────────────────────────────

#define NRF_CH_COUNT  126   // usable channels 0-125

void nrf24_module_run();
bool nrf24_init();
void nrf24_deinit();

void nrf24_scan_run();        // 2.4 GHz channel spectrum (on-device display)
void nrf24_mousejack_run();   // HID injection over NRF24
void nrf24_sniff_run();       // Passive packet capture
void nrf24_jam_detect_run();  // 2.4 GHz jamming detection (RPD sweep)

// ── Headless API (used by remote_ctrl FreeRTOS tasks) ─────────
// These share the same s_radio instance as the display-based
// functions. Call nrf24_headless_init() at task start and
// nrf24_headless_deinit() at task end — they manage SPI bus
// ownership (deassert CC1101_CS, end/begin SPI) exactly as
// nrf24_init()/nrf24_deinit() do.
//
// Do NOT call any CC1101 / rf_* function while an NRF headless
// task is running: both peripherals share the SPI bus.

// Init — returns true if NRF24 detected. Wraps nrf24_init().
bool nrf24_headless_init();

// Deinit — powers down radio, releases bus. Wraps nrf24_deinit().
void nrf24_headless_deinit();

// Channel scan: one full NRF_CH_COUNT sweep.
//   ch_power[i] is updated with EMA: +12 on hit, -1 on miss.
//   Call in a loop to accumulate a spectrum.
void nrf24_headless_scan_sweep(uint8_t* ch_power, int count);

// Jam detect: one full sweep; returns number of hot channels.
//   dwell_us  — µs to listen per poll (150 recommended)
//   polls     — polls per channel (1 or 2; 2 = double-poll)
int  nrf24_headless_jamdet_sweep(int dwell_us, int polls);

// Sniffer: configure promiscuous mode for headless use.
//   rate_idx: 0=1Mbps (default)  1=250Kbps  2=2Mbps
void nrf24_headless_sniff_init(uint8_t rate_idx);
void nrf24_headless_sniff_set_ch(uint8_t ch);
bool nrf24_headless_sniff_poll(uint8_t* buf32, uint8_t* ch_out);
