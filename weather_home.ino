/*
 * Weather Statio
 * Hardware: ESP8266 + ILI9341 TFT
 * API: OpenWeatherMap (JSON)
 */

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

// --- NETWORK CONFIGURATION ---
const char *wifi_ssid = SSID;
const char *wifi_password = PASSWORD;

// --- WEATHER CONFIGURATION ---
// Get a free API key here: https://openweathermap.org/api
const char *api_key = API_KEY;

// Coordinates
const char *lat = LAT;
const char *lon = LON;
const char *lang = LANG;
const char *city = CITY;

const bool get_ip_geolocation = false; // Obtain geolocation by IP (to display city) - requires Wi‑Fi at startup

// Active coordinates used for weather requests. Initialized from above defaults,
// but may be overwritten by `getGeoLocation()` when `get_ip_geolocation == true`.
char active_lat[16];
char active_lon[16];
char active_lang[8];

// --- DISPLAY AND TIME SETTINGS ---
TFT_eSPI tft = TFT_eSPI();
WiFiUDP ntpUDP;
// Offset for Dublin (GMT/BST). Dublin observes DST; this can be handled via API offset
// or by using system time libraries. Winter 0, Summer 3600. Adjust logic as needed.
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 0, 60000);

unsigned long lastWeatherUpdate = 0;
unsigned long weatherInterval = 900000; // Update weather every 15 minutes (900000 ms)
unsigned long lastCycle = 0;
const unsigned long cycleMs = 900000; // 15 minutes
const unsigned long sleepMs = 840000; // 14 minutes (Wi‑Fi off)
bool sleepingRadio = false;
char location_str[64] = "Unknown";

// NTP synchronization
unsigned long lastNTPSync = 0;
const unsigned long ntpSyncInterval = 86400000; // 24 hours

// Variables for tracking changes (static UI)
char last_time[6] = "";
float last_temp = -999.0;
float last_feels = -999.0;
int last_humidity = -1;
int last_pressure = -1;
float last_wind_speed = -999.0;
int last_wind_deg = -1;
char last_weather_main[32] = "";
char last_weather_desc[64] = "";
bool firstDraw = true;

// Variables to store data
float temp_c = 0;
float feels_like = 0;
int humidity = 0;
int pressure_hpa = 0;
float wind_speed_ms = 0;
char weather_desc[64] = "--";
char weather_main[32] = "";
long timezone_offset = 0;
int wind_deg = 0;
bool dataValid = false;
char lastUpdateTime[6] = "--:--";

const uint8_t BACKLIGHT_PIN = D2; // GPIO5, supports PWM
const uint16_t PWM_MAX = 1023;    // ESP8266 range
const uint8_t BUTTON_PIN = 0;     // FLASH button (GPIO 0)

// Button variables
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool buttonHandled = false;
bool isLongPressProcessed = false;

// UI overlay for the button
unsigned long overlayShowTime = 0;
bool overlayActive = false;
char overlayMessage[32] = "";

// Brightness control
uint8_t brightnessLevels[] = {2, 10, 15, 25, 30, 60};
uint8_t currentBrightnessIndex = 4; // Start at 30%
bool manualBrightnessSet = false;
unsigned long lastBrightness = 0;

void setBacklightPercent(uint8_t pct)
{
  pct = constrain(pct, 0, 100);
  uint16_t pwm = (uint32_t)pct * PWM_MAX / 100;
  analogWrite(BACKLIGHT_PIN, pwm);
}

