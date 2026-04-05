/*
 * Weather Station for Dublin (Saggart)
 * Hardware: ESP8266 + ILI9341 TFT
 * API: OpenWeatherMap (JSON)
 */

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>    // Graphics library
#include <ArduinoJson.h> // JSON Parsing
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include "secrets.h"

const char *SSID = SSID;
const char *PASSWORD = PASSWORD;
const char *API_KEY = API_KEY;

// --- КОНФИГУРАЦИЯ СЕТИ ---
const char *ssid = SSID;
const char *password = PASSWORD;

// --- КОНФИГУРАЦИЯ ПОГОДЫ ---
// Получите бесплатный ключ здесь: https://openweathermap.org/api
String apiKey = API_KEY;

// Координаты 
String lat = LAT;
String lon = LON;
String lang = LANG;

// --- НАСТРОЙКИ ДИСПЛЕЯ И ВРЕМЕНИ ---
TFT_eSPI tft = TFT_eSPI();
WiFiUDP ntpUDP;
// Смещение для Дублина (GMT/BST). Дублин меняет время, здесь ставим базовое + автокоррекцию
// Зимой 0, Летом 3600. Для простоты ставим 0 и добавляем логику, или используем time.h
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 0, 60000);

unsigned long lastWeatherUpdate = 0;
unsigned long weatherInterval = 900000; // Обновлять погоду каждые 15 минут (900000 мс)
unsigned long lastCycle = 0;
const unsigned long cycleMs = 900000; // 15 минут
const unsigned long sleepMs = 840000; // 14 минут (Wi-Fi off)
bool sleepingRadio = false;
String location_str = "Unknown";

// NTP синхронизация
unsigned long lastNTPSync = 0;
const unsigned long ntpSyncInterval = 86400000; // 24 часа

// Переменные для отслеживания изменений (статичный интерфейс)
String last_time = "";
float last_temp = -999.0;
float last_feels = -999.0;
int last_humidity = -1;
int last_pressure = -1;
float last_wind_speed = -999.0;
int last_wind_deg = -1;
String last_weather_main = "";
String last_weather_desc = "";
bool firstDraw = true;

// Переменные для хранения данных
float temp_c = 0;
float feels_like = 0;
int humidity = 0;
int pressure_hpa = 0;
float wind_speed_ms = 0;
String weather_desc = "--";
String weather_main = "";
long timezone_offset = 0;
int wind_deg = 0;
bool dataValid = false;
String lastUpdateTime = "--:--";

const uint8_t BACKLIGHT_PIN = D2; // GPIO5, поддерживает PWM
const uint16_t PWM_MAX = 1023;    // диапазон ESP8266
const uint8_t BUTTON_PIN = 0;     // Кнопка FLASH (GPIO 0)

// Переменные для кнопки
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool buttonHandled = false;
bool isLongPressProcessed = false;

// UI оверлей для кнопки
unsigned long overlayShowTime = 0;
bool overlayActive = false;
String overlayMessage = "";

// Управление яркостью
uint8_t brightnessLevels[] = {2, 10, 15, 25, 30, 60};
uint8_t currentBrightnessIndex = 4; // Начинаем с 30%
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

  // Снижение частоты CPU для экономии энергии
  system_update_cpu_freq(80); // 80 МГц вместо 160 МГц (-30% энергии)

  // Режим энергосбережения Wi-Fi
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  // Настройка кнопки FLASH
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWriteRange(PWM_MAX); // установить диапазон
  analogWriteFreq(100);      // частота PWM, Гц (опционально)
  setBacklightPercent(30);   // начальная яркость 30%

  // ПРИМЕЧАНИЕ: Для снижения SPI частоты до 20 МГц
  // измените #define SPI_FREQUENCY в файле User_Setup.h библиотеки TFT_eSPI

  // Инициализация дисплея
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.println("Connecting to WiFi...");
  tft.println(ssid);

  // Подключение к WiFi
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    tft.print(".");
    attempt++;
    if (attempt > 20)
    {
      tft.fillScreen(TFT_RED);
      tft.setCursor(10, 50);
      tft.println("WiFi Failed!");
      delay(2000);
      ESP.restart();
    }
  }

  // Wi-Fi подключен
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 20);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("WiFi Connected!");

  // Получение геолокации
  tft.setCursor(10, 60);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Getting Geo...");

  getGeoLocation();

  // Печатаем результат только при ошибке
  if (location_str == "Unknown")
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.println("Geo Failed");
    delay(3000);
  }
  // Иначе просто продолжаем без показа промежуточного экрана

  // Инициализация времени
  timeClient.begin();

  // Ожидание синхронизации времени
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
    // Проверяем, что время реально синхронизировано
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

  // Первый запуск обновления погоды с повторными попытками
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

  // Обновление времени в начале loop для плавности
  timeClient.update();

  // Обработка кнопки FLASH
  handleButton();

  // NTP синхронизация раз в сутки
  if (now - lastNTPSync >= ntpSyncInterval || lastNTPSync == 0)
  {
    if (WiFi.status() == WL_CONNECTED || !sleepingRadio)
    {
      timeClient.update();
      lastNTPSync = now;
      Serial.println("NTP synced");
    }
  }

  // Обновление времени только при изменении минуты
  updateTimeDisplay();

  // Ночной режим подсветки (23:00 - 07:00 → 2%) - только если яркость не установлена вручную
  if (!manualBrightnessSet)
  {
    updateBacklightMode();
  }

  // Обновление погоды каждые 15 минут
  if (now - lastWeatherUpdate >= weatherInterval)
  {
    getWeatherData();
    updateWeatherDisplay();
    lastWeatherUpdate = now;
  }
}

