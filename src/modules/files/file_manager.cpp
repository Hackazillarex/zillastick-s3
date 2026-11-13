#include "file_manager.h"
#include "../../core/display.h"
#include "../../core/menu.h"
#include "../../core/config.h"
#include "../../globals.h"
#include <M5Unified.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// ─── Collect files from a directory into a list ───────────────
struct FileEntry {
    char name[48];
    size_t size;
    bool is_dir;
};

#define MAX_FILES 32
static FileEntry s_files[MAX_FILES];
static int       s_fcount = 0;
static char      s_cur_dir[32];

static void _scan_dir(const char* dir) {
    s_fcount = 0;
    strncpy(s_cur_dir, dir, sizeof(s_cur_dir)-1);

    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;

    File f = d.openNextFile();
    while (f && s_fcount < MAX_FILES) {
        FileEntry& e = s_files[s_fcount];
        // f.name() may return just a short name (e.g. "portal.txt") or the
        // full SPIFFS path ("/captures/portal.txt") depending on the Arduino-
        // ESP32 version.  Always store the full path so LittleFS.remove() works.
        const char* raw = f.name();
        if (raw[0] == '/') {
            strncpy(e.name, raw, sizeof(e.name) - 1);
        } else {
            snprintf(e.name, sizeof(e.name), "%s/%s", dir, raw);
        }
        e.name[sizeof(e.name) - 1] = 0;
        e.size   = f.size();
        e.is_dir = f.isDirectory();
        s_fcount++;
        f = d.openNextFile();
    }
}

// ─── Human-readable size ──────────────────────────────────────
static void _fmt_size(size_t sz, char* out, size_t len) {
    if (sz < 1024)        snprintf(out, len, "%dB",  (int)sz);
    else if (sz < 1048576) snprintf(out, len, "%.1fK", sz/1024.0f);
    else                   snprintf(out, len, "%.1fM", sz/1048576.0f);
}

// ─── Detail view for a single file ───────────────────────────
static void _show_file_detail(const FileEntry& fe) {
    display_clear();
    draw_header("File Info");

    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    // Filename (may be long — truncate display)
    char display_name[32];
    const char* short_name = fe.name;
    // Strip leading /payloads/ or /captures/
    const char* slash = strrchr(fe.name, '/');
    if (slash) short_name = slash + 1;
    strncpy(display_name, short_name, sizeof(display_name)-1);
    M5.Lcd.print(display_name);

    char szbuf[12];
    _fmt_size(fe.size, szbuf, sizeof(szbuf));

    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 18);
    M5.Lcd.printf("Size: %s", szbuf);

    M5.Lcd.setCursor(4, CONTENT_Y + 30);
    M5.Lcd.printf("Path: %s", fe.name);

    // Show first 3 lines of content if text file
    if (strstr(fe.name, ".txt") || strstr(fe.name, ".duck") || strstr(fe.name, ".log")) {
        M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
        M5.Lcd.setCursor(4, CONTENT_Y + 46);
        M5.Lcd.print("--- Preview ---");
        M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);

        File fp = LittleFS.open(fe.name, "r");
        if (fp) {
            int y = CONTENT_Y + 58;
            int lines = 0;
            char linebuf[40];
            while (fp.available() && lines < 3 && y < DISP_H - FOOTER_H - 2) {
                int n = fp.readBytesUntil('\n', linebuf, sizeof(linebuf)-1);
                linebuf[n] = 0;
                if (n > 0 && linebuf[n-1] == '\r') linebuf[--n] = 0;
                M5.Lcd.setCursor(4, y);
                M5.Lcd.print(linebuf);
                y += 11;
                lines++;
            }
            fp.close();
        }
    }

    draw_footer("DEL", "Back");

    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            // Delete confirm
            display_clear();
            draw_header("Delete File?");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_ERR, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 10);
            M5.Lcd.print(display_name);
            M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 26);
            M5.Lcd.print("This cannot be undone.");
            draw_footer("YES/Del", "No/Back");

            while (true) {
                M5.update();
                if (M5.BtnA.wasPressed()) {
                    Serial.printf("[FM] Remove: '%s'\n", fe.name);
                    if (LittleFS.remove(fe.name)) {
                        show_success("Deleted!");
                    } else {
                        // Show the attempted path so the bug is diagnosable
                        char errmsg[52];
                        snprintf(errmsg, sizeof(errmsg), "Failed:\n%.46s", fe.name);
                        show_error(errmsg);
                    }
                    delay(1200);
                    return;
                }
                if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
                    return;
                }
                delay(10);
            }
        }
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
        delay(10);
    }
}

