#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  Bad USB — USB HID keyboard injection
//
//  All payloads live in SPIFFS /payloads/ as plain .txt files.
//  Add, edit or remove files there — no recompile needed.
//
//  Script format: one command per line
//    STRING <text>       Type text
//    ENTER               Press Enter
//    DELAY <ms>          Wait
//    GUI <key>           Win/Cmd + key
//    CTRL <key>          Ctrl + key
//    ALT <key>           Alt + key
//    SHIFT <key>         Shift + key
//    CTRL-ALT <key>      Ctrl+Alt+key
//    CTRL-SHIFT <key>    Ctrl+Shift+key
//    REM <comment>       Ignored
// ─────────────────────────────────────────────────────────────

#define PAYLOAD_DIR  "/payloads"

void badusb_ensure_defaults();   // call once from setup() — writes defaults if missing
void badusb_module_run();
void badusb_run_script(const char* path);
