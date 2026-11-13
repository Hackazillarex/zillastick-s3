#include "rf_cc1101.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <LittleFS.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ─── Presets ──────────────────────────────────────────────────
const RFPreset RF_PRESETS[] = {
    {"Garage 433",  433.92f, 0},
    {"Car Keyfob",  433.92f, 0},
    {"Weather Sen", 433.92f, 0},
    {"Gate 433",    433.92f, 0},
    {"Custom",      0.0f,    0},
};
const int RF_PRESET_COUNT = 5;

static RFCapture s_cap;

// ─── Init ─────────────────────────────────────────────────────
bool rf_init() {
    ELECHOUSE_cc1101.setSpiPin(RF_SPI_SCK, RF_SPI_MISO, RF_SPI_MOSI, CC1101_CS);
    // GDO2 is not connected on RF Pack S3 v1 — pass GDO0 twice as workaround
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO0);

    // Send SRES (reset) strobe first — this also wakes the chip from SPWD sleep.
    // goSleep() puts CC1101 into power-down; getCC1101() reads the version register
    // and fails silently if the chip is still asleep.  Init() → SRES fixes that.
    ELECHOUSE_cc1101.Init();
    delay(10);  // let chip come out of reset

    if (!ELECHOUSE_cc1101.getCC1101()) {
        Serial.println("[RF] CC1101 NOT FOUND — check wiring/pins");
        g_rf_ready = false;
        return false;
    }

    // Init() already set default registers; now apply our config
    ELECHOUSE_cc1101.setMHZ(g_cfg.rf_freq);
    ELECHOUSE_cc1101.setModulation(g_cfg.rf_modulation);  // 0=2-FSK,1=GFSK,2=ASK/OOK,3=4-FSK,4=MSK
    ELECHOUSE_cc1101.setRxBW(g_cfg.rf_bw == 0 ? 203.0f :
                              g_cfg.rf_bw == 1 ? 101.0f : 58.0f);
    // setPA must be called AFTER setMHZ — the library picks the correct
    // PATABLE byte for the active frequency band.  +10 dBm is the CC1101 max.
    // Init() resets PATABLE to chip default (~0 dBm), so set it every init.
    ELECHOUSE_cc1101.setPA(10);
    ELECHOUSE_cc1101.SetRx();

    g_rf_ready = true;
    Serial.printf("[RF] CC1101 ready @ %.2f MHz\n", g_cfg.rf_freq);
    return true;
}

void rf_deinit() {
    // Strobe SIDLE first — takes CC1101 from any state (RX, TX, FSTXON …) to
    // IDLE.  This is the critical step: without it the chip stays in async TX
    // mode after rf_replay_run(), and even with GDO0 pulled LOW via
    // INPUT_PULLDOWN the weak 45kΩ pulldown can be overcome by RF-induced noise
    // or CC1101 internal bus capacitance, causing a continuous carrier until
    // the next SRES resets the chip.  SIDLE stops TX immediately over SPI with
    // no GDO0 interaction, so the order is safe.
    ELECHOUSE_cc1101.SpiStrobe(0x36);   // SIDLE
    delayMicroseconds(150);             // chip needs ~75 µs to settle to IDLE

    // Now release GDO0.  CC1101 is IDLE — no TX, no RX — so GDO0 function
    // reverts to IOCFG0 output (CLK or carrier-sense); the pulldown is safely
    // overridden by CC1101's strong driver and causes no bus conflict.
    // INPUT_PULLDOWN (not plain INPUT) is still used defensively: if an
    // rf_init() hasn't run yet and some code reads the pin, it reads LOW
    // rather than floating.
    pinMode(CC1101_GDO0, INPUT_PULLDOWN);
    g_rf_ready = false;
}

