// SPDX-License-Identifier: GPL-3.0-only

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "display_ui.h"
#include "picaso_protocol.h"

namespace {

constexpr uint32_t kWifiConnectTimeoutMs = 25000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kStatePollIntervalMs = 2500;
constexpr uint32_t kDiscoveryIntervalMs = 30000;
constexpr uint32_t kConnectionStaleMs = 8000;

DisplayUI displayUi;
PicasoClient printerClient;
PrinterSnapshot liveSnapshot;
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

String wifiSsid;
String wifiPassword;
String printerIpText;
String setupApName;
String setupApPassword;
IPAddress printerIp;
uint16_t screenRotationDegrees = 0;
int16_t timezoneOffsetMinutes = 180;

bool portalMode = false;
bool webServerStarted = false;
bool udpStarted = false;
bool mdnsStarted = false;
bool wifiDisconnectTimerActive = false;
uint32_t nextWifiRetry = 0;
uint32_t wifiDisconnectedSince = 0;
uint32_t lastWifiAnimation = 0;
uint32_t nextStatePoll = 0;
uint32_t nextDiscovery = 0;
uint32_t lastGoodState = 0;
uint32_t lastDraw = 0;
uint32_t restartAt = 0;

String htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '&') output += "&amp;";
    else if (c == '<') output += "&lt;";
    else if (c == '>') output += "&gt;";
    else if (c == '"') output += "&quot;";
    else if (c == '\'') output += "&#39;";
    else output += c;
  }
  return output;
}

