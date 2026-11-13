#ifndef KEY_PRINT_SCREEN
#define KEY_PRINT_SCREEN  0x46
#endif
#ifndef KEY_PAUSE
#define KEY_PAUSE         0x48
#endif
#ifndef KEY_NUM_LOCK
#define KEY_NUM_LOCK      0x53
#endif
#ifndef KEY_SCROLL_LOCK
#define KEY_SCROLL_LOCK   0x47
#endif
#ifndef KEY_MENU
#define KEY_MENU          0x65
#endif

#include "badusb.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <LittleFS.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

static USBHIDKeyboard Keyboard;
static bool s_usb_init = false;

// ─── Default payloads written to SPIFFS on first boot ────────
struct DefaultPayload { const char* filename; const char* content; };
static const DefaultPayload DEFAULT_PAYLOADS[] = {
    { "win_run.txt",
      "REM Open Windows Run Dialog\nGUI r\n" },
    { "win_cmd.txt",
      "REM Open CMD prompt\nGUI r\nDELAY 800\nSTRING cmd\nENTER\n" },
    { "win_powershell.txt",
      "REM Open PowerShell\nGUI r\nDELAY 800\nSTRING powershell\nENTER\n" },
    { "win_lock.txt",
      "REM Lock Windows screen\nGUI l\n" },
    { "mac_terminal.txt",
      "REM Open Mac Terminal via Spotlight\nGUI SPACE\nDELAY 800\nSTRING Terminal\nENTER\n" },
};

void badusb_ensure_defaults() {
    if (!LittleFS.begin(true)) return;   // true = format if blank
    for (auto& p : DEFAULT_PAYLOADS) {
        char path[72];
        snprintf(path, sizeof(path), "%s/%s", PAYLOAD_DIR, p.filename);
        if (!LittleFS.exists(path)) {
            File f = LittleFS.open(path, "w");
            if (f) { f.print(p.content); f.close(); }
        }
    }
}

// ─── USB init (done once) ─────────────────────────────────────
static void _usb_begin() {
    if (!s_usb_init) {
        USB.manufacturerName("ZILLASTICK S3");
        USB.productName("RF Tools HID");
        USB.VID(0x239A);
        USB.PID(0x800B);
        Keyboard.begin();
        USB.begin();
        delay(2000);  // Let host enumerate (bumped from 1500)
        s_usb_init = true;
    }
}

// ─── HID key name → keycode ───────────────────────────────────
// IMPORTANT: single-char keys must use tolower() so the HID library
// sends the UNSHIFTED keycode. toupper() causes press('R') which the
// library internally maps as SHIFT+R, turning GUI r into WIN+SHIFT+R.
static uint8_t _key_code(const char* name) {
    if (!name) return 0;
    if (strlen(name) == 1) return (uint8_t)tolower((unsigned char)name[0]);

    // Navigation / editing
    if (strcasecmp(name, "ENTER")      == 0) return KEY_RETURN;
    if (strcasecmp(name, "SPACE")      == 0) return ' ';
    if (strcasecmp(name, "TAB")        == 0) return KEY_TAB;
    if (strcasecmp(name, "ESC")        == 0) return KEY_ESC;
    if (strcasecmp(name, "ESCAPE")     == 0) return KEY_ESC;
    if (strcasecmp(name, "BACKSPACE")  == 0) return KEY_BACKSPACE;
    if (strcasecmp(name, "DELETE")     == 0) return KEY_DELETE;
    if (strcasecmp(name, "DEL")        == 0) return KEY_DELETE;
    if (strcasecmp(name, "INSERT")     == 0) return KEY_INSERT;
    if (strcasecmp(name, "HOME")       == 0) return KEY_HOME;
    if (strcasecmp(name, "END")        == 0) return KEY_END;
    if (strcasecmp(name, "PAGEUP")     == 0) return KEY_PAGE_UP;
    if (strcasecmp(name, "PAGEDOWN")   == 0) return KEY_PAGE_DOWN;
    if (strcasecmp(name, "CAPSLOCK")   == 0) return KEY_CAPS_LOCK;
    if (strcasecmp(name, "PRINTSCREEN")== 0) return KEY_PRINT_SCREEN;
    if (strcasecmp(name, "PAUSE")      == 0) return KEY_PAUSE;
    if (strcasecmp(name, "BREAK")      == 0) return KEY_PAUSE;
    if (strcasecmp(name, "NUMLOCK")    == 0) return KEY_NUM_LOCK;
    if (strcasecmp(name, "SCROLLLOCK") == 0) return KEY_SCROLL_LOCK;
    if (strcasecmp(name, "MENU")       == 0) return KEY_MENU;
    if (strcasecmp(name, "APP")        == 0) return KEY_MENU;

    // Arrow keys
    if (strcasecmp(name, "UP")         == 0) return KEY_UP_ARROW;
    if (strcasecmp(name, "DOWN")       == 0) return KEY_DOWN_ARROW;
    if (strcasecmp(name, "LEFT")       == 0) return KEY_LEFT_ARROW;
    if (strcasecmp(name, "RIGHT")      == 0) return KEY_RIGHT_ARROW;

    // Function keys F1-F24
    if ((name[0] == 'F' || name[0] == 'f') && strlen(name) <= 3) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12)  return KEY_F1  + (n - 1);
        if (n >= 13 && n <= 24) return KEY_F13 + (n - 13);
    }
    return 0;
}

