/*
 * Weather Station v2 - Neon Cyberpunk Edition
 *
 * Hardware (identical to arduino-simdash/dashboard_digital.ino):
 *  - ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
 *  - 1x TFT SPI 3.5" (480x320) ST7796
 *
 * Wiring:
 *   MOSI: GPIO 11  ->  Display SDI
 *   SCLK: GPIO 12  ->  Display SCK
 *   DC:   GPIO  9  ->  Display DC
 *   RST:  GPIO  8  ->  Display RESET
 *   CS:   GPIO 10  ->  Display CS
 *   LED:  GPIO 13  ->  Display LED (PWM)
 *   VCC:  5V, GND: GND
 *
 * API: OpenWeatherMap, free tier
 *  - Current weather: /data/2.5/weather
 *  - 5 day / 3 hour forecast: /data/2.5/forecast (used to build a 2-day outlook)
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include "esp_task_wdt.h"
#include "secrets.h"

// ======================== NETWORK / API CONFIG ========================
const char *wifi_ssid = SSID;
const char *wifi_password = PASSWORD;
// Get a free API key here: https://openweathermap.org/api
const char *api_key = API_KEY;

// Coordinates for this (second) device - see secrets.h
const char *lat = LAT2;
const char *lon = LON2;
const char *lang = LANG2;
const char *city = CITY2;

char location_str[64];

// ======================== HARDWARE PINS (same as dashboard_digital.ino) ========================
#define RST_PIN 8
#define DC_PIN 9
#define CS_PIN 10
#define MOSI_PIN 11
#define SCLK_PIN 12
#define LED_PIN 13

// ======================== COLOR PALETTE (VFD neon) ========================
#define COLOR_BG 0x0000       // Black
#define COLOR_VFD_MAIN 0x0679 // #04C4CA Teal (bright)
#define COLOR_VFD_GLOW 0x0235 // #01468A Darker teal for glow effect
#define COLOR_PEAK_RED 0xF802 // Red
#define COLOR_YELLOW 0xFFE0   // Yellow
#define COLOR_GHOST 0x18E3    // Dim grey (inactive)
#define COLOR_BLUE 0x2837     // Blue
#define COLOR_WHITE 0xFFFF

// ======================== LAYOUT ========================
#define SCREEN_W 480
#define SCREEN_H 320
#define INFO_X 105  // Left block: temperature / feels-like / description
#define STATS_X 305 // Right block: humidity / pressure / wind
#define COMPASS_CX 432
#define COMPASS_CY 150
#define COMPASS_R 24

// ======================== POWER SAVING ========================
// ESP32-S3 supports 240/160/80 MHz; APB stays at 80 MHz for all three, so the
// 80 MHz display SPI and Wi-Fi are unaffected. Below 80 MHz APB drops too.
#define CPU_FREQ_MHZ 80
#define LOOP_IDLE_MS 250

// ======================== LGFX CONFIGURATION (identical panel/bus to dashboard_digital.ino) ========================
class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;

  LGFX()
  {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 0;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.pin_sclk = SCLK_PIN;
      cfg.pin_mosi = MOSI_PIN;
      cfg.pin_miso = -1;
      cfg.pin_dc = DC_PIN;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs = CS_PIN;
      cfg.pin_rst = RST_PIN;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = false;
      cfg.rgb_order = false;
      cfg.invert = false;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;
LGFX_Sprite sprite(&lcd);

// ======================== BACKLIGHT ========================
constexpr int BL_CH = 0;

void backlight_init()
{
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(LED_PIN, BL_CH);
}

void setBacklightPercent(uint8_t pct)
{
  pct = constrain(pct, 0, 100);
  uint16_t pwm = (uint32_t)pct * 255 / 100;
  ledcWrite(BL_CH, pwm);
}

// ======================== TIME ========================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 0, 60000);

unsigned long lastNTPSync = 0;
const unsigned long ntpSyncInterval = 86400000UL; // 24h

const char *weekdayNames[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *monthNames[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// Local wall-clock epoch: timeClient is configured with the location's UTC offset,
// so its epoch is already shifted and matches the forecast timestamps we compute.
long localEpochNow()
{
  return (long)timeClient.getEpochTime();
}

// NTP has never landed if the epoch is still counting up from zero.
bool timeIsValid()
{
  return localEpochNow() > 1000000L;
}

void getLocalTm(struct tm &out)
{
  time_t rawtime = (time_t)localEpochNow();
  gmtime_r(&rawtime, &out);
}

// ======================== WATCHDOG ========================
#define WDT_TIMEOUT 30

// ======================== CURRENT WEATHER DATA ========================
float temp_c = 0;
float feels_like = 0;
int humidity = 0;
int pressure_hpa = 0;
float wind_speed_ms = 0;
int wind_deg = 0;
char weather_main[32] = "--";
char weather_desc[64] = "--";
long timezone_offset = 0;
bool dataValid = false;
char lastUpdateTime[6] = "--:--";
char sunriseTime[6] = "--:--";
char sunsetTime[6] = "--:--";

unsigned long lastWeatherUpdate = 0;
const unsigned long weatherInterval = 900000UL; // 15 minutes

// ======================== FORECAST DATA (next 2 days) ========================
struct DayForecast
{
  bool valid = false;
  char label[12] = "";
  float tempMin = 999;
  float tempMax = -999;
  char main[32] = "--";
  int pop = 0; // probability of precipitation, %
};
DayForecast forecastDays[2];

unsigned long lastForecastUpdate = 0;
const unsigned long forecastInterval = 3600000UL; // 1 hour

// ======================== HELPERS ========================
void copyStr(char *dst, size_t len, const char *src)
{
  strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

// Formats a UTC unix timestamp as local HH:MM using the location's offset.
void formatLocalHM(long epochUtc, char *out, size_t len)
{
  if (epochUtc <= 0)
  {
    copyStr(out, len, "--:--");
    return;
  }
  time_t t = (time_t)(epochUtc + timezone_offset);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, len, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// Three-layer VFD glow effect: outer glow, inner glow, core (bright text on top)
void drawVFDText(const char *text, int x, int y, uint16_t color)
{
  uint8_t r = (color >> 11) << 3;
  uint8_t g = ((color >> 5) & 0x3F) << 2;
  uint8_t b = (color & 0x1F) << 3;

  uint16_t glowOuter = sprite.color565(r * 0.2, g * 0.2, b * 0.2);
  uint16_t glowInner = sprite.color565(r * 0.5, g * 0.5, b * 0.5);

  sprite.setTextColor(glowOuter);
  for (int dx = -2; dx <= 2; dx++)
  {
    for (int dy = -2; dy <= 2; dy++)
    {
      if (dx == 0 && dy == 0)
        continue;
      if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
        continue;
      sprite.drawString(text, x + dx, y + dy);
    }
  }

  sprite.setTextColor(glowInner);
  sprite.drawString(text, x - 1, y);
  sprite.drawString(text, x + 1, y);
  sprite.drawString(text, x, y - 1);
  sprite.drawString(text, x, y + 1);

  sprite.setTextColor(color);
  sprite.drawString(text, x, y);
}

uint16_t iconColorFor(const char *type)
{
  if (strcmp(type, "Thunderstorm") == 0)
    return COLOR_YELLOW;
  if (strcmp(type, "Snow") == 0)
    return COLOR_WHITE;
  if (strcmp(type, "Rain") == 0 || strcmp(type, "Drizzle") == 0)
    return COLOR_BLUE;
  if (strcmp(type, "Clear") == 0)
    return COLOR_YELLOW;
  return COLOR_VFD_MAIN;
}

// Compact neon vector weather icon (Clear/Clouds/Rain/Drizzle/Thunderstorm/Snow/Mist-Fog-Haze/default)
void drawWeatherIcon(int cx, int cy, const char *type, int size)
{
  uint16_t col = iconColorFor(type);

  if (strcmp(type, "Clear") == 0)
  {
    sprite.fillCircle(cx, cy, size * 0.4, COLOR_VFD_GLOW);
    sprite.drawCircle(cx, cy, size * 0.4, col);
    sprite.drawCircle(cx, cy, size * 0.4 - 1, col);
    for (int i = 0; i < 8; i++)
    {
      float angle = i * PI / 4.0;
      int x1 = cx + cos(angle) * size * 0.55;
      int y1 = cy + sin(angle) * size * 0.55;
      int x2 = cx + cos(angle) * size * 0.85;
      int y2 = cy + sin(angle) * size * 0.85;
      sprite.drawLine(x1, y1, x2, y2, col);
    }
  }
  else if (strcmp(type, "Clouds") == 0)
  {
    int r = size * 0.35;
    sprite.fillCircle(cx - r, cy + r * 0.3, r * 0.8, COLOR_VFD_GLOW);
    sprite.fillCircle(cx, cy - r * 0.2, r, COLOR_VFD_GLOW);
    sprite.fillCircle(cx + r, cy + r * 0.3, r * 0.8, COLOR_VFD_GLOW);
    sprite.drawCircle(cx - r, cy + r * 0.3, r * 0.8, col);
    sprite.drawCircle(cx, cy - r * 0.2, r, col);
    sprite.drawCircle(cx + r, cy + r * 0.3, r * 0.8, col);
    sprite.drawFastHLine(cx - r - int(r * 0.8), cy + r * 0.3, 2 * (r + int(r * 0.8)), col);
  }
  else if (strcmp(type, "Rain") == 0 || strcmp(type, "Drizzle") == 0)
  {
    int r = size * 0.3;
    sprite.drawCircle(cx - r, cy - r * 0.4, r * 0.7, COLOR_GHOST);
    sprite.drawCircle(cx, cy - r * 0.8, r * 0.9, COLOR_GHOST);
    sprite.drawCircle(cx + r, cy - r * 0.4, r * 0.7, COLOR_GHOST);
    sprite.drawFastHLine(cx - r - int(r * 0.7), cy - r * 0.4, 2 * (r + int(r * 0.7)), COLOR_GHOST);
    for (int i = -1; i <= 1; i++)
    {
      int dx = i * (r * 0.8);
      sprite.drawLine(cx + dx, cy + r * 0.2, cx + dx - 2, cy + r * 0.8, col);
    }
  }
  else if (strcmp(type, "Thunderstorm") == 0)
  {
    int r = size * 0.3;
    sprite.drawCircle(cx, cy - r * 0.6, r, COLOR_GHOST);
    sprite.drawFastHLine(cx - r, cy - r * 0.6, 2 * r, COLOR_GHOST);
    sprite.fillTriangle(cx + 3, cy - r * 0.1, cx - 4, cy + r * 0.7, cx + 1, cy + r * 0.7, col);
    sprite.fillTriangle(cx - 4, cy + r * 0.7, cx + 1, cy + r * 0.7, cx - 3, cy + r * 1.4, col);
  }
  else if (strcmp(type, "Snow") == 0)
  {
    int r = size * 0.3;
    sprite.drawCircle(cx, cy - r * 0.6, r, COLOR_GHOST);
    for (int i = -1; i <= 1; i++)
    {
      int sx = cx + i * (r * 0.8);
      int sy = cy + r * 0.5;
      sprite.drawLine(sx - 3, sy, sx + 3, sy, col);
      sprite.drawLine(sx, sy - 3, sx, sy + 3, col);
      sprite.drawLine(sx - 2, sy - 2, sx + 2, sy + 2, col);
      sprite.drawLine(sx - 2, sy + 2, sx + 2, sy - 2, col);
    }
  }
  else if (strcmp(type, "Mist") == 0 || strcmp(type, "Fog") == 0 || strcmp(type, "Haze") == 0)
  {
    for (int i = -2; i <= 2; i++)
    {
      int w = size * (0.9 - abs(i) * 0.1);
      sprite.drawFastHLine(cx - w, cy + i * (size * 0.22), 2 * w, col);
    }
  }
  else
  {
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(col);
    sprite.setTextSize(2);
    sprite.drawString("?", cx, cy);
    sprite.setTextDatum(top_left);
    sprite.setTextSize(1);
  }
}

// ======================== NETWORK FETCH ========================
#define HTTP_TIMEOUT_MS 8000
#define WIFI_CONNECT_TIMEOUT_MS 20000

bool radioOn = false;

// The radio is the dominant consumer, so it is only powered for the few seconds
// per interval that we actually need it.
bool radioEnable()
{
  if (radioOn && WiFi.status() == WL_CONNECTED)
    return true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.begin(wifi_ssid, wifi_password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(200);
    esp_task_wdt_reset();
  }

  radioOn = (WiFi.status() == WL_CONNECTED);
  if (radioOn)
    timeClient.begin();
  else
    Serial.println("WiFi connect failed");
  return radioOn;
}

void radioDisable()
{
  // The UDP socket does not survive the interface going down; re-opened by radioEnable().
  timeClient.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  radioOn = false;
}

void buildApiUrl(char *out, size_t len, const char *endpoint, const char *extraParams)
{
  snprintf(out, len,
           "https://api.openweathermap.org/data/2.5/%s?lat=%s&lon=%s&units=metric&appid=%s&lang=%s%s",
           endpoint, lat, lon, api_key, lang, extraParams);
}

bool httpGetJson(const char *url, JsonDocument &doc)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  // TODO: pin the OpenWeatherMap CA certificate instead of skipping verification.
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(client, url);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("HTTP GET failed, code: %d\n", httpCode);
    http.end();
    return false;
  }

  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error)
  {
    Serial.printf("deserializeJson() failed: %s\n", error.c_str());
    return false;
  }
  return true;
}

void getWeatherData()
{
  char url[256];
  buildApiUrl(url, sizeof(url), "weather", "");

  DynamicJsonDocument doc(2048);
  if (!httpGetJson(url, doc) || !doc.containsKey("main"))
  {
    dataValid = false;
    return;
  }

  temp_c = doc["main"]["temp"] | 0.0;
  feels_like = doc["main"]["feels_like"] | 0.0;
  humidity = doc["main"]["humidity"] | 0;
  pressure_hpa = doc["main"]["pressure"] | 0;
  wind_speed_ms = doc["wind"]["speed"] | 0.0;
  wind_deg = doc["wind"]["deg"] | 0;

  copyStr(weather_main, sizeof(weather_main), doc["weather"][0]["main"] | "");
  copyStr(weather_desc, sizeof(weather_desc), doc["weather"][0]["description"] | "--");

  timezone_offset = doc["timezone"] | 0;
  dataValid = true;

  formatLocalHM(doc["sys"]["sunrise"] | 0L, sunriseTime, sizeof(sunriseTime));
  formatLocalHM(doc["sys"]["sunset"] | 0L, sunsetTime, sizeof(sunsetTime));

  // setTimeOffset applies immediately to getEpochTime(); no NTP round-trip needed.
  timeClient.setTimeOffset(timezone_offset);

  if (timeIsValid())
  {
    snprintf(lastUpdateTime, sizeof(lastUpdateTime), "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
  }

  Serial.println("Current weather updated");
}

void getForecastData()
{
  // Day bucketing is relative to "today", so a bogus clock would silently drop every entry.
  if (!timeIsValid())
    return;

  // cnt=16 -> 48h of 3h-step data, enough to cover "tomorrow" + "day after tomorrow"
  char url[280];
  buildApiUrl(url, sizeof(url), "forecast", "&cnt=16");

  DynamicJsonDocument doc(12288);
  if (!httpGetJson(url, doc) || !doc.containsKey("list"))
    return;

  long todayDayIndex = localEpochNow() / 86400L;

  DayForecast newDays[2];
  int bestDist[2] = {999, 999};

  JsonArray list = doc["list"].as<JsonArray>();
  for (JsonObject item : list)
  {
    long dt = item["dt"] | 0L;
    long localEpoch = dt + timezone_offset;
    long dayIndex = localEpoch / 86400L;
    long rel = dayIndex - todayDayIndex; // 1 = tomorrow, 2 = day after tomorrow

    if (rel != 1 && rel != 2)
      continue;

    DayForecast &d = newDays[rel - 1];
    d.valid = true;

    float tmin = item["main"]["temp_min"] | 0.0;
    float tmax = item["main"]["temp_max"] | 0.0;
    if (tmin < d.tempMin)
      d.tempMin = tmin;
    if (tmax > d.tempMax)
      d.tempMax = tmax;

    int pop = (int)((item["pop"] | 0.0) * 100.0 + 0.5);
    if (pop > d.pop)
      d.pop = pop;

    // Pick the entry closest to local noon as the representative icon/condition for the day
    int hourLocal = (int)((localEpoch % 86400L) / 3600L);
    int dist = abs(hourLocal - 13);
    if (dist < bestDist[rel - 1])
    {
      bestDist[rel - 1] = dist;
      copyStr(d.main, sizeof(d.main), item["weather"][0]["main"] | "");
    }
  }

  struct tm tmNow;
  getLocalTm(tmNow);
  copyStr(newDays[0].label, sizeof(newDays[0].label), "TOMORROW");
  copyStr(newDays[1].label, sizeof(newDays[1].label), weekdayNames[(tmNow.tm_wday + 2) % 7]);

  if (newDays[0].valid || newDays[1].valid)
  {
    forecastDays[0] = newDays[0];
    forecastDays[1] = newDays[1];
    Serial.println("Forecast updated");
  }
}

// ======================== DRAWING ========================
void drawBootScreen(const char *msg)
{
  sprite.fillSprite(COLOR_BG);
  sprite.setTextDatum(middle_center);
  sprite.setTextSize(1);
  sprite.setFont(&fonts::Orbitron_Light_24);
  drawVFDText("WEATHER STATION", SCREEN_W / 2, 130, COLOR_VFD_MAIN);
  sprite.setFont(nullptr);
  sprite.setTextSize(2);
  sprite.setTextColor(COLOR_GHOST);
  sprite.drawString(msg, SCREEN_W / 2, 180);
  sprite.setTextDatum(top_left);
  sprite.setTextSize(1);
  sprite.pushSprite(0, 0);
}

void drawHeader()
{
  struct tm tmNow;
  getLocalTm(tmNow);

  sprite.setFont(nullptr);
  sprite.setTextDatum(top_left);
  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.drawString(location_str, 6, 4);

  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d %s", tmNow.tm_mday, monthNames[tmNow.tm_mon]);
  sprite.setTextDatum(top_center);
  sprite.drawString(dateStr, SCREEN_W / 2, 4);

  sprite.setTextDatum(top_right);
  sprite.setTextColor(dataValid ? COLOR_GHOST : COLOR_PEAK_RED);
  char updStr[16];
  snprintf(updStr, sizeof(updStr), "UPD %s", lastUpdateTime);
  sprite.drawString(updStr, SCREEN_W - 6, 4);
  sprite.setTextDatum(top_left);
}

void drawSunEvent(int cx, int cy, bool rising, const char *hhmm)
{
  const int r = 8;

  sprite.fillCircle(cx, cy, r, COLOR_VFD_GLOW);
  sprite.drawCircle(cx, cy, r, COLOR_YELLOW);
  // Clip to a half-sun sitting on the horizon line.
  sprite.fillRect(cx - r - 1, cy + 1, 2 * r + 3, r + 2, COLOR_BG);
  sprite.drawFastHLine(cx - r - 5, cy, 2 * r + 11, COLOR_YELLOW);

  int ay = cy - r - 4;
  if (rising)
    sprite.fillTriangle(cx + r + 8, ay, cx + r + 4, ay + 6, cx + r + 12, ay + 6, COLOR_YELLOW);
  else
    sprite.fillTriangle(cx + r + 8, ay + 6, cx + r + 4, ay, cx + r + 12, ay, COLOR_YELLOW);

  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.setTextDatum(top_center);
  sprite.drawString(hhmm, cx, cy + 5);
  sprite.setTextDatum(top_left);
}

void drawClock()
{
  char hh[3], mm[3];
  snprintf(hh, sizeof(hh), "%02d", timeClient.getHours());
  snprintf(mm, sizeof(mm), "%02d", timeClient.getMinutes());

  sprite.setFont(&fonts::Orbitron_Light_32);
  sprite.setTextSize(1);

  sprite.setTextDatum(middle_right);
  drawVFDText(hh, SCREEN_W / 2 - 10, 50, COLOR_VFD_MAIN);
  sprite.setTextDatum(middle_left);
  drawVFDText(mm, SCREEN_W / 2 + 10, 50, COLOR_VFD_MAIN);

  // Colon dots (avoids relying on a ':' glyph in the digital font)
  sprite.fillCircle(SCREEN_W / 2, 40, 3, COLOR_VFD_MAIN);
  sprite.fillCircle(SCREEN_W / 2, 60, 3, COLOR_VFD_MAIN);

  sprite.setFont(nullptr);
  sprite.setTextDatum(top_left);

  drawSunEvent(90, 46, true, sunriseTime);
  drawSunEvent(380, 46, false, sunsetTime);
}

void drawStat(int x, int y, const char *label, const char *value)
{
  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.drawString(label, x, y);
  sprite.setTextSize(2);
  sprite.setTextColor(COLOR_VFD_MAIN);
  sprite.drawString(value, x, y + 13);
}

void drawWindCompass(int cx, int cy, int deg)
{
  sprite.drawCircle(cx, cy, COMPASS_R, COLOR_VFD_GLOW);
  sprite.drawCircle(cx, cy, COMPASS_R - 4, COLOR_VFD_GLOW);

  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.setTextDatum(middle_center);
  sprite.drawString("N", cx, cy - COMPASS_R - 7);
  sprite.drawString("S", cx, cy + COMPASS_R + 7);
  sprite.drawString("E", cx + COMPASS_R + 7, cy);
  sprite.drawString("W", cx - COMPASS_R - 7, cy);
  sprite.setTextDatum(top_left);

  // OpenWeather reports the bearing the wind comes FROM; the arrow shows where it blows TO.
  float rad = deg * PI / 180.0;
  float dx = -sin(rad);
  float dy = cos(rad);

  int tipX = cx + (int)(dx * COMPASS_R * 0.78);
  int tipY = cy + (int)(dy * COMPASS_R * 0.78);
  int tailX = cx - (int)(dx * COMPASS_R * 0.6);
  int tailY = cy - (int)(dy * COMPASS_R * 0.6);

  int headLen = COMPASS_R / 3;
  int headW = COMPASS_R / 5;
  float px = -dy;
  float py = dx;

  int h1x = tipX + (int)(-dx * headLen + px * headW);
  int h1y = tipY + (int)(-dy * headLen + py * headW);
  int h2x = tipX + (int)(-dx * headLen - px * headW);
  int h2y = tipY + (int)(-dy * headLen - py * headW);

  sprite.drawLine(tailX, tailY, tipX, tipY, COLOR_VFD_MAIN);
  sprite.fillTriangle(tipX, tipY, h1x, h1y, h2x, h2y, COLOR_VFD_MAIN);
  sprite.fillCircle(cx, cy, 2, COLOR_VFD_MAIN);
}

void drawCurrentWeather()
{
  drawWeatherIcon(55, 130, weather_main, 38);

  sprite.setFont(nullptr);
  // Size 5 keeps the widest reading ("-12.3C") clear of the stats column at STATS_X.
  sprite.setTextSize(5);
  char tempStr[12];
  snprintf(tempStr, sizeof(tempStr), "%.1fC", temp_c);
  drawVFDText(tempStr, INFO_X, 105, COLOR_VFD_MAIN);

  sprite.setTextSize(2);
  sprite.setTextColor(COLOR_GHOST);
  char feelsStr[20];
  snprintf(feelsStr, sizeof(feelsStr), "FEELS %.1fC", feels_like);
  sprite.drawString(feelsStr, INFO_X, 152);

  // The left block is 195px wide: 16 chars at size 2, 32 at size 1.
  char descStr[33];
  copyStr(descStr, sizeof(descStr), weather_desc);
  sprite.setTextSize(strlen(descStr) > 16 ? 1 : 2);
  sprite.setTextColor(COLOR_VFD_MAIN);
  sprite.drawString(descStr, INFO_X, 178);

  char buf[12];
  snprintf(buf, sizeof(buf), "%d", humidity);
  drawStat(STATS_X, 95, "HUMIDITY %", buf);

  snprintf(buf, sizeof(buf), "%d", (int)(pressure_hpa * 0.75006));
  drawStat(STATS_X, 140, "PRESSURE MMHG", buf);

  snprintf(buf, sizeof(buf), "%.1f", wind_speed_ms * 3.6);
  drawStat(STATS_X, 185, "WIND KM/H", buf);

  drawWindCompass(COMPASS_CX, COMPASS_CY, wind_deg);
}

void drawForecastCard(int x, int y, int w, int h, DayForecast &d)
{
  sprite.drawRoundRect(x, y, w, h, 6, COLOR_VFD_GLOW);

  sprite.setFont(nullptr);
  sprite.setTextSize(2);
  drawVFDText(d.valid ? d.label : "N/A", x + 10, y + 4, COLOR_VFD_MAIN);

  if (!d.valid)
    return;

  drawWeatherIcon(x + 35, y + 42, d.main, 22);

  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.drawString("MAX", x + 70, y + 26);
  sprite.drawString("MIN", x + 130, y + 26);

  sprite.setTextSize(2);
  sprite.setTextColor(COLOR_YELLOW);
  char maxStr[8];
  snprintf(maxStr, sizeof(maxStr), "%.0fC", d.tempMax);
  sprite.drawString(maxStr, x + 70, y + 36);

  sprite.setTextColor(COLOR_VFD_MAIN);
  char minStr[8];
  snprintf(minStr, sizeof(minStr), "%.0fC", d.tempMin);
  sprite.drawString(minStr, x + 130, y + 36);

  sprite.setTextSize(1);
  sprite.setTextColor(d.pop >= 30 ? COLOR_BLUE : COLOR_GHOST);
  char popStr[16];
  snprintf(popStr, sizeof(popStr), "RAIN %d%%", d.pop);
  sprite.drawString(popStr, x + 150, y + 54);
}

void drawForecastPanel()
{
  sprite.drawFastHLine(0, 228, SCREEN_W, COLOR_VFD_GLOW);
  sprite.setTextDatum(top_center);
  sprite.setTextSize(1);
  sprite.setTextColor(COLOR_GHOST);
  sprite.drawString("2-DAY FORECAST", SCREEN_W / 2, 232);
  sprite.setTextDatum(top_left);

  drawForecastCard(8, 248, 228, 68, forecastDays[0]);
  drawForecastCard(244, 248, 228, 68, forecastDays[1]);
}

void drawScreen()
{
  sprite.fillSprite(COLOR_BG);
  drawHeader();
  drawClock();
  sprite.drawFastHLine(0, 82, SCREEN_W, COLOR_VFD_GLOW);
  drawCurrentWeather();
  drawForecastPanel();
  sprite.pushSprite(0, 0);
}

// Night backlight mode (23:00 - 07:00 -> 1%, otherwise 30%)
#define BRIGHTNESS_NIGHT 1
#define BRIGHTNESS_DAY 30

void updateBacklightMode()
{
  static uint8_t lastBrightness = 0;
  int hour = timeClient.getHours();
  uint8_t target = (hour >= 23 || hour < 7) ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;
  if (target != lastBrightness)
  {
    setBacklightPercent(target);
    lastBrightness = target;
    Serial.printf("Backlight: %d%%\n", target);
  }
}

// ======================== SETUP ========================
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Weather Station v2 (Neon) ===");

  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  // The radio reconnects every interval; without this every begin() may hit NVS.
  WiFi.persistent(false);

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  lcd.init();
  lcd.setRotation(1); // Landscape 480x320
  lcd.fillScreen(COLOR_BG);

  backlight_init();
  setBacklightPercent(BRIGHTNESS_DAY);

  // 480x320x16bpp = 300KB: must live in PSRAM, internal RAM is needed by WiFi/TLS.
  sprite.setPsram(true);
  if (!sprite.createSprite(SCREEN_W, SCREEN_H))
  {
    Serial.println("ERROR: Sprite creation failed!");
    lcd.setTextColor(COLOR_PEAK_RED);
    lcd.setTextDatum(middle_center);
    lcd.setTextSize(2);
    lcd.drawString("SPRITE ALLOC FAILED", SCREEN_W / 2, SCREEN_H / 2);
    while (1)
    {
      esp_task_wdt_reset();
      delay(1000);
    }
  }

  copyStr(location_str, sizeof(location_str), city);

  drawBootScreen("CONNECTING WIFI...");
  radioEnable();

  drawBootScreen("SYNCING TIME...");
  unsigned long timeStart = millis();
  while (!timeIsValid() && (millis() - timeStart) < 10000)
  {
    timeClient.update();
    delay(200);
    esp_task_wdt_reset();
  }

  drawBootScreen("FETCHING WEATHER...");
  int retries = 0;
  while (!dataValid && retries < 3)
  {
    getWeatherData();
    if (!dataValid)
    {
      retries++;
      delay(1500);
    }
    esp_task_wdt_reset();
  }

  getForecastData();
  esp_task_wdt_reset();
  radioDisable();

  lastWeatherUpdate = millis();
  lastForecastUpdate = millis();
  lastNTPSync = millis();

  drawScreen();
}

// ======================== LOOP ========================
void loop()
{
  esp_task_wdt_reset();

  unsigned long now = millis();
  bool needsRedraw = false;

  bool weatherDue = (now - lastWeatherUpdate >= weatherInterval);
  bool forecastDue = (now - lastForecastUpdate >= forecastInterval);
  // Until the clock is set, retry NTP at the short interval instead of waiting a day.
  bool ntpDue = (now - lastNTPSync >= (timeIsValid() ? ntpSyncInterval : weatherInterval));

  // All network work is batched into a single radio window per interval.
  if (weatherDue || forecastDue || ntpDue)
  {
    if (radioEnable())
    {
      // Each step can block for seconds, so the watchdog is fed between them.
      if (ntpDue)
        timeClient.forceUpdate();
      esp_task_wdt_reset();
      if (weatherDue)
        getWeatherData();
      esp_task_wdt_reset();
      if (forecastDue)
        getForecastData();
      esp_task_wdt_reset();
    }
    radioDisable();

    // Timestamps advance even on failure so a dead link is not retried every loop.
    if (weatherDue)
      lastWeatherUpdate = now;
    if (forecastDue)
      lastForecastUpdate = now;
    if (ntpDue)
      lastNTPSync = now;
    needsRedraw = true;
  }

  static char lastMinute[6] = "";
  char nowMinute[6];
  snprintf(nowMinute, sizeof(nowMinute), "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
  if (strcmp(nowMinute, lastMinute) != 0)
  {
    copyStr(lastMinute, sizeof(lastMinute), nowMinute);
    needsRedraw = true;
  }

  updateBacklightMode();

  if (needsRedraw)
    drawScreen();

  delay(LOOP_IDLE_MS);
}
