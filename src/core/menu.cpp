#include "menu.h"
#include "display.h"
#include <M5Unified.h>

// ─────────────────────────────────────────────────────────────
//  Menu implementation
// ─────────────────────────────────────────────────────────────

Menu::Menu(const char* title, const char* footer_left, const char* footer_right)
    : _title(title), _footer_l(footer_left), _footer_r(footer_right),
      _count(0), _sel(0), _scroll(0) {}

void Menu::add(const char* label, const char* icon, bool enabled) {
    if (_count >= MENU_MAX_ITEMS) return;
    _items[_count++] = {label, icon, enabled};
}

void Menu::clear() {
    _count  = 0;
    _sel    = 0;
    _scroll = 0;
}

void Menu::set_selected(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= _count) idx = _count - 1;
    _sel = idx;
    if (_sel < _scroll) _scroll = _sel;
    if (_sel >= _scroll + MENU_VISIBLE) _scroll = _sel - MENU_VISIBLE + 1;
}

void Menu::scroll_down() {
    set_selected(_sel + 1 >= _count ? 0 : _sel + 1);
    draw();
}

void Menu::scroll_up() {
    set_selected(_sel - 1 < 0 ? _count - 1 : _sel - 1);
    draw();
}

void Menu::draw() {
    // Header
    draw_header(_title);

    // Items area
    M5.Lcd.fillRect(0, HEADER_H + 1, DISP_W, DISP_H - HEADER_H - FOOTER_H - 1, CLR_BG);

    int visible = min(_count - _scroll, MENU_VISIBLE);
    for (int i = 0; i < visible; i++) {
        _draw_item(_scroll + i, i);
    }

    // Scroll indicator
    if (_count > MENU_VISIBLE) {
        int bar_h = CONTENT_H;
        int thumb_h = max(6, bar_h * MENU_VISIBLE / _count);
        int thumb_y = CONTENT_Y + (_scroll * (bar_h - thumb_h)) / max(1, _count - MENU_VISIBLE);
        M5.Lcd.fillRect(DISP_W - 4, CONTENT_Y, 4, bar_h, CLR_DARK);
        M5.Lcd.fillRect(DISP_W - 4, thumb_y, 4, thumb_h, CLR_PRIMARY);
    }

    draw_footer(_footer_l, _footer_r);
}

void Menu::_draw_item(int list_idx, int screen_row) {
    if (list_idx < 0 || list_idx >= _count) return;
    const MenuItem& item = _items[list_idx];
    bool selected = (list_idx == _sel);
    bool enabled  = item.enabled;

    int y  = CONTENT_Y + screen_row * MENU_ITEM_H;
    int w  = _count > MENU_VISIBLE ? DISP_W - 5 : DISP_W;

    uint16_t bg = selected ? CLR_SEL_BG : CLR_BG;
    uint16_t fg = !enabled ? CLR_TEXT_DIM :
                  selected ? CLR_SEL_FG  : CLR_TEXT;

    M5.Lcd.fillRect(0, y, w, MENU_ITEM_H - 1, bg);

    // Left accent bar for selected
    if (selected) {
        M5.Lcd.fillRect(0, y, 3, MENU_ITEM_H - 1, CLR_BRIGHT);
    }

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(fg, bg);

    // Label (icons removed — start flush against left accent bar)
    M5.Lcd.setCursor(8, y + 1);
    M5.Lcd.print(item.label);

    // Separator line
    if (!selected) {
        M5.Lcd.drawFastHLine(0, y + MENU_ITEM_H - 1, w, CLR_BORDER);
    }
}
