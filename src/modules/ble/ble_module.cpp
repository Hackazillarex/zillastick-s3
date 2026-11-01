#include "ble_module.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// ─── Device store ─────────────────────────────────────────────
#define BLE_MAX_DEVS  30
struct BLEDevInfo { String name; String addr; int rssi; };
static BLEDevInfo  s_devs[BLE_MAX_DEVS];
static int         s_dev_count = 0;
static BLEScan*    s_scan = nullptr;

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        // Deduplicate by address; update RSSI on re-sighting
        String addr = dev.getAddress().toString().c_str();
        for (int i = 0; i < s_dev_count; i++) {
            if (s_devs[i].addr == addr) {
                s_devs[i].rssi = dev.getRSSI();
                return;
            }
        }
        if (s_dev_count >= BLE_MAX_DEVS) return;

        s_devs[s_dev_count].addr = addr;
        // Use device name when available; fall back to first 3 MAC bytes
        // (e.g. "aa:bb:cc") so every device has a unique visual identifier.
        // Many devices simply don't broadcast names in ADV or scan response,
        // so "[unknown]" would be uninformative — the OUI prefix is better.
        if (dev.haveName()) {
            s_devs[s_dev_count].name = dev.getName().c_str();
        } else {
            // addr is "aa:bb:cc:dd:ee:ff" — first 8 chars = "aa:bb:cc"
            s_devs[s_dev_count].name = addr.substring(0, 8);
        }
        s_devs[s_dev_count].rssi = dev.getRSSI();
        s_dev_count++;
    }
};
static ScanCallbacks s_scan_cb;

// ─── Fox Hunt: live RSSI tracker for one device ───────────────
// BLEScan::start(duration, false) is the BLOCKING 2-param overload.
// With duration=0 it waits on a semaphore forever, so the event loop
// below would never run and the button could never be checked.
// Fix: use the 3-param overload start(duration, callback, is_continue)
// which returns immediately.  We run 5-second sweeps and restart via a
// flag set by the completion callback.
static volatile bool s_fox_scan_running = false;
static void _fox_scan_done(BLEScanResults) { s_fox_scan_running = false; }

static void _ble_track(int idx) {
    display_clear();
    draw_header("Fox Hunt");
    draw_footer("", "B:stop");

    s_scan->setActiveScan(true);
    s_scan->setInterval(100);
    s_scan->setWindow(99);

    // 3-param non-blocking overload — returns immediately; callback fires
    // from the BLE task when the sweep ends.
    s_fox_scan_running = true;
    s_scan->start(5, _fox_scan_done, false);

    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Restart scan automatically when previous 5-second sweep finishes
        if (!s_fox_scan_running) {
            s_fox_scan_running = true;
            s_scan->start(5, _fox_scan_done, true);  // true = continue, keeps dedup
        }

        int rssi = s_devs[idx].rssi;
        // Map -100 dBm (floor) … -30 dBm (ceiling) to 0–100 %
        int pct = (rssi + 100) * 100 / 70;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;

        uint16_t bar_col = pct > 70 ? CLR_SUCCESS :
                           pct > 35 ? CLR_WARN    : CLR_ERR;

        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);

        // Device identifier (name or MAC prefix)
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 4);
        M5.Lcd.printf("%-36.36s", s_devs[idx].name.c_str());

        // Full MAC address
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 16);
        M5.Lcd.print(s_devs[idx].addr.c_str());

        // RSSI value — large, colour-coded
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(bar_col, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 32);
        M5.Lcd.printf("%d dBm    ", rssi);

        // Signal strength bar
        int bar_max = DISP_W - 12;
        int bar_fill = bar_max * pct / 100;
        M5.Lcd.fillRect(6,           CONTENT_Y + 58, bar_max,  16, CLR_DARK);
        if (bar_fill > 0)
            M5.Lcd.fillRect(6,       CONTENT_Y + 58, bar_fill, 16, bar_col);

        // Strength label
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 78);
        const char* sig = pct > 70 ? "STRONG" :
                          pct > 35 ? "MEDIUM" : "WEAK";
        M5.Lcd.printf("%-6s  %d%%", sig, pct);

        delay(150);  // ~7 fps
    }

    if (s_fox_scan_running) {
        s_scan->stop();
        s_fox_scan_running = false;
    }
}

