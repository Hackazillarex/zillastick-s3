#include "display.h"

// ─────────────────────────────────────────────────────────────
//  Display implementation
// ─────────────────────────────────────────────────────────────

void display_init() {
    M5.Lcd.setRotation(1);          // Landscape
    M5.Lcd.fillScreen(CLR_BG);
    M5.Lcd.setTextWrap(false);
}

// ── Splash screen ─────────────────────────────────────────────
void display_splash() {
    M5.Lcd.fillScreen(CLR_BG);

    // Top decorative bar
    M5.Lcd.fillRect(0, 0, DISP_W, 4, CLR_PRIMARY);

    // Brand name — sized to fit (textSize 2 = 12px/char keeps 13-char name inside 240px)
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setTextSize(2);
    int tw = strlen(FW_NAME) * 12;
    M5.Lcd.setCursor((DISP_W - tw) / 2, 20);
    M5.Lcd.print(FW_NAME);

    // Underline accent
    M5.Lcd.fillRect(40, 42, DISP_W - 80, 2, CLR_PRIMARY);

    // Subtitle
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CLR_ACCENT, CLR_BG);
    tw = strlen(FW_SUBTITLE) * 12;
    M5.Lcd.setCursor((DISP_W - tw) / 2, 52);
    M5.Lcd.print(FW_SUBTITLE);

    // M5StickC S3 hardware label
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    const char* hw = "M5StickC S3 + RF Pack S3 v1";
    tw = strlen(hw) * 6;
    M5.Lcd.setCursor((DISP_W - tw) / 2, 88);
    M5.Lcd.print(hw);

    // Version + website
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s  |  %s", FW_VERSION, FW_AUTHOR);
    tw = strlen(ver) * 6;
    M5.Lcd.setCursor((DISP_W - tw) / 2, 100);
    M5.Lcd.print(ver);

    // Bottom bar
    M5.Lcd.fillRect(0, DISP_H - 4, DISP_W, 4, CLR_PRIMARY);

    // Corner accents
    M5.Lcd.fillRect(0, 0, 8, 8, CLR_DARK);
    M5.Lcd.fillRect(DISP_W - 8, 0, 8, 8, CLR_DARK);
    M5.Lcd.fillRect(0, DISP_H - 8, 8, 8, CLR_DARK);
    M5.Lcd.fillRect(DISP_W - 8, DISP_H - 8, 8, 8, CLR_DARK);
}

// ── Basic layout ──────────────────────────────────────────────
void display_clear() {
    M5.Lcd.fillScreen(CLR_BG);
}

void draw_header(const char* title, const char* right_label) {
    M5.Lcd.fillRect(0, 0, DISP_W, HEADER_H, CLR_HEADER_BG);
    M5.Lcd.fillRect(0, HEADER_H, DISP_W, 1, CLR_PRIMARY);

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_HEADER_BG);
    M5.Lcd.setCursor(4, 3);
    M5.Lcd.print(title);

    if (right_label) {
        int rw = strlen(right_label) * 12;
        M5.Lcd.setCursor(DISP_W - rw - 4, 3);
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_HEADER_BG);
        M5.Lcd.print(right_label);
    }
}

void draw_footer(const char* left_hint, const char* right_hint) {
    int fy = DISP_H - FOOTER_H;
    M5.Lcd.fillRect(0, fy, DISP_W, FOOTER_H, CLR_DARK);
    M5.Lcd.fillRect(0, fy, DISP_W, 1, CLR_BORDER);

    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_DARK);

    if (left_hint) {
        M5.Lcd.setCursor(4, fy + 3);
        M5.Lcd.print(left_hint);
    }
    if (right_hint) {
        int rw = strlen(right_hint) * 6;
        M5.Lcd.setCursor(DISP_W - rw - 4, fy + 3);
        M5.Lcd.print(right_hint);
    }
}

void draw_divider(int y, uint16_t color) {
    M5.Lcd.fillRect(0, y, DISP_W, 1, color);
}