// ─── Last executed line (for REPEAT support) ──────────────────
static char s_last_line[256] = {0};
static uint16_t s_default_delay = 0;   // set by DEFAULT_DELAY command

// ─── Press a modifier combo: MOD + key, then release ─────────
static void _press_combo(uint8_t mod1, uint8_t mod2, uint8_t key, uint16_t post_dly) {
    if (mod1) Keyboard.press(mod1);
    if (mod2) Keyboard.press(mod2);
    if (key)  Keyboard.press(key);
    delay(80);
    Keyboard.releaseAll();
    if (post_dly) delay(post_dly);
}

// ─── Execute one script line ──────────────────────────────────
static void _exec_line(const char* line) {
    if (!line || *line == '\0') return;
    if (strncasecmp(line, "REM", 3) == 0) return;

    // Save for REPEAT (before parsing so REPEAT itself isn't saved)
    bool is_repeat = (strncasecmp(line, "REPEAT", 6) == 0);

    char cmd[32] = {0};
    const char* arg = nullptr;

    int i = 0;
    while (line[i] && line[i] != ' ' && i < 31) { cmd[i] = line[i]; i++; }
    if (line[i] == ' ') arg = &line[i + 1];

    uint16_t dly = g_cfg.badusb_delay + s_default_delay;

    // ── REPEAT ───────────────────────────────────────────────
    if (is_repeat) {
        int times = arg ? atoi(arg) : 1;
        for (int r = 0; r < times && s_last_line[0]; r++) _exec_line(s_last_line);
        return;
    }

    // Save this line for future REPEAT before executing
    if (!is_repeat) strncpy(s_last_line, line, sizeof(s_last_line) - 1);

    // ── Delay / timing ───────────────────────────────────────
    if (strcasecmp(cmd, "DELAY") == 0 && arg) {
        delay(atoi(arg));
        return;
    }
    if (strcasecmp(cmd, "DEFAULT_DELAY") == 0 || strcasecmp(cmd, "DEFAULTDELAY") == 0) {
        if (arg) s_default_delay = (uint16_t)atoi(arg);
        return;
    }

    // ── String output ────────────────────────────────────────
    if (strcasecmp(cmd, "STRING") == 0 && arg) {
        Keyboard.print(arg);
        delay(dly);
        return;
    }
    if (strcasecmp(cmd, "STRINGLN") == 0 && arg) {
        Keyboard.println(arg);
        delay(dly);
        return;
    }

    // ── Standalone keys ──────────────────────────────────────
    if (strcasecmp(cmd, "ENTER") == 0) {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "SPACE") == 0) {
        Keyboard.press(' '); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "TAB") == 0) {
        Keyboard.press(KEY_TAB); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "ESC") == 0 || strcasecmp(cmd, "ESCAPE") == 0) {
        Keyboard.press(KEY_ESC); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "BACKSPACE") == 0) {
        Keyboard.press(KEY_BACKSPACE); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "DELETE") == 0) {
        Keyboard.press(KEY_DELETE); delay(50); Keyboard.releaseAll();
        return;
    }
    if (strcasecmp(cmd, "UP")    == 0) { Keyboard.press(KEY_UP_ARROW);    delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "DOWN")  == 0) { Keyboard.press(KEY_DOWN_ARROW);  delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "LEFT")  == 0) { Keyboard.press(KEY_LEFT_ARROW);  delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "RIGHT") == 0) { Keyboard.press(KEY_RIGHT_ARROW); delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "HOME")  == 0) { Keyboard.press(KEY_HOME);        delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "END")   == 0) { Keyboard.press(KEY_END);         delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "PAGEUP")   == 0) { Keyboard.press(KEY_PAGE_UP);   delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "PAGEDOWN") == 0) { Keyboard.press(KEY_PAGE_DOWN); delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "CAPSLOCK")    == 0) { Keyboard.press(KEY_CAPS_LOCK);   delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "NUMLOCK")     == 0) { Keyboard.press(KEY_NUM_LOCK);    delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "SCROLLLOCK")  == 0) { Keyboard.press(KEY_SCROLL_LOCK); delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "PRINTSCREEN") == 0) { Keyboard.press(KEY_PRINT_SCREEN);delay(50); Keyboard.releaseAll(); return; }
    if (strcasecmp(cmd, "MENU") == 0 || strcasecmp(cmd, "APP") == 0) {
        Keyboard.press(KEY_MENU); delay(50); Keyboard.releaseAll();
        return;
    }

    // Standalone Fn keys
    if ((cmd[0] == 'F' || cmd[0] == 'f') && cmd[1] != '\0' && arg == nullptr) {
        int n = atoi(cmd + 1);
        uint8_t kc = 0;
        if (n >= 1  && n <= 12) kc = KEY_F1  + (n - 1);
        if (n >= 13 && n <= 24) kc = KEY_F13 + (n - 13);
        if (kc) { Keyboard.press(kc); delay(50); Keyboard.releaseAll(); }
        return;
    }

    // ── Modifier combos ──────────────────────────────────────
    if (!arg) return;   // no key after modifier — ignore

    if (strcasecmp(cmd, "GUI")     == 0 || strcasecmp(cmd, "WINDOWS") == 0)
        { _press_combo(KEY_LEFT_GUI,   0,              _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "CTRL")    == 0)
        { _press_combo(KEY_LEFT_CTRL,  0,              _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "ALT")     == 0)
        { _press_combo(KEY_LEFT_ALT,   0,              _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "SHIFT")   == 0)
        { _press_combo(KEY_LEFT_SHIFT, 0,              _key_code(arg), dly); return; }

    if (strcasecmp(cmd, "CTRL-ALT")   == 0)
        { _press_combo(KEY_LEFT_CTRL,  KEY_LEFT_ALT,   _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "CTRL-SHIFT") == 0)
        { _press_combo(KEY_LEFT_CTRL,  KEY_LEFT_SHIFT, _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "ALT-SHIFT")  == 0)
        { _press_combo(KEY_LEFT_ALT,   KEY_LEFT_SHIFT, _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "GUI-SHIFT")  == 0)
        { _press_combo(KEY_LEFT_GUI,   KEY_LEFT_SHIFT, _key_code(arg), dly); return; }
    if (strcasecmp(cmd, "GUI-CTRL")   == 0)
        { _press_combo(KEY_LEFT_GUI,   KEY_LEFT_CTRL,  _key_code(arg), dly); return; }
}

