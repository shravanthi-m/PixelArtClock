#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include "character_sprite.h"
#include "hat_sprite.h"

const char* NTP_SERVER = "pool.ntp.org";
const unsigned long TZ_REFRESH_MS = 6UL * 60UL * 60UL * 1000UL; // re-check every 6 hours

long gmtOffsetSec = -5 * 3600; // fallback default if the timezone lookup ever fails
unsigned long lastTzFetch = 0;

TFT_eSPI tft = TFT_eSPI();
const uint8_t SCREEN_ROTATION = 0;

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

int charY = 125;
int lastMinute = -1;
bool blinkState = false;
bool touchWasDown = false;
unsigned long lastBlinkToggle = 0;
const unsigned long BLINK_INTERVAL_MS = 7000;
const unsigned long BLINK_DURATION_MS = 150;
float sunsetHourLocal = 19.0; // fallback default (7pm) if the lookup ever fails
float userLat = 0, userLon = 0;

enum CharState { NORMAL, HAT, DISH, DOG };
CharState charState = NORMAL;

enum TimeBand { MORNING, DAY, EVENING, NIGHT };

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(34)); // reads noise off an unused analog pin for a real random seed
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting...");

  WiFiManager wm;
  bool connected = wm.autoConnect("PixelClock-Setup");

  if (!connected) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.println("WiFi setup failed.\nRestarting...");
    delay(3000);
    ESP.restart();
  }

  // fetchTimezoneOffset();
  fetchTimezoneAndLocation();
  configTime(gmtOffsetSec, 0, NTP_SERVER);
  lastTzFetch = millis();

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(SCREEN_ROTATION);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    tft.fillScreen(bgColorFor(timeBandFor(timeinfo.tm_hour)));
    drawTimeDate(timeinfo);
    lastMinute = timeinfo.tm_min;
  }
  drawCharacter();
  initFireflies();
}

void loop() {
  unsigned long now = millis();

  if (now - lastTzFetch >= TZ_REFRESH_MS) {
    fetchTimezoneAndLocation();
    configTime(gmtOffsetSec, 0, NTP_SERVER);
    lastTzFetch = now;
  }

  bool blinkChanged = false;
  if (!blinkState && now - lastBlinkToggle >= BLINK_INTERVAL_MS) {
    blinkState = true; lastBlinkToggle = now; blinkChanged = true;
  } else if (blinkState && now - lastBlinkToggle >= BLINK_DURATION_MS) {
    blinkState = false; lastBlinkToggle = now; blinkChanged = true;
  }

  bool touchDown = touchscreen.touched();
  bool stateChanged = false;
  if (touchDown && !touchWasDown) {
    charState = (CharState)((charState + 1) % 4);
    stateChanged = true;
  }
  touchWasDown = touchDown;

  if (blinkChanged || stateChanged) drawCharacter();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo) && timeinfo.tm_min != lastMinute) {
    lastMinute = timeinfo.tm_min;
    tft.fillScreen(bgColorFor(timeBandFor(timeinfo.tm_hour)));
    drawTimeDate(timeinfo);
    drawCharacter();
  }
  struct tm timeinfo2;
  if (getLocalTime(&timeinfo2)) {
    TimeBand currentBand = timeBandFor(timeinfo2.tm_hour);
  if (currentBand == EVENING || currentBand == NIGHT) {
    updateFireflies(bgColorFor(currentBand));
  }
}
  delay(50);
}

void fetchTimezoneAndLocation() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://ipapi.co/json/");
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, payload)) {
      const char* offsetStr = doc["utc_offset"];
      if (offsetStr && strlen(offsetStr) >= 5) {
        int sign = (offsetStr[0] == '-') ? -1 : 1;
        int hours = (offsetStr[1]-'0')*10 + (offsetStr[2]-'0');
        int mins  = (offsetStr[3]-'0')*10 + (offsetStr[4]-'0');
        gmtOffsetSec = sign * (hours*3600 + mins*60);
      }
      userLat = doc["latitude"] | 0.0;
      userLon = doc["longitude"] | 0.0;
      Serial.printf("Location: %.4f, %.4f | Offset: %ld sec\n", userLat, userLon, gmtOffsetSec);
    }
  } else {
    Serial.printf("Location/timezone fetch failed (%d)\n", code);
  }
  http.end();

  fetchSunsetTime();
}

void fetchSunsetTime() {
  if (WiFi.status() != WL_CONNECTED || (userLat == 0 && userLon == 0)) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  char url[150];
  snprintf(url, sizeof(url),
           "https://api.sunrise-sunset.org/json?lat=%.4f&lng=%.4f&formatted=0",
           userLat, userLon);
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, payload)) {
      const char* sunsetStr = doc["results"]["sunset"]; // e.g. "2026-07-12T23:45:00+00:00"
      if (sunsetStr) {
        int utcHour = (sunsetStr[11]-'0')*10 + (sunsetStr[12]-'0');
        int utcMin  = (sunsetStr[14]-'0')*10 + (sunsetStr[15]-'0');
        float utcHourFloat = utcHour + utcMin / 60.0;
        float localHour = utcHourFloat + (gmtOffsetSec / 3600.0);
        while (localHour < 0) localHour += 24;
        while (localHour >= 24) localHour -= 24;
        sunsetHourLocal = localHour;
        Serial.printf("Sunset today (local): %.2f\n", sunsetHourLocal);
      }
    }
  } else {
    Serial.printf("Sunset fetch failed (%d), using fallback\n", sunsetHourLocal);
  }
  http.end();
}