void loadSettings() {
  preferences.begin("picaso", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  printerIpText = preferences.getString("printer", "");
  screenRotationDegrees = preferences.getUShort("rotate", 0);
  timezoneOffsetMinutes = preferences.getShort("tzmin", 180);
  preferences.end();
  if (screenRotationDegrees != 0 && screenRotationDegrees != 90 &&
      screenRotationDegrees != 180 && screenRotationDegrees != 270) {
    screenRotationDegrees = 0;
  }
  if (timezoneOffsetMinutes < -720 || timezoneOffsetMinutes > 840 ||
      timezoneOffsetMinutes % 15 != 0) {
    timezoneOffsetMinutes = 180;
  }
  if (printerIpText.length()) printerIp.fromString(printerIpText);
}

void saveSettings(const String& newSsid, const String& newPassword,
                  const String& newPrinterIp, uint16_t newRotationDegrees,
                  int16_t newTimezoneOffsetMinutes) {
  preferences.begin("picaso", false);
  preferences.putString("ssid", newSsid);
  preferences.putString("pass", newPassword);
  preferences.putString("printer", newPrinterIp);
  preferences.putUShort("rotate", newRotationDegrees);
  preferences.putShort("tzmin", newTimezoneOffsetMinutes);
  preferences.end();

  wifiSsid = newSsid;
  wifiPassword = newPassword;
  printerIpText = newPrinterIp;
  screenRotationDegrees = newRotationDegrees;
  timezoneOffsetMinutes = newTimezoneOffsetMinutes;
  printerIp.fromString(printerIpText);
}

void appendRotationOption(String& html, uint16_t degrees) {
  html += F("<option value='");
  html += String(degrees);
  html += F("'");
  if (screenRotationDegrees == degrees) html += F(" selected");
  html += F(">");
  html += String(degrees);
  html += F("°</option>");
}

void appendTimezoneOption(String& html, int16_t offsetMinutes) {
  const int16_t magnitude = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  char label[40];
  snprintf(label, sizeof(label), "UTC%c%02d:%02d%s",
           offsetMinutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60,
           offsetMinutes == 180 ? " (Москва)" : "");

  html += F("<option value='");
  html += String(offsetMinutes);
  html += F("'");
  if (timezoneOffsetMinutes == offsetMinutes) html += F(" selected");
  html += F(">");
  html += label;
  html += F("</option>");
}

String posixTimezone() {
  if (timezoneOffsetMinutes == 0) return String("UTC0");
  const int16_t magnitude = timezoneOffsetMinutes < 0
                                ? -timezoneOffsetMinutes
                                : timezoneOffsetMinutes;
  char timezone[20];
  snprintf(timezone, sizeof(timezone), "UTC%c%d:%02d",
           timezoneOffsetMinutes > 0 ? '-' : '+', magnitude / 60, magnitude % 60);
  return String(timezone);
}

void formatLocalClock(String& clockTime, String& clockDate) {
  clockTime = "--:--";
  clockDate = "--.--.----";
  const time_t epoch = time(nullptr);
  if (epoch < 1704067200) return;

  struct tm localTime;
  if (localtime_r(&epoch, &localTime) == nullptr) return;
  char timeBuffer[8];
  char dateBuffer[16];
  if (strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", &localTime) == 0 ||
      strftime(dateBuffer, sizeof(dateBuffer), "%d.%m.%Y", &localTime) == 0) {
    return;
  }
  clockTime = timeBuffer;
  clockDate = dateBuffer;
}

String configPage() {
  String html;
  html.reserve(9000);
  html += F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Picaso Monitor</title><style>");
  html += F("body{font:16px system-ui,sans-serif;max-width:520px;margin:32px auto;padding:0 16px;background:#101820;color:#eef5f7}");
  html += F("h1{font-size:22px;color:#36d1dc}label{display:block;margin:18px 0 6px}input,select{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #425466;background:#182531;color:white;font-size:16px}button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:8px;background:#23b5a8;color:#061014;font-size:16px;font-weight:700}small{display:block;color:#adbdc7;line-height:1.5}.hint{margin-top:12px}.hint+.hint{margin-top:14px}.card{padding:16px;border:1px solid #344856;border-radius:12px;background:#15222d}");
  html += F("</style></head><body><h1>Picaso Monitor</h1><div class='card'>");
  html += F("<p>Введите Wi-Fi и адрес принтера в локальной сети.</p><form method='post' action='/save'>");
  html += F("<label>Имя Wi-Fi сети</label><input name='ssid' maxlength='32' required value='");
  html += htmlEscape(wifiSsid);
  html += F("'><label>Пароль Wi-Fi</label><input name='password' type='password' maxlength='63' autocomplete='new-password' placeholder='Оставьте пустым, чтобы сохранить текущий пароль'>");
  html += F("<label>IP принтера PICASO</label><input name='printer' inputmode='decimal' placeholder='192.168.1.50' required value='");
  html += htmlEscape(printerIpText);
  html += F("'><label>Поворот экрана</label><select name='rotation' required>");
  appendRotationOption(html, 0);
  appendRotationOption(html, 90);
  appendRotationOption(html, 180);
  appendRotationOption(html, 270);
  html += F("</select><label>Часовой пояс (смещение UTC)</label><select name='timezone' required>");
  for (int16_t offset = -720; offset <= 840; offset += 15) {
    appendTimezoneOption(html, offset);
  }
  html += F("</select><small class='hint'>Москва: UTC+03:00. Время синхронизируется через интернет. Летний перевод часов не применяется автоматически.</small><small class='hint'>ESP32-C3 подключается к сети Wi-Fi 2,4 ГГц.<br>Для принтера желательно закрепить IP в настройках роутера.</small><button type='submit'>Сохранить и перезапустить</button></form></div></body></html>");
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", configPage());
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  String newPrinterIp = server.arg("printer");
  String newRotationText = server.arg("rotation");
  String newTimezoneText = server.arg("timezone");
  newSsid.trim();
  newPrinterIp.trim();

  const bool validRotation = newRotationText == "0" || newRotationText == "90" ||
                             newRotationText == "180" || newRotationText == "270";
  const uint16_t newRotationDegrees = static_cast<uint16_t>(newRotationText.toInt());
  const long parsedTimezoneOffset = newTimezoneText.toInt();
  const bool validTimezone = String(parsedTimezoneOffset) == newTimezoneText &&
                             parsedTimezoneOffset >= -720 && parsedTimezoneOffset <= 840 &&
                             parsedTimezoneOffset % 15 == 0;
  const int16_t newTimezoneOffsetMinutes = static_cast<int16_t>(parsedTimezoneOffset);

  IPAddress parsedPrinterIp;
  if (newSsid.length() == 0 || newSsid.length() > 32 ||
      newPassword.length() > 63 || !parsedPrinterIp.fromString(newPrinterIp) ||
      !validRotation || !validTimezone) {
    server.send(400, "text/html; charset=utf-8",
                "<meta charset='utf-8'><p>Проверьте имя Wi-Fi, адрес принтера, поворот экрана и часовой пояс.</p><a href='/'>Назад</a>");
    return;
  }

  if (newPassword.length() == 0 && newSsid == wifiSsid) {
    newPassword = wifiPassword;
  }
  saveSettings(newSsid, newPassword, newPrinterIp, newRotationDegrees,
               newTimezoneOffsetMinutes);
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><meta http-equiv='refresh' content='3;url=/'><h2>Сохранено</h2><p>Устройство перезапускается и подключается к сети.</p>");
  restartAt = millis() + 1800;
}

void startWebServer() {
  if (webServerStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  webServerStarted = true;
}

void startSetupPortal() {
  portalMode = true;
  udpStarted = false;
  WiFi.mode(WIFI_AP);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String suffix = mac.length() >= 6 ? mac.substring(mac.length() - 6) : String("C3S012");
  suffix.toUpperCase();
  setupApName = "Picaso-" + suffix;
  setupApPassword = "px" + suffix;

  WiFi.softAP(setupApName.c_str(), setupApPassword.c_str());
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  startWebServer();
  displayUi.showSetupPortal(setupApName, setupApPassword);
  Serial.printf("Setup portal: SSID=%s, IP=192.168.4.1\n", setupApName.c_str());
}

bool connectStation(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("picaso-monitor");
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  const uint32_t startedAt = millis();
  uint32_t lastProgress = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    if (millis() - lastProgress >= 100) {
      const uint8_t animationStep = static_cast<uint8_t>((millis() / 300) % 6);
      displayUi.showWifiConnecting(wifiSsid, animationStep);
      lastProgress = millis();
    }
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startStationServices() {
  portalMode = false;
  const String timezone = posixTimezone();
  configTzTime(timezone.c_str(), "pool.ntp.org", "time.google.com",
               "time.cloudflare.com");
  printerClient.setPrinterIP(printerIp);
  udpStarted = printerClient.begin();
  startWebServer();
  if (MDNS.begin("picaso-monitor")) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
  Serial.printf("Wi-Fi connected. ESP32 IP=%s, printer IP=%s\n",
                WiFi.localIP().toString().c_str(), printerIpText.c_str());
  if (udpStarted) printerClient.discover(1000);
  nextStatePoll = millis() + 100;
  nextDiscovery = millis() + kDiscoveryIntervalMs;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  loadSettings();
  displayUi.begin(screenRotationDegrees);

  if (wifiSsid.length() == 0 || !printerIpText.length() || printerIp == IPAddress()) {
    startSetupPortal();
    return;
  }

  if (!connectStation(kWifiConnectTimeoutMs)) {
    Serial.println("Wi-Fi connection timed out; opening setup portal.");
    startSetupPortal();
    return;
  }
  startStationServices();
}

void loop() {
  if (webServerStarted) server.handleClient();
  if (portalMode) dnsServer.processNextRequest();

  if (restartAt != 0 && static_cast<int32_t>(millis() - restartAt) >= 0) {
    ESP.restart();
  }

  if (portalMode) {
    if (millis() - lastDraw >= 1000) {
      displayUi.showSetupPortal(setupApName, setupApPassword);
      lastDraw = millis();
    }
    delay(1);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    const uint32_t now = millis();
    if (!wifiDisconnectTimerActive) {
      wifiDisconnectTimerActive = true;
      wifiDisconnectedSince = now;
      lastWifiAnimation = now - 300UL;
    }
    liveSnapshot.connected = false;
    if (now - wifiDisconnectedSince >= kWifiConnectTimeoutMs) {
      Serial.println("Wi-Fi reconnect timed out; opening setup portal.");
      startSetupPortal();
      lastDraw = now;
      delay(1);
      return;
    }
    if (now - lastWifiAnimation >= 300UL) {
      const uint8_t animationStep = static_cast<uint8_t>((now / 300UL) % 6UL);
      displayUi.showWifiConnecting(wifiSsid, animationStep);
      lastWifiAnimation = now;
    }
    if (static_cast<int32_t>(now - nextWifiRetry) >= 0) {
      WiFi.reconnect();
      nextWifiRetry = now + kWifiRetryIntervalMs;
    }
  } else {
    wifiDisconnectTimerActive = false;
    if (!udpStarted) {
      udpStarted = printerClient.begin();
      printerClient.setPrinterIP(printerIp);
      nextStatePoll = millis();
    }
    if (udpStarted && static_cast<int32_t>(millis() - nextDiscovery) >= 0) {
      printerClient.discover(500);
      nextDiscovery = millis() + kDiscoveryIntervalMs;
    }
    if (udpStarted && static_cast<int32_t>(millis() - nextStatePoll) >= 0) {
      PrinterSnapshot nextSnapshot;
      if (printerClient.poll(nextSnapshot)) {
        nextSnapshot.connected = true;
        liveSnapshot = nextSnapshot;
        lastGoodState = millis();
      } else if (lastGoodState == 0 || millis() - lastGoodState > kConnectionStaleMs) {
        liveSnapshot.connected = false;
      }
      nextStatePoll = millis() + kStatePollIntervalMs;
    }
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastDraw >= 1000) {
    String clockTime;
    String clockDate;
    formatLocalClock(clockTime, clockDate);
    displayUi.showDashboard(printerClient.discovery(), liveSnapshot,
                            printerIpText, WiFi.localIP().toString(),
                            WiFi.status() == WL_CONNECTED, clockTime, clockDate);
    lastDraw = millis();
  }
  delay(1);
}
