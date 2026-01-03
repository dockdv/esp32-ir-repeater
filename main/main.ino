/*
  ESP32 Wi-Fi Setup Portal + IR (Arduino-IRremote) + IR relay + HTTP API + Web UI + mDNS (unique per device)

  - If saved Wi-Fi connects: starts web UI + APIs on STA IP and mDNS name (http://<name>.local/)
  - If connect fails or BOOT held at boot (GPIO0 LOW): starts OPEN AP (no password) and setup page at http://192.168.4.1
  - Setup page lets you enter: SSID, password, and optional device name
  - After saving, shows a redirect page that auto-opens http://<mdns>.local/ once your phone/laptop reconnects to home Wi-Fi
  - IR send API supports selecting NEC address & repeats:
      /api/ir/send?addr=0x01&cmd=0x1B&repeats=0
  - IR relay: received IR is re-transmitted (toggle via /api/ir/relay?enable=0|1)
  - Web UI shows last received IR decode.

  Hardware:
  - IR_SEND_PIN is GPIO2. Change as necessary.
  - IR_RECEIVE_PIN is GPIO27. Change as necessary.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ---- IRremote: define pins before including IRremote.hpp ----
#define IR_SEND_PIN 2
#define IR_RECEIVE_PIN 27
#include <IRremote.hpp>

// ---------- Defaults ----------
static constexpr uint8_t NEC_ADDR_DEFAULT    = 0x01;
static constexpr uint8_t NEC_REPEATS_DEFAULT = 0;

// Example commands (optional UI presets)
static constexpr uint8_t CMD_1 = 0x1B;
static constexpr uint8_t CMD_2 = 0x1E;
static constexpr uint8_t CMD_3 = 0x0D;

// Force portal if BOOT held at boot
static const uint8_t FORCE_PORTAL_PIN = 0;

// AP settings (always OPEN)
static const char* AP_PREFIX = "ESP32-Setup-";

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// ---------- Globals ----------
Preferences prefs;
WebServer server(80);

String savedSsid, savedPass, savedDevName;
String apSsid;
String mdnsName;

static constexpr size_t LOG_CAP = 40;
String logsBuf[LOG_CAP];
size_t logsPos = 0;
size_t logsCount = 0;

uint32_t bootMs = 0;

// IR relay + last RX shown on web
bool relayEnabled = true;
uint32_t lastTxMs = 0;
static constexpr uint32_t RELAY_COOLDOWN_MS = 200;
String lastRxLine = "";

// ---------- Logging ----------
void logAdd(const String& s) {
  String line = String(millis() / 1000) + "s: " + s;
  logsBuf[logsPos] = line;
  logsPos = (logsPos + 1) % LOG_CAP;
  if (logsCount < LOG_CAP) logsCount++;
  Serial.println(line);
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\"': out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default: out += c; break;
    }
  }
  return out;
}

String buildLogsJsonArray() {
  String j = "[";
  size_t start = (logsPos + LOG_CAP - logsCount) % LOG_CAP; // oldest -> newest
  for (size_t i = 0; i < logsCount; i++) {
    size_t idx = (start + i) % LOG_CAP;
    if (i) j += ",";
    j += "\"" + jsonEscape(logsBuf[idx]) + "\"";
  }
  j += "]";
  return j;
}

// ---------- Unique naming ----------
String deviceSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t low = (uint32_t)(mac & 0xFFFFFF); // last 24 bits
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", low);
  return String(buf); // e.g. "A1B2C3"
}

String sanitizeHost(const String& in) {
  String s = in;
  s.trim();
  s.toLowerCase();

  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
    if (ok) out += c;
    else if (c == ' ' || c == '_' || c == '.') out += '-';
  }

  if (out.length() == 0 || !(out[0] >= 'a' && out[0] <= 'z')) {
    out = "esp32-ir";
  }

  while (out.endsWith("-")) out.remove(out.length() - 1);
  if (out.length() == 0) out = "esp32-ir";
  return out;
}

String makeMdnsName(const String& userName) {
  String base = sanitizeHost(userName);
  return base + "-" + deviceSuffix();
}

// ---------- Parse helpers ----------
bool parseHexOrDecU8(const String& s, uint8_t& out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;

  long v = 0;
  if (t.startsWith("0x") || t.startsWith("0X")) v = strtol(t.c_str(), nullptr, 16);
  else v = strtol(t.c_str(), nullptr, 10);

  if (v < 0 || v > 255) return false;
  out = (uint8_t)v;
  return true;
}

bool parseHexOrDecU8OrDefault(const String& argName, uint8_t& out, uint8_t defVal) {
  if (!server.hasArg(argName)) { out = defVal; return true; }
  return parseHexOrDecU8(server.arg(argName), out);
}

// ---------- Wi-Fi credentials ----------
void loadSettings() {
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  prefs.begin("dev", true);
  savedDevName = prefs.getString("name", "");
  prefs.end();

  apSsid = String(AP_PREFIX) + deviceSuffix();
  mdnsName = makeMdnsName(savedDevName);
}

bool hasSavedWiFi() {
  return !savedSsid.isEmpty();
}

void saveWiFiAndName(const String& ssid, const String& pass, const String& devName) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  prefs.begin("dev", false);
  prefs.putString("name", devName);
  prefs.end();

  loadSettings();
}

void clearCredentials() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ---------- HTML helpers ----------
String pageHeader(const String& title) {
  return "<!doctype html><html><head>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + title + "</title>"
         "<style>"
         "body{font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 12px}"
         ".card{border:1px solid #ddd;border-radius:12px;padding:14px;margin:12px 0}"
         ".btnrow{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}"
         "button{padding:10px;font-size:16px;width:100%;cursor:pointer;border-radius:10px;border:1px solid #ccc;background:#fff}"
         "button:active{transform:scale(0.99)}"
         "input{padding:10px;font-size:16px;border-radius:10px;border:1px solid #ccc;width:100%;box-sizing:border-box}"
         "label{font-size:14px;color:#222}"
         "pre{white-space:pre-wrap;background:#f6f6f6;padding:10px;border-radius:10px;margin:0}"
         ".formgrid{display:grid;grid-template-columns:140px 1fr;gap:10px;align-items:center}"
         ".formgrid .full{grid-column:1 / -1}"
         "@media (max-width:520px){.formgrid{grid-template-columns:1fr}.formgrid label{margin-top:6px}}"
         "</style></head><body>";
}

String pageFooter() { return "</body></html>"; }

// ---------- IR ----------
void sendNEC(uint8_t addr, uint8_t cmd, uint8_t repeats) {
  IrSender.sendNEC(addr, cmd, repeats);
  logAdd("IR NEC sent addr=0x" + String(addr, HEX) + " cmd=0x" + String(cmd, HEX) + " repeats=" + String(repeats));
}

void relayIR(const IRData& d) {
  // optional: ignore repeats
  if (d.flags & IRDATA_FLAGS_IS_REPEAT) {
    return;
  }

  if (d.protocol != UNKNOWN) {
    // write() needs non-const IRData*
    IRData sendData = d;
    IrSender.write(&sendData, 0 /*repeats*/);
  } else {
    // rawDataPtr is gone in IRremote 4.5.0+: use IrReceiver.irparams.rawbuf / rawlen
    IRRawlenType rawlen = IrReceiver.irparams.rawlen; // includes index 0 (empty)
    if (rawlen > 1) {
      // Convert ticks -> microseconds (rawbuf[0] is empty; start at 1)
      static uint16_t rawUs[600];
      uint16_t n = rawlen - 1;
      if (n > 600) n = 600;

      for (uint16_t i = 0; i < n; i++) {
        rawUs[i] = (uint16_t)IrReceiver.irparams.rawbuf[i + 1] * MICROS_PER_TICK;
      }

      IrSender.sendRaw(rawUs, n, 38 /*kHz*/);
    }
  }

  lastTxMs = millis();

  // Helpful when receiving+sending in one sketch
  IrReceiver.restartAfterSend();
}