// --- ФУНКЦИЯ ПОЛУЧЕНИЯ ПОГОДЫ ---
void getWeatherData()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClient client;
    HTTPClient http;

    // Формируем URL запроса
    String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + lat + "&lon=" + lon + "&units=metric&appid=" + apiKey + "&lang=" + lang;

    Serial.println("Requesting: " + url);

    // Используем HTTPClient для корректной обработки кодов ответа
    http.begin(client, url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
      String payload = http.getString();

      // Парсинг JSON с уменьшенным размером буфера
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (!error && doc.containsKey("main"))
      {
        // Безопасное чтение с значениями по умолчанию
        temp_c = doc["main"]["temp"] | 0.0;
        feels_like = doc["main"]["feels_like"] | 0.0;
        humidity = doc["main"]["humidity"] | 0;
        pressure_hpa = doc["main"]["pressure"] | 0;
        wind_speed_ms = doc["wind"]["speed"] | 0.0;
        wind_deg = doc["wind"]["deg"] | 0;
        weather_desc = doc["weather"][0]["description"].as<String>();
        weather_main = doc["weather"][0]["main"].as<String>();
        timezone_offset = doc["timezone"] | 0;

        // Данные валидны, если JSON корректен и содержит ключ "main"
        dataValid = true;

        // Сохраняем время последнего успешного обновления (только если время синхронизировано)
        if (timeClient.getEpochTime() > 1000000)
        {
          lastUpdateTime = timeClient.getFormattedTime().substring(0, 5);
        }

        // Коррекция времени по смещению из API (авто-детект часового пояса)
        timeClient.setTimeOffset(timezone_offset);

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

// cx, cy - центр стрелки
// deg    - направление ветра в градусах (из OpenWeatherMap)
// size   - длина стрелки
// showTrajectory = true  -> стрелка показывает "куда дует"
// showTrajectory = false -> стрелка показывает "откуда дует"
void drawWindArrow(int cx, int cy, int deg, int size, bool showTrajectory)
{
  int pad = 4;
  tft.fillRect(cx - size - pad, cy - size - pad,
               (size * 2) + pad * 2, (size * 2) + pad * 2, TFT_BLACK);

  // Выбираем угол
  float angle_deg = showTrajectory ? deg + 180 : deg;

  // Переводим в радианы (инверсия по Y для TFT)
  float angle = -angle_deg * PI / 180.0;

  // Конечная точка стрелки
  float dx = cos(angle);
  float dy = sin(angle);
  int x2 = cx + int(dx * size);
  int y2 = cy + int(dy * size);

  // Размер "головки" стрелки
  int headLen = max(6, size / 3);
  int headWidth = max(4, size / 6);

  // Вектор перпендикулярный
  float px = -dy;
  float py = dx;

  // Координаты концов "головки"
  int hx1 = x2 + int(-dx * headLen + px * headWidth);
  int hy1 = y2 + int(-dy * headLen + py * headWidth);
  int hx2 = x2 + int(-dx * headLen - px * headWidth);
  int hy2 = y2 + int(-dy * headLen - py * headWidth);

  // Цвет стрелки
  uint16_t col = (deg >= 135 && deg <= 225) ? TFT_YELLOW : TFT_CYAN;

  // Рисуем стрелку
  tft.drawLine(cx, cy, x2, y2, col);
  tft.fillTriangle(x2, y2, hx1, hy1, hx2, hy2, col);
  tft.drawTriangle(x2, y2, hx1, hy1, hx2, hy2, TFT_BLACK);

  // --- Компас ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);

  // Север
  tft.setCursor(cx - 3, cy - size - 10);
  tft.print("N");
  // Юг
  tft.setCursor(cx - 3, cy + size + 2);
  tft.print("S");
  // Восток
  tft.setCursor(cx + size + 4, cy - 3);
  tft.print("E");
  // Запад
  tft.setCursor(cx - size - 10, cy - 3);
  tft.print("W");
}

// Умное обновление погодных данных - перерисовываем только изменения
void updateWeatherDisplay()
{
  // Обновляем индикатор всегда (показывает статус данных)
  drawUpdateIndicator();

  if (!dataValid)
    return;

  // Первая отрисовка - рисуем все
  if (firstDraw)
  {
    drawInterface();
    firstDraw = false;
    return;
  }

  // Конвертация для сравнения
  int pressure_mmhg = pressure_hpa * 0.75006;
  float wind_kmh = wind_speed_ms * 3.6;

  // === ТЕМПЕРАТУРА ===
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

  // === ОПИСАНИЕ ПОГОДЫ ===
  if (weather_desc != last_weather_desc)
  {
    tft.fillRect(10, 60, 320, 20, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    int textSize = (weather_desc.length() > 23) ? 1 : 2;
    tft.setTextSize(textSize);
    tft.setCursor(10, 60);
    tft.print(weather_desc);
    last_weather_desc = weather_desc;
    Serial.println("Desc updated");
  }

  // === ИКОНКА ПОГОДЫ ===
  if (weather_main != last_weather_main)
  {
    drawWeatherIcon(260, 38, weather_main);
    last_weather_main = weather_main;
    Serial.println("Icon updated");
  }

  // === ВЛАЖНОСТЬ ===
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

  // === ВЕТЕР (СКОРОСТЬ) ===
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

  // === КОМПАС ВЕТРА (с учетом перехода 359°→0°) ===
  int deg_diff = abs(wind_deg - last_wind_deg);
  if (deg_diff > 180)
    deg_diff = 360 - deg_diff;
  if (deg_diff > 5 || last_wind_deg == -1)
  {
    drawWindArrow(270, 185, wind_deg, 18, true);
    last_wind_deg = wind_deg;
    Serial.println("Wind dir updated");
  }

  // === ДАВЛЕНИЕ ===
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

// --- ОТРИСОВКА ИНТЕРФЕЙСА (первоначальная, полная) ---
void drawInterface()
{
  // Очищаем область погоды (не трогая часы)
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  // Рисуем рамки или разделители
  tft.drawFastHLine(0, 85, 320, TFT_DARKGREY);

  // -- Описание погоды (Адаптивный размер шрифта) --
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Определяем размер шрифта в зависимости от длины строки
  int textSize = 2;
  if (weather_desc.length() > 23)
  {
    textSize = 1;
  }
  tft.setTextSize(textSize);
  tft.setCursor(10, 60);
  // Очищаем область перед выводом
  tft.fillRect(10, 60, 320, 20, TFT_BLACK);
  tft.print(weather_desc);

  // -- Погода (Крупно) --
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(5); // Крупный шрифт
  tft.setCursor(10, 100);
  tft.print(temp_c, 1);
  tft.print(" C");

  // -- Детали (Внизу) --
  tft.setTextColor(TFT_CYAN, TFT_BLACK);

  // Конвертация
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
  // «дуга» на конце
  tft.drawCircle(x_col1 + 22, y_pos + 5 + row_h, 3, TFT_WHITE);
  tft.drawCircle(x_col1 + 22, y_pos + 5 + row_h + 5, 3, TFT_WHITE);
  tft.setCursor(x_col1 + 40, y_pos + row_h);
  tft.printf("%.1f km/h", wind_kmh);

  tft.drawCircle(x_col1 + 10, y_pos + 10 + row_h * 2, 10, TFT_YELLOW);
  // Стрелка вниз внутри
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 - 5, x_col1 + 10, y_pos + 10 + row_h * 2 + 5, TFT_YELLOW);
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 + 5, x_col1 + 10 - 3, y_pos + 10 + row_h * 2 + 2, TFT_YELLOW);
  tft.drawLine(x_col1 + 10, y_pos + 10 + row_h * 2 + 5, x_col1 + 10 + 3, y_pos + 10 + row_h * 2 + 2, TFT_YELLOW);
  tft.setCursor(x_col1 + 40, y_pos + row_h * 2);
  tft.printf("%d mmHg", pressure_mmhg);

  // Локация (маленький шрифт НАД временем)
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 5);
  tft.fillRect(10, 5, 180, 10, TFT_BLACK);
  tft.printf(location_str.c_str());

  // Индикатор последнего обновления в правом верхнем углу
  drawUpdateIndicator();

  drawWindArrow(270, 185, wind_deg, 18, true);
  drawWeatherIcon(260, 38, weather_main);

  // Сохраняем все значения после полной отрисовки
  last_temp = temp_c;
  last_feels = feels_like;
  last_humidity = humidity;
  last_pressure = pressure_hpa;
  last_wind_speed = wind_speed_ms;
  last_wind_deg = wind_deg;
  last_weather_main = weather_main;
  last_weather_desc = weather_desc;
  last_time = timeClient.getFormattedTime().substring(0, 5);

  Serial.println("Full interface drawn");
}

// Отрисовка индикатора последнего обновления
void drawUpdateIndicator()
{
  // Очищаем область индикатора (достаточно широкую для "Upd: HH:MM" + "!")
  tft.fillRect(255, 230, 80, 10, TFT_BLACK);

  // Выводим время обновления
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKCYAN, TFT_BLACK);
  tft.setCursor(255, 230);
  tft.print("Upd: ");
  tft.print(lastUpdateTime);

  // Если данные не валидны, рисуем красный восклицательный знак
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

      // Читаем весь ответ целиком
      String response = "";
      while (client.connected() || client.available())
      {
        if (client.available())
        {
          response += client.readString();
        }
      }

      // Ищем начало JSON
      int jsonStart = response.indexOf('{');
      if (jsonStart >= 0)
      {
        String payload = response.substring(jsonStart);
        Serial.println("JSON payload:");
        Serial.println(payload);

        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);

        if (!error)
        {
          String ip = doc["query"].as<String>();
          String country = doc["country"].as<String>();
          String code = doc["countryCode"].as<String>();
          String city = doc["city"].as<String>();

          if (country.length() > 0 && city.length() > 0)
          {
            location_str = country + ", " + city;
          }
          else if (country.length() > 0)
          {
            location_str = country;
          }
          else if (city.length() > 0)
          {
            location_str = city;
          }
          else
          {
            location_str = "Unknown";
          }

          // Дебаг только в Serial, не показываем на экране
          Serial.printf("Geo OK: %s\n", location_str.c_str());
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
      else
      {
        Serial.println("JSON not found in response!");
      }
    }
    client.stop();
  }
}