// ─── Browse a directory ───────────────────────────────────────
static void _browse_dir(const char* dir, const char* title) {
    while (true) {
        _scan_dir(dir);

        if (s_fcount == 0) {
            display_clear();
            draw_header(title);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 20);
            M5.Lcd.print("No files found.");
            draw_footer("", "Back");
            while (true) {
                M5.update();
                if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
                delay(10);
            }
        }

        // Build menu from file list
        Menu m(title);
        for (int i = 0; i < s_fcount; i++) {
            const char* short_n = strrchr(s_files[i].name, '/');
            if (short_n) short_n++; else short_n = s_files[i].name;
            m.add(short_n, s_files[i].is_dir ? "D" : "F");
        }
        m.add("< Back", " ");
        m.draw();

        bool need_rescan = false;
        while (!need_rescan) {
            M5.update();
            if (M5.BtnA.wasPressed()) m.scroll_down();
            if (M5.BtnB.wasPressed()) {
                int sel = m.selected();
                if (sel == s_fcount) return;  // Back
                _show_file_detail(s_files[sel]);
                need_rescan = true;  // File may have been deleted
            }
            if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
            delay(10);
        }
    }
}

// ─── SPIFFS stats bar ─────────────────────────────────────────
static void _draw_storage_bar() {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    if (total == 0) return;

    char buf[40];
    char u[10], t[10];
    _fmt_size(used,  u, sizeof(u));
    _fmt_size(total, t, sizeof(t));
    snprintf(buf, sizeof(buf), "%s / %s", u, t);

    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 2);
    M5.Lcd.print("Storage:");
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(60, CONTENT_Y + 2);
    M5.Lcd.print(buf);

    // Progress bar
    int bar_w = DISP_W - 8;
    int fill  = (int)((float)used / total * bar_w);
    uint16_t col = (used > total * 80 / 100) ? CLR_ERR : CLR_PRIMARY;
    M5.Lcd.fillRect(4, CONTENT_Y + 14, bar_w, 5, CLR_BORDER);
    M5.Lcd.fillRect(4, CONTENT_Y + 14, fill,  5, col);
}

// ═════════════════════════════════════════════════════════════
//  WiFi File Server
//  Starts a SoftAP and serves a browser-based file manager.
//  Connect to SSID "ZILLASTICK_FILES" then open 192.168.4.1
// ═════════════════════════════════════════════════════════════

#define FS_AP_SSID   "ZILLASTICK_FILES"
#define FS_AP_PASS   ""          // Open AP — no password
#define FS_AP_PORT   80

// State for the multipart upload handler (shared between the two
// AsyncWebServer callbacks; must live at file scope).
static File   s_ul_fp;
static String s_ul_path;
static bool   s_ul_ok = false;