// ---------- Pages ----------
String portalIndexPage(const String& msg) {
  String html = pageHeader("ESP32 Wi-Fi Setup");
  html += "<h2>ESP32 Wi-Fi Setup</h2>";
  html += "<div class='card'>" + msg + "</div>";

  html += "<div class='card'>"
          "<form method='POST' action='/save'>"
          "<div class='formgrid'>"

          "<label for='ssid'><b>SSID</b></label>"
          "<input id='ssid' name='ssid' placeholder='Enter SSID (network name)' required>"

          "<label for='pass'><b>Password</b></label>"
          "<input id='pass' name='pass' type='password' placeholder='Wi-Fi password'>"

          "<label for='name'><b>Device name</b></label>"
          "<input id='name' name='name' placeholder='Optional (e.g. livingroom)'>"

          "<div class='full'>"
          "<button type='submit'>Save & Reboot</button>"
          "</div>"

          "</div>"
          "</form>"
          "</div>";

  html += "<div class='card'><b>Setup AP SSID:</b> " + apSsid +
          "<br><b>Open:</b> http://192.168.4.1</div>";

  html += pageFooter();
  return html;
}

String portalRedirectPage(const String& ssid, const String& targetUrl) {
  String html = pageHeader("Connecting...");
  html += "<h2>Connecting...</h2>";
  html += "<div class='card'>Saved Wi-Fi for <b>" + ssid + "</b>.<br>"
          "Reconnect your PC/phone to your home Wi-Fi.<br>"
          "Then this page will automatically open:<br><b>" + targetUrl + "</b></div>";
  html += "<div class='card'><pre id='s'>Waiting for device...</pre></div>";

  html += "<script>"
          "const target='" + targetUrl + "';"
          "const s=document.getElementById('s');"
          "async function probe(){"
          "  try {"
          "    await fetch(target + 'api/status?ts=' + Date.now(), { mode: 'no-cors', cache: 'no-store' });"
          "    window.location = target;"
          "  } catch(e) {"
          "    s.textContent = 'Still waiting... (make sure you reconnected to home Wi-Fi)';"
          "    setTimeout(probe, 1000);"
          "  }"
          "}"
          "probe();"
          "</script>";

  html += pageFooter();
  return html;
}

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String staIndexPage() {
  String ip = WiFi.localIP().toString();
  String hostUrl = String("http://") + mdnsName + ".local/";

  String ssidEsc = htmlEscape(savedSsid);
  String devEsc  = htmlEscape(savedDevName);
  String hostEsc = htmlEscape(hostUrl);

  String html = pageHeader("ESP32 IR Control");

  html += "<h2>ESP32 IR Control</h2>";
  html += "<div class='card'><b>Wi-Fi:</b> " + ssidEsc +
          "<br><b>IP:</b> " + htmlEscape(ip) +
          "<br><b>mDNS:</b> " + hostEsc +
          "</div>";

  // --- Wi-Fi + Device name settings ---
  html += "<div class='card'><h3>Device Settings</h3>"
          "<div class='formgrid'>"
          "<label for='wssid'><b>Wi-Fi SSID</b></label>"
          "<input id='wssid' placeholder='Enter SSID' value=\"" + ssidEsc + "\">"

          "<label for='wpass'><b>Wi-Fi Password</b></label>"
          "<input id='wpass' type='password' placeholder='Enter password'>"

          "<label for='wname'><b>Device name</b></label>"
          "<input id='wname' placeholder='e.g. livingroom' value=\"" + devEsc + "\">"

          "<div class='full'>"
          "<button type='button' onclick='saveDeviceSettings(); return false;'>Save & Reboot</button>"
          "<div style='font-size:13px;margin-top:6px;color:#444'>"
          "After reboot the device may get a new IP. Use mDNS: <b id='mdnsLabel'>" + hostEsc + "</b>"
          "</div>"
          "<pre id='wmsg' style='margin-top:10px;display:none'></pre>"
          "</div>"
          "</div>"
          "</div>";

  // IR Settings (address + repeats)
  html += "<div class='card'><h3>IR Settings</h3>"
          "<div class='formgrid'>"
          "<label for='addr'><b>NEC Address</b></label>"
          "<input id='addr' value='0x01' placeholder='0x01 or 1'>"
          "<label for='rep'><b>Repeats</b></label>"
          "<input id='rep' placeholder='(optional)'>"
          "</div>"
          "</div>";

  // Presets
  html += "<div class='card'><h3>Send Presets</h3>"
          "<div class='btnrow'>"
          "<button type='button' onclick='sendPreset(\"1B\"); return false;'>Send cmd 0x1B</button>"
          "<button type='button' onclick='sendPreset(\"1E\"); return false;'>Send cmd 0x1E</button>"
          "<button type='button' onclick='sendPreset(\"0D\"); return false;'>Send cmd 0x0D</button>"
          "</div></div>";

  // Custom send
  html += "<div class='card'><h3>Send Custom</h3>"
          "<div class='formgrid'>"
          "<label for='cmd'><b>Command</b></label>"
          "<input id='cmd' placeholder='0x1B or 27'>"
          "<div class='full'><button type='button' onclick='sendCustom(); return false;'>Send Custom</button></div>"
          "</div>"
          "</div>";

  // NEW: last received IR shown on the page
  html += "<div class='card'><h3>Last received IR</h3><pre id='rx'>Waiting...</pre></div>";

  // Log
  html += "<div class='card'><h3>Log</h3><pre id='log'>Loading...</pre></div>";

  // API examples
  html += "<div class='card'><h3>API examples</h3>"
          "<pre>"
          "GET  /api/ir/send?addr=0x01&cmd=0x1B\n"
          "GET  /api/ir/send?addr=1&cmd=27\n"
          "GET  /api/ir/send?addr=0x01&cmd=0x1B&repeats=3\n"
          "GET  /api/ir/relay?enable=0\n"
          "GET  /api/ir/relay?enable=1\n"
          "POST /api/wifi/set  (ssid=...&pass=...&name=...)\n"
          "GET  /api/status\n"
          "GET  /api/wifi/forget\n"
          "</pre></div>";

  // Script
  html += "<script>"
          "function getAddr(){ return document.getElementById('addr').value.trim(); }"
          "function getRep(){ return document.getElementById('rep').value.trim(); }"
          "function buildUrl(cmd){"
          "  const addr=getAddr();"
          "  const rep=getRep();"
          "  let url='/api/ir/send?cmd='+encodeURIComponent(cmd);"
          "  if(addr) url+='&addr='+encodeURIComponent(addr);"
          "  if(rep)  url+='&repeats='+encodeURIComponent(rep);"
          "  return url;"
          "}"

          "async function sendPreset(hex){"
          "  const url=buildUrl('0x'+hex);"
          "  try{"
          "    const r=await fetch(url, {cache:'no-store'});"
          "    const t=await r.text();"
          "    if(!r.ok) alert('IR API error:\\n'+t);"
          "  }catch(e){"
          "    alert('Request failed:\\n'+e);"
          "  }"
          "  await refresh();"
          "}"

          "async function sendCustom(){"
          "  const cmd=document.getElementById('cmd').value.trim();"
          "  if(!cmd){ alert('Enter a command'); return; }"
          "  const url=buildUrl(cmd);"
          "  try{"
          "    const r=await fetch(url, {cache:'no-store'});"
          "    const t=await r.text();"
          "    if(!r.ok) alert('IR API error:\\n'+t);"
          "  }catch(e){"
          "    alert('Request failed:\\n'+e);"
          "  }"
          "  await refresh();"
          "}"

          "async function saveDeviceSettings(){"
          "  const ssid=document.getElementById('wssid').value.trim();"
          "  const pass=document.getElementById('wpass').value;"
          "  const name=document.getElementById('wname').value.trim();"
          "  if(!ssid){ alert('Enter SSID'); return; }"
          "  const msg=document.getElementById('wmsg');"
          "  msg.style.display='block';"
          "  msg.textContent='Saving...';"
          "  try{"
          "    const body='ssid='+encodeURIComponent(ssid)"
          "      +'&pass='+encodeURIComponent(pass)"
          "      +'&name='+encodeURIComponent(name);"
          "    const r=await fetch('/api/wifi/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
          "    const txt=await r.text();"
          "    msg.textContent=(r.ok ? 'Saved. Rebooting...\\n' : 'Error:\\n') + txt;"
          "    try{ const j=JSON.parse(txt); if(j.mdns){ document.getElementById('mdnsLabel').textContent=j.mdns; } }catch(e){}"
          "  }catch(e){"
          "    msg.textContent='Request failed: '+e;"
          "  }"
          "}"

          "async function refresh(){"
          "  try{"
          "    const r=await fetch('/api/status', {cache:'no-store'});"
          "    const j=await r.json();"
          "    document.getElementById('log').textContent=(j.logs||[]).join('\\n');"
          "    document.getElementById('rx').textContent=j.lastRx || 'Waiting...';"
          "  }catch(e){"
          "    document.getElementById('log').textContent='Status fetch failed: '+e;"
          "  }"
          "}"
          "setInterval(refresh, 1000);"
          "refresh();"
          "</script>";

  html += pageFooter();
  return html;
}