// ─── Scan (with inline capture-on-detect) ─────────────────────
// When the sweep finds a signal above SCAN_SIG_THRESH the display freezes and
// offers A:capture / B:skip.  Pressing A jumps straight into the async OOK
// tight capture loop — RSSI already confirmed the signal so there is no wait
// phase.  The result is saved to SPIFFS exactly like a normal capture.
void rf_scan_run() {
    if (!rf_init()) { show_error("CC1101 not found!\nCheck RF Pack wiring."); delay(2000); return; }

    display_clear();
    draw_header("RF Scan 433");
    draw_footer("", "B:stop");

    static constexpr float SCAN_FREQ = 433.92f;
    uint8_t spinner  = 0;

    // dBm threshold — signals stronger than this trigger the "capture?" prompt.
    // −80 dBm works well for a Flipper held within a few metres.
    static constexpr int SCAN_SIG_THRESH = -80;

    float best_freq = 0;
    int   best_rssi = -120;

    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // ── Sweep ±1 MHz around 433.92 ────────────────────────────────────────
        float center = SCAN_FREQ;
        for (float f = center - 1.0f; f <= center + 1.0f; f += 0.1f) {
            ELECHOUSE_cc1101.setMHZ(f);
            ELECHOUSE_cc1101.SetRx();
            delayMicroseconds(2000);
            int rssi = ELECHOUSE_cc1101.getRssi();
            if (rssi > best_rssi) { best_rssi = rssi; best_freq = f; }
        }

        // ── Signal detected? — auto-capture immediately, ask save/discard after ──
        // Do NOT ask the user to press a button before capturing: the Flipper's burst
        // is only ~100–200 ms.  By the time they read "Signal found!" and press A the
        // signal is gone.  Instead, capture first, then show the result and ask.
        if (best_rssi > SCAN_SIG_THRESH) {

            // Show "capturing" instantly — no button wait
            M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.printf(">> %.2f MHz  %d dBm", best_freq, best_rssi);
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 18);
            M5.Lcd.print("Capturing...");
            draw_footer("", "");

            // Switch to async OOK immediately — RSSI gate already passed
            ELECHOUSE_cc1101.setMHZ(best_freq);
            ELECHOUSE_cc1101.SpiStrobe(0x36);          // SIDLE
            delayMicroseconds(200);
            ELECHOUSE_cc1101.SpiWriteReg(0x12, 0x30); // MDMCFG2: OOK, no sync
            ELECHOUSE_cc1101.SpiWriteReg(0x08, 0x32); // PKTCTRL0: async serial (PKT_FORMAT=11) → GDO0 = raw OOK
            pinMode(CC1101_GDO0, INPUT);
            ELECHOUSE_cc1101.SetRx();
            // Brief settle: the CC1101's OOK amplitude-decision threshold is
            // adapted over a moving average (~128 bytes at the configured rate).
            // Without this delay the threshold hasn't settled and GDO0 outputs
            // garbage transitions → count < 8 → "too short".  15 ms is enough
            // for the threshold to latch on to a moderate-strength signal while
            // still leaving plenty of a 100–200 ms Flipper burst to capture.
            delay(15);

            s_cap.freq  = best_freq;
            s_cap.mod   = 2;
            s_cap.count = 0;

            // Wait for first rising edge — up to 2 s (catches the next Flipper repeat)
            uint32_t edge_dl    = millis() + 2000;
            uint32_t last_us    = 0;
            bool     last_state = false;
            while (millis() < edge_dl) {
                if (digitalRead(CC1101_GDO0) == HIGH) {
                    last_state = true;
                    last_us    = micros();
                    break;
                }
            }

            // Tight capture loop
            uint32_t cap_dl = micros() + 10000000UL;
            while (s_cap.count < RF_CAPTURE_MAX_SAMPLES) {
                bool cur = (digitalRead(CC1101_GDO0) == HIGH);
                if (cur != last_state) {
                    uint32_t now = micros();
                    s_cap.samples[s_cap.count++] = (uint16_t)min(now - last_us, (uint32_t)65535);
                    last_us    = now;
                    last_state = cur;
                }
                if (!last_state && (micros() - last_us) > 20000) break;
                if (micros() > cap_dl) break;
            }

            // ── Result screen — ask save or discard ───────────────────────────
            display_clear();
            if (s_cap.count < 8) {
                // Too short — let user decide whether to keep trying
                draw_header("RF Scan");
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 2);
                M5.Lcd.printf("%.2f MHz  %d dBm", best_freq, best_rssi);
                M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 18);
                M5.Lcd.print("Capture too short.");
                M5.Lcd.setCursor(4, CONTENT_Y + 32);
                M5.Lcd.print("Hold Flipper btn &");
                M5.Lcd.setCursor(4, CONTENT_Y + 46);
                M5.Lcd.print("press A to retry.");
                draw_footer("A:retry", "B:skip");

                bool retry_wait = true;
                while (retry_wait) {
                    M5.update();
                    // Reset best_rssi/best_freq on BOTH paths.  Without the
                    // reset on A:retry the stale RSSI value (e.g. -70 dBm from
                    // the failed attempt) keeps the scan condition
                    // (best_rssi > SCAN_SIG_THRESH) true on the very next
                    // sweep iteration → immediate re-capture → "too short" again
                    // → infinite loop with no way out.
                    if (M5.BtnA.wasPressed()) { retry_wait = false; best_rssi = -120; best_freq = 0; }
                    if (M5.BtnB.wasPressed()) { retry_wait = false; best_rssi = -120; best_freq = 0; }
                    delay(10);
                }
                // Re-init for next sweep iteration
                if (!rf_init()) { rf_deinit(); return; }
                ELECHOUSE_cc1101.setMHZ(SCAN_FREQ);
                ELECHOUSE_cc1101.SetRx();
                display_clear();
                draw_header("RF Scan 433");
                draw_footer("", "B:stop");
                continue;
            }

            // Good capture — offer save or discard.
            // Compute total signal duration (sum of all pulse/space widths).
            uint32_t cap_dur_us = 0;
            for (uint32_t ci = 0; ci < s_cap.count; ci++) cap_dur_us += s_cap.samples[ci];

            draw_header("Captured!");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.printf("%.2f MHz  %d dBm", best_freq, best_rssi);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 18);
            // Show transitions and burst length so user can verify capture quality.
            // A typical OOK remote burst: 24-200 transitions, 10-400 ms.
            M5.Lcd.printf("%lu edges  ~%lu ms", s_cap.count, cap_dur_us / 1000UL);
            draw_footer("A:save", "B:discard");

            bool save_wait = true;
            bool do_save   = false;
            while (save_wait) {
                M5.update();
                if (M5.BtnA.wasPressed()) { do_save = true;  save_wait = false; }
                if (M5.BtnB.wasPressed()) { do_save = false; save_wait = false; }
                delay(10);
            }

            if (do_save) {
                char fname[40];
                snprintf(fname, sizeof(fname), "%s/cap_%lu.raw", CAPTURES_DIR, millis());
                File fh = LittleFS.open(fname, "w");
                if (fh) {
                    fh.write((uint8_t*)&s_cap.freq,  sizeof(float));
                    fh.write(&s_cap.mod, 1);
                    fh.write((uint8_t*)&s_cap.count, sizeof(uint32_t));
                    fh.write((uint8_t*)s_cap.samples, s_cap.count * sizeof(uint16_t));
                    fh.close();
                    strncpy(s_cap.filename, fname, 31);
                }
                M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 34);
                M5.Lcd.print("Saved!");
                draw_footer("A:scan more", "B:done");

                bool more_wait = true;
                bool scan_more = false;
                while (more_wait) {
                    M5.update();
                    if (M5.BtnA.wasPressed()) { scan_more = true;  more_wait = false; }
                    if (M5.BtnB.wasPressed()) { scan_more = false; more_wait = false; }
                    delay(10);
                }
                if (!scan_more) { rf_deinit(); return; }
            }
            // else: discarded — just keep scanning

            best_rssi = -120; best_freq = 0;
            if (!rf_init()) { rf_deinit(); return; }
            ELECHOUSE_cc1101.setMHZ(SCAN_FREQ);
            ELECHOUSE_cc1101.SetRx();
            display_clear();
            draw_header("RF Scan 433");
            draw_footer("", "B:stop");
            continue;
        }

        // ── Normal sweep redraw ────────────────────────────────────────────────
        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 2);
        M5.Lcd.print("Band:  433 MHz");
        M5.Lcd.setCursor(4, CONTENT_Y + 14);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.printf("Best:  %.2f MHz  %d dBm", best_freq, best_rssi);
        M5.Lcd.setCursor(4, CONTENT_Y + 26);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.printf("Current RSSI: %d dBm", ELECHOUSE_cc1101.getRssi());

        int bar_val = map(constrain(best_rssi, -120, -20), -120, -20, 0, DISP_W - 8);
        draw_progress(4, CONTENT_Y + 42, DISP_W - 8, 10, bar_val, CLR_PRIMARY, CLR_DARK);

        draw_spinner(DISP_W - 20, CONTENT_Y + 2, spinner++);
        delay(10);
    }
    rf_deinit();
}

