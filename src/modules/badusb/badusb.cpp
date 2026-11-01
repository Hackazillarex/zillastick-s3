#include "badusb.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <SPIFFS.h>
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
      "REM Open CMD prompt\nGUI r\nDELAY 600\nSTRING cmd\nENTER\n" },
    { "win_powershell.txt",
      "REM Open PowerShell\nGUI r\nDELAY 600\nSTRING powershell\nENTER\n" },
    { "win_lock.txt",
      "REM Lock Windows screen\nGUI l\n" },
    { "mac_terminal.txt",
      "REM Open Mac Terminal via Spotlight\nGUI SPACE\nDELAY 600\nSTRING Terminal\nENTER\n" },
};

void badusb_ensure_defaults() {
    if (!SPIFFS.begin(true)) return;   // true = format if blank
    // Create /payloads dir marker (SPIFFS is flat but open() needs the path)
    for (auto& p : DEFAULT_PAYLOADS) {
        char path[72];
        snprintf(path, sizeof(path), "%s/%s", PAYLOAD_DIR, p.filename);
        if (!SPIFFS.exists(path)) {
            File f = SPIFFS.open(path, "w");
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
        delay(1500);  // Let host enumerate
        s_usb_init = true;
    }
}

// ─── HID key name → keycode ───────────────────────────────────
static uint8_t _key_code(const char* name) {
    if (!name) return 0;
    if (strlen(name) == 1) return (uint8_t)toupper(name[0]);
    if (strcasecmp(name, "ENTER")     == 0) return KEY_RETURN;
    if (strcasecmp(name, "SPACE")     == 0) return ' ';
    if (strcasecmp(name, "TAB")       == 0) return KEY_TAB;
    if (strcasecmp(name, "ESC")       == 0) return KEY_ESC;
    if (strcasecmp(name, "BACKSPACE") == 0) return KEY_BACKSPACE;
    if (strcasecmp(name, "DELETE")    == 0) return KEY_DELETE;
    if (strcasecmp(name, "HOME")      == 0) return KEY_HOME;
    if (strcasecmp(name, "END")       == 0) return KEY_END;
    if (strcasecmp(name, "PAGEUP")    == 0) return KEY_PAGE_UP;
    if (strcasecmp(name, "PAGEDOWN")  == 0) return KEY_PAGE_DOWN;
    if (strcasecmp(name, "UP")        == 0) return KEY_UP_ARROW;
    if (strcasecmp(name, "DOWN")      == 0) return KEY_DOWN_ARROW;
    if (strcasecmp(name, "LEFT")      == 0) return KEY_LEFT_ARROW;
    if (strcasecmp(name, "RIGHT")     == 0) return KEY_RIGHT_ARROW;
    if (name[0] == 'F' && strlen(name) <= 3) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return KEY_F1 + (n - 1);
    }
    return 0;
}

// ─── Execute one script line ──────────────────────────────────
static void _exec_line(const char* line) {
    if (!line || *line == '\0' || strncasecmp(line, "REM", 3) == 0) return;

    char cmd[16] = {0};
    const char* arg = nullptr;

    int i = 0;
    while (line[i] && line[i] != ' ' && i < 15) { cmd[i] = line[i]; i++; }
    if (line[i] == ' ') arg = &line[i + 1];

    uint16_t dly = g_cfg.badusb_delay;

    if (strcasecmp(cmd, "STRING") == 0 && arg) {
        Keyboard.print(arg);
        delay(dly);
    }
    else if (strcasecmp(cmd, "ENTER") == 0) {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
    }
    else if (strcasecmp(cmd, "DELAY") == 0 && arg) {
        delay(atoi(arg));
    }
    else if (strcasecmp(cmd, "GUI") == 0 && arg) {
        Keyboard.press(KEY_LEFT_GUI); Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
    else if (strcasecmp(cmd, "CTRL") == 0 && arg) {
        Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
    else if (strcasecmp(cmd, "ALT") == 0 && arg) {
        Keyboard.press(KEY_LEFT_ALT); Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
    else if (strcasecmp(cmd, "SHIFT") == 0 && arg) {
        Keyboard.press(KEY_LEFT_SHIFT); Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
    else if (strcasecmp(cmd, "CTRL-ALT") == 0 && arg) {
        Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_ALT);
        Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
    else if (strcasecmp(cmd, "CTRL-SHIFT") == 0 && arg) {
        Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_SHIFT);
        Keyboard.press(_key_code(arg));
        delay(100); Keyboard.releaseAll(); delay(dly);
    }
}

// ─── Run script from /payloads/ file ─────────────────────────
void badusb_run_script(const char* path) {
    if (!SPIFFS.begin(false)) { show_error("SPIFFS error"); delay(2000); return; }
    File f = SPIFFS.open(path, "r");
    if (!f) { show_error("Script not found"); delay(2000); return; }

    _usb_begin();
    delay(1000);

    display_clear();
    draw_header("Running Script");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    // Show just the filename, not full path
    const char* name = strrchr(path, '/');
    M5.Lcd.printf("%.34s", name ? name + 1 : path);
    draw_status_bar("Injecting...", CLR_PRIMARY);

    char line[256];
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
    if (!SPIFFS.begin(false)) { show_error("SPIFFS error"); delay(2000); return; }

    // Collect payload filenames
    struct Entry { char name[48]; char path[64]; };
    static Entry entries[MENU_MAX_ITEMS - 1];
    int count = 0;

    File dir = SPIFFS.open(PAYLOAD_DIR);
    if (dir) {
        File f = dir.openNextFile();
        while (f && count < (int)(sizeof(entries) / sizeof(entries[0]))) {
            if (!f.isDirectory()) {
                const char* fn = f.name();
                // strip leading slash if present
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
            if (sel >= count) return;   // Back or empty
            badusb_run_script(entries[sel].path);
            m.draw();
        }
        if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}