// Обновление времени только при изменении минуты
void updateTimeDisplay()
{
  // Проверяем валидность времени
  if (timeClient.getEpochTime() < 1000000)
  {
    return; // NTP еще не синхронизировано
  }

  String currentTime = timeClient.getFormattedTime().substring(0, 5);

  // Обновляем только если минута изменилась
  if (currentTime != last_time || firstDraw)
  {
    // Рисуем текст с фоном для перезаписи старых символов без мерцания
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(4);
    tft.setCursor(10, 25);
    // Используем print() с пробелами для гарантированной очистки остатков
    tft.print(currentTime);
    tft.print("  "); // Дополнительные пробелы очищают остатки широких цифр

    last_time = currentTime;
    Serial.printf("Time updated: %s\n", currentTime.c_str());
  }
}

// Управление яркостью подсветки в зависимости от времени суток
void updateBacklightMode()
{
  static uint8_t lastBrightness = 0;
  uint8_t targetBrightness;

  int hour = timeClient.getHours();

  // Ночной режим: 23:00 - 07:00 → 2%
  if (hour >= 23 || hour < 7)
  {
    targetBrightness = 2;
  }
  else
  {
    targetBrightness = 30;
  }

  // При смене режима день/ночь сбрасываем ручную настройку
  if (targetBrightness != lastBrightness && lastBrightness != 0)
  {
    manualBrightnessSet = false;
  }

  // Обновляем только если изменилось
  if (targetBrightness != lastBrightness)
  {
    setBacklightPercent(targetBrightness);
    lastBrightness = targetBrightness;
    Serial.printf("Backlight: %d%%\n", targetBrightness);
  }
}