// ─── Capture ──────────────────────────────────────────────────
void rf_capture_run() {
    if (!rf_init()) { show_error("CC1101 not found!"); delay(2000); return; }

    ELECHOUSE_cc1101.setMHZ(g_cfg.rf_freq);

    // Async OOK mode: PKT_FORMAT=10 forces GDO0 to the raw demodulated OOK signal —
    // HIGH = carrier ON (mark), LOW = carrier OFF (space).  This is the only way to
    // time individual OOK pulses on this board because GDO2 is not connected.
    //
    // Why NOT carrier sense (IOCFG0=0x0E)?
    //   Carrier sense is RSSI-based.  It goes HIGH when the overall signal level
    //   exceeds the threshold and STAYS HIGH for the entire OOK burst — it does NOT
    //   track individual marks/spaces.  The tight capture loop would see 0 transitions
    //   → count stays 0 → "Capture too short".
    //
    // Noise immunity in the wait phase:
    //   At idle the AGC runs at max gain so async GDO0 can randomly toggle.  The wait
    //   loop below reads RSSI via SPI first; only when RSSI confirms a genuine carrier
    //   (> CAPTURE_RSSI_THRESH) does it fast-poll GDO0 for a rising edge.  This
    //   two-gate approach prevents noise spikes from starting a false capture.
    //
    // MDMCFG2=0x30 : OOK, no sync word
    // PKTCTRL0=0x32 : async serial (PKT_FORMAT=11), infinite packet → GDO0 = raw OOK data
    // IOCFG0 not written — async mode overrides it

    ELECHOUSE_cc1101.SpiStrobe(0x36);          // SIDLE — idle before register writes
    delayMicroseconds(200);
    ELECHOUSE_cc1101.SpiWriteReg(0x12, 0x30); // MDMCFG2: OOK, no sync word
    ELECHOUSE_cc1101.SpiWriteReg(0x08, 0x32); // PKTCTRL0: async serial (PKT_FORMAT=11) → GDO0 = raw OOK
    pinMode(CC1101_GDO0, INPUT);
    ELECHOUSE_cc1101.SetRx();
    delay(15);  // let AGC settle

    display_clear();
    draw_header("RF Capture");
    draw_footer("", "B:cancel");

    s_cap.freq  = g_cfg.rf_freq;
    s_cap.mod   = 2;   // OOK — pulse-timing capture only makes sense for OOK/ASK
    s_cap.count = 0;

    // ── Waiting phase: RSSI gate + GDO0 edge detection ──────────────────────────
    // Two-gate trigger for noise immunity:
    //   Gate 1 — RSSI: read via SPI.  If below CAPTURE_RSSI_THRESH we know there is
    //             no real carrier; skip GDO0 entirely.  Noise spikes can't clear this.
    //   Gate 2 — GDO0: once RSSI confirms a carrier, fast-poll GDO0 for up to 5 ms
    //             for a rising edge.  OOK marks at 1–10 kbps are 100–1000 µs wide,
    //             so within 5 ms we catch at least one HIGH period.
    //
    // After trigger: last_us is set to micros() at the exact rising edge; last_state
    // is true (HIGH).  The tight loop below records all subsequent transitions.

    // Raise this toward −70 for a very strong Flipper; lower toward −95 if far away.
    static constexpr int CAPTURE_RSSI_THRESH = -85; // dBm

    uint32_t wait_timeout = millis() + 30000;
    uint32_t last_draw    = 0;
    uint32_t last_us      = 0;
    bool     last_state   = false;
    bool     got_signal   = false;
    int      rssi_now     = -120;

    while (millis() < wait_timeout) {
        M5.update();
        if (M5.BtnB.wasPressed()) { rf_deinit(); return; }

        // Gate 1: is there a real carrier above the noise floor?
        rssi_now = ELECHOUSE_cc1101.getRssi(); // ~50 µs SPI read
        if (rssi_now > CAPTURE_RSSI_THRESH) {
            // Gate 2: fast-poll GDO0 for a rising edge (up to 5 ms)
            uint32_t scan_end = micros() + 5000;
            while (micros() < scan_end) {
                if (digitalRead(CC1101_GDO0) == HIGH) {
                    last_state = true;
                    last_us    = micros();
                    got_signal = true;
                    break;
                }
            }
            if (got_signal) break;
            // RSSI was high but no rising edge yet — update display and retry
        }

        if (millis() - last_draw > 120) {
            last_draw = millis();
            M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.printf("%.2f MHz  OOK", g_cfg.rf_freq);

            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 16);
            M5.Lcd.printf("RSSI: %d dBm", rssi_now);

            int bar = map(constrain(rssi_now, -120, -20), -120, -20, 0, DISP_W - 8);
            draw_progress(4, CONTENT_Y + 30, DISP_W - 8, 10, bar, CLR_PRIMARY, CLR_DARK);

            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 46);
            M5.Lcd.print("Waiting for signal...");
            uint32_t rem = (wait_timeout - millis()) / 1000;
            M5.Lcd.setCursor(DISP_W - 26, CONTENT_Y + 46);
            M5.Lcd.printf("%lus", rem);
        }
        delay(5);
    }

    if (!got_signal) {
        show_error("No signal detected\n(30s timeout)");
        delay(2000); rf_deinit(); return;
    }

    // ── Tight capture loop — no delays, no display calls, no M5.update ──
    // Runs from the INSTANT the first carrier edge was detected above.
    //
    // *** DO NOT insert any function call here ***
    // Even a single SPI write to the LCD (e.g. draw_status_bar) adds ~3–5 ms,
    // during which OOK pulses keep arriving and are silently lost.  The display
    // update must happen BEFORE the trigger fires, not after.
    //
    // Absolute hang-guard: if GDO0 is stuck HIGH (constant ambient carrier)
    // the state never changes, so the 20 ms end-of-burst check never fires.
    // The deadline below guarantees we always escape within 10 seconds.
    uint32_t cap_deadline = micros() + 10000000UL;  // 10 s max

    while (s_cap.count < RF_CAPTURE_MAX_SAMPLES) {
        bool cur = (digitalRead(CC1101_GDO0) == HIGH);
        if (cur != last_state) {
            uint32_t now = micros();
            s_cap.samples[s_cap.count++] = (uint16_t)min(now - last_us, (uint32_t)65535);
            last_us    = now;
            last_state = cur;
        }
        // End-of-burst: 20 ms of no carrier = transmission is done
        if (!last_state && (micros() - last_us) > 20000) break;
        // Absolute timeout — escape if GDO0 is stuck HIGH (strong constant carrier)
        if (micros() > cap_deadline) break;
    }

    if (s_cap.count < 8) {
        show_error("Capture too short"); delay(2000); rf_deinit(); return;
    }

    // Save to SPIFFS
    char fname[40];
    snprintf(fname, sizeof(fname), "%s/cap_%lu.raw", CAPTURES_DIR, millis());
    File f = LittleFS.open(fname, "w");
    if (f) {
        f.write((uint8_t*)&s_cap.freq,  sizeof(float));
        f.write(&s_cap.mod, 1);
        f.write((uint8_t*)&s_cap.count, sizeof(uint32_t));
        f.write((uint8_t*)s_cap.samples, s_cap.count * sizeof(uint16_t));
        f.close();
        strncpy(s_cap.filename, fname, 31);
    }

    display_clear();
    draw_header("Captured!");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("Samples: %lu\nFreq: %.2f MHz\nSaved: %s",
        s_cap.count, s_cap.freq, fname);
    draw_footer("", "B:continue");
    while (!M5.BtnB.wasPressed()) { M5.update(); delay(20); }
    rf_deinit();
}