void setup()
{
  Serial.begin(115200);

  // Lower CPU frequency to save power
  system_update_cpu_freq(80); // 80 MHz instead of 160 MHz (approx -30% power)

  // Wi‑Fi power saving mode
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  // Configure FLASH button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWriteRange(PWM_MAX); // set range
  analogWriteFreq(100);      // PWM frequency, Hz (optional)
  setBacklightPercent(30);   // initial brightness 30%

  // NOTE: To reduce SPI frequency to 20 MHz,
  // change #define SPI_FREQUENCY in User_Setup.h of the TFT_eSPI library

  // Initialize display
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.println("Connecting to WiFi...");
  tft.println(wifi_ssid);

  // Connect to WiFi (20s timeout)
  WiFi.begin(wifi_ssid, wifi_password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 20000)
  {
    delay(200);
    Serial.print(".");
    tft.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 20);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("WiFi Connected!");
  }
  else
  {
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 50);
    tft.println("WiFi Failed (continuing)");
    Serial.println("WiFi connection failed — continuing in degraded mode");
    // continue in degraded mode
  }

  // Initialize active coordinates from compile-time defaults
  strncpy(active_lat, lat, sizeof(active_lat) - 1);
  active_lat[sizeof(active_lat) - 1] = '\0';
  strncpy(active_lon, lon, sizeof(active_lon) - 1);
  active_lon[sizeof(active_lon) - 1] = '\0';
  strncpy(active_lang, lang, sizeof(active_lang) - 1);
  active_lang[sizeof(active_lang) - 1] = '\0';

  // Obtain geolocation (only if enabled)
  if (get_ip_geolocation)
  {
    tft.setCursor(10, 60);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("Getting Geo...");
    getGeoLocation();
  }
  else
  {
    // If IP geolocation is disabled — use compile-time CITY for display
    if (city != NULL && city[0] != '\0')
    {
      strncpy(location_str, city, sizeof(location_str) - 1);
      location_str[sizeof(location_str) - 1] = '\0';
    }
  }

  // Show result only on error
  if (strcmp(location_str, "Unknown") == 0)
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.println("Geo Failed");
    delay(3000);
  }
  // Otherwise continue without showing an intermediate screen

  // Initialize time
  timeClient.begin();

  // Wait for time synchronization
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 100);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.println("Syncing Time...");

  unsigned long timeoutStart = millis();
  bool timeSync = false;
  while (!timeSync && (millis() - timeoutStart < 10000))
  {
    timeSync = timeClient.update();
    if (!timeSync)
    {
      delay(200);
      tft.print(".");
    }
    // Verify that time is actually synchronized
    if (timeClient.getEpochTime() > 1000000)
    {
      timeSync = true;
    }
  }

  if (!timeSync)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("Time Sync Failed!");
    delay(2000);
  }
  else
  {
    Serial.println("Time synced successfully");
  }

  // First weather update with retries
  int retries = 0;
  while (!dataValid && retries < 3)
  {
    getWeatherData();
    if (!dataValid)
    {
      retries++;
      if (retries < 3)
      {
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(10, 100);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.printf("Retry %d/3...", retries);
        delay(2000);
      }
    }
  }

  if (!dataValid)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("Weather Failed!");
    delay(3000);
  }

  tft.fillScreen(TFT_BLACK);
  drawInterface();
}

void loop()
{
  unsigned long now = millis();

  // Update time at the start of loop for smoothness
  timeClient.update();

  // Handle FLASH button
  handleButton();

  // NTP sync once per day
  if (now - lastNTPSync >= ntpSyncInterval || lastNTPSync == 0)
  {
    if (WiFi.status() == WL_CONNECTED || !sleepingRadio)
    {
      timeClient.update();
      lastNTPSync = now;
      Serial.println("NTP synced");
    }
  }

  // Update time display only when the minute changes
  updateTimeDisplay();

  // Night backlight mode (23:00 - 07:00 -> 2%) - only if brightness is not set manually
  if (!manualBrightnessSet)
  {
    updateBacklightMode();
  }

  // Update weather every 15 minutes
  if (now - lastWeatherUpdate >= weatherInterval)
  {
    getWeatherData();
    updateWeatherDisplay();
    lastWeatherUpdate = now;
  }
}

