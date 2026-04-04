// ============================================================
// Wireless UART Bridge - ESP32-C3 SuperMini
// ============================================================
// Features:
// - UART <-> TCP bridge on port 23
// - Password authentication
// - OTA firmware updates via web browser
// - Web config panel on port 80
// - WiFi auto-reconnect, AP fallback
// - mDNS (default: uart.local)
// - Static IP config
// - Session recording to flash
// - Web terminal with ANSI color
// - Theme switcher (green/dark purple/light purple)
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <driver/gpio.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>

struct ThemeColors;

// ---- Configuration (defaults, changeable via web panel) ----
String wifi_ssid = "YourWiFi";
String wifi_password = "YourPassword";
String bridge_password = "root";
String web_password = "esp32";
int uart_baud = 115200;
int tcp_port = 23;
int uart_delay = 10;
String static_ip = "";
String static_gw = "192.168.1.1";
String static_subnet = "255.255.255.0";
String mdns_name = "uart";
int theme = 0; // 0=green cyberpunk, 1=dark purple, 2=light purple

const int UART_RX_PIN = 20;
const int UART_TX_PIN = 21;

// ---- Globals ----
WiFiServer *bridgeServer;
WiFiClient bridgeClient;
WebServer webServer(80);
Preferences prefs;
bool authenticated = false;
bool uart_active = false;
unsigned long boot_time = 0;
unsigned long bytes_rx = 0;
unsigned long bytes_tx = 0;
bool client_ever_connected = false;
bool ap_mode = false;
String ap_ssid = "";
String ap_password = "Password123";
bool first_run = false;

// ---- Session Recording ----
bool recording = false;
File recFile;
String recFilename = "";
unsigned long lastRecFlush = 0;

// ---- UART Log Ring Buffer ----
const int LOG_BUF_SIZE = 90 * 1024;
char logBuf[LOG_BUF_SIZE];
int logHead = 0;
unsigned long long logSeq = 0;

void logWrite(const uint8_t *data, int len) {
  for (int i = 0; i < len; i++) {
    logBuf[logHead] = data[i];
    logHead = (logHead + 1) % LOG_BUF_SIZE;
  }
  logSeq += len;
  if (recording && recFile) {
    recFile.write(data, len);
  }
}

// ---- AP SSID from chip MAC ----
String getDefaultHostname() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "ESP32C3-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

// ---- Theme Colors ----
struct ThemeColors {
  const char* ac; const char* ar; const char* a2;
  const char* bg; const char* bx; const char* ix;
  const char* tx; const char* lb; const char* l2; const char* l3; const char* dt;
  const char* tb; const char* go; const char* hc;
};

static const ThemeColors themeTable[] = {
  // 0: Green Cyberpunk
  {"#00ffc8","0,255,200","0,80,255","#060a0e","rgba(8,16,24,0.75)","rgba(0,12,24,0.6)",
   "#c0d8e0","#4a6a7a","#5a7a8a","#8aa0a8","#607a88","rgba(0,4,8,0.85)","0.03","#fff"},
  // 1: Dark Purple
  {"#b48eff","180,142,255","100,40,200","#0a060e","rgba(16,8,24,0.75)","rgba(12,0,24,0.6)",
   "#d0c0e0","#6a4a8a","#7a5a9a","#a088b8","#806a98","rgba(8,0,12,0.85)","0.03","#fff"},
  // 2: Light Purple
  {"#7c3aed","124,58,237","100,60,220","#f8f5ff","rgba(255,252,255,0.85)","rgba(240,235,255,0.9)",
   "#2d1b4e","#6b5a8a","#7a6a9a","#5a4a7a","#5a4a6a","rgba(240,235,255,0.92)","0.06","#4c1d95"}
};

const ThemeColors& T() { return themeTable[constrain(theme, 0, 2)]; }

