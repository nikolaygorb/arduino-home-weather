ESP8266 Weather Display

A minimal firmware for ESP8266 that displays current weather and time on an ILI9341 TFT.

What it does
- Fetches current weather (temperature, humidity, wind, pressure) from OpenWeatherMap and shows a simple icon.
- Syncs time via NTP.
- Optional IP geolocation (ip-api.com) to show city name.

Data sources
- OpenWeatherMap API (requires API key)
- ip-api.com (optional, for city via IP)
- NTP servers for time

Hardware
- ESP8266 (e.g., NodeMCU)
- ILI9341 TFT (TFT_eSPI)

Configuration
- Put Wi‑Fi credentials and OpenWeatherMap API key in `secrets.h`.
- Set `get_ip_geolocation` to `true` to enable IP geolocation, or `false` to use compile-time `LAT/LON/LANG/CITY`.

---

## v2: `weather_home_v2.ino` (ESP32-S3)

Second-generation firmware for a different hardware module, sharing the same wiring/config
pattern as `arduino-simdash/dashboard_digital.ino`.

Hardware
- ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- 1x TFT SPI 3.5" (480x320), ST7796, driven via LovyanGFX

Wiring (identical to `dashboard_digital.ino`)
```
MOSI: GPIO 11    SCLK: GPIO 12    DC: GPIO 9
RST:  GPIO  8    CS:   GPIO 10    LED: GPIO 13 (PWM)
VCC: 5V, GND: GND
```

What it does
- Same current-weather fetch/display approach as `weather_home.ino` (OpenWeatherMap
  `/data/2.5/weather`, NTP time sync, day/night backlight).
- Adds a 2-day outlook using OpenWeatherMap's free **5 Day / 3 Hour Forecast**
  (`/data/2.5/forecast`), aggregated client-side into "tomorrow" / "day after tomorrow"
  min/max temperature, rain probability and a representative condition icon.
- Neon/VFD-style rendering (glow text, full-screen sprite buffer) matching the look of
  `arduino-simdash/dashboard_digital.ino`.
- No physical button; brightness is set automatically by time of day (2% at night, 30% by day).

Configuration
- Reuses the same `API_KEY` from `secrets.h`.
- Uses its own location: `LAT2` / `LON2` / `LANG2` / `CITY2` in `secrets.h` (set these to the
  second station's real location before flashing).

Libraries required
- LovyanGFX, ArduinoJson, NTPClient (same set used by the ESP32 sim-dashboard project), plus
  the ESP32 core (WiFi, HTTPClient, WiFiClientSecure).


Build & flash
- Compile and flash `weather_home.ino` using the Arduino IDE or PlatformIO.

Notes
- Uses HTTPS for weather requests (WiFiClientSecure). Consider enabling certificate verification for stronger security.
- For memory tuning, reduce DynamicJsonDocument sizes or enable stream-only parsing if needed.
