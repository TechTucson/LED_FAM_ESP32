#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>

// ---------- CONFIGURATION ----------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* WEB_USER = "admin";
const char* WEB_PASSWORD = "CHANGE_THIS_PASSWORD"; // Change before using!

constexpr uint8_t IR_PIN = 26;
constexpr uint8_t BUTTON_PIN = 0;
constexpr uint8_t MAX_CODES = 24;
constexpr unsigned long WIFI_RETRY_MS = 15000;

IRsend irsend(IR_PIN);
WebServer server(80);
Preferences prefs;

struct IRCode {
  String name;
  uint16_t address;
  uint8_t command;
};
IRCode codes[MAX_CODES];
uint8_t codeCount = 0;
uint8_t nextCode = 0;

bool buttonDown = false;
unsigned long pressedAt = 0;
unsigned long lastWiFiAttempt = 0;
bool wasConnected = false;
bool updateSuccess = false;

uint8_t reverseBits(uint8_t v) {
  uint8_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result = (result << 1) | (v & 1);
    v >>= 1;
  }
  return result;
}

// Flipper NEC values are bit-reversed when constructing the MSB-first frame.
// Address 0x00 / command 0x85 => 0x00FFA15E (previously confirmed working).
uint32_t necFrame(uint16_t address, uint8_t command) {
  uint8_t a = reverseBits((uint8_t)address);
  uint8_t c = reverseBits(command);
  uint8_t secondAddressByte = (address <= 0xFF) ? (uint8_t)~a
                                                     : reverseBits((uint8_t)(address >> 8));
  return ((uint32_t)a << 24) |
         ((uint32_t)secondAddressByte << 16) |
         ((uint32_t)c << 8) | (uint8_t)~c;
}

void transmit(uint16_t address, uint8_t command) {
  uint32_t frame = necFrame(address, command);
  Serial.printf("[IR] Address=%04X Command=%02X Frame=%08lX\n",
                address, command, (unsigned long)frame);
  irsend.sendNEC(frame, 32);
}

void setDefaults() {
  codeCount = 6;
  codes[0] = {"OFF", 0x00, 0x82};
  codes[1] = {"C83", 0x00, 0x83};
  codes[2] = {"GREEN", 0x00, 0x85};
  codes[3] = {"C86", 0x00, 0x86};
  codes[4] = {"C88", 0x00, 0x88};
  codes[5] = {"C89", 0x00, 0x89};
}

// NVS keys are short and stable; save each code as a separate string.
void saveCodes() {
  prefs.begin("irremote", false);
  prefs.putUChar("count", codeCount);
  for (uint8_t i = 0; i < MAX_CODES; ++i) {
    String key = "code" + String(i);
    if (i < codeCount) {
      String value = codes[i].name + "|" + String(codes[i].address, HEX) +
                     "|" + String(codes[i].command, HEX);
      prefs.putString(key.c_str(), value);
    } else {
      prefs.remove(key.c_str());
    }
  }
  prefs.end();
  Serial.printf("[NVS] Saved %u IR codes\n", codeCount);
}

void loadCodes() {
  prefs.begin("irremote", true);
  uint8_t count = prefs.getUChar("count", 0);
  if (count == 0 || count > MAX_CODES) {
    prefs.end();
    setDefaults();
    saveCodes();
    Serial.println("[NVS] Initialized default IR codes");
    return;
  }
  codeCount = 0;
  for (uint8_t i = 0; i < count; ++i) {
    String key = "code" + String(i);
    String entry = prefs.getString(key.c_str(), "");
    int p = entry.indexOf('|');
    int q = entry.indexOf('|', p + 1);
    if (p <= 0 || q <= p) continue;
    String name = entry.substring(0, p);
    uint16_t addr = (uint16_t)strtoul(entry.substring(p + 1, q).c_str(), nullptr, 16);
    uint8_t cmd = (uint8_t)strtoul(entry.substring(q + 1).c_str(), nullptr, 16);
    codes[codeCount++] = {name, addr, cmd};
  }
  prefs.end();
  if (codeCount == 0) {
    setDefaults();
    saveCodes();
  }
  Serial.printf("[NVS] Loaded %u IR codes\n", codeCount);
}

String escapeHTML(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += s[i];
    }
  }
  return out;
}

String hexByte(uint8_t b) {
  char buffer[3];
  snprintf(buffer, sizeof(buffer), "%02X", b);
  return String(buffer);
}

bool parseHex(const String &value, unsigned long maxValue, unsigned long &result) {
  if (value.isEmpty() || value.length() > 4) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isxdigit((unsigned char)value[i])) return false;
  }
  result = strtoul(value.c_str(), nullptr, 16);
  return result <= maxValue;
}

