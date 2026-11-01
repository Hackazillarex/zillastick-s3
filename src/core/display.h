#pragma once
#include <M5Unified.h>
#include "../globals.h"

// ─────────────────────────────────────────────────────────────
//  Display helpers — thin wrapper around M5.Lcd (M5GFX)
// ─────────────────────────────────────────────────────────────

void display_init();
void display_splash();
void display_clear();

// Layout primitives
void draw_header(const char* title, const char* right_label = nullptr);
void draw_footer(const char* left_hint, const char* right_hint);
void draw_divider(int y, uint16_t color = CLR_BORDER);
void draw_status_bar(const char* msg, uint16_t color = CLR_TEXT_DIM);

// Convenience
void draw_centered_text(const char* text, int y, uint8_t size = 2,
                        uint16_t fg = CLR_TEXT, uint16_t bg = CLR_BG);
void draw_text(int x, int y, const char* text, uint8_t size = 1,
               uint16_t fg = CLR_TEXT, uint16_t bg = CLR_BG);
void draw_box(int x, int y, int w, int h, uint16_t color);
void draw_filled_box(int x, int y, int w, int h, uint16_t color);

// Progress / feedback
void draw_progress(int x, int y, int w, int h, int pct,
                   uint16_t fg = CLR_PRIMARY, uint16_t bg = CLR_DARK);
void draw_spinner(int x, int y, uint8_t frame, uint16_t color = CLR_PRIMARY);

// Full-screen messages
void show_message(const char* title, const char* body,
                  uint16_t title_color = CLR_PRIMARY);
void show_error(const char* msg);
void show_success(const char* msg);

// Battery indicator
void draw_battery(int x, int y);