// ---------- Portal mode ----------
void setupPortalRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", portalIndexPage("Enter your Wi-Fi credentials."));
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String name = server.arg("name");
    ssid.trim();
    name.trim();

    if (ssid.length() == 0) {
      server.send(400, "text/plain", "Missing SSID");
      return;
    }

    saveWiFiAndName(ssid, pass, name);
    String target = String("http://") + mdnsName + ".local/";

    server.send(200, "text/html", portalRedirectPage(ssid, target));
    delay(800);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
}

void startPortal(const String& reason) {
  WiFi.mode(WIFI_AP);

  apSsid = String(AP_PREFIX) + deviceSuffix();
  WiFi.softAP(apSsid.c_str()); // ALWAYS OPEN (no password)

  IPAddress ip = WiFi.softAPIP();
  logAdd("Portal mode: " + reason);
  logAdd("AP SSID: " + apSsid);
  logAdd("AP IP: " + ip.toString());

  setupPortalRoutes();
  server.begin();
}

// ---------- STA mode ----------
bool connectWiFiSTA() {
  if (!hasSavedWiFi()) {
    logAdd("No saved Wi-Fi credentials.");
    return false;
  }

  logAdd("Connecting STA to SSID: " + savedSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logAdd("STA connected. IP: " + WiFi.localIP().toString() + " RSSI: " + String(WiFi.RSSI()));
    return true;
  }

  logAdd("STA connect failed.");
  WiFi.disconnect(true);
  delay(100);
  return false;
}

void setupStaRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", staIndexPage());
  });

  // Change Wi-Fi credentials + device name from the main page (STA mode) and reboot.
  // POST /api/wifi/set  (form fields: ssid, pass, name)
  server.on("/api/wifi/set", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String name = server.arg("name");   // optional but accepted

    ssid.trim();
    name.trim();

    if (ssid.length() == 0) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }

    saveWiFiAndName(ssid, pass, name);

    String newHost = String("http://") + mdnsName + ".local/";
    String j = "{\"ok\":true,\"msg\":\"Saved. Rebooting...\",\"mdns\":\"" + newHost + "\"}";
    server.send(200, "application/json", j);

    delay(500);
    ESP.restart();
  });

  // Toggle relay
  server.on("/api/ir/relay", HTTP_GET, []() {
    if (server.hasArg("enable")) {
      relayEnabled = (server.arg("enable") != "0");
    }
    server.send(200, "application/json",
                String("{\"ok\":true,\"relayEnabled\":") + (relayEnabled ? "true" : "false") + "}");
  });

  // Status used by the web UI
  server.on("/api/status", HTTP_GET, []() {
    String j = "{";
    j += "\"mode\":\"sta\",";
    j += "\"ssid\":\"" + jsonEscape(savedSsid) + "\",";
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"mdns\":\"" + jsonEscape(mdnsName) + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    j += "\"uptime_s\":" + String((millis() - bootMs) / 1000) + ",";
    j += "\"relayEnabled\":" + String(relayEnabled ? "true" : "false") + ",";
    j += "\"lastRx\":\"" + jsonEscape(lastRxLine) + "\",";
    j += "\"logs\":" + buildLogsJsonArray();
    j += "}";
    server.send(200, "application/json", j);
  });

  // Main IR API (addr optional; cmd required; repeats optional)
  // GET /api/ir/send?addr=0x01&cmd=0x1B&repeats=0
  server.on("/api/ir/send", HTTP_ANY, []() {
    if (!server.hasArg("cmd")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing cmd\"}");
      return;
    }

    uint8_t cmd, addr, reps;
    if (!parseHexOrDecU8(server.arg("cmd"), cmd)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid cmd (0-255)\"}");
      return;
    }
    if (!parseHexOrDecU8OrDefault("addr", addr, NEC_ADDR_DEFAULT)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid addr (0-255)\"}");
      return;
    }
    if (!parseHexOrDecU8OrDefault("repeats", reps, NEC_REPEATS_DEFAULT)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid repeats (0-255)\"}");
      return;
    }

    sendNEC(addr, cmd, reps);

    String j = "{";
    j += "\"ok\":true,";
    j += "\"addr\":\"0x" + String(addr, HEX) + "\",";
    j += "\"cmd\":\"0x" + String(cmd, HEX) + "\",";
    j += "\"repeats\":" + String(reps);
    j += "}";
    server.send(200, "application/json", j);
  });

  // Optional convenience presets (still use default addr/repeats)
  server.on("/api/ir/preset1", HTTP_GET, []() { sendNEC(NEC_ADDR_DEFAULT, CMD_1, NEC_REPEATS_DEFAULT); server.send(200, "text/plain", "OK"); });
  server.on("/api/ir/preset2", HTTP_GET, []() { sendNEC(NEC_ADDR_DEFAULT, CMD_2, NEC_REPEATS_DEFAULT); server.send(200, "text/plain", "OK"); });
  server.on("/api/ir/preset3", HTTP_GET, []() { sendNEC(NEC_ADDR_DEFAULT, CMD_3, NEC_REPEATS_DEFAULT); server.send(200, "text/plain", "OK"); });

  server.on("/api/wifi/forget", HTTP_GET, []() {
    clearCredentials();
    server.send(200, "text/plain", "Cleared Wi-Fi credentials. Rebooting...");
    delay(400);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
}

void startStaServer() {
  if (MDNS.begin(mdnsName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    logAdd(String("mDNS started: http://") + mdnsName + ".local/");
  } else {
    logAdd("mDNS failed to start");
  }

  setupStaRoutes();
  server.begin();
  logAdd("HTTP server started on STA IP: " + WiFi.localIP().toString());
}

// ---------- Setup / Loop ----------
void setup() {
  bootMs = millis();
  Serial.begin(115200);
  delay(300);

  loadSettings();

  // IR init
  IrSender.begin(IR_SEND_PIN);
  logAdd("IR sender ready on GPIO" + String(IR_SEND_PIN));

  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  logAdd("IR receiver ready on GPIO" + String(IR_RECEIVE_PIN));

  pinMode(FORCE_PORTAL_PIN, INPUT_PULLUP);
  bool forcePortal = (digitalRead(FORCE_PORTAL_PIN) == LOW);

  if (!forcePortal && connectWiFiSTA()) {
    startStaServer();
  } else {
    startPortal(forcePortal ? "forced by BOOT button" : "no/failed Wi-Fi");
  }
}

void loop() {
  server.handleClient();

  if (IrReceiver.decode()) {
    const IRData& d = IrReceiver.decodedIRData;

    // One line shown on the web page (and logged)
    lastRxLine = "IR RX protocol=" + String((int)d.protocol) +
                 " addr=0x" + String((uint32_t)d.address, HEX) +
                 " cmd=0x"  + String((uint32_t)d.command, HEX) +
                 ((d.flags & IRDATA_FLAGS_IS_REPEAT) ? " (repeat)" : "");

    logAdd(lastRxLine);

    // Relay if enabled and not within cooldown window (helps avoid self-echo)
    if (relayEnabled && (millis() - lastTxMs) > RELAY_COOLDOWN_MS) {
      relayIR(d);
      logAdd("IR relayed");
    }

    IrReceiver.resume();
  }

  delay(2);
}