// ---- Shared HTML Helpers ----
String getHead(const char* title) {
  const ThemeColors& t = T();
  String h;
  h.reserve(5500);
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>";
  h += title;
  h += "</title><style>";
  char rv[400];
  snprintf(rv, sizeof(rv),
    ":root{--ac:%s;--ar:%s;--a2:%s;--bg:%s;--bx:%s;--ix:%s;--tx:%s;--lb:%s;--l2:%s;--l3:%s;--dt:%s;--tb:%s;--go:%s;--hc:%s}",
    t.ac, t.ar, t.a2, t.bg, t.bx, t.ix, t.tx, t.lb, t.l2, t.l3, t.dt, t.tb, t.go, t.hc);
  h += rv;
  h += "*{box-sizing:border-box;margin:0}"
    "body{background:var(--bg);color:var(--tx);font-family:'Courier New',monospace;min-height:100vh;overflow-x:hidden}"
    "@keyframes gridDrift{0%{transform:translate(0,0)}100%{transform:translate(40px,40px)}}"
    "@keyframes blobMove{0%{transform:translate(0,0) scale(1)}33%{transform:translate(4vw,6vh) scale(1.1)}"
    "66%{transform:translate(-3vw,3vh) scale(.95)}100%{transform:translate(0,0) scale(1)}}"
    "@keyframes blobMove2{0%{transform:translate(0,0) scale(1)}33%{transform:translate(-5vw,-4vh) scale(.9)}"
    "66%{transform:translate(3vw,-6vh) scale(1.08)}100%{transform:translate(0,0) scale(1)}}"
    "@keyframes floatA{0%{transform:translate(0,0)}25%{transform:translate(60px,-80px)}"
    "50%{transform:translate(-40px,-160px)}75%{transform:translate(80px,-60px)}100%{transform:translate(0,0)}}"
    "@keyframes floatB{0%{transform:translate(0,0)}25%{transform:translate(-70px,50px)}"
    "50%{transform:translate(50px,120px)}75%{transform:translate(-30px,70px)}100%{transform:translate(0,0)}}"
    "@keyframes floatC{0%{transform:translate(0,0)}25%{transform:translate(40px,90px)}"
    "50%{transform:translate(-80px,30px)}75%{transform:translate(-20px,-50px)}100%{transform:translate(0,0)}}"
    "@keyframes scanline{0%{transform:translateY(-100%)}100%{transform:translateY(calc(100% + 100px))}}"
    "@keyframes borderGlow{0%,100%{border-color:rgba(var(--ar),0.1);"
    "box-shadow:0 0 15px rgba(var(--ar),0.03),inset 0 1px 0 rgba(var(--ar),0.05)}"
    "50%{border-color:rgba(var(--ar),0.24);"
    "box-shadow:0 0 25px rgba(var(--ar),0.08),inset 0 1px 0 rgba(var(--ar),0.1)}}"
    "@keyframes lineGlow{0%,100%{opacity:.3}50%{opacity:.8}}"
    "body::before{content:'';position:fixed;inset:-40px;pointer-events:none;z-index:0;"
    "background:"
    "repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(var(--ar),var(--go)) 39px,rgba(var(--ar),var(--go)) 40px),"
    "repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(var(--ar),var(--go)) 39px,rgba(var(--ar),var(--go)) 40px);"
    "animation:gridDrift 20s linear infinite}"
    "#bgBlobs{position:fixed;inset:0;pointer-events:none;z-index:0;overflow:hidden}"
    "#bgBlobs i{position:absolute;border-radius:50%;filter:blur(60px)}"
    "#bgBlobs i:nth-child(1){width:50vw;height:50vw;top:-10%;left:-5%;"
    "background:radial-gradient(circle,rgba(var(--ar),0.09),transparent 70%);"
    "animation:blobMove 25s ease-in-out infinite}"
    "#bgBlobs i:nth-child(2){width:40vw;height:40vw;bottom:-10%;right:-5%;"
    "background:radial-gradient(circle,rgba(var(--a2),0.07),transparent 70%);"
    "animation:blobMove2 30s ease-in-out infinite}"
    "#dots{position:fixed;inset:0;pointer-events:none;z-index:1;overflow:hidden}"
    "#dots i{position:absolute;width:2px;height:2px;border-radius:50%;"
    "background:rgba(var(--ar),0.5);opacity:.6}"
    "#dots i:nth-child(1){top:15%;left:10%;animation:floatA 18s ease-in-out infinite}"
    "#dots i:nth-child(2){top:70%;left:80%;animation:floatB 22s ease-in-out infinite;opacity:.4}"
    "#dots i:nth-child(3){top:40%;left:60%;animation:floatC 26s ease-in-out infinite;width:3px;height:3px;opacity:.3}"
    "#dots i:nth-child(4){top:80%;left:25%;animation:floatA 30s ease-in-out infinite reverse;opacity:.35}"
    "#dots i:nth-child(5){top:20%;left:75%;animation:floatB 20s ease-in-out infinite reverse;opacity:.45}"
    "#dots i:nth-child(6){top:55%;left:40%;animation:floatC 24s ease-in-out infinite;opacity:.25;width:2px;height:2px;"
    "box-shadow:0 0 6px rgba(var(--ar),0.4)}"
    "body::after{content:'';position:fixed;top:0;left:0;right:0;height:1px;"
    "background:linear-gradient(90deg,transparent 10%,rgba(var(--ar),0.5) 50%,transparent 90%);"
    "animation:lineGlow 3s ease-in-out infinite;pointer-events:none;z-index:10}"
    ".wrap{max-width:720px;margin:0 auto;padding:24px;position:relative;z-index:2}"
    "h1{color:var(--ac);font-size:1.3em;text-shadow:0 0 20px rgba(var(--ar),0.4);"
    "padding-bottom:14px;margin-bottom:20px;border:none;position:relative}"
    "h1::after{content:'';position:absolute;bottom:0;left:0;right:0;height:1px;"
    "background:linear-gradient(90deg,rgba(var(--ar),0.4),rgba(var(--ar),0.1),transparent)}"
    ".nav{display:flex;gap:8px;margin-bottom:22px;flex-wrap:wrap}"
    ".nav a{color:var(--ac);text-decoration:none;padding:7px 15px;font-size:.85em;"
    "border:1px solid rgba(var(--ar),0.12);border-radius:6px;background:rgba(var(--ar),0.03);"
    "transition:all .3s ease}"
    ".nav a:hover{background:rgba(var(--ar),0.1);box-shadow:0 0 15px rgba(var(--ar),0.2),"
    "inset 0 0 15px rgba(var(--ar),0.05);border-color:rgba(var(--ar),0.35);color:var(--hc)}"
    ".box{background:var(--bx);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);"
    "border:1px solid rgba(var(--ar),0.1);border-radius:12px;padding:20px;margin-bottom:18px;"
    "position:relative;overflow:hidden;z-index:2;"
    "animation:borderGlow 4s ease-in-out infinite}"
    ".box:nth-child(odd){animation-duration:4.4s}"
    ".box:nth-child(3n){animation-duration:3.6s}"
    ".box:nth-child(4n){animation-duration:5s}"
    ".box::before{content:'';position:absolute;left:0;right:0;height:40px;pointer-events:none;"
    "background:linear-gradient(180deg,rgba(var(--ar),0.04),transparent);opacity:.7;"
    "animation:scanline 6s linear infinite}"
    ".box:nth-child(odd)::before{animation-duration:7s;animation-delay:-.5s}"
    ".box:nth-child(3n)::before{animation-duration:8s;animation-delay:-2s}"
    ".stat{display:flex;justify-content:space-between;padding:9px 0;border-bottom:1px solid rgba(var(--ar),0.05)}"
    ".stat:last-child{border-bottom:none}"
    ".label{color:var(--lb)}.val{color:var(--ac)}"
    "input,select{background:var(--ix);color:var(--ac);border:1px solid rgba(var(--ar),0.12);"
    "border-radius:6px;padding:10px 12px;font-family:inherit;font-size:.9em;width:100%;"
    "transition:all .3s ease;outline:none}"
    "input:focus,select:focus{border-color:rgba(var(--ar),0.45);box-shadow:0 0 15px rgba(var(--ar),0.1)}"
    "button{background:linear-gradient(135deg,rgba(var(--ar),0.1),rgba(var(--ar),0.03));"
    "color:var(--ac);border:1px solid rgba(var(--ar),0.2);border-radius:6px;padding:10px 22px;"
    "cursor:pointer;font-family:inherit;font-weight:bold;font-size:.9em;transition:all .3s ease;"
    "position:relative;z-index:2}"
    "button:hover{background:linear-gradient(135deg,rgba(var(--ar),0.2),rgba(var(--ar),0.07));"
    "box-shadow:0 0 25px rgba(var(--ar),0.25),inset 0 0 10px rgba(var(--ar),0.05);"
    "border-color:rgba(var(--ar),0.5);transform:translateY(-1px)}"
    "button:active{transform:translateY(0);transition:transform .1s}"
    ".danger{background:linear-gradient(135deg,rgba(255,60,60,0.1),rgba(255,60,60,0.03));"
    "color:#ff6b6b;border-color:rgba(255,60,60,0.2)}"
    ".danger:hover{background:linear-gradient(135deg,rgba(255,60,60,0.2),rgba(255,60,60,0.07));"
    "box-shadow:0 0 25px rgba(255,60,60,0.25);border-color:rgba(255,60,60,0.5)}"
    "label{color:var(--l2);display:block;margin-top:12px;font-size:.85em}"
    "a{color:var(--ac);text-decoration:none;transition:color .3s}a:hover{color:var(--hc)}"
    "p{color:var(--dt);line-height:1.6;margin:8px 0}"
    "</style></head><body>"
    "<div id='bgBlobs'><i></i><i></i></div>"
    "<div id='dots'><i></i><i></i><i></i><i></i><i></i><i></i></div>";
  return h;
}

String getNav() {
  return "<div class='nav'>"
    "<a href='/panel'>Status</a>"
    "<a href='/terminal'>Terminal</a>"
    "<a href='/logs'>Logs</a>"
    "<a href='/config'>Config</a>"
    "<a href='/update'>OTA</a>"
    "</div>";
}