// --- WEATHER FETCH FUNCTION ---
void getWeatherData()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    // Form the request URL (HTTPS) using active coordinates
    char url[256];
    snprintf(url, sizeof(url), "https://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=metric&appid=%s&lang=%s", active_lat, active_lon, api_key, active_lang);

    Serial.println("Requesting weather data (HTTPS)");

    // Use HTTPClient over the secure client
    http.begin(client, url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
      // Parse JSON directly from the stream to avoid allocating a large String
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, http.getStream());

      if (!error && doc.containsKey("main"))
      {
        temp_c = doc["main"]["temp"] | 0.0;
        feels_like = doc["main"]["feels_like"] | 0.0;
        humidity = doc["main"]["humidity"] | 0;
        pressure_hpa = doc["main"]["pressure"] | 0;
        wind_speed_ms = doc["wind"]["speed"] | 0.0;
        wind_deg = doc["wind"]["deg"] | 0;

        const char *desc = doc["weather"][0]["description"] | "--";
        const char *mainw = doc["weather"][0]["main"] | "";
        strncpy(weather_desc, desc, sizeof(weather_desc) - 1);
        weather_desc[sizeof(weather_desc) - 1] = '\0';
        strncpy(weather_main, mainw, sizeof(weather_main) - 1);
        weather_main[sizeof(weather_main) - 1] = '\0';

        timezone_offset = doc["timezone"] | 0;

        dataValid = true;

        // Adjust time using offset from API (auto-detect timezone)
        timeClient.setTimeOffset(timezone_offset);
        // Update time client immediately to reflect new offset
        timeClient.update();

        // Save the time of the last successful update (only if time is synchronized)
        if (timeClient.getEpochTime() > 1000000)
        {
          int hh = timeClient.getHours();
          int mm = timeClient.getMinutes();
          snprintf(lastUpdateTime, sizeof(lastUpdateTime), "%02d:%02d", hh, mm);
        }

        Serial.println("Weather data updated successfully");
      }
      else
      {
        Serial.print("deserializeJson() failed or missing 'main': ");
        Serial.println(error.c_str());
        dataValid = false;
      }
    }
    else
    {
      Serial.printf("HTTP request failed, code: %d\n", httpCode);
      dataValid = false;
    }

    http.end();
  }
  else
  {
    Serial.println("WiFi not connected");
    dataValid = false;
  }
}

// cx, cy - arrow center
// deg    - wind direction in degrees (from OpenWeatherMap)
// size   - arrow length
// showTrajectory = true  -> arrow shows "where the wind is going to"
// showTrajectory = false -> arrow shows "where the wind is coming from"
void drawWindArrow(int cx, int cy, int deg, int size, bool showTrajectory)
{
  int pad = 4;
  tft.fillRect(cx - size - pad, cy - size - pad,
               (size * 2) + pad * 2, (size * 2) + pad * 2, TFT_BLACK);

  // Choose angle
  float angle_deg = showTrajectory ? deg + 180 : deg;

  // Convert to radians (Y-axis inverted for TFT)
  float angle = -angle_deg * PI / 180.0;

  // Arrow end point
  float dx = cos(angle);
  float dy = sin(angle);
  int x2 = cx + int(dx * size);
  int y2 = cy + int(dy * size);

  // Arrow head size
  int headLen = max(6, size / 3);
  int headWidth = max(4, size / 6);

  // Perpendicular vector
  float px = -dy;
  float py = dx;

  // Coordinates of head endpoints
  int hx1 = x2 + int(-dx * headLen + px * headWidth);
  int hy1 = y2 + int(-dy * headLen + py * headWidth);
  int hx2 = x2 + int(-dx * headLen - px * headWidth);
  int hy2 = y2 + int(-dy * headLen - py * headWidth);

  // Arrow color
  uint16_t col = (deg >= 135 && deg <= 225) ? TFT_YELLOW : TFT_CYAN;

  // Draw the arrow
  tft.drawLine(cx, cy, x2, y2, col);
  tft.fillTriangle(x2, y2, hx1, hy1, hx2, hy2, col);
  tft.drawTriangle(x2, y2, hx1, hy1, hx2, hy2, TFT_BLACK);

  // --- Compass ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);

  // North
  tft.setCursor(cx - 3, cy - size - 10);
  tft.print("N");
  // South
  tft.setCursor(cx - 3, cy + size + 2);
  tft.print("S");
  // East
  tft.setCursor(cx + size + 4, cy - 3);
  tft.print("E");
  // West
  tft.setCursor(cx - size - 10, cy - 3);
  tft.print("W");
}