// ─── Run script from /payloads/ file ─────────────────────────
void badusb_run_script(const char* path) {
    if (!LittleFS.begin(false)) { show_error("SPIFFS error"); delay(2000); return; }
    File f = LittleFS.open(path, "r");
    if (!f) { show_error("Script not found"); delay(2000); return; }

    _usb_begin();
    delay(1000);

    // Reset per-script state
    s_last_line[0]  = '\0';
    s_default_delay = 0;

    display_clear();
    draw_header("Running Script");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    const char* name = strrchr(path, '/');
    M5.Lcd.printf("%.34s", name ? name + 1 : path);
    draw_status_bar("Injecting...", CLR_PRIMARY);

    char line[1024];
    uint32_t line_num = 0;
    while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = 0;
        if (len > 0 && line[len - 1] == '\r') line[--len] = 0;
        _exec_line(line);
        line_num++;
        M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 16);
        M5.Lcd.printf("Line: %lu", line_num);
    }
    f.close();

    Keyboard.releaseAll();
    show_success("Script complete!");
    delay(1500);
}

// ─── Entry point — lists all files in /payloads/ ─────────────
void badusb_module_run() {
    if (!LittleFS.begin(false)) { show_error("SPIFFS error"); delay(2000); return; }

    struct Entry { char name[48]; char path[64]; };
    static Entry entries[MENU_MAX_ITEMS - 1];
    int count = 0;

    File dir = LittleFS.open(PAYLOAD_DIR);
    if (dir) {
        File f = dir.openNextFile();
        while (f && count < (int)(sizeof(entries) / sizeof(entries[0]))) {
            if (!f.isDirectory()) {
                const char* fn = f.name();
                const char* base = strrchr(fn, '/');
                base = base ? base + 1 : fn;
                if (strstr(base, ".txt") || strstr(base, ".duck")) {
                    strncpy(entries[count].name, base, 47);
                    entries[count].name[47] = '\0';
                    snprintf(entries[count].path, 64, "%s/%s", PAYLOAD_DIR, base);
                    count++;
                }
            }
            f = dir.openNextFile();
        }
    }

    Menu m("Bad USB");
    for (int i = 0; i < count; i++) m.add(entries[i].name, ">");
    if (count == 0) m.add("(no payloads found)", " ");
    m.add("< Back", " ");
    m.draw();

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) m.scroll_down();
        if (M5.BtnB.wasPressed()) {
            int sel = m.selected();
            if (sel >= count) return;
            badusb_run_script(entries[sel].path);
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}