// ---- HTML Templates ----
String getStatusPage(size_t freeHeap) {
  String html;
  html.reserve(8000);
  html += getHead("UART Bridge");
  html += "<div class='wrap'><h1>UART Bridge</h1>";
  html += getNav();

  html += "<div class='box'>";
  html += "<div class='stat'><span class='label'>WiFi Mode</span><span class='val'>" + String(ap_mode ? "AP (" + ap_ssid + ")" : "STA") + "</span></div>";
  html += "<div class='stat'><span class='label'>WiFi SSID</span><span class='val'>" + (ap_mode ? ap_ssid : wifi_ssid) + "</span></div>";
  html += "<div class='stat'><span class='label'>IP Address</span><span class='val'>" + (ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</span></div>";
  if (!ap_mode) {
    html += "<div class='stat'><span class='label'>Signal</span><span class='val'>" + String(WiFi.RSSI()) + " dBm</span></div>";
    html += "<div class='stat'><span class='label'>mDNS</span><span class='val'>" + mdns_name + ".local</span></div>";
  }
  html += "<div class='stat'><span class='label'>Uptime</span><span class='val'>" + String(millis() / 1000) + "s</span></div>";
  html += "<div class='stat'><span class='label'>Free RAM</span><span class='val'>" + String(freeHeap) + " bytes</span></div>";
  html += "<div class='stat'><span class='label'>CPU Temp</span><span class='val'>" + String(temperatureRead(), 1) + " &deg;C</span></div>";
  html += "</div>";

  html += "<div class='box'>";
  html += "<div class='stat'><span class='label'>UART Baud</span><span class='val'>" + String(uart_baud) + "</span></div>";
  html += "<div class='stat'><span class='label'>UART Status</span><span class='val'>" + String(uart_active ? "Active" : "Waiting") + "</span></div>";
  html += "<div class='stat'><span class='label'>TCP Port</span><span class='val'>" + String(tcp_port) + "</span></div>";
  html += "<div class='stat'><span class='label'>Client</span><span class='val'>" + String(bridgeClient && bridgeClient.connected() ? "Connected" : "None") + "</span></div>";
  html += "<div class='stat'><span class='label'>Bytes RX</span><span class='val'>" + String(bytes_rx) + "</span></div>";
  html += "<div class='stat'><span class='label'>Bytes TX</span><span class='val'>" + String(bytes_tx) + "</span></div>";
  html += "<div class='stat'><span class='label'>Recording</span><span class='val'>" + String(recording ? recFilename : "Off") + "</span></div>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

String getConfigPage() {
  String html = getHead("UART Bridge - Config");
  html += "<div class='wrap'><h1>Configuration</h1>";
  html += getNav();

  // Theme selector
  html += "<div class='box'>";
  html += "<label style='margin-top:0'>Theme</label>";
  html += "<div style='display:flex;gap:8px;margin-top:8px'>";
  const char* tNames[] = {"Green Cyber", "Dark Purple", "Light Purple"};
  for (int i = 0; i < 3; i++) {
    html += "<button onclick=\"setTheme(" + String(i) + ")\" style='flex:1";
    if (theme == i) html += ";background:rgba(var(--ar),0.25);border-color:var(--ac);box-shadow:0 0 15px rgba(var(--ar),0.15)";
    html += "'>";
    html += tNames[i];
    html += "</button>";
  }
  html += "</div>";
  html += "<script>function setTheme(t){fetch('/api/theme',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'t='+t}).then(()=>location.reload());}</script>";
  html += "</div>";

  // Single form spanning both config boxes
  html += "<form method='POST' action='/save'>";

  html += "<div class='box'>";
  html += "<label style='margin-top:0'>WiFi SSID</label><input name='ssid' value='" + wifi_ssid + "'>";
  html += "<label>WiFi Password</label><input name='wifipass' type='password' value='" + wifi_password + "'>";
  html += "<label>Bridge Password (TCP port 23)</label><input name='brpass' type='password' value='" + bridge_password + "'>";
  html += "<label>Web Panel Password</label><input name='webpass' type='password' value='" + web_password + "'>";
  html += "<label>UART Baud Rate</label><select name='baud'>";
  int bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  for (int i = 0; i < 8; i++) {
    html += "<option value='" + String(bauds[i]) + "'";
    if (bauds[i] == uart_baud) html += " selected";
    html += ">" + String(bauds[i]) + "</option>";
  }
  html += "</select>";
  html += "<label>TCP Port</label><input name='port' type='number' value='" + String(tcp_port) + "'>";
  html += "<label>UART Boot Delay (seconds, 0 = no delay)</label><input name='delay' type='number' value='" + String(uart_delay) + "'>";
  html += "<label>AP Fallback Password</label><input name='appass' type='password' value='" + ap_password + "'>";
  html += "</div>";

  html += "<div class='box'>";
  html += "<label style='margin-top:0'>Static IP (leave empty for DHCP)</label><input name='sip' value='" + static_ip + "'>";
  html += "<label>Gateway</label><input name='sgw' value='" + static_gw + "'>";
  html += "<label>Subnet Mask</label><input name='ssn' value='" + static_subnet + "'>";
  html += "<label>mDNS Hostname (.local)</label><input name='mdns' value='" + mdns_name + "'>";
  html += "<br><br><button type='submit'>Save &amp; Reboot</button>";
  html += "</div>";

  html += "</form>";

  html += "<div class='box'>";
  html += "<form method='POST' action='/reboot'>";
  html += "<button class='danger' type='submit'>Reboot ESP32</button>";
  html += "</form></div>";

  html += "<div class='box'>";
  html += "<button class='danger' style='width:100%' onclick=\"if(confirm('Factory reset will erase ALL settings (WiFi, passwords, theme) and reboot into the setup wizard. Continue?'))fetch('/factory-reset',{method:'POST'}).then(()=>document.body.innerHTML='<p style=\\'text-align:center;margin-top:40vh;color:var(--ac)\\'>Resetting...</p>')\">Factory Reset</button>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

String getUpdatePage() {
  String html = getHead("UART Bridge - OTA");
  html += "<div class='wrap'><h1>OTA Firmware Update</h1>";
  html += getNav();

  html += "<div class='box'>";
  html += "<p style='color:var(--l3)'>Upload a compiled .bin firmware file:</p>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin' style='margin:8px 0'><br><br>";
  html += "<button type='submit'>Upload &amp; Flash</button>";
  html += "</form></div>";

  html += "<div class='box'>";
  html += "<p style='color:var(--l3)'>To export firmware from Arduino IDE:</p>";
  html += "<p>Sketch &rarr; Export Compiled Binary &rarr; upload the .bin file here</p>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

String getLogsPage() {
  String html = getHead("UART Bridge - Logs");
  html += "<div class='wrap'><h1>Session Logs</h1>";
  html += getNav();

  File root = LittleFS.open("/");
  File f = root.openNextFile();
  int count = 0;

  while (f) {
    String fname = f.name();
    if (fname.startsWith("/")) fname = fname.substring(1);
    if (!f.isDirectory() && fname.startsWith("rec_")) {
      html += "<div class='box' style='display:flex;justify-content:space-between;align-items:center;padding:14px 20px'>";
      html += "<div><span class='val'>" + fname + "</span><br>";
      html += "<span class='label'>" + String(f.size()) + " bytes</span></div>";
      html += "<div style='display:flex;gap:8px;align-items:center'>";
      html += "<a href='/logs/dl?f=" + fname + "' style='padding:6px 14px;border:1px solid rgba(var(--ar),0.2);border-radius:6px;font-size:.8em'>Raw</a>";
      html += "<a href='/logs/dl?f=" + fname + "&clean=1' style='padding:6px 14px;border:1px solid rgba(var(--ar),0.2);border-radius:6px;font-size:.8em'>Clean</a>";
      html += "<button class='danger' style='padding:6px 14px;font-size:.8em' onclick=\"fetch('/logs/rm',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'f=" + fname + "'}).then(()=>location.reload())\">Delete</button>";
      html += "</div></div>";
      count++;
    }
    f = root.openNextFile();
  }

  if (count == 0) {
    html += "<div class='box'><p style='color:var(--l2);text-align:center'>No recordings yet. Start recording from the Terminal page.</p></div>";
  }

  html += "<div class='box'>";
  html += "<div class='stat'><span class='label'>Flash Used</span><span class='val'>" + String(LittleFS.usedBytes()) + " bytes</span></div>";
  html += "<div class='stat'><span class='label'>Flash Total</span><span class='val'>" + String(LittleFS.totalBytes()) + " bytes</span></div>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

String getLoginPage() {
  String html = getHead("UART Bridge - Login");
  html += "<div class='wrap' style='display:flex;align-items:center;justify-content:center;min-height:80vh'>";
  html += "<div class='box' style='max-width:380px;width:100%;text-align:center'>";
  html += "<h1 style='font-size:1.1em;margin-bottom:20px'>UART Bridge</h1>";
  html += "<p style='margin-bottom:16px'>Enter password to continue</p>";
  html += "<input id='pass' type='password' placeholder='Password' autofocus style='margin-bottom:12px'>";
  html += "<button onclick='doLogin()' style='width:100%'>Login</button>";
  html += "<p id='err' style='color:#ff6b6b;margin-top:10px;display:none'>Wrong password</p>";
  html += "</div></div>";
  html += "<script>"
    "document.getElementById('pass').addEventListener('keydown',function(e){"
    "if(e.key==='Enter')doLogin();});"
    "document.getElementById('pass').addEventListener('input',function(){"
    "document.getElementById('err').style.display='none';});"
    "function doLogin(){"
    "var p=document.getElementById('pass').value;"
    "fetch('/auth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'pass='+encodeURIComponent(p)})"
    ".then(r=>r.json()).then(d=>{"
    "if(d.ok){document.cookie='tok='+d.tok+';path=/';location.href='/panel';}"
    "else{document.getElementById('err').style.display='block';"
    "document.getElementById('pass').value='';document.getElementById('pass').focus();}"
    "});"
    "}"
    "</script></body></html>";
  return html;
}

String getSetupPage() {
  String html = getHead("UART Bridge - Setup");
  html += "<div class='wrap' style='display:flex;align-items:center;justify-content:center;min-height:90vh'>";
  html += "<div style='max-width:480px;width:100%'>";
  html += "<h1 style='text-align:center'>Initial Setup</h1>";
  html += "<p style='text-align:center;margin-bottom:20px'>Configure your UART bridge for the first time.</p>";
  html += "<form method='POST' action='/setup'>";
  html += "<div class='box'>";
  html += "<label style='margin-top:0'>WiFi SSID</label><input name='ssid' required>";
  html += "<label>WiFi Password</label><input name='wifipass' type='password' required>";
  html += "</div>";
  html += "<div class='box'>";
  html += "<label style='margin-top:0'>Bridge Password (TCP port 23)</label><input name='brpass' value='root'>";
  html += "<label>Web Panel Password</label><input name='webpass' type='password' value='esp32'>";
  html += "<label>UART Baud Rate</label><select name='baud'>";
  int bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  for (int i = 0; i < 8; i++) {
    html += "<option value='" + String(bauds[i]) + "'";
    if (bauds[i] == 115200) html += " selected";
    html += ">" + String(bauds[i]) + "</option>";
  }
  html += "</select>";
  html += "</div>";
  html += "<div class='box' style='text-align:center'>";
  html += "<button type='submit' style='width:100%'>Save &amp; Connect</button>";
  html += "</div>";
  html += "</form>";
  html += "</div></div></body></html>";
  return html;
}

String getTerminalPage() {
  String html = getHead("UART Terminal");
  html += "<style>"
    "body{display:flex;flex-direction:column;height:100vh;padding:16px}"
    "#term{background:var(--tb);border:1px solid rgba(var(--ar),0.1);border-radius:10px;"
    "flex:1;overflow-y:auto;overflow-x:hidden;padding:12px;white-space:pre-wrap;word-wrap:break-word;font-size:13px;"
    "min-height:0;box-shadow:inset 0 0 40px rgba(0,0,0,0.5),0 0 20px rgba(var(--ar),0.03);"
    "position:relative;z-index:2;"
    "animation:borderGlow 3.8s ease-in-out infinite}"
    ".hdr{padding:0 4px;position:relative;z-index:2}"
    "#inputrow{display:flex;margin-top:8px;gap:8px;position:relative;z-index:2}"
    "#cmd{flex:1;font-size:13px}"
    "#status{color:var(--lb);font-size:11px;margin-top:4px;padding:0 4px;position:relative;z-index:2}"
    ".ctrl{display:flex;gap:6px;margin-top:6px;flex-wrap:wrap;align-items:center;padding:0 4px;"
    "position:relative;z-index:2}"
    ".ctrl button{padding:5px 12px;font-size:.8em}"
    ".ctrl label{margin:0;font-size:11px;display:flex;align-items:center;gap:4px;color:var(--lb)}"
    "</style>";

  html += "<div class='hdr'><h1 style='font-size:1.1em;padding-bottom:10px;margin-bottom:12px'>UART Terminal</h1>";
  html += getNav();
  html += "</div>";

  html += "<div id='term'></div>";
  html += "<div id='inputrow'>";
  html += "<input id='cmd' placeholder='Type command and press Enter (adds CR+LF)' autofocus>";
  html += "<button onclick='sendCmd()'>Send</button>";
  html += "</div>";
  html += "<div class='ctrl'>";
  html += "<button onclick='sendRaw(\"\\x03\")'>Ctrl+C</button>";
  html += "<button onclick='sendRaw(\"\\r\")'>CR only</button>";
  html += "<button onclick='clearTerm()'>Clear</button>";
  html += "<button id='recBtn' onclick='toggleRec()'>Record</button>";
  html += "<label><input type='checkbox' id='autoscroll' checked>Auto-scroll</label>";
  html += "<label><input type='checkbox' id='hexmode'>Hex</label>";
  html += "</div>";
  html += "<div id='status'>Loading buffer...</div>";

  html += "<script>\n";

  // ANSI state and color tables
  html += "var seq=0,term=document.getElementById('term'),isRec=false;\n";
  html += "var aS={fg:null,bg:null,bold:false,dim:false,it:false,ul:false};\n";
  html += "var aBuf='';\n";
  html += "var aC=['#000','#a00','#0a0','#a50','#00a','#a0a','#0aa','#aaa'];\n";
  html += "var aB=['#555','#f55','#5f5','#ff5','#55f','#f5f','#5ff','#fff'];\n";

  // Build inline CSS from current ANSI state
  html += "function aStyle(s){\n";
  html += "  var c='';\n";
  html += "  if(s.fg!==null)c+='color:'+(s.bold?aB:aC)[s.fg]+';';\n";
  html += "  else if(s.bold)c+='color:#fff;';\n";
  html += "  if(s.bg!==null)c+='background:'+aC[s.bg]+';';\n";
  html += "  if(s.dim)c+='opacity:0.5;';\n";
  html += "  if(s.it)c+='font-style:italic;';\n";
  html += "  if(s.ul)c+='text-decoration:underline;';\n";
  html += "  return c;\n";
  html += "}\n";

  html += "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}\n";

  // Parse ANSI escape sequences into styled HTML spans
  html += "function parseAnsi(t){\n";
  html += "  var h='',re=/\\x1b\\[([0-9;?]*)([A-Za-z])|\\x1b\\][^\\x07]*\\x07|\\x1b[^\\x5b\\x5d]/g,last=0,m;\n";
  html += "  while((m=re.exec(t))!==null){\n";
  html += "    if(m.index>last){\n";
  html += "      var st=aStyle(aS),ch=esc(t.substring(last,m.index));\n";
  html += "      h+=st?'<span style=\"'+st+'\">'+ch+'</span>':ch;\n";
  html += "    }\n";
  html += "    last=re.lastIndex;\n";
  html += "    if(m[2]==='m'){\n";
  html += "      var codes=m[1]?m[1].split(';').map(Number):[0];\n";
  html += "      for(var i=0;i<codes.length;i++){\n";
  html += "        var c=codes[i];\n";
  html += "        if(c===0)aS={fg:null,bg:null,bold:false,dim:false,it:false,ul:false};\n";
  html += "        else if(c===1)aS.bold=true;\n";
  html += "        else if(c===2)aS.dim=true;\n";
  html += "        else if(c===3)aS.it=true;\n";
  html += "        else if(c===4)aS.ul=true;\n";
  html += "        else if(c===22){aS.bold=false;aS.dim=false;}\n";
  html += "        else if(c===23)aS.it=false;\n";
  html += "        else if(c===24)aS.ul=false;\n";
  html += "        else if(c>=30&&c<=37)aS.fg=c-30;\n";
  html += "        else if(c===38&&codes[i+1]===5){var n=codes[i+2];if(n!==undefined){if(n<8)aS.fg=n;else if(n<16){aS.fg=n-8;aS.bold=true;}}i+=2;}\n";
  html += "        else if(c===39)aS.fg=null;\n";
  html += "        else if(c>=40&&c<=47)aS.bg=c-40;\n";
  html += "        else if(c===48&&codes[i+1]===5){var n=codes[i+2];if(n!==undefined){if(n<8)aS.bg=n;else if(n<16)aS.bg=n-8;}i+=2;}\n";
  html += "        else if(c===49)aS.bg=null;\n";
  html += "        else if(c>=90&&c<=97){aS.fg=c-90;aS.bold=true;}\n";
  html += "        else if(c>=100&&c<=107)aS.bg=c-100;\n";
  html += "      }\n";
  html += "    }\n";
  html += "  }\n";
  html += "  if(last<t.length){\n";
  html += "    var st=aStyle(aS),ch=esc(t.substring(last));\n";
  html += "    h+=st?'<span style=\"'+st+'\">'+ch+'</span>':ch;\n";
  html += "  }\n";
  html += "  return h;\n";
  html += "}\n";

  // Append data to terminal
  html += "function appendText(t){\n";
  html += "  if(document.getElementById('hexmode').checked){\n";
  html += "    var h='';\n";
  html += "    for(var i=0;i<t.length;i++)h+=('0'+t.charCodeAt(i).toString(16)).slice(-2)+' ';\n";
  html += "    term.insertAdjacentHTML('beforeend',esc(h));\n";
  html += "  }else{\n";
  html += "    t=aBuf+t;aBuf='';\n";
  html += "    var ei=t.lastIndexOf('\\x1b');\n";
  html += "    if(ei!==-1&&ei>=t.length-50){\n";
  html += "      var tail=t.substring(ei);\n";
  html += "      if(!/^\\x1b(?:\\[[0-9;?]*[A-Za-z]|\\][^\\x07]*\\x07|[^\\x5b\\x5d])/.test(tail)){aBuf=tail;t=t.substring(0,ei);}\n";
  html += "    }\n";
  html += "    term.insertAdjacentHTML('beforeend',parseAnsi(t));\n";
  html += "  }\n";
  html += "  while(term.textContent.length>65536&&term.firstChild)term.removeChild(term.firstChild);\n";
  html += "  if(document.getElementById('autoscroll').checked)term.scrollTop=term.scrollHeight;\n";
  html += "}\n";

  // Poll for new log data (chained setTimeout to prevent overlapping requests)
  html += "function fetchLog(){\n";
  html += "  fetch('/api/log?seq='+seq).then(r=>r.json()).then(d=>{\n";
  html += "    if(d.d.length>0)appendText(d.d);\n";
  html += "    seq=d.seq;\n";
  html += "    if(d.rec!==undefined&&d.rec!==isRec){isRec=d.rec;updRec();}\n";
  html += "    document.getElementById('status').textContent='Connected | seq:'+seq+' | '+d.rx+' RX / '+d.tx+' TX'+(isRec?' | REC':'');\n";
  html += "  }).catch(e=>{\n";
  html += "    document.getElementById('status').textContent='Poll error: '+e;\n";
  html += "  }).finally(()=>{\n";
  html += "    setTimeout(fetchLog,300);\n";
  html += "  });\n";
  html += "}\n";

  // Recording toggle
  html += "function toggleRec(){\n";
  html += "  fetch('/api/rec/'+(isRec?'stop':'start'),{method:'POST'}).then(r=>r.json()).then(d=>{\n";
  html += "    if(d.ok){isRec=!isRec;updRec();}\n";
  html += "  });\n";
  html += "}\n";
  html += "function updRec(){\n";
  html += "  var b=document.getElementById('recBtn');\n";
  html += "  b.textContent=isRec?'Stop Rec':'Record';\n";
  html += "  b.style.color=isRec?'#ff6b6b':'';\n";
  html += "  b.style.borderColor=isRec?'rgba(255,60,60,0.5)':'';\n";
  html += "}\n";

  // Send command with CR+LF
  html += "function sendCmd(){\n";
  html += "  var c=document.getElementById('cmd').value;\n";
  html += "  fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c+'\\r\\n')});\n";
  html += "  document.getElementById('cmd').value='';\n";
  html += "}\n";
  html += "function sendRaw(s){\n";
  html += "  fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(s)});\n";
  html += "}\n";
  html += "function clearTerm(){term.innerHTML='';aS={fg:null,bg:null,bold:false,dim:false,it:false,ul:false};aBuf='';}\n";
  html += "document.getElementById('cmd').addEventListener('keydown',function(e){\n";
  html += "  if(e.key==='Enter'){e.preventDefault();sendCmd();}\n";
  html += "});\n";

  // Initial fetch loads ring buffer backlog, then start polling
  html += "fetchLog();\n";
  html += "</script>";

  html += "</body></html>";
  return html;
}

// ---- Splash page for reboots ----
String getSplashPage(const char* msg, const char* color) {
  const ThemeColors& t = T();
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0}body{background:";
  h += t.bg;
  h += ";display:flex;align-items:center;justify-content:center;"
    "min-height:100vh;font-family:'Courier New',monospace;"
    "background-image:"
    "repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(";
  h += t.ar;
  h += ",0.03) 39px,rgba(";
  h += t.ar;
  h += ",0.03) 40px),"
    "repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(";
  h += t.ar;
  h += ",0.03) 39px,rgba(";
  h += t.ar;
  h += ",0.03) 40px)}"
    ".card{background:";
  h += t.bx;
  h += ";backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);"
    "border:1px solid rgba(";
  h += t.ar;
  h += ",0.15);border-radius:14px;padding:40px 50px;text-align:center;"
    "box-shadow:0 0 40px rgba(";
  h += t.ar;
  h += ",0.06)}"
    "h1{font-size:1.2em;margin-bottom:8px}"
    "p{color:";
  h += t.l2;
  h += ";font-size:.9em}</style></head><body><div class='card'><h1 style='color:";
  h += color;
  h += ";text-shadow:0 0 20px ";
  h += color;
  h += "'>";
  h += msg;
  h += "</h1><p>Reconnect in a few seconds.</p></div></body></html>";
  return h;
}

String auth_token = "";

bool isAuthenticated() {
  if (auth_token == "") return false;
  String cookie = webServer.header("Cookie");
  return cookie.indexOf("tok=" + auth_token) >= 0;
}

// ---- Web Server Handlers ----
void handleRoot() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  webServer.send(200, "text/html", getLoginPage());
}

void handleAuth() {
  if (webServer.hasArg("pass") && webServer.arg("pass") == web_password) {
    auth_token = String(esp_random());
    webServer.send(200, "application/json", "{\"ok\":true,\"tok\":\"" + auth_token + "\"}");
  } else {
    webServer.send(200, "application/json", "{\"ok\":false}");
  }
}

void handlePanel() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  size_t freeHeap = esp_get_free_heap_size();
  webServer.send(200, "text/html", getStatusPage(freeHeap));
}
void handleConfigPage() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.send(200, "text/html", getConfigPage());
}
void handleUpdatePage() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.send(200, "text/html", getUpdatePage());
}
void handleTerminalPage() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.send(200, "text/html", getTerminalPage());
}
void handleLogsPage() {
  if (first_run) { webServer.sendHeader("Location", "/setup"); webServer.send(302); return; }
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.send(200, "text/html", getLogsPage());
}

