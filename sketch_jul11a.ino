#include <WiFi.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include "character_sprite.h"
#include "hat_sprite.h"

const char* WIFI_SSID     = "WIFI_NAME";
const char* WIFI_PASSWORD = "WIFI_PWD";
const long  GMT_OFFSET_SEC      = -5 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 3600;
const char* NTP_SERVER = "pool.ntp.org";

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

enum CharState { NORMAL, HAT, DOG, DISH };
CharState charState = NORMAL;

enum TimeBand { MORNING, DAY, EVENING, NIGHT };

void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting WiFi...");

  connectWiFi();
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
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
}

void loop() {
  unsigned long now = millis();
  bool blinkChanged = false;
  if (!blinkState && now - lastBlinkToggle >= BLINK_INTERVAL_MS) {
    blinkState = true; lastBlinkToggle = now; blinkChanged = true;
  } else if (blinkState && now - lastBlinkToggle >= BLINK_DURATION_MS) {
    blinkState = false; lastBlinkToggle = now; blinkChanged = true;
  }

  bool touchDown = touchscreen.touched();
  bool stateChanged = false;
  if (touchDown && !touchWasDown) {
    charState = (CharState)((charState + 1) % 4); // normal -> hat -> chef -> dish -> normal
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
  delay(50);
}

TimeBand timeBandFor(int hour) {
  if (hour < 7) return NIGHT;          // 12am - 8am
  if (hour >= 7 && hour < 11) return MORNING;
  if (hour >= 11 && hour < 17) return DAY;
  return EVENING;                       // 5pm - midnight
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

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { delay(300); attempts++; }
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 10);
    tft.println("WiFi failed.");
    while (true) delay(1000);
  }
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
  } else if (band == EVENING) {
    frame = blinkState ? character_sprite_run_blink : character_sprite_run;
  } else {
    frame = blinkState ? character_sprite_blink : character_sprite_open;
  }
  tft.pushImage(x, charY, CHAR_WIDTH, CHAR_HEIGHT, frame, (uint16_t)0xF81F);
}