// ─── Replay current capture ───────────────────────────────────
void rf_replay_run() {
    if (s_cap.count == 0) {
        show_error("No capture in memory.\nCapture a signal first.");
        delay(2000); return;
    }

    if (!rf_init()) { show_error("CC1101 not found!"); delay(2000); return; }

    ELECHOUSE_cc1101.setMHZ(s_cap.freq);

    // Async OOK TX: in async serial mode (PKTCTRL0 PKT_FORMAT=11) GDO0 becomes
    // a DATA INPUT — driving it HIGH transmits the carrier, LOW kills it.
    // Without this, SetTx() goes into FIFO mode where GDO0 is a chip output;
    // bit-banging it from the ESP32 does nothing to the RF signal.
    // 0x32 = 0b00110010: no whitening | PKT_FORMAT=11 (async serial) | no CRC | infinite length
    ELECHOUSE_cc1101.SpiWriteReg(0x12, 0x30); // MDMCFG2: OOK, no sync word
    ELECHOUSE_cc1101.SpiWriteReg(0x08, 0x32); // PKTCTRL0: async serial TX (PKT_FORMAT=11), infinite pkt
    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);           // idle — no carrier before TX starts
    ELECHOUSE_cc1101.SetTx();

    display_clear();
    draw_header("RF Replay");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("%.2f MHz  OOK\nSamples: %lu\nA:replay  B:stop",
        s_cap.freq, s_cap.count);
    draw_footer("A:replay", "B:stop");

    // How many times to repeat the pulse train per button press.
    // Real OOK remotes transmit the same code 3-8× in rapid succession;
    // the receiver confirms the command only after seeing multiple identical
    // packets.  A single transmission is almost always ignored.