// Smart weather update - redraw only changed elements
void updateWeatherDisplay()
{
  // Always update the indicator (shows data status)
  drawUpdateIndicator();

  if (!dataValid)
    return;

  // First draw - render everything
  if (firstDraw)
  {
    drawInterface();
    firstDraw = false;
    return;
  }

  // Conversions for comparison
  int pressure_mmhg = pressure_hpa * 0.75006;
  float wind_kmh = wind_speed_ms * 3.6;

  // === TEMPERATURE ===
  if (abs(temp_c - last_temp) > 0.1)
  {
    tft.fillRect(10, 100, 180, 40, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(5);
    tft.setCursor(10, 100);
    tft.print(temp_c, 1);
    tft.print(" C");
    last_temp = temp_c;
    Serial.println("Temp updated");
  }

  // === FEELS LIKE ===
  if (abs(feels_like - last_feels) > 0.1)
  {
    tft.fillRect(200, 105, 120, 30, TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(200, 105);
    tft.printf("%.1f C", feels_like);
    last_feels = feels_like;
    Serial.println("Feels updated");
  }

  // === WEATHER DESCRIPTION ===
  if (strcmp(weather_desc, last_weather_desc) != 0)
  {
    tft.fillRect(10, 60, 320, 20, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    int textSize = (strlen(weather_desc) > 23) ? 1 : 2;
    tft.setTextSize(textSize);
    tft.setCursor(10, 60);
    tft.print(weather_desc);
    strncpy(last_weather_desc, weather_desc, sizeof(last_weather_desc) - 1);
    last_weather_desc[sizeof(last_weather_desc) - 1] = '\0';
    Serial.println("Desc updated");
  }

  // === WEATHER ICON ===
  if (strcmp(weather_main, last_weather_main) != 0)
  {
    drawWeatherIcon(260, 38, weather_main);
    strncpy(last_weather_main, weather_main, sizeof(last_weather_main) - 1);
    last_weather_main[sizeof(last_weather_main) - 1] = '\0';
    Serial.println("Icon updated");
  }

  // === HUMIDITY ===
  if (humidity != last_humidity)
  {
    int y_pos = 150;
    tft.fillRect(50, y_pos, 100, 25, TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(50, y_pos);
    tft.printf("%d %%", humidity);
    last_humidity = humidity;
    Serial.println("Humidity updated");
  }

  // === WIND (SPEED) ===
  if (abs(wind_speed_ms - last_wind_speed) > 0.1)
  {
    int y_pos = 150;
    int row_h = 30;
    tft.fillRect(50, y_pos + row_h, 120, 25, TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(50, y_pos + row_h);
    tft.printf("%.1f km/h", wind_kmh);
    last_wind_speed = wind_speed_ms;
    Serial.println("Wind speed updated");
  }

  // === WIND COMPASS (handling 359°→0° wrap) ===
  int deg_diff = abs(wind_deg - last_wind_deg);
  if (deg_diff > 180)
    deg_diff = 360 - deg_diff;
  if (deg_diff > 5 || last_wind_deg == -1)
  {
    drawWindArrow(270, 185, wind_deg, 18, true);
    last_wind_deg = wind_deg;
    Serial.println("Wind dir updated");
  }

  // === PRESSURE ===
  if (abs(pressure_hpa - last_pressure) > 1)
  {
    int y_pos = 150;
    int row_h = 30;
    tft.fillRect(50, y_pos + row_h * 2, 150, 25, TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(50, y_pos + row_h * 2);
    tft.printf("%d mmHg", pressure_mmhg);
    last_pressure = pressure_hpa;
    Serial.println("Pressure updated");
  }
}

// --- INTERFACE RENDER (initial, full) ---
void drawInterface()
{
  // Clear weather area (leave clock untouched)
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  // Draw frames or separators
  tft.drawFastHLine(0, 85, 320, TFT_DARKGREY);

  // -- Weather description (adaptive font size) --
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Determine font size based on string length
  int textSize = 2;
  if (strlen(weather_desc) > 23)
  {
    textSize = 1;
  }
  tft.setTextSize(textSize);
  tft.setCursor(10, 60);
  // Clear area before printing
  tft.fillRect(10, 60, 320, 20, TFT_BLACK);
  tft.print(weather_desc);

  // -- Main weather (large) --
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(5); // Large font
  tft.setCursor(10, 100);
  tft.print(temp_c, 1);
  tft.print(" C");

  // -- Details (bottom) --
  tft.setTextColor(TFT_CYAN, TFT_BLACK);

  // Conversions
  int pressure_mmhg = pressure_hpa * 0.75006;
  float wind_kmh = wind_speed_ms * 3.6;

  int y_pos = 150;
  int x_col1 = 10;
  int x_col2 = 170;
  int row_h = 30;

  tft.setTextSize(3);
  tft.setCursor(200, 105);
  tft.printf("%.1f C", feels_like);

  tft.fillCircle(x_col1 + 10, y_pos + 5, 8, TFT_CYAN);
  tft.fillTriangle(x_col1 + 10 - 6, y_pos + 5, x_col1 + 10 + 6, y_pos + 5, x_col1 + 10, y_pos + 14 + 5, TFT_CYAN);
  tft.setCursor(x_col1 + 40, y_pos);
  tft.printf("%d %%", humidity);

  tft.drawLine(x_col1, y_pos + 5 + row_h, x_col1 + 20, y_pos + 5 + row_h, TFT_WHITE);
  tft.drawLine(x_col1, y_pos + 5 + row_h + 5, x_col1 + 20, y_pos + 5 + row_h + 5, TFT_WHITE);
  tft.drawLine(x_col1, y_pos + 5 + row_h + 10, x_col1 + 15, y_pos + 5 + row_h + 10, TFT_WHITE);
  // 'arc' at the end
  tft.drawCircle(x_col1 + 22, y_pos + 5 + row_h, 3, TFT_WHITE);
  tft.drawCircle(x_col1 + 22, y_pos + 5 + row_h + 5, 3, TFT_WHITE);
  tft.setCursor(x_col1 + 40, y_pos + row_h);
  tft.printf("%.1f km/h", wind_kmh);

  tft.drawCircle(x_col1 + 10, y_pos + 10 + row_h * 2, 10, TFT_YELLOW);
  // Down arrow inside
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 - 5, x_col1 + 10, y_pos + 10 + row_h * 2 + 5, TFT_YELLOW);
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 + 5, x_col1 + 10 - 3, y_pos + 10 + row_h * 2 + 2, TFT_YELLOW);
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 + 5, x_col1 + 10 + 3, y_pos + 10 + row_h * 2 + 2, TFT_YELLOW);
  tft.setCursor(x_col1 + 40, y_pos + row_h * 2);
  tft.printf("%d mmHg", pressure_mmhg);

  // Location (small font above time)
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 5);
  tft.fillRect(10, 5, 180, 10, TFT_BLACK);
  tft.print(location_str);

  // Last update indicator in the top-right corner
  drawUpdateIndicator();

  drawWindArrow(270, 185, wind_deg, 18, true);
  drawWeatherIcon(260, 38, weather_main);

  // Save all values after full draw
  last_temp = temp_c;
  last_feels = feels_like;
  last_humidity = humidity;
  last_pressure = pressure_hpa;
  last_wind_speed = wind_speed_ms;
  last_wind_deg = wind_deg;
  strncpy(last_weather_main, weather_main, sizeof(last_weather_main) - 1);
  last_weather_main[sizeof(last_weather_main) - 1] = '\0';
  strncpy(last_weather_desc, weather_desc, sizeof(last_weather_desc) - 1);
  last_weather_desc[sizeof(last_weather_desc) - 1] = '\0';
  {
    int hh = timeClient.getHours();
    int mm = timeClient.getMinutes();
    snprintf(last_time, sizeof(last_time), "%02d:%02d", hh, mm);
  }

  Serial.println("Full interface drawn");
}

// Draw the last-update indicator
void drawUpdateIndicator()
{
  // Clear update indicator area (wide enough for "Upd: HH:MM" + "!")
  tft.fillRect(255, 230, 80, 10, TFT_BLACK);

  // Print the update time
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKCYAN, TFT_BLACK);
  tft.setCursor(255, 230);
  tft.print("Upd: ");
  tft.print(lastUpdateTime);

  // If data is invalid, draw a red exclamation mark
  // First clear the top-right area where '!' may appear
  tft.fillRect(295, 0, 25, 16, TFT_BLACK);
  if (!dataValid)
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(305, 3);
    tft.print("!");
  }
}