// ─── BLE Scan ─────────────────────────────────────────────────
void ble_scan_run() {
    s_dev_count = 0;
    display_clear();
    draw_header("BLE Scan");
    draw_status_bar("Initialising BLE...", CLR_ACCENT);

    BLEDevice::init("PINGEQUA");
    s_scan = BLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_scan_cb, true);  // true = want duplicates
    s_scan->setActiveScan(true);   // request scan responses → better chance of names
    s_scan->setInterval(100);
    s_scan->setWindow(99);

    draw_status_bar("Scanning...", CLR_ACCENT);
    s_scan->start(g_cfg.ble_scan_time, false);  // blocking finite scan
    s_scan->clearResults();  // free BLE stack's internal buffer (our s_devs[] is untouched)

    // ── Results menu ──────────────────────────────────────────
    // Format: 14 chars name | 4 chars RSSI  = 18 chars × 12px = 216px
    // Starting at x=8 → ends at x=224, well clear of the 4px scroll bar at x=236.
    // Static per-entry buffers prevent the raw-pointer bug (all items pointing
    // to the same reused stack buffer, which would show only the last item's text).
    static char dev_labels[BLE_MAX_DEVS][20];

    Menu rm("BLE Devices", "A:scroll", "B:info");
    for (int i = 0; i < s_dev_count; i++) {
        snprintf(dev_labels[i], sizeof(dev_labels[i]),
                 "%-14.14s%4d", s_devs[i].name.c_str(), s_devs[i].rssi);
        rm.add(dev_labels[i], " ");
    }
    if (!s_dev_count) rm.add("No devices found", " ", false);
    rm.add("< Back", " ");

    delay(200); M5.update();  // flush stale button events
    rm.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) rm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = rm.selected();
            if (sel == s_dev_count || sel < 0) break;  // < Back
            if (sel < s_dev_count) {
                // ── Device detail + fox hunt ───────────────────
                auto _draw_detail = [&]() {
                    display_clear();
                    draw_header("BLE Device");
                    M5.Lcd.setTextSize(1);

                    M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 4);
                    M5.Lcd.print(s_devs[sel].name.c_str());

                    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 18);
                    M5.Lcd.printf("MAC: %s", s_devs[sel].addr.c_str());

                    M5.Lcd.setCursor(4, CONTENT_Y + 32);
                    M5.Lcd.printf("RSSI: %d dBm", s_devs[sel].rssi);

                    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                    M5.Lcd.setCursor(4, CONTENT_Y + 50);
                    M5.Lcd.print("A:fox hunt");

                    draw_footer("A:track", "B:back");
                };
                _draw_detail();
                delay(250); M5.update();  // flush the B press that entered detail

                bool in_detail = true;
                while (in_detail) {
                    M5.update();
                    if (M5.BtnA.wasPressed()) {
                        // Enter fox hunt — starts its own continuous scan
                        _ble_track(sel);
                        _draw_detail();
                    }
                    if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                        in_detail = false;
                    }
                    delay(10);
                }
                // Flush any lingering hold before returning to list
                delay(350); M5.update();
                rm.draw();
            }
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(10);
    }
    BLEDevice::deinit(true);
}