#define RF_REPLAY_REPS    5    // repetitions per press
#define RF_REPLAY_GAP_MS  50  // gap between reps (ms) — matches typical remote cadence

    uint32_t tx_count  = 0;
    bool     user_stop = false;

    // Flush any button state carried from menu navigation (e.g. an A-press
    // used to scroll the menu to "Replay last" still pending in wasPressed()).
    // Also waits for A to be physically released if it is still held down
    // from the selection gesture.  500 ms is long enough for any residual
    // M5Unified debounce window; the user will barely notice the pause.
    delay(500);
    M5.update();

    while (!user_stop) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            for (int rep = 0; rep < RF_REPLAY_REPS && !user_stop; rep++) {
                // Show which repetition we're on (between bursts, not inside them)
                M5.Lcd.setTextColor(CLR_ACCENT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 54);
                M5.Lcd.printf("Burst %d/%d...       ", rep + 1, RF_REPLAY_REPS);

                // ── Tight bit-bang loop — no SPI/display inside ──────────
                // samples[0] = first carrier-ON duration (even indices = ON),
                // samples[1] = first carrier-OFF duration, etc.
                for (uint32_t i = 0; i < s_cap.count; i++) {
                    digitalWrite(CC1101_GDO0, (i % 2 == 0) ? HIGH : LOW);
                    delayMicroseconds(s_cap.samples[i]);
                }
                digitalWrite(CC1101_GDO0, LOW);  // ensure carrier off after burst

                // ── Inter-repetition gap — poll BtnB here so the user can stop ──
                // Plain delay() would block M5.update(), swallowing any button press
                // during the gap.  Instead we loop with short sleeps + M5.update().
                if (rep < RF_REPLAY_REPS - 1) {
                    uint32_t gap_start = millis();
                    while (millis() - gap_start < (uint32_t)RF_REPLAY_GAP_MS) {
                        M5.update();
                        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                            user_stop = true;
                            break;
                        }
                        delay(5);
                    }
                }
            }
            if (!user_stop) {
                tx_count++;
                M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 54);
                M5.Lcd.printf("Sent: %lu x%d      ", tx_count, RF_REPLAY_REPS);
                // Wait until BtnA is physically released (up to 600 ms) before
                // re-enabling replay.  A single M5.update() drain is not enough
                // because some M5Unified builds keep wasPressed() true while the
                // button is held, causing the outer loop to fire another burst
                // the instant it re-checks — making replay appear to loop forever.
                {
                    uint32_t rel_deadline = millis() + 600;
                    while (millis() < rel_deadline) {
                        M5.update();
                        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                            user_stop = true;
                            break;
                        }
                        if (!M5.BtnA.isPressed()) break;  // A released — safe to continue
                        delay(5);
                    }
                }
                if (!user_stop) M5.update(); // one final clean drain
            }
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) user_stop = true;
        if (!user_stop) delay(20);
    }
    rf_deinit();
}