void getGeoLocation()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClient client;
    if (client.connect("ip-api.com", 80))
    {
      client.print("GET /json HTTP/1.1\r\nHost: ip-api.com\r\nConnection: close\r\n\r\n");

      // Read response headers (until empty line), then parse JSON from the stream
      unsigned long startRead = millis();
      while ((millis() - startRead) < 5000 && client.connected())
      {
        // read header line-by-line into a small buffer
        char lineBuf[128];
        int idx = 0;
        while ((millis() - startRead) < 5000 && client.available())
        {
          char c = client.read();
          startRead = millis();
          if (c == '\n' || idx >= (int)sizeof(lineBuf) - 1)
          {
            break;
          }
          lineBuf[idx++] = c;
        }
        lineBuf[idx] = '\0';
        // account for CR
        if (idx <= 1)
        {
          // Empty line — end of headers
          break;
        }
      }

      // Now parse JSON directly from the client stream
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, client);

      if (!error)
      {
        const char *ip = doc["query"] | "";
        const char *country = doc["country"] | "";
        const char *code = doc["countryCode"] | "";
        const char *city_name = doc["city"] | "";

        // If IP geolocation returned latitude/longitude, copy them into active buffers
        if (doc.containsKey("lat") && doc.containsKey("lon"))
        {
          float latf = doc["lat"] | 0.0;
          float lonf = doc["lon"] | 0.0;
          // format with 5 decimal places
          snprintf(active_lat, sizeof(active_lat), "%.5f", latf);
          snprintf(active_lon, sizeof(active_lon), "%.5f", lonf);
        }

        if (country[0] != '\0' && city_name[0] != '\0')
        {
          snprintf(location_str, sizeof(location_str), "%s, %s", country, city_name);
        }
        else if (country[0] != '\0')
        {
          strncpy(location_str, country, sizeof(location_str) - 1);
          location_str[sizeof(location_str) - 1] = '\0';
        }
        else if (city_name[0] != '\0')
        {
          strncpy(location_str, city_name, sizeof(location_str) - 1);
          location_str[sizeof(location_str) - 1] = '\0';
        }
        else
        {
          strncpy(location_str, "Unknown", sizeof(location_str) - 1);
          location_str[sizeof(location_str) - 1] = '\0';
        }

        Serial.printf("Geo OK: %s\n", location_str);
      }
      else
      {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setCursor(10, 20);
        tft.println("Geo JSON Error!");
        tft.setCursor(10, 50);
        tft.println(error.c_str());
        delay(5000);
      }
    }
    client.stop();
  }
}

