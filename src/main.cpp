// SPDX-License-Identifier: GPL-3.0-only

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "display_ui.h"
#include "yandex_weather_root_ca.h"
#include "picaso_protocol.h"

namespace {

constexpr uint32_t kWifiConnectTimeoutMs = 25000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kStatePollIntervalMs = 2500;
constexpr uint32_t kDiscoveryIntervalMs = 30000;
constexpr uint32_t kConnectionStaleMs = 8000;
constexpr uint32_t kWeatherClockRetryMs = 60000UL;
constexpr uint32_t kWeatherManualCooldownSeconds = 60UL * 60UL;
constexpr uint32_t kWeatherFailedManualCooldownSeconds = 5UL * 60UL;
constexpr uint8_t kWeatherUpdateHours[] = {7, 10, 16};

DisplayUI displayUi;
PicasoClient printerClient;
PrinterSnapshot liveSnapshot;
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

String wifiSsid;
String wifiPassword;
String printerIpText;
String weatherCity;
String weatherApiKey;
String setupApName;
String setupApPassword;
IPAddress printerIp;
uint16_t screenRotationDegrees = 0;
int16_t timezoneOffsetMinutes = 180;
float weatherLatitude = 0.0f;
float weatherLongitude = 0.0f;
WeatherSnapshot weatherSnapshot;

bool portalMode = false;
bool webServerStarted = false;
bool udpStarted = false;
bool mdnsStarted = false;
bool wifiDisconnectTimerActive = false;
bool weatherUpdateRequested = false;
uint32_t nextWifiRetry = 0;
uint32_t wifiDisconnectedSince = 0;
uint32_t lastWifiAnimation = 0;
uint32_t nextStatePoll = 0;
uint32_t nextDiscovery = 0;
uint32_t lastGoodState = 0;
uint32_t lastDraw = 0;
uint32_t restartAt = 0;
uint32_t nextWeatherUpdate = 0;
uint32_t weatherLastRequestEpoch = 0;
bool weatherLastRequestSucceeded = false;

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
  weatherCity = preferences.getString("wcity", "");
  weatherApiKey = preferences.getString("wkey", "");
  weatherLatitude = preferences.getFloat("wlat", 0.0f);
  weatherLongitude = preferences.getFloat("wlon", 0.0f);
  weatherLastRequestEpoch = preferences.getULong("wreq", 0);
  weatherLastRequestSucceeded = preferences.getBool("wok", false);
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
                  int16_t newTimezoneOffsetMinutes,
                  const String& newWeatherCity, float newWeatherLatitude,
                  float newWeatherLongitude, const String& newWeatherApiKey) {
  preferences.begin("picaso", false);
  preferences.putString("ssid", newSsid);
  preferences.putString("pass", newPassword);
  preferences.putString("printer", newPrinterIp);
  preferences.putUShort("rotate", newRotationDegrees);
  preferences.putShort("tzmin", newTimezoneOffsetMinutes);
  preferences.putString("wcity", newWeatherCity);
  preferences.putFloat("wlat", newWeatherLatitude);
  preferences.putFloat("wlon", newWeatherLongitude);
  preferences.putString("wkey", newWeatherApiKey);
  preferences.end();

  wifiSsid = newSsid;
  wifiPassword = newPassword;
  printerIpText = newPrinterIp;
  screenRotationDegrees = newRotationDegrees;
  timezoneOffsetMinutes = newTimezoneOffsetMinutes;
  weatherCity = newWeatherCity;
  weatherLatitude = newWeatherLatitude;
  weatherLongitude = newWeatherLongitude;
  weatherApiKey = newWeatherApiKey;
  printerIp.fromString(printerIpText);
}

