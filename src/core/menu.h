#pragma once
#include <Arduino.h>
#include "../globals.h"

// ─────────────────────────────────────────────────────────────
//  Menu system — scrollable list with header/footer
// ─────────────────────────────────────────────────────────────

#define MENU_MAX_ITEMS   16
#define MENU_ITEM_H      17     // pixels per row at text size 2
#define MENU_VISIBLE     5      // rows that fit in content area

struct MenuItem {
    const char* label;
    const char* icon;     // 1-2 char prefix, e.g. ">" or "W"
    bool        enabled;
};

class Menu {
public:
    Menu(const char* title, const char* footer_left = "A:scroll",
         const char* footer_right = "B:select");

    void add(const char* label, const char* icon = ">", bool enabled = true);
    void clear();
    void draw();
    void scroll_down();
    void scroll_up();
    int  selected() const { return _sel; }
    void set_selected(int idx);
    int  count() const { return _count; }
    void set_title(const char* t) { _title = t; }

private:
    const char* _title;
    const char* _footer_l;
    const char* _footer_r;
    MenuItem    _items[MENU_MAX_ITEMS];
    int         _count  = 0;
    int         _sel    = 0;
    int         _scroll = 0;    // top visible index

    void _draw_item(int list_idx, int screen_row);
};