// ─── Embedded HTML (PROGMEM) ──────────────────────────────────
static const char FS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ZillaStick Files</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{background:#0d0000;color:#fff;font-family:monospace;padding:10px;font-size:14px}
    h1{color:#dc0000;margin-bottom:2px;font-size:1.15em}
    .sub{color:#888;font-size:.75em;margin-bottom:10px}
    #msg{display:none;padding:5px 8px;border-radius:3px;margin-bottom:8px;font-size:.8em}
    .ok{background:#003018;color:#00c850}.err{background:#3c0000;color:#ff4040}
    #stat{color:#888;font-size:.75em;margin-bottom:6px}
    #stat span{color:#ccc}
    .tabs{display:flex;gap:4px;margin-bottom:0}
    .tab{padding:4px 14px;border-radius:3px 3px 0 0;cursor:pointer;
         background:#1a0000;color:#888;border:1px solid #3c0000;border-bottom:none;font-size:.8em}
    .tab.active{background:#3c0000;color:#dc0000;font-weight:bold}
    table{width:100%;border-collapse:collapse;font-size:.8em;
          border:1px solid #3c0000;border-top:none}
    th{background:#3c0000;color:#dc0000;padding:5px 6px;text-align:left}
    tr:nth-child(even){background:#150000}
    td{padding:4px 6px;border-bottom:1px solid #280000;word-break:break-all}
    .sz{color:#888;white-space:nowrap;width:52px}
    .ac{width:80px;white-space:nowrap}
    .btn{display:inline-block;padding:2px 7px;border-radius:2px;cursor:pointer;
         border:none;font-size:.75em;margin:1px;font-family:monospace;text-decoration:none}
    .dl{background:#002a14;color:#00c850}.dl:hover{background:#004020}
    .del{background:#3c0000;color:#ff4040}.del:hover{background:#600000}
    .up-box{margin-top:12px;border-top:1px solid #3c0000;padding-top:10px}
    .up-box h2{color:#dc0000;font-size:.9em;margin-bottom:6px}
    .up-dir{margin-bottom:6px;font-size:.8em;color:#888}
    .up-dir span{color:#ccc}
    input[type=file]{color:#ccc;font-size:.8em;display:block;margin-bottom:6px}
    .ubtn{background:#6e0000;color:#fff;border:none;padding:5px 14px;border-radius:3px;
          cursor:pointer;font-size:.85em;font-family:monospace}
    .ubtn:hover{background:#dc0000}
    .empty{color:#555;padding:10px;text-align:center}
  </style>
</head>
<body>
  <h1>&#9889; ZillaStick File Manager</h1>
  <div class="sub">SSID: ZILLASTICK_FILES &nbsp;&#183;&nbsp; 192.168.4.1</div>
  <div id="msg"></div>
  <div id="stat"></div>
  <div class="tabs">
    <div class="tab active" id="t-cap" onclick="loadDir('/captures')">Captures</div>
    <div class="tab"        id="t-pay" onclick="loadDir('/payloads')">Payloads</div>
    <div class="tab"        id="t-wif" onclick="loadDir('/wifi')">Creds</div>
  </div>
  <table>
    <thead><tr><th>File</th><th class="sz">Size</th><th class="ac">Actions</th></tr></thead>
    <tbody id="fb"><tr><td colspan="3" class="empty">Loading...</td></tr></tbody>
  </table>
  <div class="up-box">
    <h2>&#8593; Upload File</h2>
    <div class="up-dir">Destination: <span id="updir">/captures</span></div>
    <input type="file" id="fi" accept=".txt,.duck,.raw">
    <button class="ubtn" onclick="doUpload()">Upload</button>
  </div>
  <script>
    var cd='/captures';
    function setMsg(t,ok){var d=document.getElementById('msg');d.className=ok?'ok':'err';d.textContent=t;d.style.display='block';setTimeout(function(){d.style.display='none'},3500)}
    function fmtSz(n){if(n<1024)return n+'B';if(n<1048576)return(n/1024).toFixed(1)+'K';return(n/1048576).toFixed(2)+'M'}
    function loadDir(dir){
      cd=dir;
      document.getElementById('t-cap').className='tab'+(dir==='/captures'?' active':'');
      document.getElementById('t-pay').className='tab'+(dir==='/payloads'?' active':'');
      document.getElementById('t-wif').className='tab'+(dir==='/wifi'?' active':'');
      document.getElementById('updir').textContent=dir;
      fetch('/list?dir='+encodeURIComponent(dir))
        .then(function(r){return r.json()})
        .then(function(d){
          var tb=document.getElementById('fb');
          if(!d.files||!d.files.length){tb.innerHTML='<tr><td colspan="3" class="empty">No files.</td></tr>';return}
          tb.innerHTML=d.files.map(function(f){
            var n=f.path.split('/').pop();
            return'<tr><td>'+escH(n)+'</td><td class="sz">'+fmtSz(f.size)+'</td><td class="ac">'+
              '<a class="btn dl" href="/dl?f='+encodeURIComponent(f.path)+'" download="'+escH(n)+'">&#8595;DL</a> '+
              '<button class="btn del" onclick="doRm(\''+escJ(f.path)+'\')">DEL</button></td></tr>'
          }).join('');
          if(d.total){document.getElementById('stat').innerHTML='Storage: <span>'+fmtSz(d.used)+' / '+fmtSz(d.total)+'</span> ('+Math.round(d.used*100/d.total)+'% used)'}
        }).catch(function(){setMsg('Failed to load',false)})
    }
    function doRm(p){
      if(!confirm('Delete '+p+'?'))return;
      fetch('/rm?f='+encodeURIComponent(p))
        .then(function(r){return r.json()})
        .then(function(d){d.ok?setMsg('Deleted: '+p,true):setMsg('Delete failed',false);loadDir(cd)})
        .catch(function(){setMsg('Error',false)})
    }
    function doUpload(){
      var fi=document.getElementById('fi');
      if(!fi.files.length){setMsg('Select a file first',false);return}
      var fd=new FormData();fd.append('file',fi.files[0]);
      fetch('/ul?dir='+encodeURIComponent(cd),{method:'POST',body:fd})
        .then(function(r){return r.json()})
        .then(function(d){
          if(d.ok){setMsg('Uploaded: '+d.name,true);loadDir(cd);fi.value=''}
          else setMsg('Upload failed: '+(d.err||''),false)
        }).catch(function(){setMsg('Upload error',false)})
    }
    function escH(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
    function escJ(s){return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'")}
    loadDir('/captures');
  </script>
</body>
</html>
)rawliteral";

// ─── WiFi File Server entry point ─────────────────────────────
void file_wifi_server_run() {
    // ── Bring up SoftAP ──────────────────────────────────────
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(150);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(FS_AP_SSID, FS_AP_PASS);
    delay(200);
    IPAddress ip = WiFi.softAPIP();

    // ── Build server ──────────────────────────────────────────
    AsyncWebServer srv(FS_AP_PORT);

    // GET / — serve the HTML shell
    srv.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
        req->send_P(200, "text/html", FS_HTML);
    });

    // GET /list?dir=PATH — JSON file listing + storage stats
    srv.on("/list", HTTP_GET, [](AsyncWebServerRequest* req){
        String dir = CAPTURES_DIR;
        if (req->hasParam("dir")) {
            String d = req->getParam("dir")->value();
            if (d == CAPTURES_DIR || d == PAYLOAD_DIR || d == WIFI_DIR) dir = d;
        }

        String json = "{\"files\":[";
        bool first = true;
        File root = LittleFS.open(dir);
        if (root && root.isDirectory()) {
            File f = root.openNextFile();
            while (f) {
                if (!f.isDirectory()) {
                    if (!first) json += ",";
                    const char* raw = f.name();
                    String fullPath;
                    if (raw[0] == '/') fullPath = String(raw);
                    else               fullPath = dir + "/" + String(raw);
                    json += "{\"path\":\"" + fullPath + "\",\"size\":" + f.size() + "}";
                    first = false;
                }
                f = root.openNextFile();
            }
        }
        json += "],\"total\":" + String(LittleFS.totalBytes());
        json += ",\"used\":"  + String(LittleFS.usedBytes()) + "}";
        req->send(200, "application/json", json);
    });

    // GET /dl?f=PATH — download a file
    srv.on("/dl", HTTP_GET, [](AsyncWebServerRequest* req){
        if (!req->hasParam("f")) { req->send(400, "text/plain", "missing f"); return; }
        String path = req->getParam("f")->value();
        // Constrain to our two dirs
        if (!path.startsWith(CAPTURES_DIR) && !path.startsWith(PAYLOAD_DIR) && !path.startsWith(WIFI_DIR)) {
            req->send(403, "text/plain", "forbidden"); return;
        }
        if (!LittleFS.exists(path)) { req->send(404, "text/plain", "not found"); return; }
        req->send(LittleFS, path, "application/octet-stream", /*download=*/true);
    });

    // GET /rm?f=PATH — delete a file
    srv.on("/rm", HTTP_GET, [](AsyncWebServerRequest* req){
        if (!req->hasParam("f")) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing f\"}"); return; }
        String path = req->getParam("f")->value();
        if (!path.startsWith(CAPTURES_DIR) && !path.startsWith(PAYLOAD_DIR) && !path.startsWith(WIFI_DIR)) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}"); return;
        }
        bool ok = LittleFS.exists(path) && LittleFS.remove(path);
        Serial.printf("[FS] rm %s -> %s\n", path.c_str(), ok ? "ok" : "fail");
        req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"remove failed\"}");
    });

    // POST /ul?dir=PATH — multipart file upload
    // The request-complete callback fires after all chunks have been received.
    srv.on("/ul", HTTP_POST,
        // ① Called once the entire body has been received
        [](AsyncWebServerRequest* req){
            if (s_ul_ok && s_ul_path.length()) {
                String fname = s_ul_path.substring(s_ul_path.lastIndexOf('/') + 1);
                Serial.printf("[FS] upload ok -> %s\n", s_ul_path.c_str());
                req->send(200, "application/json",
                    "{\"ok\":true,\"name\":\"" + fname + "\"}");
            } else {
                req->send(500, "application/json",
                    "{\"ok\":false,\"err\":\"write failed\"}");
            }
            s_ul_ok = false;
            s_ul_path = "";
        },
        // ② Called once per chunk of the uploaded file body
        [](AsyncWebServerRequest* req,
           const String& filename,
           size_t index,
           uint8_t* data,
           size_t len,
           bool final)
        {
            if (index == 0) {
                // Determine destination directory
                String dir = PAYLOAD_DIR;
                if (req->hasParam("dir")) {
                    String d = req->getParam("dir")->value();
                    if (d == CAPTURES_DIR || d == PAYLOAD_DIR || d == WIFI_DIR) dir = d;
                }
                s_ul_path = dir + "/" + filename;
                s_ul_ok   = false;
                s_ul_fp   = LittleFS.open(s_ul_path, "w");
                if (!s_ul_fp) {
                    Serial.printf("[FS] upload open failed: %s\n", s_ul_path.c_str());
                    return;
                }
                Serial.printf("[FS] upload start -> %s\n", s_ul_path.c_str());
            }
            if (s_ul_fp && len > 0) {
                s_ul_fp.write(data, len);
            }
            if (final) {
                if (s_ul_fp) {
                    s_ul_fp.close();
                    s_ul_ok = true;
                }
            }
        }
    );

    // 404 handler
    srv.onNotFound([](AsyncWebServerRequest* req){
        req->send(404, "text/plain", "not found");
    });

    srv.begin();
    Serial.printf("[FS] Web server up: http://%d.%d.%d.%d/\n", ip[0], ip[1], ip[2], ip[3]);

    // ── Draw device UI ────────────────────────────────────────
    display_clear();
    draw_header("WiFi File Server");

    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 4);
    M5.Lcd.printf("SSID: %s", FS_AP_SSID);

    M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 18);
    M5.Lcd.printf("URL:  http://%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 32);
    M5.Lcd.print("Connect Wi-Fi then");
    M5.Lcd.setCursor(4, CONTENT_Y + 43);
    M5.Lcd.print("open URL in browser.");

    draw_divider(CONTENT_Y + 54);
    draw_footer("", "B:stop");

    uint32_t last_tick = 0;
    uint8_t  spinner   = 0;

    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) break;

        if (millis() - last_tick >= 1000) {
            last_tick = millis();

            // Clients connected
            uint8_t cl = WiFi.softAPgetStationNum();
            M5.Lcd.fillRect(0, CONTENT_Y + 57, DISP_W, 26, CLR_BG);
            M5.Lcd.setTextSize(1);
            if (cl > 0) {
                M5.Lcd.setTextColor(CLR_SUCCESS, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 58);
                M5.Lcd.printf("Client connected (%d)", (int)cl);
            } else {
                M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
                M5.Lcd.setCursor(4, CONTENT_Y + 58);
                M5.Lcd.print("Waiting for client...");
            }

            // Storage usage
            char u[10], t[10];
            _fmt_size(LittleFS.usedBytes(),  u, sizeof(u));
            _fmt_size(LittleFS.totalBytes(), t, sizeof(t));
            M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 70);
            M5.Lcd.printf("Storage: %s / %s", u, t);

            // Spinner top-right corner
            draw_spinner(DISP_W - 16, CONTENT_Y + 4, spinner++);
        }

        delay(10);
    }

    // ── Teardown ──────────────────────────────────────────────
    srv.end();
    if (s_ul_fp) s_ul_fp.close();
    s_ul_ok   = false;
    s_ul_path = "";

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[FS] Web server stopped.");
}

// ─── Entry point ──────────────────────────────────────────────
void file_manager_run() {
    while (true) {
        display_clear();
        draw_header("File Manager");
        _draw_storage_bar();
        draw_divider(CONTENT_Y + 22);

        Menu m("Files");
        m.add("Captures",    "C");
        m.add("Payloads",    "P");
        m.add("WiFi Server", "W");
        m.add("< Back",      " ");
        m.draw();

        bool handled = false;
        while (!handled) {
            M5.update();
            if (M5.BtnA.wasPressed()) { m.scroll_down(); }
            if (M5.BtnB.wasPressed()) {
                switch(m.selected()) {
                    case 0: _browse_dir(CAPTURES_DIR, "Captures");  handled = true; break;
                    case 1: _browse_dir(PAYLOAD_DIR,  "Payloads");  handled = true; break;
                    case 2: file_wifi_server_run();                  handled = true; break;
                    case 3: return;
                }
            }
            if (M5.BtnB.pressedFor(LONG_PRESS_MS)) return;
            delay(10);
        }
    }
}