bool parseFiniteFloat(const String& text, float& value) {
  if (text.length() == 0) return false;
  char* end = nullptr;
  value = strtof(text.c_str(), &end);
  return end != text.c_str() && *end == '\0' && isfinite(value);
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

void rememberWeatherRequest(uint32_t epoch) {
  weatherLastRequestEpoch = epoch;
  weatherLastRequestSucceeded = false;
  preferences.begin("picaso", false);
  preferences.putULong("wreq", epoch);
  preferences.putBool("wok", false);
  preferences.end();
}

void rememberWeatherRequestResult(bool succeeded) {
  weatherLastRequestSucceeded = succeeded;
  preferences.begin("picaso", false);
  preferences.putBool("wok", succeeded);
  preferences.end();
}

time_t nextWeatherUpdateEpoch(time_t afterEpoch) {
  struct tm localTime;
  if (localtime_r(&afterEpoch, &localTime) == nullptr) return 0;

  for (const uint8_t hour : kWeatherUpdateHours) {
    struct tm candidate = localTime;
    candidate.tm_hour = hour;
    candidate.tm_min = 0;
    candidate.tm_sec = 0;
    candidate.tm_isdst = -1;
    const time_t candidateEpoch = mktime(&candidate);
    if (candidateEpoch > afterEpoch) return candidateEpoch;
  }

  struct tm tomorrow = localTime;
  tomorrow.tm_mday += 1;
  tomorrow.tm_hour = kWeatherUpdateHours[0];
  tomorrow.tm_min = 0;
  tomorrow.tm_sec = 0;
  tomorrow.tm_isdst = -1;
  return mktime(&tomorrow);
}

bool fetchWeatherJson(const String& requestBody, DynamicJsonDocument& document) {
  WiFiClientSecure secureClient;
  secureClient.setCACert(kYandexWeatherRoot);
  secureClient.setHandshakeTimeout(8);
  secureClient.setTimeout(15000);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(secureClient, "https://api.weather.yandex.ru/graphql/query")) {
    Serial.println("Weather: HTTP client initialization failed.");
    return false;
  }

  http.addHeader("X-Yandex-Weather-Key", weatherApiKey);
  http.addHeader("Content-Type", "application/json");
  const int responseCode = http.POST(requestBody);
  if (responseCode != HTTP_CODE_OK) {
    if (responseCode < 0) {
      char tlsError[128] = {};
      const int tlsErrorCode = secureClient.lastError(tlsError, sizeof(tlsError));
      Serial.printf("Weather: request failed (%d), TLS detail (%d): %s\n",
                    responseCode, tlsErrorCode, tlsError);
    } else {
      Serial.printf("Weather: HTTP server returned status %d.\n", responseCode);
    }
    http.end();
    return false;
  }

  const DeserializationError jsonError = deserializeJson(document, http.getStream());
  http.end();
  if (jsonError) {
    Serial.printf("Weather: JSON parse failed: %s\n", jsonError.c_str());
    return false;
  }
  return true;
}

String yandexWeatherCondition(const char* condition) {
  if (condition == nullptr) return "";
  if (strcmp(condition, "CLEAR") == 0) return "Ясно";
  if (strcmp(condition, "PARTLY_CLOUDY") == 0) return "Облачно с прояснениями";
  if (strcmp(condition, "CLOUDY") == 0) return "Облачно";
  if (strcmp(condition, "OVERCAST") == 0) return "Пасмурно";
  if (strcmp(condition, "LIGHT_RAIN") == 0) return "Неб. дождь";
  if (strcmp(condition, "RAIN") == 0) return "Дождь";
  if (strcmp(condition, "HEAVY_RAIN") == 0 || strcmp(condition, "SHOWERS") == 0) {
    return "Ливень";
  }
  if (strcmp(condition, "SLEET") == 0) return "Дождь + снег";
  if (strcmp(condition, "LIGHT_SNOW") == 0) return "Неб. снег";
  if (strcmp(condition, "SNOW") == 0) return "Снег";
  if (strcmp(condition, "SNOWFALL") == 0) return "Снегопад";
  if (strcmp(condition, "HAIL") == 0) return "Град";
  if (strcmp(condition, "THUNDERSTORM") == 0) return "Гроза";
  if (strcmp(condition, "THUNDERSTORM_WITH_RAIN") == 0) return "Гроза + дождь";
  if (strcmp(condition, "THUNDERSTORM_WITH_HAIL") == 0) return "Гроза + град";
  return "Погода";
}