TimeBand timeBandFor(int hour) {
  if (hour < 8) return NIGHT;
  if (hour >= 8 && hour < 11) return MORNING;
  if (hour >= 11 && hour < (int)sunsetHourLocal) return DAY;
  return EVENING;
}

uint16_t bgColorFor(TimeBand band) {
  switch (band) {
    case MORNING: return tft.color565(35, 28, 20);
    case DAY:     return tft.color565(10, 15, 25);
    case EVENING: return tft.color565(35, 18, 10);
    case NIGHT:   return tft.color565(5, 5, 15);
  }
  return TFT_BLACK;
}

void drawTimeDate(struct tm &timeinfo) {
  uint16_t bg = bgColorFor(timeBandFor(timeinfo.tm_hour));
  tft.setTextColor(TFT_WHITE, bg);
  char timeStr[6];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  tft.setTextSize(5);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(timeStr, tft.width() / 2, 24);
  char dateStr[16];
  const char* days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
  snprintf(dateStr, sizeof(dateStr), "%s %s %d", days[timeinfo.tm_wday], months[timeinfo.tm_mon], timeinfo.tm_mday);
  tft.setTextSize(2);
  tft.drawString(dateStr, tft.width() / 2, 88);
  tft.setTextDatum(TL_DATUM);
}

void drawCharacter() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  TimeBand band = timeBandFor(timeinfo.tm_hour);
  uint16_t bg = bgColorFor(band);
  tft.fillRect(0, charY, tft.width(), CHAR_HEIGHT, bg);

  int x = (tft.width() - CHAR_WIDTH) / 2;
  const uint16_t* frame;

  if (charState == HAT) {
    frame = blinkState ? character_sprite_hat_blink : character_sprite_hat_open;
  } else if (charState == DOG) {
    frame = blinkState ? character_sprite_dog_blink : character_sprite_dog;
  } else if (charState == DISH) {
    frame = blinkState ? character_sprite_dish_blink : character_sprite_dish_open;
  } else if (band == NIGHT) {
    frame = blinkState ? character_sprite_sleepy_blink : character_sprite_sleepy;
  } else {
    frame = blinkState ? character_sprite_blink : character_sprite_open;
  }
  tft.pushImage(x, charY, CHAR_WIDTH, CHAR_HEIGHT, frame, (uint16_t)0xF81F);
}

struct Firefly { int x, y; unsigned long nextToggle; bool on; };
Firefly fireflies[6];

void initFireflies() {
  for (int i = 0; i < 6; i++) fireflies[i] = {0, 0, millis() + random(500, 3000), false};
}

void updateFireflies(uint16_t bg) {
  unsigned long now = millis();
  for (int i = 0; i < 6; i++) {
    if (now >= fireflies[i].nextToggle) {
      if (fireflies[i].on) drawFireflyGlow(fireflies[i].x, fireflies[i].y, false, bg);
      fireflies[i].on = !fireflies[i].on;
      if (fireflies[i].on) {
        int zone = random(3);
        if (zone == 0) {
          fireflies[i].x = random(20, 220);
          fireflies[i].y = random(charY - 22, charY - 4);
        } else if (zone == 1) {
          fireflies[i].x = random(15, 44);
          fireflies[i].y = random(charY, charY + CHAR_HEIGHT);
        } else {
          fireflies[i].x = random(196, 225);
          fireflies[i].y = random(charY, charY + CHAR_HEIGHT);
        }
        drawFireflyGlow(fireflies[i].x, fireflies[i].y, true, bg);
      }
      fireflies[i].nextToggle = now + random(400, 1800);
    }
  }
}

void drawFireflyGlow(int x, int y, bool on, uint16_t bg) {
  uint16_t CORE = tft.color565(255, 255, 210);
  uint16_t GLOW = tft.color565(255, 205, 60);
  if (!on) {
    tft.fillRect(x - 2, y - 2, 5, 5, bg);
    return;
  }

  tft.drawPixel(x,     y - 2, GLOW);
  tft.drawPixel(x - 2, y,     GLOW);
  tft.drawPixel(x + 2, y,     GLOW);
  tft.drawPixel(x,     y + 2, GLOW);
  tft.drawPixel(x - 1, y - 1, GLOW);
  tft.drawPixel(x + 1, y - 1, GLOW);
  tft.drawPixel(x - 1, y + 1, GLOW);
  tft.drawPixel(x + 1, y + 1, GLOW);

  tft.fillRect(x, y, 2, 2, CORE);
}