void draw_status_bar(const char* msg, uint16_t color) {
    M5.Lcd.fillRect(0, DISP_H - FOOTER_H, DISP_W, FOOTER_H, CLR_DARK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(color, CLR_DARK);
    M5.Lcd.setCursor(4, DISP_H - FOOTER_H + 3);
    M5.Lcd.print(msg);
}

// ── Text helpers ──────────────────────────────────────────────
void draw_centered_text(const char* text, int y, uint8_t size,
                        uint16_t fg, uint16_t bg) {
    M5.Lcd.setTextSize(size);
    int tw = strlen(text) * 6 * size;
    M5.Lcd.setTextColor(fg, bg);
    M5.Lcd.setCursor((DISP_W - tw) / 2, y);
    M5.Lcd.print(text);
}

void draw_text(int x, int y, const char* text, uint8_t size,
               uint16_t fg, uint16_t bg) {
    M5.Lcd.setTextSize(size);
    M5.Lcd.setTextColor(fg, bg);
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(text);
}

void draw_box(int x, int y, int w, int h, uint16_t color) {
    M5.Lcd.drawRect(x, y, w, h, color);
}

void draw_filled_box(int x, int y, int w, int h, uint16_t color) {
    M5.Lcd.fillRect(x, y, w, h, color);
}

// ── Progress bar ──────────────────────────────────────────────
void draw_progress(int x, int y, int w, int h, int pct,
                   uint16_t fg, uint16_t bg) {
    M5.Lcd.fillRect(x, y, w, h, bg);
    int filled = (w * pct) / 100;
    if (filled > 0) M5.Lcd.fillRect(x, y, filled, h, fg);
    M5.Lcd.drawRect(x, y, w, h, CLR_BORDER);
}

// ── Spinner (call with incrementing frame each loop) ──────────
void draw_spinner(int x, int y, uint8_t frame, uint16_t color) {
    const char frames[] = {'|', '/', '-', '\\'};
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(color, CLR_BG);
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(frames[frame % 4]);
}

// ── Full-screen messages ──────────────────────────────────────
void show_message(const char* title, const char* body, uint16_t title_color) {
    display_clear();
    draw_header(title);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 8);
    M5.Lcd.print(body);
    draw_footer("BtnA: next", "BtnB: back");
}

void show_error(const char* msg) {
    display_clear();
    M5.Lcd.fillRect(0, 0, DISP_W, HEADER_H, CLR_ERR);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CLR_WHITE, CLR_ERR);
    M5.Lcd.setCursor(4, 3);
    M5.Lcd.print("ERROR");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 4);
    M5.Lcd.print(msg);
}

void show_success(const char* msg) {
    display_clear();
    M5.Lcd.fillRect(0, 0, DISP_W, HEADER_H, CLR_SUCCESS);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CLR_WHITE, CLR_SUCCESS);
    M5.Lcd.setCursor(4, 3);
    M5.Lcd.print("OK");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 4);
    M5.Lcd.print(msg);
}

// ── Smoothed battery percentage ───────────────────────────────
// Samples up to 8 readings, one per second, and returns the average.
// Eliminates the AXP2101 voltage-curve bounce and fast initial drop
// seen when the device is under WiFi + RF load.
int battery_pct() {
    static int     samples[8]  = { -1,-1,-1,-1,-1,-1,-1,-1 };
    static uint8_t idx         = 0;
    static uint32_t last_ms    = 0;

    uint32_t now = millis();
    // Seed the buffer on first call so we have something to return immediately
    if (samples[0] < 0) {
        int raw = M5.Power.getBatteryLevel();
        if (raw < 0)   raw = 0;
        if (raw > 100) raw = 100;
        for (int i = 0; i < 8; i++) samples[i] = raw;
    }

    if (now - last_ms >= 1000) {
        last_ms = now;
        int raw = M5.Power.getBatteryLevel();
        if (raw < 0)   raw = 0;
        if (raw > 100) raw = 100;
        samples[idx] = raw;
        idx = (idx + 1) % 8;
    }

    int sum = 0;
    for (int i = 0; i < 8; i++) sum += samples[i];
    return sum / 8;
}

// ── Battery indicator (top-right corner) ─────────────────────
void draw_battery(int x, int y) {
    int pct = battery_pct();
    uint16_t color = (pct > 50) ? CLR_SUCCESS : (pct > 20) ? CLR_WARN : CLR_ERR;
    int filled = (16 * pct) / 100;
    M5.Lcd.drawRect(x, y, 18, 8, CLR_TEXT_DIM);
    M5.Lcd.fillRect(x + 18, y + 2, 2, 4, CLR_TEXT_DIM);
    if (filled > 0) M5.Lcd.fillRect(x + 1, y + 1, filled, 6, color);
}