WeatherIcon yandexWeatherIcon(const char* condition) {
  if (condition == nullptr) return WeatherIcon::Cloud;
  if (strcmp(condition, "CLEAR") == 0) return WeatherIcon::Sun;
  if (strcmp(condition, "PARTLY_CLOUDY") == 0) return WeatherIcon::PartlyCloudy;
  if (strcmp(condition, "LIGHT_RAIN") == 0 || strcmp(condition, "RAIN") == 0 ||
      strcmp(condition, "HEAVY_RAIN") == 0 || strcmp(condition, "SHOWERS") == 0 ||
      strcmp(condition, "SLEET") == 0) {
    return WeatherIcon::Rain;
  }
  if (strcmp(condition, "LIGHT_SNOW") == 0 || strcmp(condition, "SNOW") == 0 ||
      strcmp(condition, "SNOWFALL") == 0 || strcmp(condition, "HAIL") == 0) {
    return WeatherIcon::Snowflake;
  }
  if (strcmp(condition, "THUNDERSTORM") == 0 ||
      strcmp(condition, "THUNDERSTORM_WITH_RAIN") == 0 ||
      strcmp(condition, "THUNDERSTORM_WITH_HAIL") == 0) {
    return WeatherIcon::Lightning;
  }
  return WeatherIcon::Cloud;
}