// Update time only when the minute changes
void updateTimeDisplay()
{
  // Check time validity
  if (timeClient.getEpochTime() < 1000000)
  {
    return; // NTP not synchronized yet
  }

  char currentTimeBuf[6] = "";
  int hh = timeClient.getHours();
  int mm = timeClient.getMinutes();
  snprintf(currentTimeBuf, sizeof(currentTimeBuf), "%02d:%02d", hh, mm);

  // Update only if the minute changed
  if (strcmp(currentTimeBuf, last_time) != 0 || firstDraw)
  {
    // Draw text with background to overwrite old characters without flicker
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(4);
    tft.setCursor(10, 25);
    // Use print() with spaces to ensure leftover characters are cleared
    tft.print(currentTimeBuf);
    tft.print("  "); // Extra spaces clear remnants of wide digits

    strncpy(last_time, currentTimeBuf, sizeof(last_time) - 1);
    last_time[sizeof(last_time) - 1] = '\0';
    Serial.printf("Time updated: %s\n", currentTimeBuf);
  }
}

// Backlight brightness control based on time of day
void updateBacklightMode()
{
  static uint8_t lastBrightness = 0;
  uint8_t targetBrightness;

  int hour = timeClient.getHours();

  // Night mode: 23:00 - 07:00 -> 2%
  if (hour >= 23 || hour < 7)
  {
    targetBrightness = 2;
  }
  else
  {
    targetBrightness = 30;
  }

  // Reset manual brightness when day/night mode changes
  if (targetBrightness != lastBrightness && lastBrightness != 0)
  {
    manualBrightnessSet = false;
  }

  // Update only if changed
  if (targetBrightness != lastBrightness)
  {
    setBacklightPercent(targetBrightness);
    lastBrightness = targetBrightness;
    Serial.printf("Backlight: %d%%\n", targetBrightness);
  }
}