// ─── Replay from file ─────────────────────────────────────────
void rf_replay_file_run() {
    // List capture files
    File dir = LittleFS.open(CAPTURES_DIR);
    if (!dir) { show_error("No captures dir"); delay(2000); return; }

    Menu fm("Captures", "A:scroll", "B:select");
    String files[RF_CAPTURE_MAX_FILES];
    int fcount = 0;

    File entry = dir.openNextFile();
    while (entry && fcount < RF_CAPTURE_MAX_FILES) {
        if (!entry.isDirectory()) {
            // entry.name() on ESP32 SPIFFS returns the bare filename only
            // ("cap_12345.raw"), not the full path.  LittleFS.open() needs the
            // full path or it silently fails.  Build it from CAPTURES_DIR.
            String full = String(CAPTURES_DIR) + "/" + String(entry.name());
            files[fcount] = full;
            fm.add(entry.name(), ">");   // show short name in the menu
            fcount++;
        }
        entry = dir.openNextFile();
    }

    if (!fcount) { show_error("No capture files found"); delay(2000); return; }
    fm.add("< Back", " ");
    fm.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) fm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = fm.selected();
            if (sel == fcount) return;
            // Load file into s_cap
            File f = LittleFS.open(files[sel]);
            if (f) {
                f.read((uint8_t*)&s_cap.freq, sizeof(float));
                f.read(&s_cap.mod, 1);
                f.read((uint8_t*)&s_cap.count, sizeof(uint32_t));
                s_cap.count = min(s_cap.count, (uint32_t)RF_CAPTURE_MAX_SAMPLES);
                f.read((uint8_t*)s_cap.samples, s_cap.count * sizeof(uint16_t));
                f.close();
                rf_replay_run();
                fm.draw();
            }
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}