void handleApiLog() {
  if (!isAuthenticated()) { webServer.send(403, "application/json", "{\"err\":\"auth\"}"); return; }

  unsigned long long clientSeq = 0;
  if (webServer.hasArg("seq")) clientSeq = strtoull(webServer.arg("seq").c_str(), NULL, 10);

  String data = "";
  data.reserve(4096);
  unsigned long long seqToReport = logSeq;

  if (clientSeq < logSeq) {
    unsigned long long behind = logSeq - clientSeq;
    if (behind > LOG_BUF_SIZE) behind = LOG_BUF_SIZE;
    // Cap per-response to 16KB to prevent OOM on initial load
    const unsigned long long MAX_CHUNK = 16384;
    if (behind > MAX_CHUNK) behind = MAX_CHUNK;

    // Trim incomplete UTF-8 at end
    unsigned long long usable = behind;
    for (int back = 1; back <= 4 && back <= (int)usable; back++) {
      uint8_t b = (uint8_t)logBuf[(logHead - back + LOG_BUF_SIZE) % LOG_BUF_SIZE];
      if (b < 0x80) break;
      if (b >= 0xC0) {
        int need = (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
        if (back < need) usable -= back;
        break;
      }
    }
    seqToReport = logSeq - behind + usable;

    int start = (logHead - (int)behind + LOG_BUF_SIZE) % LOG_BUF_SIZE;
    for (unsigned long long i = 0; i < usable; i++) {
      char c = logBuf[(start + i) % LOG_BUF_SIZE];
      if (c == '\\') data += "\\\\";
      else if (c == '"') data += "\\\"";
      else if (c == '\n') data += "\\n";
      else if (c == '\r') data += "\\r";
      else if (c == '\t') data += "\\t";
      else if ((uint8_t)c >= 0x20 && (uint8_t)c != 0x7f) data += c;
      else {
        char hex[8];
        snprintf(hex, sizeof(hex), "\\u%04x", (uint8_t)c);
        data += hex;
      }
    }
  }

  char seqBuf[24];
  snprintf(seqBuf, sizeof(seqBuf), "%llu", seqToReport);
  String json = "{\"seq\":" + String(seqBuf) + ",\"d\":\"" + data + "\",\"rx\":" + String(bytes_rx) + ",\"tx\":" + String(bytes_tx) + ",\"rec\":" + (recording ? "true" : "false") + "}";
  webServer.send(200, "application/json", json);
}

void handleApiSend() {
  if (!isAuthenticated()) { webServer.send(403, "application/json", "{\"err\":\"auth\"}"); return; }
  if (!uart_active) { webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"UART not active\"}"); return; }
  if (webServer.hasArg("cmd")) {
    String cmd = webServer.arg("cmd");
    Serial1.print(cmd);
    bytes_tx += cmd.length();
    webServer.send(200, "application/json", "{\"ok\":true}");
  } else {
    webServer.send(200, "application/json", "{\"ok\":false}");
  }
}