// --- УЛУЧШЕННЫЕ ИКОНКИ ПОГОДЫ ---
void drawWeatherIcon(int x, int y, String type)
{
  // Очищаем область иконки полностью (50x50 пикселей)
  tft.fillRect(x - 25, y - 25, 50, 50, TFT_BLACK);

  if (type == "Clear")
  {
    // Солнце с градиентными слоями (три концентрических круга)
    // Слой 1: Ядро (белое)
    tft.fillCircle(x, y, 6, TFT_WHITE);
    // Слой 2: Основное тело (ярко-желтое)
    tft.fillCircle(x, y, 10, TFT_YELLOW);
    tft.drawCircle(x, y, 6, TFT_YELLOW); // Плавный переход
    // Слой 3: Внешнее кольцо (оранжевый градиент)
    tft.drawCircle(x, y, 11, TFT_ORANGE);
    tft.drawCircle(x, y, 12, TFT_ORANGE);
    tft.drawCircle(x, y, 13, 0xFD20); // Темно-оранжевый

    // Лучи солнца (8 направлений)
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
  else if (type == "Clouds")
  {
    // Облако с эффектом тени (Drop Shadow)
    int shadowOffset = 2;

    // Слой 1: Тень (темно-серое облако со смещением)
    tft.fillCircle(x - 12 + shadowOffset, y + 3 + shadowOffset, 8, TFT_DARKGREY);
    tft.fillCircle(x - 5 + shadowOffset, y - 2 + shadowOffset, 10, TFT_DARKGREY);
    tft.fillCircle(x + 5 + shadowOffset, y + shadowOffset, 9, TFT_DARKGREY);
    tft.fillCircle(x + 12 + shadowOffset, y + 3 + shadowOffset, 7, TFT_DARKGREY);
    tft.fillRect(x - 12 + shadowOffset, y + 3 + shadowOffset, 24, 8, TFT_DARKGREY);

    // Слой 2: Основное облако (белое)
    tft.fillCircle(x - 12, y + 3, 8, TFT_WHITE);
    tft.fillCircle(x - 5, y - 2, 10, TFT_WHITE);
    tft.fillCircle(x + 5, y, 9, TFT_WHITE);
    tft.fillCircle(x + 12, y + 3, 7, TFT_WHITE);
    tft.fillRect(x - 12, y + 3, 24, 8, TFT_WHITE);

    // Обводка для четкости
    tft.drawCircle(x - 12, y + 3, 8, TFT_LIGHTGREY);
    tft.drawCircle(x - 5, y - 2, 10, TFT_LIGHTGREY);
    tft.drawCircle(x + 5, y, 9, TFT_LIGHTGREY);
    tft.drawCircle(x + 12, y + 3, 7, TFT_LIGHTGREY);
  }
  else if (type == "Rain" || type == "Drizzle")
  {
    // Дождевое облако + капли с градиентом
    tft.fillCircle(x - 10, y - 5, 7, TFT_DARKGREY);
    tft.fillCircle(x - 3, y - 8, 8, TFT_DARKGREY);
    tft.fillCircle(x + 5, y - 6, 7, TFT_DARKGREY);
    tft.fillCircle(x + 10, y - 3, 6, TFT_DARKGREY);
    tft.fillRect(x - 10, y - 3, 20, 6, TFT_DARKGREY);

    // Капли дождя с градиентом (голубой → синий)
    // Капля 1
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x - 8, y + 6 + i, color);
    }
    // Капля 2
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x, y + 8 + i, color);
    }
    // Капля 3
    for (int i = 0; i < 8; i++)
    {
      uint16_t color = (i < 4) ? TFT_CYAN : TFT_BLUE;
      tft.drawPixel(x + 8, y + 6 + i, color);
    }

    // Точки внизу капель
    tft.fillCircle(x - 8, y + 15, 1, TFT_CYAN);
    tft.fillCircle(x, y + 17, 1, TFT_CYAN);
    tft.fillCircle(x + 8, y + 15, 1, TFT_CYAN);
  }
  else if (type == "Thunderstorm")
  {
    // Грозовое облако + молния
    tft.fillCircle(x - 10, y - 8, 7, 0x4208); // Темно-серый
    tft.fillCircle(x - 3, y - 10, 8, 0x4208);
    tft.fillCircle(x + 5, y - 8, 7, 0x4208);
    tft.fillRect(x - 10, y - 5, 20, 6, 0x4208);
    // Молния
    tft.drawLine(x + 2, y - 3, x - 2, y + 4, TFT_YELLOW);
    tft.drawLine(x - 2, y + 4, x + 1, y + 4, TFT_YELLOW);
    tft.drawLine(x + 1, y + 4, x - 3, y + 12, TFT_YELLOW);
    tft.drawLine(x + 3, y - 3, x - 1, y + 4, TFT_YELLOW);
  }
  else if (type == "Snow")
  {
    // Облако + снежинки
    tft.fillCircle(x - 10, y - 5, 7, TFT_LIGHTGREY);
    tft.fillCircle(x - 3, y - 8, 8, TFT_LIGHTGREY);
    tft.fillCircle(x + 5, y - 6, 7, TFT_LIGHTGREY);
    tft.fillRect(x - 10, y - 3, 18, 6, TFT_LIGHTGREY);
    // Снежинки
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
  else if (type == "Mist" || type == "Fog" || type == "Haze")
  {
    // Туман (горизонтальные линии)
    tft.drawLine(x - 15, y - 8, x + 15, y - 8, TFT_LIGHTGREY);
    tft.drawLine(x - 18, y - 3, x + 18, y - 3, TFT_LIGHTGREY);
    tft.drawLine(x - 15, y + 2, x + 15, y + 2, TFT_LIGHTGREY);
    tft.drawLine(x - 18, y + 7, x + 18, y + 7, TFT_LIGHTGREY);
    tft.drawLine(x - 12, y + 12, x + 12, y + 12, TFT_LIGHTGREY);
  }
  else
  {
    // Дефолт - вопросительный знак
    tft.setTextSize(3);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(x - 8, y - 12);
    tft.print("?");
  }
}