// ─── Config ───────────────────────────────────────────────────
void rf_config_run() {
    // Frequency selection (step by 0.01 MHz)
    display_clear();
    draw_header("RF Config");

    float freq = g_cfg.rf_freq;
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("BtnA: +0.1 MHz    B: save");

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            freq += 0.1f;
            if (freq > 434.0f) freq = 433.0f;
        }
        if (M5.BtnA.pressedFor(LONG_PRESS_MS)) {
            freq -= 0.1f;
            if (freq < 433.0f) freq = 434.0f;
        }
        M5.Lcd.fillRect(0, CONTENT_Y + 16, DISP_W, 30, CLR_BG);
        M5.Lcd.setTextSize(3);
        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 18);
        M5.Lcd.printf("%.2f", freq);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.print(" MHz");

        if (M5.BtnB.wasPressed()) {
            g_cfg.rf_freq = freq;
            config_save();
            show_success("Frequency saved!");
            delay(1200);
            return;
        }
        delay(50);
    }
}

// ─── Presets ──────────────────────────────────────────────────
void rf_presets_run() {
    Menu pm("RF Presets", "A:scroll", "B:select");
    for (int i = 0; i < RF_PRESET_COUNT; i++) pm.add(RF_PRESETS[i].name, ">");
    pm.add("< Back", " ");
    pm.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) pm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = pm.selected();
            if (sel == RF_PRESET_COUNT) return;
            if (RF_PRESETS[sel].freq > 0) {
                g_cfg.rf_freq      = RF_PRESETS[sel].freq;
                g_cfg.rf_modulation = RF_PRESETS[sel].mod;
                config_save();
                char msg[48];
                snprintf(msg, sizeof(msg), "Set to %.2f MHz", g_cfg.rf_freq);
                show_success(msg);
                delay(1200);
            }
            pm.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}