void handleRecStart() {
  if (!isAuthenticated()) { webServer.send(403, "application/json", "{\"err\":\"auth\"}"); return; }
  if (recording) { webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"Already recording\"}"); return; }

  recFilename = "/rec_" + String(millis()) + ".txt";
  recFile = LittleFS.open(recFilename, "w");
  if (!recFile) {
    webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"File error\"}");
    return;
  }
  recording = true;
  lastRecFlush = millis();
  Serial.println("[rec] Started: " + recFilename);
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleRecStop() {
  if (!isAuthenticated()) { webServer.send(403, "application/json", "{\"err\":\"auth\"}"); return; }
  if (!recording) { webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"Not recording\"}"); return; }

  recording = false;
  recFile.close();
  Serial.println("[rec] Stopped: " + recFilename);
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleLogDownload() {
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  if (!webServer.hasArg("f")) { webServer.send(400); return; }
  String fn = webServer.arg("f");
  if (!fn.startsWith("rec_") || fn.indexOf('/') >= 0) { webServer.send(403); return; }
  File f = LittleFS.open("/" + fn, "r");
  if (!f) { webServer.send(404, "text/plain", "Not found"); return; }

  bool clean = webServer.hasArg("clean") && webServer.arg("clean") == "1";
  String dlName = fn;
  if (clean) dlName = fn.substring(0, fn.length() - 4) + "_clean.txt";
  webServer.sendHeader("Content-Disposition", "attachment; filename=" + dlName);

  if (!clean) {
    webServer.streamFile(f, "text/plain");
  } else {
    // Strip ANSI: two passes — count output size, then emit
    // State machine: 0=normal, 1=saw ESC, 2=in CSI (ESC[), 3=in OSC (ESC])
    uint8_t buf[512];

    // Pass 1: count clean bytes
    size_t cleanLen = 0;
    int st = 0;
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        switch (st) {
          case 0: if (c == 0x1B) st = 1; else cleanLen++; break;
          case 1: st = (c == '[') ? 2 : (c == ']') ? 3 : 0; break;
          case 2: if (c >= 0x40 && c <= 0x7E) st = 0; break;
          case 3: if (c == 0x07) st = 0; else if (c == 0x1B) st = 1; break;
        }
      }
    }
    f.seek(0);

    // Pass 2: send with known content length
    webServer.setContentLength(cleanLen);
    webServer.send(200, "text/plain", "");
    st = 0;
    uint8_t out[512];
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      int op = 0;
      for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        switch (st) {
          case 0: if (c == 0x1B) st = 1; else out[op++] = c; break;
          case 1: st = (c == '[') ? 2 : (c == ']') ? 3 : 0; break;
          case 2: if (c >= 0x40 && c <= 0x7E) st = 0; break;
          case 3: if (c == 0x07) st = 0; else if (c == 0x1B) st = 1; break;
        }
        if (op >= (int)sizeof(out)) { webServer.client().write(out, op); op = 0; }
      }
      if (op > 0) webServer.client().write(out, op);
    }
  }
  f.close();
}