// --- ENHANCED WEATHER ICONS ---
void drawWeatherIcon(int x, int y, const char *type)
{
  // Clear the icon area completely (50x50 pixels)
  tft.fillRect(x - 25, y - 25, 50, 50, TFT_BLACK);

  if (strcmp(type, "Clear") == 0)
  {
    // Sun with gradient layers (three concentric circles)
    // Layer 1: Core (white)
    tft.fillCircle(x, y, 6, TFT_WHITE);
    // Layer 2: Main body (bright yellow)
    tft.fillCircle(x, y, 10, TFT_YELLOW);
    tft.drawCircle(x, y, 6, TFT_YELLOW); // smooth transition
    // Layer 3: Outer ring (orange gradient)
    tft.drawCircle(x, y, 11, TFT_ORANGE);
    tft.drawCircle(x, y, 12, TFT_ORANGE);
    tft.drawCircle(x, y, 13, 0xFD20); // Dark orange

    // Sun rays (8 directions)
    for (int i = 0; i < 8; i++)
    {
      float angle = i * PI / 4.0;
      int x1 = x + cos(angle) * 16;
      int y1 = y + sin(angle) * 16;
      int x2 = x + cos(angle) * 22;
      int y2 = y + sin(angle) * 22;
      tft.drawLine(x1, y1, x2, y2, TFT_ORANGE);
    }
  }
  else if (strcmp(type, "Clouds") == 0)
  {
    // Cloud with drop shadow effect
    int shadowOffset = 2;

    // Layer 1: Shadow (dark grey cloud with offset)
    tft.fillCircle(x - 12 + shadowOffset, y + 3 + shadowOffset, 8, TFT_DARKGREY);
    tft.fillCircle(x - 5 + shadowOffset, y - 2 + shadowOffset, 10, TFT_DARKGREY);
    tft.fillCircle(x + 5 + shadowOffset, y + shadowOffset, 9, TFT_DARKGREY);
    tft.fillCircle(x + 12 + shadowOffset, y + 3 + shadowOffset, 7, TFT_DARKGREY);
    tft.fillRect(x - 12 + shadowOffset, y + 3 + shadowOffset, 24, 8, TFT_DARKGREY);

    // Layer 2: Main cloud (white)
    tft.fillCircle(x - 12, y + 3, 8, TFT_WHITE);
    tft.fillCircle(x - 5, y - 2, 10, TFT_WHITE);
    tft.fillCircle(x + 5, y, 9, TFT_WHITE);
    tft.fillCircle(x + 12, y + 3, 7, TFT_WHITE);
    tft.fillRect(x - 12, y + 3, 24, 8, TFT_WHITE);

    // Outline for clarity
    tft.drawCircle(x - 12, y + 3, 8, TFT_LIGHTGREY);
    tft.drawCircle(x - 5, y - 2, 10, TFT_LIGHTGREY);
    tft.drawCircle(x + 5, y, 9, TFT_LIGHTGREY);
    tft.drawCircle(x + 12, y + 3, 7, TFT_LIGHTGREY);
  }
  else if (strcmp(type, "Rain") == 0 || strcmp(type, "Drizzle") == 0)
  {
    // Rain cloud + drops with gradient
    tft.fillCircle(x - 10, y - 5, 7, TFT_DARKGREY);
    tft.fillCircle(x - 3, y - 8, 8, TFT_DARKGREY);
    tft.fillCircle(x + 5, y - 6, 7, TFT_DARKGREY);
    tft.fillCircle(x + 10, y - 3, 6, TFT_DARKGREY);
    tft.fillRect(x - 10, y - 3, 20, 6, TFT_DARKGREY);

    // Rain drops with gradient (cyan → blue)
    // Drop 1
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x - 8, y + 6 + i, color);
    }
    // Drop 2
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x, y + 8 + i, color);
    }
    // Drop 3
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x + 8, y + 6 + i, color);
    }

    // Dots at the bottom of drops
    tft.fillCircle(x - 8, y + 15, 1, TFT_CYAN);
    tft.fillCircle(x, y + 17, 1, TFT_CYAN);
    tft.fillCircle(x + 8, y + 15, 1, TFT_CYAN);
  }
  else if (strcmp(type, "Thunderstorm") == 0)
  {
    // Thunder cloud + lightning
    tft.fillCircle(x - 10, y - 8, 7, 0x4208); // Dark grey
    tft.fillCircle(x - 3, y - 10, 8, 0x4208);
    tft.fillCircle(x + 5, y - 8, 7, 0x4208);
    tft.fillRect(x - 10, y - 5, 20, 6, 0x4208);
    // Lightning
    tft.drawLine(x + 2, y - 3, x - 2, y + 4, TFT_YELLOW);
    tft.drawLine(x - 2, y + 4, x + 1, y + 4, TFT_YELLOW);
    tft.drawLine(x + 1, y + 4, x - 3, y + 12, TFT_YELLOW);
    tft.drawLine(x + 3, y - 3, x - 1, y + 4, TFT_YELLOW);
  }
  else if (strcmp(type, "Snow") == 0)
  {
    // Cloud + snowflakes
    tft.fillCircle(x - 10, y - 5, 7, TFT_LIGHTGREY);
    tft.fillCircle(x - 3, y - 8, 8, TFT_LIGHTGREY);
    tft.fillCircle(x + 5, y - 6, 7, TFT_LIGHTGREY);
    tft.fillRect(x - 10, y - 3, 18, 6, TFT_LIGHTGREY);
    // Snowflakes
    for (int i = 0; i < 3; i++)
    {
      int sx = x - 8 + i * 8;
      int sy = y + 6 + (i % 2) * 4;
      tft.drawLine(sx - 3, sy, sx + 3, sy, TFT_WHITE);
      tft.drawLine(sx, sy - 3, sx, sy + 3, TFT_WHITE);
      tft.drawLine(sx - 2, sy - 2, sx + 2, sy + 2, TFT_WHITE);
      tft.drawLine(sx - 2, sy + 2, sx + 2, sy - 2, TFT_WHITE);
    }
  }
  else if (strcmp(type, "Mist") == 0 || strcmp(type, "Fog") == 0 || strcmp(type, "Haze") == 0)
  {
    // Fog (horizontal lines)
    tft.drawLine(x - 15, y - 8, x + 15, y - 8, TFT_LIGHTGREY);
    tft.drawLine(x - 18, y - 3, x + 18, y - 3, TFT_LIGHTGREY);
    tft.drawLine(x - 15, y + 2, x + 15, y + 2, TFT_LIGHTGREY);
    tft.drawLine(x - 18, y + 7, x + 18, y + 7, TFT_LIGHTGREY);
    tft.drawLine(x - 12, y + 12, x + 12, y + 12, TFT_LIGHTGREY);
  }
  else
  {
    // Default - question mark
    tft.setTextSize(3);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(x - 8, y - 12);
    tft.print("?");
  }
}