bool authorized() {
  if (server.authenticate(WEB_USER, WEB_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

String pageStart(const String &title) {
  return "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta charset='utf-8'><title>" + escapeHTML(title) + "</title>"
         "<style>body{font-family:system-ui;background:#151922;color:#fff;max-width:650px;margin:auto;padding:18px}"
         "a{color:#93c5fd}button,input{font:inherit;padding:12px;border-radius:8px;border:1px solid #64748b}"
         "button{background:#334155;color:white;cursor:pointer}input{max-width:95%}"
         ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.item{background:#222b3a;padding:12px;border-radius:12px}"
         ".off{background:#b91c1c}.green{background:#15803d}small{color:#cbd5e1}"
         "form{margin:8px 0}label{display:block;margin:8px 0 3px}</style></head><body>"
         "<h1>" + escapeHTML(title) + "</h1><p><a href='/'>Remote</a> · <a href='/manage'>Manage</a> · "
         "<a href='/update'>Firmware OTA</a> · <a href='/status'>Status</a></p>";
}

void handleHome() {
  if (!authorized()) return;
  String html = pageStart("ESP32 LED Remote") + "<div class='grid'>";
  for (uint8_t i = 0; i < codeCount; ++i) {
    String cls = codes[i].command == 0x82 ? "off" : (codes[i].command == 0x85 ? "green" : "");
    html += "<div class='item'><button class='" + cls + "' style='width:100%' onclick='send(" + String(i) + ")'>" +
            escapeHTML(codes[i].name) + "</button><small>Address " + String(codes[i].address, HEX) +
            " / C" + hexByte(codes[i].command) + "</small></div>";
  }
  html += "</div><p id='msg'>Ready</p><script>async function send(i){let m=document.getElementById('msg');"
          "try{let r=await fetch('/send?id='+i,{method:'POST'});m.textContent=await r.text();}"
          "catch(e){m.textContent='Request failed';}}</script></body></html>";
  server.send(200, "text/html", html);
}

void handleSend() {
  if (!authorized()) return;
  String arg = server.arg("id");
  if (arg.isEmpty()) { server.send(400, "text/plain", "Missing ID"); return; }
  for (char c : arg) if (!isdigit((unsigned char)c)) { server.send(400, "text/plain", "Invalid ID"); return; }
  int i = arg.toInt();
  if (i < 0 || i >= codeCount) { server.send(404, "text/plain", "Unknown code"); return; }
  transmit(codes[i].address, codes[i].command);
  server.send(200, "text/plain", "Sent " + codes[i].name);
}

void handleManage() {
  if (!authorized()) return;
  String html = pageStart("Manage IR Codes");
  html += "<p>Saved in NVS. Maximum " + String(MAX_CODES) + " commands.</p>";
  for (uint8_t i = 0; i < codeCount; ++i) {
    html += "<div class='item'><b>" + escapeHTML(codes[i].name) + "</b> (A" +
            String(codes[i].address, HEX) + " / C" + hexByte(codes[i].command) + ")";
    html += "<form method='POST' action='/delete'><input type='hidden' name='id' value='" + String(i) +
            "'><button type='submit'>Delete</button></form></div>";
  }
  html += "<h2>Add or rename code</h2><p>Adding an existing address/command updates its name.</p>"
          "<form method='POST' action='/add'><label>Name</label><input name='name' maxlength='24' required>"
          "<label>NEC address (hex, e.g. 00)</label><input name='address' value='00' maxlength='4' required>"
          "<label>NEC command (hex, e.g. 85)</label><input name='command' maxlength='2' required>"
          "<p><button type='submit'>Save code</button></p></form></body></html>";
  server.send(200, "text/html", html);
}

void handleAdd() {
  if (!authorized()) return;
  String name = server.arg("name");
  name.trim();
  unsigned long address, command;
  if (name.isEmpty() || name.length() > 24 ||
      !parseHex(server.arg("address"), 0xFFFF, address) ||
      !parseHex(server.arg("command"), 0xFF, command)) {
    server.send(400, "text/plain", "Invalid name, address, or command");
    return;
  }
  for (uint8_t i = 0; i < codeCount; ++i) {
    if (codes[i].address == address && codes[i].command == command) {
      codes[i].name = name;
      saveCodes();
      server.sendHeader("Location", "/manage"); server.send(303); return;
    }
  }
  if (codeCount >= MAX_CODES) { server.send(409, "text/plain", "Storage list full"); return; }
  codes[codeCount++] = {name, (uint16_t)address, (uint8_t)command};
  saveCodes();
  server.sendHeader("Location", "/manage"); server.send(303);
}

void handleDelete() {
  if (!authorized()) return;
  String arg = server.arg("id");
  if (arg.isEmpty()) { server.send(400, "text/plain", "Missing ID"); return; }
  for (char c : arg) if (!isdigit((unsigned char)c)) { server.send(400, "text/plain", "Invalid ID"); return; }
  int index = arg.toInt();
  if (index < 0 || index >= codeCount) { server.send(404, "text/plain", "Unknown ID"); return; }
  for (int i = index; i < codeCount - 1; ++i) codes[i] = codes[i + 1];
  --codeCount;
  nextCode = 0;
  saveCodes();
  server.sendHeader("Location", "/manage"); server.send(303);
}

void printNetwork() {
  Serial.println("\n========== Wi-Fi Diagnostics ==========");
  Serial.print("Station MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("Status: "); Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    Serial.print("Gateway: "); Serial.println(WiFi.gatewayIP());
    Serial.print("Subnet: "); Serial.println(WiFi.subnetMask());
    Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    Serial.print("Web page: http://"); Serial.println(WiFi.localIP());
    Serial.print("OTA page: http://"); Serial.print(WiFi.localIP()); Serial.println("/update");
  }
  Serial.println("======================================\n");
}

void handleStatus() {
  if (!authorized()) return;
  String html = pageStart("Network Status") + "<pre>";
  html += "MAC: " + WiFi.macAddress() + "\n";
  html += "Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\n";
  if (WiFi.status() == WL_CONNECTED) {
    html += "SSID: " + escapeHTML(WiFi.SSID()) + "\n";
    html += "IP: " + WiFi.localIP().toString() + "\n";
    html += "Gateway: " + WiFi.gatewayIP().toString() + "\n";
    html += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
  }
  html += "Saved codes: " + String(codeCount) + "\nUptime: " + String(millis() / 1000) + " seconds</pre></body></html>";
  server.send(200, "text/html", html);
}

void handleUpdatePage() {
  if (!authorized()) return;
  String html = pageStart("Firmware OTA Update") +
    "<p>Upload an ESP32 firmware .bin exported by Arduino IDE. Keep power connected during the update.</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin' required>"
    "<p><button type='submit'>Upload firmware</button></p></form></body></html>";
  server.send(200, "text/html", html);
}

void handleUpdateUpload() {
  // Basic auth must be checked before processing the upload body.
  if (!server.authenticate(WEB_USER, WEB_PASSWORD)) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    updateSuccess = false;
    Serial.printf("[OTA] Starting firmware upload: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      updateSuccess = true;
      Serial.printf("[OTA] Upload complete: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[OTA] Upload aborted");
  }
}

void handleUpdateComplete() {
  if (!authorized()) return;
  bool ok = updateSuccess && !Update.hasError();
  server.send(ok ? 200 : 500, "text/plain", ok ? "Update complete. Rebooting..." : "Firmware update failed. Check Serial Monitor.");
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void startWebServer() {
  server.on("/", HTTP_GET, handleHome);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/manage", HTTP_GET, handleManage);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
  server.onNotFound([]() { if (authorized()) server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("[WEB] HTTP server started on port 80");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  irsend.begin();
  loadCodes();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.print("[Wi-Fi] Station MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("[Wi-Fi] Connecting to SSID: "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();

  // Don't block BOOT button operation if Wi-Fi is unavailable.
  startWebServer();
  Serial.println("[IR] BOOT short press: next code; long press (2s): OFF C82");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    if (!wasConnected) {
      wasConnected = true;
      Serial.println("[Wi-Fi] Connected!");
      printNetwork();
    }
  } else {
    if (wasConnected) {
      wasConnected = false;
      Serial.println("[Wi-Fi] Connection lost");
    }
    if (millis() - lastWiFiAttempt >= WIFI_RETRY_MS) {
      lastWiFiAttempt = millis();
      Serial.println("[Wi-Fi] Reconnecting...");
      WiFi.reconnect();
    }
  }

  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !buttonDown) {
    buttonDown = true;
    pressedAt = millis();
  } else if (!down && buttonDown) {
    buttonDown = false;
    unsigned long held = millis() - pressedAt;
    if (held >= 2000) {
      Serial.println("[BUTTON] OFF C82");
      transmit(0x00, 0x82);
    } else if (held >= 30 && codeCount > 0) {
      Serial.printf("[BUTTON] %s\n", codes[nextCode].name.c_str());
      transmit(codes[nextCode].address, codes[nextCode].command);
      nextCode = (nextCode + 1) % codeCount;
    }
  }
  delay(10);
}