// --- ОБРАБОТКА КНОПКИ FLASH (STATE MACHINE) ---
void handleButton()
{
  bool currentState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Защита от дребезга - игнорируем изменения быстрее 50 мс
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH;

  // Очистка оверлея по таймауту (1.5 секунды)
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
    // Кнопка нажата (LOW из-за INPUT_PULLUP)
    if (currentState == LOW && !buttonPressed)
    {
      buttonPressed = true;
      buttonPressStart = now;
      buttonHandled = false;
      isLongPressProcessed = false;
      Serial.println("Button pressed");
    }

    // Кнопка отпущена
    if (currentState == HIGH && buttonPressed)
    {
      unsigned long pressDuration = now - buttonPressStart;
      buttonPressed = false;

      if (!buttonHandled && pressDuration < 600)
      {
        // Короткое нажатие - обновление погоды
        Serial.println("Short press - refreshing weather");

        overlayMessage = "Refreshing...";
        showOverlay(overlayMessage, TFT_YELLOW);

        getWeatherData();
        updateWeatherDisplay();

        buttonHandled = true;
      }

      // Сброс флагов
      isLongPressProcessed = false;
    }

    // Длинное нажатие - смена яркости (только один раз)
    if (currentState == LOW && buttonPressed && !isLongPressProcessed)
    {
      unsigned long pressDuration = now - buttonPressStart;
      if (pressDuration >= 600)
      {
        isLongPressProcessed = true;
        buttonHandled = true;

        // Циклическое переключение яркости: 2% -> 10% -> 15% -> 25% -> 30% -> 60% -> 2%
        currentBrightnessIndex = (currentBrightnessIndex + 1) % 6;
        uint8_t newBrightness = brightnessLevels[currentBrightnessIndex];
        setBacklightPercent(newBrightness);
        manualBrightnessSet = true;

        Serial.printf("Long press - brightness changed to %d%%\n", newBrightness);

        // Показываем уровень яркости
        char msg[20];
        sprintf(msg, "LED: %d%%", newBrightness);
        showOverlay(String(msg), TFT_CYAN);
      }
    }
  }

  lastButtonState = currentState;
}

// Показ оверлея с сообщением
void showOverlay(String message, uint16_t color)
{
  tft.fillRect(100, 110, 140, 20, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(100, 110);
  tft.print(message);

  overlayActive = true;
  overlayShowTime = millis();
}