// ─── BLE Spam ─────────────────────────────────────────────────
static const uint8_t APPLE_NEARBY_TEMPLATE[] = {
    0xFF, 0x4C, 0x00,
    0x10, 0x05,
    0x01, 0x18, 0x00, 0x33, 0xBF
};
static const uint8_t SAMSUNG_BUDS_TEMPLATE[] = {
    0xFF, 0x75, 0x00,
    0x42, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t WIN_SWIFT_TEMPLATE[] = {
    0xFF, 0x06, 0x00,
    0x09, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1D, 0x04, 0x00
};

void ble_spam_run() {
    Menu sm("BLE Spam", "A:scroll", "B:select");
    sm.add("Apple Devices",   "A");
    sm.add("Samsung Devices", "S");
    sm.add("Windows Nearby",  "W");
    sm.add("All Types",       "!");
    sm.add("< Back",          " ");
    sm.draw();

    int spam_type = -1;
    while (spam_type < 0) {
        M5.update();
        if (M5.BtnA.wasPressed()) sm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            spam_type = sm.selected();
            if (spam_type == 4) return;
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }

    BLEDevice::init("PINGEQUA");
    BLEAdvertising* adv = BLEDevice::getAdvertising();

    display_clear();
    draw_header("BLE Spam");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Spamming BLE advertisements...");
    draw_footer("", "B:stop");

    const char* types[] = {"Apple","Samsung","Windows","All"};
    M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 16);
    M5.Lcd.printf("Type: %s", types[spam_type]);

    uint32_t count = 0;
    while (true) {
        M5.update();

        BLEAdvertisementData ad;

        auto _set = [&](const uint8_t* tmpl, size_t len) {
            String s((char*)tmpl, len);
            uint8_t last4[4];
            for (auto& b : last4) b = random(0,256);
            s.replace(s.substring(s.length()-4), String((char*)last4, 4));
            ad.addData(std::string(s.c_str(), s.length()));
        };

        int t = (spam_type == 3) ? (count % 3) : spam_type;
        switch(t) {
            case 0: _set(APPLE_NEARBY_TEMPLATE,  sizeof(APPLE_NEARBY_TEMPLATE));  break;
            case 1: _set(SAMSUNG_BUDS_TEMPLATE,  sizeof(SAMSUNG_BUDS_TEMPLATE));  break;
            case 2: _set(WIN_SWIFT_TEMPLATE,     sizeof(WIN_SWIFT_TEMPLATE));     break;
        }

        adv->setAdvertisementData(ad);
        adv->start();
        delay(20);
        adv->stop();

        count++;
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 28);
        M5.Lcd.printf("Sent: %lu   ", count);

        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
    }

    BLEDevice::deinit(true);
}

// ─── BLE Sniffer (passive continuous scan) ────────────────────
void ble_sniffer_run() {
    s_dev_count = 0;
    BLEDevice::init("PINGEQUA");
    s_scan = BLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_scan_cb, true);
    s_scan->setActiveScan(false);  // passive — just listen
    s_scan->setInterval(200);
    s_scan->setWindow(199);
    s_scan->start(0, false);  // continuous

    display_clear();
    draw_header("BLE Sniffer");
    draw_footer("", "B:stop");

    uint32_t last_draw = 0;
    while (true) {
        M5.update();
        uint32_t now = millis();
        if (now - last_draw > 400) {
            M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
            M5.Lcd.setTextSize(1);
            int show = min(s_dev_count, 5);
            for (int i = max(0, s_dev_count - show); i < s_dev_count; i++) {
                int row = i - max(0, s_dev_count - show);
                M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + row * 14);
                // textSize=1: 6px/char, "%-14.14s%4d" = 18×6=108px from x=4 → fine
                M5.Lcd.printf("%-14.14s%4d", s_devs[i].name.c_str(), s_devs[i].rssi);
            }
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, DISP_H - FOOTER_H - 14);
            M5.Lcd.printf("Total: %d", s_dev_count);
            last_draw = now;
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(20);
    }

    s_scan->stop();
    BLEDevice::deinit(true);
}

// ─── Entry point ──────────────────────────────────────────────
void ble_module_run() {
    Menu m("Bluetooth");
    m.add("BLE Scan",    "S");
    m.add("BLE Spam",    "!");
    m.add("< Back",      " ");
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) m.scroll_down();
        if (M5.BtnB.wasPressed()) {
            switch(m.selected()) {
                case 0: ble_scan_run(); break;
                case 1: ble_spam_run(); break;
                case 2: return;
            }
            // Flush any long-press that fired inside the sub-function so it
            // doesn't cascade and exit the Bluetooth menu too.
            delay(350); M5.update();
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}