void handleLogDelete() {
  if (!isAuthenticated()) { webServer.send(403); return; }
  if (!webServer.hasArg("f")) { webServer.send(400); return; }
  String fn = webServer.arg("f");
  if (!fn.startsWith("rec_") || fn.indexOf('/') >= 0) { webServer.send(403); return; }
  // Stop recording if deleting the active file
  if (recording && recFilename == "/" + fn) {
    recording = false;
    recFile.close();
  }
  LittleFS.remove("/" + fn);
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleThemeApi() {
  if (!isAuthenticated()) { webServer.send(403, "application/json", "{\"err\":\"auth\"}"); return; }
  if (webServer.hasArg("t")) {
    int t = webServer.arg("t").toInt();
    if (t >= 0 && t <= 2) {
      theme = t;
      Preferences p;
      p.begin("bridge", false);
      p.putInt("theme", theme);
      p.end();
    }
  }
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleSetupPage() {
  if (!first_run) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  if (webServer.method() == HTTP_POST) {
    if (webServer.hasArg("ssid")) wifi_ssid = webServer.arg("ssid");
    if (webServer.hasArg("wifipass")) wifi_password = webServer.arg("wifipass");
    if (webServer.hasArg("brpass")) bridge_password = webServer.arg("brpass");
    if (webServer.hasArg("webpass")) web_password = webServer.arg("webpass");
    if (webServer.hasArg("baud")) uart_baud = webServer.arg("baud").toInt();
    saveConfig();
    first_run = false;
    webServer.send(200, "text/html", getSplashPage("Setup Complete! Rebooting...", T().ac));
    delay(1000);
    ESP.restart();
    return;
  }
  webServer.send(200, "text/html", getSetupPage());
}

void handleSave() {
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  if (webServer.hasArg("ssid")) wifi_ssid = webServer.arg("ssid");
  if (webServer.hasArg("wifipass")) wifi_password = webServer.arg("wifipass");
  if (webServer.hasArg("brpass")) bridge_password = webServer.arg("brpass");
  if (webServer.hasArg("webpass")) web_password = webServer.arg("webpass");
  if (webServer.hasArg("baud")) uart_baud = webServer.arg("baud").toInt();
  if (webServer.hasArg("port")) tcp_port = webServer.arg("port").toInt();
  if (webServer.hasArg("delay")) uart_delay = webServer.arg("delay").toInt();
  if (webServer.hasArg("sip")) static_ip = webServer.arg("sip");
  if (webServer.hasArg("sgw")) static_gw = webServer.arg("sgw");
  if (webServer.hasArg("ssn")) static_subnet = webServer.arg("ssn");
  if (webServer.hasArg("mdns")) mdns_name = webServer.arg("mdns");
  if (webServer.hasArg("appass")) ap_password = webServer.arg("appass");

  saveConfig();

  webServer.send(200, "text/html", getSplashPage("Saved! Rebooting...", T().ac));
  delay(1000);
  ESP.restart();
}

void handleReboot() {
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.send(200, "text/html", getSplashPage("Rebooting...", T().ac));
  delay(1000);
  ESP.restart();
}

void handleFactoryReset() {
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  prefs.begin("bridge", false);
  prefs.clear();
  prefs.end();
  webServer.send(200, "application/json", "{\"ok\":true}");
  delay(1000);
  ESP.restart();
}

void handleUpload() {
  if (!isAuthenticated()) { webServer.sendHeader("Location", "/"); webServer.send(302); return; }
  webServer.sendHeader("Connection", "close");
  webServer.send(200, "text/html", Update.hasError()
    ? getSplashPage("Update FAILED", "#ff6b6b")
    : getSplashPage("Update OK! Rebooting...", T().ac));
  delay(1000);
  ESP.restart();
}

void handleUploadProcess() {
  if (!isAuthenticated()) return;
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] Uploading: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[ota] Update success: %u bytes\n", upload.totalSize);
    else Update.printError(Serial);
  }
}