bool fetchWeather(WeatherSnapshot& result) {
  if (weatherCity.length() == 0 || weatherApiKey.length() == 0 ||
      WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String requestBody = String("{\"query\":\"{ serverTime weatherByPoint(request: { lat: ") +
                       String(weatherLatitude, 5) + String(", lon: ") +
                       String(weatherLongitude, 5) +
                       String(" }) { now { temperature condition } forecast { days { summary { day { maxTemperature } } } } } }\"}");

  DynamicJsonDocument document(4096);
  if (!fetchWeatherJson(requestBody, document)) return false;

  const JsonVariantConst weatherByPoint = document["data"]["weatherByPoint"];
  const JsonVariantConst currentTemperature = weatherByPoint["now"]["temperature"];
  const char* condition = weatherByPoint["now"]["condition"];
  const JsonVariantConst daysValue = weatherByPoint["forecast"]["days"];
  float tomorrowMaximum = 0.0f;
  if (currentTemperature.isNull() || condition == nullptr ||
      !daysValue.is<JsonArrayConst>() || daysValue.as<JsonArrayConst>().size() < 2) {
    Serial.println("Weather: response did not include current conditions and tomorrow's forecast.");
    return false;
  }

  const float current = currentTemperature.as<float>();
  const JsonVariantConst tomorrowMaximumValue =
      daysValue[1]["summary"]["day"]["maxTemperature"];
  if (tomorrowMaximumValue.isNull()) return false;
  tomorrowMaximum = tomorrowMaximumValue.as<float>();
  if (!isfinite(current) || !isfinite(tomorrowMaximum)) {
    return false;
  }

  result.valid = true;
  result.currentTemperatureC = static_cast<int16_t>(lroundf(current));
  result.tomorrowMaximumC = static_cast<int16_t>(lroundf(tomorrowMaximum));
  result.condition = yandexWeatherCondition(condition);
  result.icon = yandexWeatherIcon(condition);
  return true;
}

void updateWeatherIfDue() {
  if (weatherCity.length() == 0 || weatherApiKey.length() == 0 ||
      WiFi.status() != WL_CONNECTED) return;
  const bool manualUpdate = weatherUpdateRequested;
  const uint32_t now = millis();
  if (!manualUpdate && nextWeatherUpdate != 0 &&
      static_cast<int32_t>(now - nextWeatherUpdate) < 0) {
    return;
  }

  // TLS certificate validation requires the device clock to be synchronized.
  if (time(nullptr) < 1704067200) {
    nextWeatherUpdate = now + kWeatherClockRetryMs;
    return;
  }

  const uint32_t currentEpoch = static_cast<uint32_t>(time(nullptr));
  if (!manualUpdate && weatherLastRequestEpoch != 0) {
    const time_t scheduledEpoch = nextWeatherUpdateEpoch(weatherLastRequestEpoch);
    if (scheduledEpoch > 0 && currentEpoch < static_cast<uint32_t>(scheduledEpoch)) {
      const uint32_t remaining =
          static_cast<uint32_t>(scheduledEpoch) - currentEpoch;
      nextWeatherUpdate = now + remaining * 1000UL;
      return;
    }
  }

  weatherUpdateRequested = false;
  // Persist the request time so restarts cannot exceed the free API quota.
  rememberWeatherRequest(currentEpoch);

  WeatherSnapshot freshWeather;
  if (fetchWeather(freshWeather)) {
    rememberWeatherRequestResult(true);
    weatherSnapshot = freshWeather;
    Serial.printf("Weather updated for %s: %d C, %s\n",
                  weatherCity.c_str(), weatherSnapshot.currentTemperatureC,
                  weatherSnapshot.condition.c_str());
    const time_t nextUpdateEpoch = nextWeatherUpdateEpoch(currentEpoch);
    nextWeatherUpdate = nextUpdateEpoch > 0
                            ? millis() + static_cast<uint32_t>(
                                             nextUpdateEpoch - currentEpoch) * 1000UL
                            : millis() + kWeatherClockRetryMs;
  } else {
    rememberWeatherRequestResult(false);
    weatherSnapshot = WeatherSnapshot();
    Serial.println("Weather update failed; retrying at the next scheduled update.");
    const time_t nextUpdateEpoch = nextWeatherUpdateEpoch(currentEpoch);
    nextWeatherUpdate = nextUpdateEpoch > 0
                            ? millis() + static_cast<uint32_t>(
                                             nextUpdateEpoch - currentEpoch) * 1000UL
                            : millis() + kWeatherClockRetryMs;
  }
}

String configPage() {
  String html;
  html.reserve(13000);
  html += F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Picaso Monitor</title><style>");
  html += F("body{font:16px system-ui,sans-serif;max-width:520px;margin:32px auto;padding:0 16px;background:#101820;color:#eef5f7}");
  html += F("h1{font-size:22px;color:#36d1dc}label{display:block;margin:18px 0 6px}input,select{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #425466;background:#182531;color:white;font-size:16px}button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:8px;background:#23b5a8;color:#061014;font-size:16px;font-weight:700}.city-search{display:flex;gap:8px}.city-search input{min-width:0;flex:1}.city-search button{width:auto;flex:none;margin:0;padding:0 14px}small{display:block;color:#adbdc7;line-height:1.5}.hint{margin-top:12px}.hint+.hint{margin-top:14px}.card{padding:16px;border:1px solid #344856;border-radius:12px;background:#15222d}a{color:#36d1dc}");
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
  html += F("</select><label for='weatherSearch'>Город для экрана погоды</label><div class='city-search'><input id='weatherSearch' maxlength='80' placeholder='Например, Тула'><button id='searchCity' type='button'>Найти</button></div><select id='weatherResults'><option value=''>Сначала найдите город</option></select><input type='hidden' name='weatherCity' id='weatherCity' value='");
  html += htmlEscape(weatherCity);
  html += F("'><input type='hidden' name='weatherLat' id='weatherLat' value='");
  html += String(weatherLatitude, 5);
  html += F("'><input type='hidden' name='weatherLon' id='weatherLon' value='");
  html += String(weatherLongitude, 5);
  html += F("'><small id='weatherHelp' class='hint'>");
  if (weatherCity.length()) {
    html += F("Сейчас выбрано: ");
    html += htmlEscape(weatherCity);
    html += F(". Новый выбор заменит сохранённый город.");
  } else {
    html += F("Город не выбран. Экран погоды можно настроить позже.");
  }
  html += F("</small><label for='weatherApiKey'>Бесплатный ключ Яндекс Погоды</label><input id='weatherApiKey' name='weatherApiKey' type='password' maxlength='200' autocomplete='new-password' placeholder='Оставьте пустым, чтобы сохранить текущий ключ'><small class='hint'>");
  html += weatherApiKey.length() ? F("Ключ уже сохранён на устройстве.") : F("Ключ не задан; экран погоды будет без данных.");
  html += F("</small><label style='display:flex;align-items:center;gap:8px'><input style='width:auto' type='checkbox' name='clearWeatherKey' value='1'>Удалить сохранённый ключ</label><small class='hint'>Создайте бесплатный ключ для личного умного дома в <a href='https://yandex.ru/pogoda/b2b/smarthome' target='_blank' rel='noopener'>Яндекс Погоде</a>. Ключ хранится в настройках ESP32. Бесплатный тариф — для личного некоммерческого использования: не более 30 запросов в сутки и 1000 в месяц. Погода обновляется в 07:00, 10:00 и 16:00 по часовому поясу устройства. Процент облачности API не передаёт; на экране показываются текущая температура, максимум на завтра и погодные условия словами.</small><button id='weatherUpdate' type='button'>Обновить погоду</button><small id='weatherUpdateStatus' class='hint'>После успешного обновления повторный запрос доступен через час; после ошибки — через 5 минут.</small><small class='hint'>Поиск названия города выполняется через Open-Meteo в браузере; запрос погоды с устройства идёт к Яндекс Погоде.</small><small class='hint'>Москва: UTC+03:00. Время синхронизируется через интернет. Летний перевод часов не применяется автоматически.</small><small class='hint'>ESP32-C3 подключается к сети Wi-Fi 2,4 ГГц.<br>Для принтера желательно закрепить IP в настройках роутера.</small><button type='submit'>Сохранить и перезапустить</button></form></div>");
  html += F(R"HTML(<script>
const searchButton=document.getElementById('searchCity');
const weatherUpdateButton=document.getElementById('weatherUpdate');
const weatherUpdateStatus=document.getElementById('weatherUpdateStatus');
const cityInput=document.getElementById('weatherSearch');
const resultSelect=document.getElementById('weatherResults');
const help=document.getElementById('weatherHelp');
const cityValue=document.getElementById('weatherCity');
const latValue=document.getElementById('weatherLat');
const lonValue=document.getElementById('weatherLon');
let found=[];
weatherUpdateButton.addEventListener('click',async()=>{
  weatherUpdateButton.disabled=true;
  weatherUpdateStatus.textContent='Отправляю запрос…';
  try{
    const response=await fetch('/weather/update',{method:'POST'});
    const message=await response.text();
    weatherUpdateStatus.textContent=message;
    if(response.status===202){
      window.setTimeout(()=>{weatherUpdateButton.disabled=false;},5000);
    }else{
      weatherUpdateButton.disabled=false;
    }
  }catch(error){
    weatherUpdateStatus.textContent='Не удалось отправить запрос. Проверьте соединение с устройством.';
    weatherUpdateButton.disabled=false;
  }
});
cityInput.addEventListener('input',()=>{cityValue.value='';latValue.value='';lonValue.value='';resultSelect.value='';});
searchButton.addEventListener('click',async()=>{
  const query=cityInput.value.trim();
  if(query.length<2){help.textContent='Введите не менее двух букв.';return;}
  help.textContent='Ищу город…';
  resultSelect.innerHTML='<option value="">Загрузка результатов…</option>';
  try{
    const url='https://geocoding-api.open-meteo.com/v1/search?name='+encodeURIComponent(query)+'&count=10&language=ru&format=json';
    const response=await fetch(url);
    if(!response.ok)throw new Error('HTTP '+response.status);
    const data=await response.json();
    found=data.results||[];
    resultSelect.innerHTML='';
    const prompt=document.createElement('option');prompt.value='';prompt.textContent=found.length?'Выберите населённый пункт':'Ничего не найдено';resultSelect.appendChild(prompt);
    found.forEach((place,index)=>{
      const option=document.createElement('option');option.value=String(index);
      option.textContent=[place.name,place.admin1,place.country].filter(Boolean).join(' · ');
      resultSelect.appendChild(option);
    });
    help.textContent=found.length?'Выберите нужный город и сохраните настройки.':'Попробуйте уточнить название или район.';
  }catch(error){resultSelect.innerHTML='<option value="">Не удалось найти город</option>';help.textContent='Проверьте подключение к интернету и повторите поиск.';}
});
resultSelect.addEventListener('change',()=>{
  const place=found[Number(resultSelect.value)];
  if(!place)return;
  cityValue.value=place.name;
  latValue.value=Number(place.latitude).toFixed(5);
  lonValue.value=Number(place.longitude).toFixed(5);
  help.textContent='Выбрано: '+[place.name,place.admin1,place.country].filter(Boolean).join(' · ');
});
</script>)HTML");
  html += F("</body></html>");
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
  String newWeatherCity = server.arg("weatherCity");
  String newWeatherApiKey = server.arg("weatherApiKey");
  String newWeatherLatitudeText = server.arg("weatherLat");
  String newWeatherLongitudeText = server.arg("weatherLon");
  newSsid.trim();
  newPrinterIp.trim();
  newWeatherApiKey.trim();
  const bool clearWeatherApiKey = server.arg("clearWeatherKey") == "1";
  if (clearWeatherApiKey) newWeatherApiKey = "";
  else if (newWeatherApiKey.length() == 0) newWeatherApiKey = weatherApiKey;

  const bool validRotation = newRotationText == "0" || newRotationText == "90" ||
                             newRotationText == "180" || newRotationText == "270";
  const uint16_t newRotationDegrees = static_cast<uint16_t>(newRotationText.toInt());
  const long parsedTimezoneOffset = newTimezoneText.toInt();
  const bool validTimezone = String(parsedTimezoneOffset) == newTimezoneText &&
                             parsedTimezoneOffset >= -720 && parsedTimezoneOffset <= 840 &&
                             parsedTimezoneOffset % 15 == 0;
  const int16_t newTimezoneOffsetMinutes = static_cast<int16_t>(parsedTimezoneOffset);

  float newWeatherLatitude = 0.0f;
  float newWeatherLongitude = 0.0f;
  const bool weatherUnconfigured = newWeatherCity.length() == 0;
  const bool validWeatherLocation = weatherUnconfigured ||
      (newWeatherCity.length() > 0 && newWeatherCity.length() <= 96 &&
       parseFiniteFloat(newWeatherLatitudeText, newWeatherLatitude) &&
       parseFiniteFloat(newWeatherLongitudeText, newWeatherLongitude) &&
       newWeatherLatitude >= -90.0f && newWeatherLatitude <= 90.0f &&
       newWeatherLongitude >= -180.0f && newWeatherLongitude <= 180.0f);
  if (weatherUnconfigured) newWeatherCity = "";

  IPAddress parsedPrinterIp;
  if (newSsid.length() == 0 || newSsid.length() > 32 ||
      newPassword.length() > 63 || !parsedPrinterIp.fromString(newPrinterIp) ||
      !validRotation || !validTimezone || !validWeatherLocation ||
      newWeatherApiKey.length() > 200 || newWeatherApiKey.indexOf('\n') >= 0 ||
      newWeatherApiKey.indexOf('\r') >= 0) {
    server.send(400, "text/html; charset=utf-8",
                "<meta charset='utf-8'><p>Проверьте имя Wi-Fi, адрес принтера, поворот экрана, часовой пояс и город для погоды.</p><a href='/'>Назад</a>");
    return;
  }

  if (newPassword.length() == 0 && newSsid == wifiSsid) {
    newPassword = wifiPassword;
  }
  saveSettings(newSsid, newPassword, newPrinterIp, newRotationDegrees,
               newTimezoneOffsetMinutes, newWeatherCity, newWeatherLatitude,
               newWeatherLongitude, newWeatherApiKey);
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><meta http-equiv='refresh' content='3;url=/'><h2>Сохранено</h2><p>Устройство перезапускается и подключается к сети.</p>");
  restartAt = millis() + 1800;
}

void handleWeatherUpdate() {
  if (weatherCity.length() == 0 || weatherApiKey.length() == 0) {
    server.send(409, "text/plain; charset=utf-8",
                "Сначала выберите город и добавьте ключ Яндекс Погоды.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain; charset=utf-8", "Нет соединения с Wi-Fi.");
    return;
  }
  if (weatherUpdateRequested) {
    server.send(202, "text/plain; charset=utf-8",
                "Запрос обновления уже ожидает выполнения.");
    return;
  }

  const time_t currentEpoch = time(nullptr);
  if (weatherLastRequestEpoch != 0 && currentEpoch >= weatherLastRequestEpoch) {
    const uint32_t cooldown = weatherLastRequestSucceeded
                                  ? kWeatherManualCooldownSeconds
                                  : kWeatherFailedManualCooldownSeconds;
    const uint32_t elapsed =
        static_cast<uint32_t>(currentEpoch - weatherLastRequestEpoch);
    if (elapsed < cooldown) {
      const uint32_t minutesRemaining = (cooldown - elapsed + 59UL) / 60UL;
      String message = "Повторное обновление будет доступно через ";
      message += String(minutesRemaining);
      message += " мин.";
      server.send(429, "text/plain; charset=utf-8", message);
      return;
    }
  }

  weatherUpdateRequested = true;
  server.send(202, "text/plain; charset=utf-8",
              "Запрос отправлен. Обновление появится на экране после получения данных.");
}

void startWebServer() {
  if (webServerStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/weather/update", HTTP_POST, handleWeatherUpdate);
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
  nextWeatherUpdate = millis() + 5000UL;
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
    updateWeatherIfDue();
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastDraw >= 1000) {
    String clockTime;
    String clockDate;
    formatLocalClock(clockTime, clockDate);
    displayUi.showDashboard(printerClient.discovery(), liveSnapshot,
                            printerIpText, WiFi.localIP().toString(),
                            WiFi.status() == WL_CONNECTED, clockTime, clockDate,
                            weatherSnapshot, weatherCity);
    lastDraw = millis();
  }
  delay(1);
}
