# ZILLASTICK S3 — Firmware Reference

**Hardware:** M5Stack StickC S3 (ESP32-S3) + Pingequa RF Pack S3 v1  
**Build system:** PlatformIO  
**Serial port:** `/dev/ttyACM0` (Linux/Kali)  
**By:** Hackazillarex

---

## 🌐 Web Flasher

Flash the firmware directly from your browser — no software to install.

**👉 [https://hackazillarex.github.io/zillastick-s3/](https://hackazillarex.github.io/zillastick-s3/)**

> Requires Chrome or Edge. Connect your M5StickC S3 via USB before clicking.

---

## ⚠️ Broken Screen? Use Headless Mode

If your screen is cracked or dead, you can control the device entirely from
a browser. Hold **BtnA** while powering on — the device skips the menu and
immediately starts the Remote Ctrl web panel.

**Connect to this network from your phone or laptop:**

| Setting | Value |
|---------|-------|
| **WiFi SSID** | `ZILLASTICK-CTRL` |
| **Password** | `zillastick1` |
| **Panel URL** | `http://192.168.6.1` |

Then open a browser and go to `192.168.6.1` — full control from any device,
no screen needed.

---

## Quick Build & Flash

```bash
# Build only
python3 -m platformio run

# Build + flash
python3 -m platformio run --target upload

# Upload SPIFFS filesystem (optional — payload defaults are seeded on first boot)
python3 -m platformio run --target uploadfs

# Serial monitor (115200 baud)
python3 -m platformio device monitor
```

> **Note:** `uploadfs` is no longer required for a working device. Default
> BadUSB payload files are written to SPIFFS automatically on the first boot
> after flashing. Run `uploadfs` only if you want to pre-load a custom set of
> payload files or WiFi credentials without using the File Manager.

---

## Button Layout

| Button | Short press | Long press (>700 ms) |
|--------|-------------|----------------------|
| **A** (top) | Scroll down in menu / toggle options | — |
| **B** (side) | Select / confirm | Back / stop / exit |

**Boot shortcut:** Hold **A** during power-on to boot straight into Remote Ctrl (headless mode).

---

## SPIFFS File Structure

All persistent data lives on the internal SPIFFS flash partition.

```
/
├── wifi/
│   └── creds.txt          ← WiFi passwords for Net Scanner (you create this)
├── captures/
│   ├── portal.txt         ← Evil Portal credential captures (auto-created)
│   ├── evil_twin.txt      ← Evil Twin credential captures (auto-created)
│   └── remote_et.txt      ← Evil Twin captures from headless Remote Ctrl (auto-created)
├── payloads/              ← BadUSB scripts (.txt or .duck) — seeded on first boot
│   ├── win_run.txt        ← seeded automatically
│   ├── win_cmd.txt        ← seeded automatically
│   ├── win_powershell.txt ← seeded automatically
│   ├── win_lock.txt       ← seeded automatically
│   ├── mac_terminal.txt   ← seeded automatically
│   └── your_script.txt    ← add your own
└── pq_config.json         ← Device config (auto-created on first run)
```

### BadUSB payload files

The five default payloads are embedded in the firmware and written to
`/payloads/` on the first boot after flashing. No `uploadfs` step is needed.

To add your own scripts:
- Use the **File Manager** module on-device to upload via the ZILLASTICK-CTRL AP, or
- Place `.txt` files in `data/payloads/` and run `python3 -m platformio run --target uploadfs`

Existing files are never overwritten by the first-boot seeding — your custom
scripts are always preserved across firmware updates.

### How to add WiFi credentials for Net Scanner

Create `/wifi/creds.txt` in your `data/wifi/` folder before running
`uploadfs`, or upload it via the device's File Manager module.

**Format — one network per line, colon-separated:**
```
HomeNetwork:mypassword123
CorpWifi:workpassword
GuestNet:guestpass99
```

- Lines starting with `#` are ignored (comments)
- SSID and password are case-sensitive
- No spaces around the colon
- If a network is open (no password), Net Scanner connects without a file entry

---

## Remote Ctrl (Headless Web Panel)

Boots a management WiFi AP and serves a mobile-friendly web control panel.
Designed for use when the device screen is broken or unavailable — full
control from any browser on the same network.

**AP credentials:**
| Setting | Value |
|---------|-------|
| SSID | `ZILLASTICK-CTRL` |
| Password | `zillastick1` |
| IP | `192.168.6.1` |

**To launch:**
- From the main menu → select **Remote Ctrl**
- Or hold **BtnA** at boot to skip the menu entirely (works with broken screens)

**To access the panel:**
1. Connect your phone or laptop to `ZILLASTICK-CTRL`
2. Open a browser and go to `192.168.6.1`

### Panel Tabs

#### WiFi Tab
- **Scan** — scans for nearby networks while the AP stays online (you are never kicked off)
- **Deauth** — launches a timed deauth attack against a selected target
- **Evil Twin** — clones the selected SSID, serves a captive portal, captures submitted passwords
- Captured credentials appear live in a table and are saved to `/captures/remote_et.txt`

> **Note:** Deauth and Evil Twin take the management AP offline while running.
> Reconnect to `ZILLASTICK-CTRL` when the attack finishes — the AP restarts
> automatically and the web page will load immediately on reconnect.

#### Sub-GHz Tab
- Live CC1101 RSSI reading at 433 MHz
- Lists capture files from `/captures/`
- Full replay/TX from headless coming in a future update

#### NRF24 Tab
- **Scan** — sweeps all 126 channels (0–125), displays a live power spectrum
- **Sniff** — passive packet capture on a selected channel, with selectable data rate
- **Jam Detect** — watches all channels for elevated power, alerts on warm/hot thresholds

> **Note:** While NRF24 operations are running, the Sub-GHz RSSI endpoint
> returns a `not ready` response. NRF24 and CC1101 share the SPI bus.

#### BadUSB Tab
Lists all `.txt` and `.duck` files from `/payloads/` on the device.
Select a file and tap **Run** — the script executes over USB HID while the
management AP stays online.

The target machine must be plugged into the device's USB port.

#### Files Tab
- Browses `/captures/` directory
- Select a file and tap View to read the contents inline in the browser

---

## WiFi Module

### Scan Networks
Passive scan of all channels. Populates the target list used by Deauth,
Evil Twin, and Net Scanner. Always run this first.

Displays: SSID · RSSI · Channel · Auth mode (OPEN / WPA2 / WPA2/3 etc.)

### Deauth Attack
Selects a target from the scan list and fires continuous deauthentication
and disassociation frames at the AP's BSSID.

- Uses `WIFI_IF_AP` (requires hidden softAP to be active) — this is the only
  interface that allows raw 802.11 frame injection on ESP32-S3
- TX power: 20.5 dBm (82 × 0.25, ESP32-S3 hardware max)
- 100 frames per burst (50 deauth + 50 disassoc), no inter-frame delay
- **WPA3 / WPA2+WPA3 targets**: a warning screen appears. PMF (802.11w)
  is mandatory on these networks and causes unprotected deauth frames to be
  silently discarded by clients. You can try anyway (option A) or pick a
  different target.

### Evil Portal
Spins up an open AP named **"Free WiFi"** and serves a fake login page.
When a victim submits a password, it is shown on-screen and appended to
`/captures/portal.txt`.

Captive-portal redirect hooks: `/generate_204` and `/hotspot-detect.html`
redirect Android and Apple devices automatically to the login page.

### Evil Twin
Combines a clone AP + captive portal in one flow.

1. Pick target from scan list
2. Device starts an **open** AP with the **exact same SSID** on the **same channel**
3. Displaced clients see two networks with the same name — one secured
   (real), one open (ours) — and may connect to ours
4. Connected clients hit the captive portal and are prompted for the WiFi
   password
5. Captured passwords appear on-screen and are saved to
   `/captures/evil_twin.txt` as `SSID:<name> PASS:<password>`

> **Tip:** Use an external deauth device to kick clients off the real AP
> while Evil Twin is running. The on-device deauth and Evil Twin AP conflict
> with each other — keep them separate.

**Note:** Modern OSes (iOS 14+, Android 11+, Windows 11) are increasingly
resistant to open-network roaming. Success rate is higher when the target
is already on a WPA2 network (not WPA3/PMF).

### Net Scanner
Connect to a WiFi network and scan it.

**Connection flow:**
- Open networks → connects directly
- Secured networks → looks up password in `/wifi/creds.txt`
- If no password found → shows the file path and format, then exits

**Sub-menu options once connected:**

| Option | What it does |
|--------|--------------|
| My Info | Shows IP, gateway, DNS, RSSI, SSID |
| Host Discovery | ICMP ping sweep of x.x.x.1–254 (150 ms timeout), with TCP fallback on 7 common ports for hosts that block ICMP. Saves results to `/captures/ns_XXXXXX.txt`. Progress bar while scanning. Results stored for Port Scan. |
| Port Scan | Pick gateway or any discovered host, then scans 18 common ports (300 ms each, ~5.5 s total). Shows open ports live. Appends results to the discovery file. |
| Disconnect | Clean disconnect + returns to menu |

**Ports scanned:** 21 FTP · 22 SSH · 23 Telnet · 25 SMTP · 53 DNS ·
80 HTTP · 110 POP3 · 135 RPC · 139 NetBIOS · 143 IMAP · 443 HTTPS ·
445 SMB · 3306 MySQL · 3389 RDP · 5000 UPnP · 8080 HTTP-alt ·
8443 HTTPS-alt · 9100 Print

**Tip:** Run Host Discovery first, then Port Scan — the Port Scan menu will
pre-populate with all discovered hosts so you don't have to guess IPs.

### Beacon Spam
Broadcasts fake AP beacons with rotating SSIDs and channels. Floods nearby
device WiFi scan lists.

### Probe Sniff
Passive listener for 802.11 probe request frames. Shows which SSIDs devices
are actively searching for (the networks they've previously connected to).
A: cycle through channels. B: stop.

### Deauth Detect
Passive monitor mode — counts incoming deauth (0xC0) and disassociation
(0xA0) management frames. Alerts if rate exceeds normal background noise,
indicating an active deauth attack on your network.

---

### HS Passive — WPA2 Handshake Capture (Passive)

Locks onto a target AP channel and waits for a client to naturally
authenticate (boot, roam, or reconnect). Captures the 4-way EAPOL handshake
without transmitting any frames.

**Usage:** Scan → WiFi → HS Passive → pick AP → wait → B to stop

Useful when stealth is required or when the target network has PMF enabled
(deauth won't work anyway — see note below).

### HS Active — WPA2 Handshake Capture (Deauth-Forced)

Sends a broadcast deauth/disassociation burst to knock all clients off the
target AP, then captures the EAPOL handshake as they reconnect. Retries up
to 3 times with a 20-second capture window each round.

**Usage:** Scan → WiFi → HS Active → pick AP → waits automatically

Both modes save captures as hc22000 lines — the native format for
hashcat `-m 22000`. Each capture overwrites the previous file for the same
SSID/mode so you always get a clean, valid file.

**Output:** `/captures/hs_<ssid8>_p.22000` (passive) or `_a.22000` (active)

**Crack offline:**
```bash
hashcat -m 22000 hs_MyNetwork_a.22000 wordlist.txt
# or with rules:
hashcat -m 22000 hs_MyNetwork_a.22000 wordlist.txt -r rules/best64.rule
```

> **hc22000 format note:** Captures use msgpair `02` in the final field, which
> is required by hashcat ≥ 6.x. If you ever manually edit a `.22000` file, make
> sure the last field is `*02` (two characters), not `*2` or `*0` — both cause a
> "Salt-length exception" and hashcat will load zero hashes.

Transfer the `.22000` file off-device via the Files module or Remote Ctrl
web panel, then run hashcat on a machine with a GPU.

> **Note on WPA3 / PMF:** Networks advertising WPA3 or WPA2 with 802.11w
> (Protected Management Frames) will silently drop deauth frames — HS Active
> won't force a reconnect on those. Use HS Passive and wait for natural
> client churn, or choose a WPA2-only target.

---

## BLE Module

### BLE Scan
Active BLE scan. Shows device MAC, name (if advertised), and RSSI.

### BLE Spam
Floods the 2.4 GHz BLE advertising channels with spoofed device announcements.

| Mode | Effect |
|------|--------|
| Apple Nearby | Triggers "New AirPods / iPhone found" popup on iOS |
| Samsung | Samsung pairing prompts on Android |
| Windows Swift Pair | Windows 10/11 pairing notifications |
| All (cycle) | Rotates through all three continuously |

Randomizes payload bytes each iteration to appear as different devices.

---

## Sub-GHz RF Module (CC1101)

**Frequency range:** 433 MHz (Pingequa RF Pack S3 v1 hardware limitation)  
**TX power:** +10 dBm (CC1101 hardware maximum, set on every init)  
**Default frequency:** 433.92 MHz

### Scan
Sweeps for signals at the configured frequency. Displays signal strength and
raw sample data. Press A to capture.

### Replay Last / Replay File
Retransmits a previously captured signal. "Replay Last" uses the in-memory
capture buffer. "Replay File" lets you pick a saved capture from SPIFFS.

### Presets
Quick-select common frequencies: Garage 433, Car Keyfob 433,
Weather Sensor 433, Gate 433, Custom.

### Config
Set frequency (MHz) and modulation. Changes persist to `pq_config.json`.

### Jam Detect
Passive detection of Sub-GHz jamming activity on 315/433/868/915 MHz.
Useful for identifying if a jammer is blocking your own RF captures.

---

## nRF24L01+ Module

**Frequency:** 2.4 GHz ISM band  
**TX power:** RF24_PA_MAX (set at init)  
**Data rate:** 2 Mbps (scan), 1 Mbps (sniff start, then auto-hop)

Provides 2.4 GHz channel scanning, passive sniffing, and jam detection.
Useful for detecting active nRF24 devices, jammers, MouseJack-vulnerable
dongles, and other 2.4 GHz ISM traffic.

### Channel Scan
Sweeps all 126 channels and measures power level on each. Displayed as a
bar graph on-device. Also available from the Remote Ctrl NRF24 tab with a
live canvas spectrum.

### Sniffer
Passive promiscuous capture using a 2-byte broadcast address `{0xAA, 0xAA}`
with CRC disabled. Captures packets on a single channel or auto-hops.
Results shown on-device and streamed to the Remote Ctrl NRF24 → Sniff tab.

### MouseJack
Targets ESB (Enhanced ShockBurst) wireless keyboard/mouse dongles that lack
proper address filtering. Attempts HID injection over the nRF24 link using
a known target address.

### Jam Detect
Samples every channel × 2 with 150 µs dwell per poll. Reports channels
with elevated power and triggers a visual alarm at configurable thresholds.
Available headless from the Remote Ctrl NRF24 → Jam Detect tab.

> **SPI note:** nRF24 and CC1101 share the SPI bus. Only one can be active
> at a time. The firmware de-inits whichever module is idle before switching.

---

## Bad USB Module

Injects keystrokes into a connected host over USB HID. The device presents
as a USB keyboard (`ZILLASTICK S3` HID device, VID 0x239A / PID 0x800B).

### Payload Files

All payloads live in `/payloads/` as plain text `.txt` or `.duck` files.
The following defaults are written automatically on first boot:

| File | Action |
|------|--------|
| `win_run.txt` | `GUI r` — opens Windows Run dialog |
| `win_cmd.txt` | `GUI r` → `cmd` → `ENTER` |
| `win_powershell.txt` | `GUI r` → `powershell` → `ENTER` |
| `win_lock.txt` | `GUI l` — locks the Windows screen |
| `mac_terminal.txt` | `GUI SPACE` → `Terminal` → `ENTER` |

Add your own `.txt` files to `/payloads/` via the **File Manager** module or
`uploadfs`. They appear in both the on-device menu and the Remote Ctrl
BadUSB tab immediately.

### Script Syntax

```
REM This is a comment
DELAY 500
GUI r
DELAY 600
STRING notepad
ENTER
DELAY 800
STRING Hello from ZILLASTICK S3
ENTER
```

**Supported commands:** `STRING`, `ENTER`, `DELAY`, `GUI`, `CTRL`, `ALT`,
`SHIFT`, `CTRL-ALT`, `CTRL-SHIFT`, `REM`

---

## Pin Mapping (Pingequa RF Pack S3 v1)

| Signal | Pin |
|--------|-----|
| SPI SCK | 5 |
| SPI MISO | 4 |
| SPI MOSI | 6 |
| CC1101 CS | 2 |
| CC1101 GDO0 | 3 |
| CC1101 GDO2 | not connected |
| nRF24 CE | 1 |
| nRF24 CS | 8 |

---

## Known Behaviors & Notes

**Deauth on WPA3:** PMF (802.11w) is mandatory on WPA3 and WPA2/WPA3
transitional networks. The firmware warns you and lets you proceed, but
unprotected deauth frames are discarded by the client at hardware level.
No workaround without cracking the PMF nonce exchange (requires PMKID/SAE).

**Evil Twin deauth:** Running the on-device deauth burst while the Evil Twin
AP is active disrupts the AP's beacon engine and knocks it offline. Use a
separate external device to handle deauth while the ZILLASTICK S3 runs the
Evil Twin AP. The screen shows a reminder tip.

**WIFI_IF_AP vs WIFI_IF_STA:** Raw 802.11 frame injection via
`esp_wifi_80211_tx()` only works reliably on `WIFI_IF_AP` when the STA
interface is unassociated. The firmware brings up a hidden softAP before
injecting frames — this is intentional and required.

**Remote Ctrl scan stays connected:** The headless scan uses `WIFI_AP_STA`
mode so the management AP beacon never stops during a scan. Your phone stays
connected throughout.

**Remote Ctrl web page after attack:** Deauth and Evil Twin temporarily take
the management AP offline. After the attack finishes, the AP restarts with a
fresh TCP socket. Navigate back to `192.168.6.1` and the page will load
immediately — no need to re-enter the password if your device remembered it.

**CC1101 PATABLE reset:** Every `rf_init()` call resets the CC1101 via
SRES strobe, which wipes the PATABLE back to chip default (~0 dBm).
The firmware calls `setPA(10)` after every `setMHZ()` to restore +10 dBm.
If you ever call `ELECHOUSE_cc1101.Init()` manually, add `setPA(10)` after.

**SPI bus sharing:** CC1101 and nRF24 share the same SPI bus. Only one can
be active at a time. The firmware de-inits whichever module is active before
switching. Do not call both `rf_init()` and nRF24 init without a deinit in
between. While a NRF24 task is running, the Sub-GHz RSSI endpoint returns
`not ready`.

**BadUSB payload seeding:** Default payload files are written to `/payloads/`
on the first boot after flashing. The firmware never overwrites existing
files — user-added or edited scripts survive firmware updates. You must
still manually create `/wifi/creds.txt` (via `uploadfs` or File Manager)
before Net Scanner can connect to secured networks.

---

## Capture File Locations

| Module | File | Format |
|--------|------|--------|
| Evil Portal | `/captures/portal.txt` | One password per line |
| Evil Twin (on-device) | `/captures/evil_twin.txt` | `SSID:<name> PASS:<password>` |
| Evil Twin (Remote Ctrl) | `/captures/remote_et.txt` | `SSID:<name> PASS:<password>` |
| HS Passive | `/captures/hs_<ssid8>_p.22000` | hc22000 — `hashcat -m 22000` |
| HS Active  | `/captures/hs_<ssid8>_a.22000` | hc22000 — `hashcat -m 22000` |
| Net Scanner | `/captures/ns_<millis>.txt` | Plain text — hosts + open ports |

Read captures via the File Manager module, the Remote Ctrl Files tab,
or by connecting to serial and using the file browser.