// ---- Config Storage ----
void saveConfig() {
  prefs.begin("bridge", false);
  prefs.putString("ssid", wifi_ssid);
  prefs.putString("wifipass", wifi_password);
  prefs.putString("brpass", bridge_password);
  prefs.putString("webpass", web_password);
  prefs.putInt("baud", uart_baud);
  prefs.putInt("port", tcp_port);
  prefs.putInt("delay", uart_delay);
  prefs.putString("sip", static_ip);
  prefs.putString("sgw", static_gw);
  prefs.putString("ssn", static_subnet);
  prefs.putString("mdns", mdns_name);
  prefs.putString("appass", ap_password);
  prefs.putInt("theme", theme);
  prefs.end();
}

void loadConfig() {
  prefs.begin("bridge", true);
  first_run = !prefs.isKey("ssid");
  if (!first_run) {
    wifi_ssid = prefs.getString("ssid", wifi_ssid);
    wifi_password = prefs.getString("wifipass", wifi_password);
    bridge_password = prefs.getString("brpass", bridge_password);
    web_password = prefs.getString("webpass", web_password);
    uart_baud = prefs.getInt("baud", uart_baud);
    tcp_port = prefs.getInt("port", tcp_port);
    uart_delay = prefs.getInt("delay", uart_delay);
  }
  static_ip = prefs.getString("sip", static_ip);
  static_gw = prefs.getString("sgw", static_gw);
  static_subnet = prefs.getString("ssn", static_subnet);
  mdns_name = prefs.getString("mdns", mdns_name);
  ap_password = prefs.getString("appass", ap_password);
  theme = prefs.getInt("theme", theme);
  if (theme < 0 || theme > 2) theme = 0;
  prefs.end();

  ap_ssid = getDefaultHostname();
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  Serial.println("[bridge] Booting...");

  loadConfig();

  // Init LittleFS for session recordings
  if (!LittleFS.begin(true)) {
    Serial.println("[bridge] LittleFS mount failed!");
  } else {
    Serial.printf("[bridge] LittleFS: %u / %u bytes used\n", LittleFS.usedBytes(), LittleFS.totalBytes());
  }

  // Force UART pins to high-impedance so router boots clean
  pinMode(UART_RX_PIN, INPUT);
  pinMode(UART_TX_PIN, INPUT);
  gpio_set_pull_mode((gpio_num_t)UART_RX_PIN, GPIO_FLOATING);
  gpio_set_pull_mode((gpio_num_t)UART_TX_PIN, GPIO_FLOATING);

  boot_time = millis();
  if (uart_delay == 0) {
    Serial1.begin(uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    uart_active = true;
    Serial.println("[bridge] UART active (no delay)");
  } else {
    Serial.printf("[bridge] UART will activate in %d seconds\n", uart_delay);
  }

  // On first run, skip STA and go straight to AP for setup
  if (first_run) {
    Serial.println("[bridge] First run detected — starting AP for setup");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
    ap_mode = true;
    Serial.println("[bridge] ================================");
    Serial.print("[bridge] AP SSID: "); Serial.println(ap_ssid);
    Serial.print("[bridge] AP Password: "); Serial.println(ap_password);
    Serial.print("[bridge] AP IP: "); Serial.println(WiFi.softAPIP());
    Serial.println("[bridge] Open http://" + WiFi.softAPIP().toString() + " to configure");
    Serial.println("[bridge] ================================");
  } else {
    // Apply static IP if configured
    if (static_ip.length() > 0) {
      IPAddress ip, gw, sn;
      ip.fromString(static_ip);
      gw.fromString(static_gw);
      sn.fromString(static_subnet);
      WiFi.config(ip, gw, sn);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

    Serial.print("[bridge] Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[bridge] Connected!");
      Serial.print("[bridge] IP: ");
      Serial.println(WiFi.localIP());

      // Start mDNS
      if (mdns_name.length() > 0 && MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[bridge] mDNS: %s.local\n", mdns_name.c_str());
      }
    } else {
      // AP mode fallback
      Serial.println("\n[bridge] WiFi failed! Starting AP...");
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
      ap_mode = true;
      Serial.println("[bridge] ================================");
      Serial.print("[bridge] AP SSID: "); Serial.println(ap_ssid);
      Serial.print("[bridge] AP Password: "); Serial.println(ap_password);
      Serial.print("[bridge] AP IP: "); Serial.println(WiFi.softAPIP());
      Serial.println("[bridge] ================================");
    }
  }

  bridgeServer = new WiFiServer(tcp_port);
  bridgeServer->begin();

  // Web server routes
  const char* headerKeys[] = {"Cookie"};
  webServer.collectHeaders(headerKeys, 1);
  webServer.on("/", handleRoot);
  webServer.on("/auth", HTTP_POST, handleAuth);
  webServer.on("/panel", handlePanel);
  webServer.on("/config", handleConfigPage);
  webServer.on("/update", handleUpdatePage);
  webServer.on("/terminal", handleTerminalPage);
  webServer.on("/logs", handleLogsPage);
  webServer.on("/logs/dl", HTTP_GET, handleLogDownload);
  webServer.on("/logs/rm", HTTP_POST, handleLogDelete);
  webServer.on("/api/log", HTTP_GET, handleApiLog);
  webServer.on("/api/send", HTTP_POST, handleApiSend);
  webServer.on("/api/rec/start", HTTP_POST, handleRecStart);
  webServer.on("/api/rec/stop", HTTP_POST, handleRecStop);
  webServer.on("/api/theme", HTTP_POST, handleThemeApi);
  webServer.on("/setup", handleSetupPage);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.on("/factory-reset", HTTP_POST, handleFactoryReset);
  webServer.on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
  webServer.begin();

  Serial.printf("[bridge] UART bridge on port %d\n", tcp_port);
  Serial.printf("[bridge] Web panel on port 80\n");
  Serial.println("[bridge] Ready!");
}

// ---- Main Loop ----
void loop() {
  webServer.handleClient();

  // Delayed UART activation
  if (!uart_active && uart_delay > 0 && millis() - boot_time >= (unsigned long)uart_delay * 1000) {
    Serial1.begin(uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    uart_active = true;
    Serial.println("[bridge] UART now active");
  }

  // Non-blocking WiFi reconnect (STA mode only)
  static unsigned long lastReconnectAttempt = 0;
  if (!ap_mode && WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      Serial.println("[bridge] WiFi lost, reconnecting...");
      WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    }
  } else if (!ap_mode && WiFi.status() == WL_CONNECTED && lastReconnectAttempt != 0) {
    Serial.println("[bridge] WiFi reconnected");
    Serial.print("[bridge] IP: ");
    Serial.println(WiFi.localIP());
    lastReconnectAttempt = 0;
  }

  // Accept new bridge client
  if (!bridgeClient || !bridgeClient.connected()) {
    WiFiClient newClient = bridgeServer->available();
    if (newClient) {
      bridgeClient = newClient;
      bridgeClient.setNoDelay(true);
      authenticated = false;
      bridgeClient.println("=== ESP32 UART Bridge ===");
      bridgeClient.print("Password: ");
      Serial.println("[bridge] Client connected, waiting for auth");
    }
  }

  // Handle TCP authentication
  if (bridgeClient && bridgeClient.connected() && !authenticated) {
    if (bridgeClient.available()) {
      String input = bridgeClient.readStringUntil('\n');
      input.trim();
      if (input == bridge_password) {
        authenticated = true;
        bridgeClient.println("\nAuthenticated. UART bridge active.\n");
        Serial.println("[bridge] Client authenticated");
      } else {
        bridgeClient.println("\nWrong password.");
        bridgeClient.print("Password: ");
        Serial.println("[bridge] Wrong password attempt");
      }
    }
    return;
  }

  // Bridge: target UART -> TCP + log buffer
  if (uart_active && Serial1.available()) {
    while (Serial1.available()) {
      uint8_t buf[256];
      int len = Serial1.read(buf, sizeof(buf));
      if (len > 0) {
        logWrite(buf, len);
        if (bridgeClient && bridgeClient.connected() && authenticated) {
          bridgeClient.write(buf, len);
        }
        bytes_rx += len;
      }
    }
  }

  // Bridge: TCP -> target UART
  if (bridgeClient && bridgeClient.connected() && authenticated && uart_active) {
    while (bridgeClient.available()) {
      uint8_t buf[256];
      int len = bridgeClient.read(buf, sizeof(buf));
      if (len > 0) {
        Serial1.write(buf, len);
        bytes_tx += len;
      }
    }
  }

  // Flush recording file periodically
  if (recording && recFile && millis() - lastRecFlush > 10000) {
    recFile.flush();
    lastRecFlush = millis();
  }

  delay(1);
}