// --- FLASH BUTTON HANDLING (STATE MACHINE) ---
void handleButton()
{
  bool currentState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Debounce protection - ignore changes faster than 50 ms
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH;

  // Clear overlay on timeout (1.5 seconds)
  if (overlayActive && (now - overlayShowTime >= 1500))
  {
    tft.fillRect(100, 110, 140, 20, TFT_BLACK);
    overlayActive = false;
  }

  if (currentState != lastButtonState)
  {
    lastDebounceTime = now;
  }

  if ((now - lastDebounceTime) > 50)
  {
    // Button pressed (LOW because of INPUT_PULLUP)
    if (currentState == LOW && !buttonPressed)
    {
      buttonPressed = true;
      buttonPressStart = now;
      buttonHandled = false;
      isLongPressProcessed = false;
      Serial.println("Button pressed");
    }

    // Button released
    if (currentState == HIGH && buttonPressed)
    {
      unsigned long pressDuration = now - buttonPressStart;
      buttonPressed = false;

      if (!buttonHandled && pressDuration < 600)
      {
        // Short press - refresh weather
        Serial.println("Short press - refreshing weather");
        strncpy(overlayMessage, "Refreshing...", sizeof(overlayMessage) - 1);
        overlayMessage[sizeof(overlayMessage) - 1] = '\0';
        showOverlay(overlayMessage, TFT_YELLOW);

        getWeatherData();
        updateWeatherDisplay();

        buttonHandled = true;
      }

      // Reset flags
      isLongPressProcessed = false;
    }

    // Long press - change brightness (only once)
    if (currentState == LOW && buttonPressed && !isLongPressProcessed)
    {
      unsigned long pressDuration = now - buttonPressStart;
      if (pressDuration >= 600)
      {
        isLongPressProcessed = true;
        buttonHandled = true;

        // Cycle brightness levels: 2% -> 10% -> 15% -> 25% -> 30% -> 60% -> 2%
        currentBrightnessIndex = (currentBrightnessIndex + 1) % 6;
        uint8_t newBrightness = brightnessLevels[currentBrightnessIndex];
        setBacklightPercent(newBrightness);
        manualBrightnessSet = true;

        Serial.printf("Long press - brightness changed to %d%%\n", newBrightness);

        // Show brightness level
        char msg[20];
        sprintf(msg, "LED: %d%%", newBrightness);
        strncpy(overlayMessage, msg, sizeof(overlayMessage) - 1);
        overlayMessage[sizeof(overlayMessage) - 1] = '\0';
        showOverlay(overlayMessage, TFT_CYAN);
      }
    }
  }

  lastButtonState = currentState;
}

// Show overlay with message
void showOverlay(const char *message, uint16_t color)
{
  tft.fillRect(100, 110, 140, 20, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(100, 110);
  tft.print(message);

  overlayActive = true;
  overlayShowTime = millis();
}