// ─── Sub-GHz Jam Detect ───────────────────────────────────────
// Monitors 433.92 MHz (the only band supported by Pingequa RF Pack S3 v1).
// If sustained high signal (> JAM_RSSI_THRESH for JAM_CONSEC_HITS
// consecutive sweeps) is detected it is flagged as a potential jammer.
#define JAM_RSSI_THRESH  -70   // dBm — above this = strong/suspicious
#define JAM_CONSEC_HITS   3    // sweeps above threshold before alarm

void rf_jam_detect_run() {
    if (!rf_init()) { show_error("CC1101 not found!\nCheck RF Pack wiring."); delay(2000); return; }

    display_clear();
    draw_header("Jam Detect 433");
    draw_footer("", "B:stop");

    int     rssi_avg   = -120;
    int     hot_streak = 0;
    bool    jam_flag   = false;
    uint8_t spinner    = 0;

    ELECHOUSE_cc1101.setMHZ(433.92f);
    ELECHOUSE_cc1101.SetRx();

    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Sample 433.92 MHz (4 readings averaged)
        delayMicroseconds(2500);
        int sum = 0;
        for (int k = 0; k < 4; k++) {
            sum += ELECHOUSE_cc1101.getRssi();
            delayMicroseconds(400);
        }
        rssi_avg = sum / 4;
        if (rssi_avg > JAM_RSSI_THRESH) {
            if (++hot_streak >= JAM_CONSEC_HITS) jam_flag = true;
        } else {
            hot_streak = 0;
            jam_flag   = false;
        }

        uint16_t col = jam_flag    ? CLR_ERR  :
                       rssi_avg > -85 ? CLR_WARN : CLR_PRIMARY;
        int      bar = map(constrain(rssi_avg, -120, -20), -120, -20, 0, DISP_W - 8);

        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(col, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 4);
        M5.Lcd.print("433.92 MHz");

        M5.Lcd.setTextSize(3);
        M5.Lcd.setCursor(4, CONTENT_Y + 28);
        M5.Lcd.printf("%d", rssi_avg);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.print(" dBm");

        M5.Lcd.fillRect(4, CONTENT_Y + 60, DISP_W - 8, 10, CLR_DARK);
        if (bar > 0) M5.Lcd.fillRect(4, CONTENT_Y + 60, bar, 10, col);

        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(4, CONTENT_Y + 76);
        if (jam_flag) {
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.print("!! JAM DETECTED !!");
        } else {
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.print("Clear");
        }

        draw_spinner(DISP_W - 20, CONTENT_Y + 2, spinner++);
        delay(20);
    }
    rf_deinit();
}

// ─── Entry point ──────────────────────────────────────────────
void rf_module_run() {
    Menu m("Sub-GHz RF");
    m.add("Scan",         "S");
    m.add("Replay Last",  "R");
    m.add("Replay File",  "F");
    m.add("Presets",      "P");
    m.add("Config",       "*");
    m.add("Jam Detect",   "J");
    m.add("< Back",       " ");
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) m.scroll_down();
        if (M5.BtnB.wasPressed()) {
            switch(m.selected()) {
                case 0: rf_scan_run();         break;
                case 1: rf_replay_run();       break;
                case 2: rf_replay_file_run();  break;
                case 3: rf_presets_run();      break;
                case 4: rf_config_run();       break;
                case 5: rf_jam_detect_run();   break;
                case 6: return;
            }
            // Flush any long-press / residual button event that may have fired
            // inside the sub-function.  Without this, the outer loop can see a
            // stale wasPressed() or pressedFor() and immediately fire another
            // selection or exit the RF module entirely (phantom press).
            delay(350);
            M5.update();
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}

// ─── Headless RSSI (used by remote_ctrl web panel) ────────────
int rf_rssi() {
    if (!g_rf_ready) return -120;
    return ELECHOUSE_cc1101.getRssi();
}
