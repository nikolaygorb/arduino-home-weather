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

Build & flash
- Compile and flash `weather_home.ino` using the Arduino IDE or PlatformIO.

Notes
- Uses HTTPS for weather requests (WiFiClientSecure). Consider enabling certificate verification for stronger security.
- For memory tuning, reduce DynamicJsonDocument sizes or enable stream-only parsing if needed.
