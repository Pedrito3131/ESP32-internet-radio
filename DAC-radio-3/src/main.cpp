#include <Arduino.h>
#include <vector>
#include <time.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <Adafruit_FT6206.h>
#include "Audio.h"
#include "nvs.h"
#define A2DP_I2S_AUDIOTOOLS 0
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"

/*
CHANGELOG (v1-stable baseline)
- Stable dual-mode architecture: Internet radio + Bluetooth A2DP sink
- PSRAM-enabled radio buffering with underrun recovery
- Shared I2S output path tuned for crackle-free playback
- Bluetooth NVS namespace pre-initialization and reconnect chime restored
*/

// -------------------- User configuration --------------------
// Wi-Fi credentials for Internet radio mode.
static const char *WIFI_SSID = "Livebox-E330";
static const char *WIFI_PASSWORD = "KdF2DMXQo4gFRjPiG6";
static const char *BT_DEVICE_NAME = "DAC-radio-3";

// PCM5102 <-> ESP32 classic I2S pins.
static constexpr int I2S_BCLK = 26; // red
static constexpr int I2S_LRCK = 25; // yellow
static constexpr int I2S_DOUT = 22; // orange
static constexpr int I2S_MCLK = 0;  // white (ensure not held low at boot)

// ILI9488 TFT (SPI) <-> ESP32 pins.
static constexpr int LCD_SCK = 18;   // orange
static constexpr int LCD_MOSI = 23;  // red
static constexpr int LCD_MISO = 19;  // green
static constexpr int LCD_CS = 5;     // white
static constexpr int LCD_SD_CS = -1; // set to your SD_CS GPIO, or -1 if not wired
static constexpr int LCD_DC = 27;    // brown, named RS on the board
static constexpr int LCD_RST = 33;   // black
static constexpr int LCD_BL = -1;    // -1 when backlight is tied directly to 3V3.

// FT6236 capacitive touch (I2C) <-> ESP32 pins.
// NOTE: If your wiring still uses the common ESP32 default SCL pin (GPIO22), leave SCL on 22.
static constexpr int TOUCH_SDA = 21;  // light gray
static constexpr int TOUCH_SCL = 32;  // blue (set back to the common default for FT6236 boards)
static constexpr int TOUCH_INT = 34;  // white, moved from GPIO25 (input-only is fine)
static constexpr int TOUCH_RST = 16;  // purple, moved from GPIO26 (used by PCM5102)

// Output volume range for ESP32-audioI2S is 0..21.
// A slightly lower default helps avoid analog clipping/blurry sound.
static constexpr uint8_t START_VOLUME = 8;
static constexpr bool ENABLE_DIAGNOSTICS = false; // boot visuals + extra logs
static constexpr bool VERBOSE_AUDIO_LOGS = ENABLE_DIAGNOSTICS;

// NVS keys used to persist selected playback mode.
static const char *PREF_NS = "dacradio3";
static const char *PREF_MODE = "mode";
static const char *PREF_VOLUME = "volume";
static const char *PREF_STATION = "station";
static const char *TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";
static constexpr float WEATHER_LATITUDE = 43.592092f;
static constexpr float WEATHER_LONGITUDE = 1.432367f;

// Radio station model.
struct Station {
  const char *name;
  const char *url;
  bool enableIcyMetadata;
};

// Built-in station presets.
static const Station STATIONS[] = {
    // Use explicit http:// to satisfy ESP32-audioI2S host parser.
    {"France Inter", "http://icecast.radiofrance.fr/franceinter-midfi.mp3", true}, // "http://icecast.radiofrance.fr/franceinter-hifi.aac",
    {"Radio Classique", "http://radioclassique.ice.infomaniak.ch/radioclassique-high.mp3", true},
    {"FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3", true}, // "http://icecast.radiofrance.fr/fip-hifi.aac",
    {"France Culture", "http://icecast.radiofrance.fr/franceculture-midfi.mp3", true}, // "http://icecast.radiofrance.fr/franceculture-hifi.aac",
    {"France Musique", "http://icecast.radiofrance.fr/francemusique-midfi.mp3", true}, // "http://icecast.radiofrance.fr/francemusique-hifi.aac",
    {"Oui FM", "http://ouifm.ice.infomaniak.ch/ouifm-high.mp3", true},
    {"Radio Nova", "http://novazz.ice.infomaniak.ch/novazz-128.mp3", true},
    {"RTL2", "http://streaming.radio.rtl2.fr/rtl2-1-44-128", false},
    {"RFM", "http://rfm.lmn.fm/rfm.mp3", false}};

static constexpr size_t STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

// Playback mode persisted in NVS.
enum class PlaybackMode : uint8_t {
  Radio = 0,
  Bluetooth = 1,
};

// Main runtime objects.
static constexpr i2s_port_t AUDIO_I2S_PORT = I2S_NUM_1;
Audio audio(false, 3, AUDIO_I2S_PORT);
BluetoothA2DPSink a2dpSink;
Preferences prefs;
Adafruit_FT6206 touch;

struct DisplayCache {
  PlaybackMode mode = PlaybackMode::Radio;
  size_t stationIndex = SIZE_MAX;
  uint8_t volume = 255;
  wl_status_t wifi = WL_IDLE_STATUS;
  String title;
  String clock;
  String weatherText;
  String weatherIcons;
};

struct WeatherState {
  bool valid = false;
  unsigned long lastFetchMs = 0;
  float currentTempC = 0.0f;
  float humidityPct = 0.0f;
  float minTodayC = 0.0f;
  float maxTodayC = 0.0f;
  float minDay1C = 0.0f;
  float maxDay1C = 0.0f;
  int codeToday = -1;
  int codeDay1 = -1;
  int codeDay2 = -1;
  int codeDay3 = -1;
};

// Mutable runtime state.
PlaybackMode currentMode = PlaybackMode::Radio;
size_t stationIndex = 0;
volatile size_t requestedStationIndex = 0;
uint8_t volumeLevel = START_VOLUME;
unsigned long lastWifiRetryMs = 0;
bool btSinkStarted = false;
volatile bool stationChangePending = false;
volatile bool radioVolumeDirty = true;
volatile bool radioPrebuffering = false;
volatile bool radioRecoveringUnderrun = false;
TaskHandle_t radioTaskHandle = nullptr;
TFT_eSPI display = TFT_eSPI();
TFT_eSprite bottomBarSprite = TFT_eSprite(&display);
bool displayReady = false;
DisplayCache displayCache;
WeatherState weather;
bool displayUpdatesEnabled = true;
bool touchReady = false;
volatile bool displayForceRefreshPending = false;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastUiMaintenanceMs = 0;
uint8_t lastSavedVolume = START_VOLUME;
size_t lastSavedStationIndex = 0;
bool statePersistencePending = false;
static constexpr size_t STREAM_TEXT_CAP = 256;
portMUX_TYPE metadataMux = portMUX_INITIALIZER_UNLOCKED;
char radioStreamTitle[STREAM_TEXT_CAP] = "";
char btTrackTitle[STREAM_TEXT_CAP] = "";
char btTrackArtist[STREAM_TEXT_CAP] = "";
static constexpr uint32_t DISPLAY_REFRESH_INTERVAL_MS = 250;
static constexpr uint32_t UI_MAINTENANCE_INTERVAL_MS = 5000;
static constexpr uint32_t WEATHER_REFRESH_INTERVAL_MS = 30UL * 60UL * 1000UL;
static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;
static constexpr int STATUS_LINE_Y = 74;
static constexpr int STATUS_LINE_H = 34;
static constexpr int CLOCK_BAR_Y = 280;
static constexpr int CLOCK_BAR_H = 40;
static constexpr int BTN_H = 44;
static constexpr int BTN_GAP = 10;
// Volume slider geometry (top area).
static constexpr int SLIDER_X = 12;
static constexpr int SLIDER_Y = 20;
static constexpr int SLIDER_W = 240;
static constexpr int SLIDER_H = 26;  // thicker bar
static constexpr int SLIDER_KNOB_W = 52; // larger to fit volume text
static constexpr int SLIDER_BTN_W = 48;
static constexpr int SLIDER_BTN_H = 44;
static constexpr int SLIDER_GAP = 12;
// Touch calibration (FT6236 / rotation = 3).
// If hits feel shifted, adjust these min/max values to the raw numbers printed over Serial.
// Defaults pull the active area slightly inward to fix the common horizontal offset.
static constexpr int TOUCH_RAW_X_MIN = 10;  // raw p.x (portrait) low end
static constexpr int TOUCH_RAW_X_MAX = 310; // raw p.x (portrait) high end
static constexpr int TOUCH_RAW_Y_MIN = 20;  // raw p.y (portrait) low end
static constexpr int TOUCH_RAW_Y_MAX = 460; // raw p.y (portrait) high end
static constexpr bool TOUCH_INVERT_X = true;  // set true if left/right are mirrored
static constexpr bool TOUCH_INVERT_Y = false; // set true if up/down are mirrored

enum class ButtonAction : uint8_t {
  VolUp,
  VolDown,
  ToggleMode,
  BtPrev,
  BtPlayPause,
  BtNext,
  StationBase // Station index will be added to this base
};

struct UiButton {
  String label;
  int x;
  int y;
  int w;
  int h;
  ButtonAction action;
  size_t stationOffset; // only used for station buttons
  bool pressed;
  unsigned long pressedAtMs;
};

struct UiState {
  std::vector<UiButton> buttons;
  bool sliderPressed = false;
  unsigned long sliderPressedAtMs = 0;
  bool volumeButtonsTouchActive = false;
  UiButton sliderMinus{"-", SLIDER_X, SLIDER_Y - 8, SLIDER_BTN_W, SLIDER_BTN_H,
                       ButtonAction::VolDown, 0, false, 0};
  UiButton sliderPlus{"+", SLIDER_X + SLIDER_BTN_W + SLIDER_GAP + SLIDER_W + SLIDER_GAP, SLIDER_Y - 8,
                      SLIDER_BTN_W, SLIDER_BTN_H, ButtonAction::VolUp, 0, false, 0};
};

UiState ui;

// Simple I2C probe for the FT6236 address (0x38).
void scanI2C() {
  Serial.println("I2C scan:");
  uint8_t count = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  found 0x%02X\n", addr);
      count++;
    }
  }
  if (count == 0) {
    Serial.println("  no devices found");
  }
}

bool probeTouchController(uint8_t &foundAddr) {
  const uint8_t candidates[] = {0x38, 0x5D}; // common FT62x6 addresses
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      foundAddr = addr;
      Serial.printf("FT6236/FT6206 found at 0x%02X.\n", addr);
      return true;
    }
  }
  Serial.println("FT6236 probe failed: no ACK on 0x38/0x5D.");
  return false;
}

// Draw one UI button with current pressed state.
void drawButton(UiButton &btn) {
  display.setTextFont(2);
  // Larger text for volume +/- and BT transport controls.
  if (btn.action == ButtonAction::VolUp || btn.action == ButtonAction::VolDown ||
      btn.action == ButtonAction::BtPrev || btn.action == ButtonAction::BtPlayPause ||
      btn.action == ButtonAction::BtNext) {
    display.setTextSize(2);
  } else {
    display.setTextSize(1);
  }
  display.setTextPadding(0);
  uint16_t bg = btn.pressed ? TFT_DARKGREY : TFT_NAVY;
  uint16_t fg = TFT_WHITE;
  bool isSelectedStation = (btn.action == ButtonAction::StationBase && btn.stationOffset == stationIndex);
  if (!btn.pressed && isSelectedStation) {
    bg = 0xFBEF; // light pink
    fg = TFT_BLACK;
  }
  // Mode button uses dynamic colors; text is always "MODE".
  if (btn.action == ButtonAction::ToggleMode) {
    bool isBt = (currentMode == PlaybackMode::Bluetooth);
    bg = isBt ? (btn.pressed ? TFT_NAVY : TFT_BLUE) : (btn.pressed ? TFT_MAROON : TFT_RED);
    btn.label = "MODE";
  }
  // Clear stale content when the button shrinks/moves (notably the mode button).
  if (btn.action == ButtonAction::ToggleMode) {
    int clearX = max(0, btn.x - 4);
    int clearY = max(0, btn.y - 4);
    int clearW = min(SCREEN_W - clearX, btn.w + 8);
    int clearH = min(SCREEN_H - clearY, btn.h + 8);
    display.fillRect(clearX, clearY, clearW, clearH, TFT_BLACK);
  }

  display.fillRoundRect(btn.x, btn.y, btn.w, btn.h, 4, bg);
  display.drawRoundRect(btn.x, btn.y, btn.w, btn.h, 4, TFT_WHITE);
  display.setTextColor(fg, bg);
  display.setTextDatum(MC_DATUM);
  display.drawString(btn.label, btn.x + btn.w / 2, btn.y + btn.h / 2);
}

// Volume slider (replaces +/- buttons).
void drawVolumeSlider(bool pressed = false) {
  if (!displayReady) {
    return;
  }
  display.setTextPadding(0);
  // Clear slider area.
  int trackX = SLIDER_X + SLIDER_BTN_W + SLIDER_GAP;
  int totalW = SLIDER_BTN_W + SLIDER_GAP + SLIDER_W + SLIDER_GAP + SLIDER_BTN_W;
  display.fillRect(SLIDER_X - 4, SLIDER_Y - 10, totalW + 8, SLIDER_BTN_H + 12, TFT_BLACK);

  // Track.
  uint16_t trackColor = pressed ? TFT_DARKGREY : TFT_NAVY;
  display.fillRoundRect(trackX, SLIDER_Y, SLIDER_W, SLIDER_H, 4, trackColor);
  display.drawRoundRect(trackX, SLIDER_Y, SLIDER_W, SLIDER_H, 4, TFT_WHITE);

  // Knob position.
  float ratio = static_cast<float>(volumeLevel) / 21.0f;
  int knobX = trackX + 4 + static_cast<int>(ratio * (SLIDER_W - SLIDER_KNOB_W - 8));
  uint16_t knobColor = pressed ? TFT_SKYBLUE : TFT_GREEN;
  display.fillRoundRect(knobX, SLIDER_Y - 6, SLIDER_KNOB_W, SLIDER_H + 12, 6, knobColor);
  display.drawRoundRect(knobX, SLIDER_Y - 6, SLIDER_KNOB_W, SLIDER_H + 12, 6, TFT_WHITE);

  // Volume text inside knob.
  display.setTextColor(TFT_BLACK, knobColor);
  display.setTextDatum(MC_DATUM);
  display.setTextSize(1);
  display.drawString(String(volumeLevel), knobX + SLIDER_KNOB_W / 2, SLIDER_Y + SLIDER_H / 2);
  display.setTextSize(1);

  // Keep the +/- buttons visible after any slider redraw.
  drawButton(ui.sliderMinus);
  drawButton(ui.sliderPlus);
}

// Lay out and draw all buttons (volume, mode, stations).
void buildButtons() {
  ui.buttons.clear();
  const int topY = 62; // move grid slightly upward
  const int colGap = BTN_GAP;
  // Mode button on the right.
  UiButton modeBtn{"",
                   SCREEN_W - 78, SLIDER_Y - 18, 60, 60, ButtonAction::ToggleMode, 0, false, 0};

  // Add slider +/- and mode to the button list for unified refresh handling.
  ui.buttons.push_back(modeBtn);

  // Only build station grid in Radio mode.
  if (currentMode == PlaybackMode::Radio) {
    // Station buttons grid (3 columns).
    const int gridStartY = topY + BTN_H + 12;
    const int cols = 3;
    const int btnW = (SCREEN_W - (cols + 1) * colGap) / cols;
    for (size_t i = 0; i < STATION_COUNT; i++) {
      int row = static_cast<int>(i / cols);
      int col = static_cast<int>(i % cols);
      int x = colGap + col * (btnW + colGap);
      int y = gridStartY + row * (BTN_H + colGap);
      String label = String(STATIONS[i].name);
      if (label.length() > 16) {
        label = label.substring(0, 16);
      }
      UiButton st{label, x, y, btnW, BTN_H, ButtonAction::StationBase, i, false, 0};
      ui.buttons.push_back(st);
    }
  } else {
    // BT transport controls row.
    const int ctrlY = topY + BTN_H + 12;
    const int ctrlH = 44;
    const int gap = 12;
    const int ctrlW = (SCREEN_W - 4 * gap) / 3;
    UiButton prevBtn{"<<", gap, ctrlY, ctrlW, ctrlH, ButtonAction::BtPrev, 0, false, 0};
    UiButton playBtn{"||>", gap * 2 + ctrlW, ctrlY, ctrlW, ctrlH, ButtonAction::BtPlayPause, 0, false, 0};
    UiButton nextBtn{">>", gap * 3 + ctrlW * 2, ctrlY, ctrlW, ctrlH, ButtonAction::BtNext, 0, false, 0};
    ui.buttons.push_back(prevBtn);
    ui.buttons.push_back(playBtn);
    ui.buttons.push_back(nextBtn);
  }

  // Draw them once.
  for (auto &b : ui.buttons) {
    drawButton(b);
  }
}

// Bluetooth and audio constants.
// Final radio tuning:
// - Use I2S_NUM_1 (same stable path as Bluetooth)
// - Larger PSRAM input buffer
// - Startup prebuffer + critical underrun recovery
// Tuned for faster station changes: unmute sooner while keeping crackles away.
static constexpr uint8_t RADIO_PREBUFFER_PERCENT = 45;
static constexpr uint32_t RADIO_PREBUFFER_MIN_BYTES = 48UL * 1024UL;
static constexpr uint32_t RADIO_CRITICAL_REBUFFER_BYTES = 32UL * 1024UL;
static constexpr uint32_t RADIO_RECOVERY_UNMUTE_BYTES = 64UL * 1024UL;

// Convert radio volume scale (0..21) to AVRCP volume scale (0..127).
static uint8_t toBtVolume(uint8_t radioVolume) {
  uint16_t scaled = (static_cast<uint16_t>(radioVolume) * 127U) / 21U;
  if (scaled > 0) {
    // Keep some digital headroom for better SNR at low UI volume levels.
    scaled = static_cast<uint16_t>(scaled + 20U);
    if (scaled > 127U) {
      scaled = 127U;
    }
  }
  return static_cast<uint8_t>(scaled);
}

// Small helper for readable logs.
static const char *modeName(PlaybackMode mode) {
  return mode == PlaybackMode::Bluetooth ? "Bluetooth" : "Radio";
}

// Initialize TFT so we can validate wiring quickly.
void initDisplay() {
  if (LCD_BL >= 0) {
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
  }

  if (LCD_SD_CS >= 0) {
    pinMode(LCD_SD_CS, OUTPUT);
    digitalWrite(LCD_SD_CS, HIGH); // deselect SD card on shared SPI bus
  }

  Serial.printf("LCD init: CS=%d DC=%d RST=%d SCK=%d MOSI=%d MISO=%d SD_CS=%d\n",
                LCD_CS, LCD_DC, LCD_RST, LCD_SCK, LCD_MOSI, LCD_MISO, LCD_SD_CS);

  display.init();
  display.setRotation(3);
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  displayReady = true;

  // Visual bring-up pattern so we can confirm the panel receives commands.
  if (ENABLE_DIAGNOSTICS) {
    display.fillScreen(TFT_RED);
    delay(120);
    display.fillScreen(TFT_GREEN);
    delay(120);
    display.fillScreen(TFT_BLUE);
    delay(120);
  }

  display.fillScreen(TFT_BLACK);
  display.setTextFont(2);
  display.setTextSize(1);
  display.setTextColor(TFT_SKYBLUE);
  display.setCursor(16, 12);
  display.println("DAC-radio-3");
  bottomBarSprite.setColorDepth(16);
  bottomBarSprite.createSprite(SCREEN_W, CLOCK_BAR_H);
  bottomBarSprite.setTextWrap(false);
}

void setSharedText(char *dest, size_t destSize, const char *src) {
  size_t copyLen = 0;
  if (src != nullptr) {
    while (copyLen + 1 < destSize && src[copyLen] != '\0') {
      dest[copyLen] = src[copyLen];
      copyLen++;
    }
  }
  dest[copyLen] = '\0';
}

String copySharedText(const char *src, size_t srcSize) {
  char local[STREAM_TEXT_CAP];
  size_t copyLen = 0;

  portENTER_CRITICAL(&metadataMux);
  while (copyLen + 1 < srcSize && src[copyLen] != '\0') {
    local[copyLen] = src[copyLen];
    copyLen++;
  }
  local[copyLen] = '\0';
  portEXIT_CRITICAL(&metadataMux);

  return String(local);
}

String currentClockText() {
  struct tm timeinfo = {};
  if (!getLocalTime(&timeinfo, 10)) {
    return "--:--";
  }

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

String currentTemperatureText() {
  if (!weather.valid) {
    return "--.-\xB0";
  }

  char buf[10];
  snprintf(buf, sizeof(buf), "%.1f\xB0", weather.currentTempC);
  return String(buf);
}

String currentHumidityText() {
  if (!weather.valid) {
    return "--%";
  }

  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f%%", weather.humidityPct);
  return String(buf);
}

String weatherIconsKey() {
  char buf[64];
  snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f",
           weather.codeToday, weather.codeDay1, weather.codeDay2, weather.codeDay3,
           weather.minTodayC, weather.maxTodayC, weather.minDay1C, weather.maxDay1C);
  return String(buf);
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  // Ask Open-Meteo for one compact payload that covers both the "now" values
  // and the next few daily blocks used by the bottom status bar.
  String url =
      String("https://api.open-meteo.com/v1/forecast?latitude=") + String(WEATHER_LATITUDE, 6) +
      "&longitude=" + String(WEATHER_LONGITUDE, 6) +
      "&current=temperature_2m,relative_humidity_2m&daily=weather_code,temperature_2m_min,temperature_2m_max&forecast_days=4&timezone=auto";

  http.setTimeout(4000);
  if (!http.begin(url)) {
    Serial.println("Weather HTTP begin failed.");
    return false;
  }

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP GET failed: %d\n", status);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Weather JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  JsonObject daily = doc["daily"];
  JsonArray weatherCodes = daily["weather_code"];
  JsonArray minTemps = daily["temperature_2m_min"];
  JsonArray maxTemps = daily["temperature_2m_max"];

  if (current.isNull() || daily.isNull() || weatherCodes.isNull() || minTemps.isNull() || maxTemps.isNull() ||
      weatherCodes.size() < 4 || minTemps.size() < 2 || maxTemps.size() < 2) {
    Serial.println("Weather JSON parse failed.");
    return false;
  }

  weather.currentTempC = current["temperature_2m"] | 0.0f;
  weather.humidityPct = current["relative_humidity_2m"] | 0.0f;
  weather.codeToday = current["weather_code"] | -1;
  weather.minTodayC = minTemps[0] | 0.0f;
  weather.maxTodayC = maxTemps[0] | 0.0f;
  weather.minDay1C = minTemps[1] | 0.0f;
  weather.maxDay1C = maxTemps[1] | 0.0f;
  weather.codeDay1 = weatherCodes[1] | -1;
  weather.codeDay2 = weatherCodes[2] | -1;
  weather.codeDay3 = weatherCodes[3] | -1;
  weather.valid = true;
  weather.lastFetchMs = millis();
  return true;
}

void saveRuntimeStateToNvs() {
  prefs.begin(PREF_NS, false);
  prefs.putUChar(PREF_VOLUME, volumeLevel);
  prefs.putUInt(PREF_STATION, static_cast<uint32_t>(stationIndex));
  prefs.end();
  lastSavedVolume = volumeLevel;
  lastSavedStationIndex = stationIndex;
  statePersistencePending = false;
}

template <typename TDisplay>
void drawWeatherIcon(TDisplay &target, int centerX, int centerY, int code) {
  uint16_t sun = TFT_YELLOW;
  uint16_t cloud = TFT_LIGHTGREY;
  uint16_t rain = TFT_CYAN;
  uint16_t storm = TFT_ORANGE;

  auto drawSun = [&]() {
    target.fillCircle(centerX, centerY, 8, sun);
    for (int i = 0; i < 8; ++i) {
      float angle = static_cast<float>(i) * 0.785398f;
      int x1 = centerX + static_cast<int>(cosf(angle) * 11.0f);
      int y1 = centerY + static_cast<int>(sinf(angle) * 11.0f);
      int x2 = centerX + static_cast<int>(cosf(angle) * 15.0f);
      int y2 = centerY + static_cast<int>(sinf(angle) * 15.0f);
      target.drawLine(x1, y1, x2, y2, sun);
    }
  };

  auto drawCloud = [&](int x, int y) {
    target.fillCircle(x - 8, y, 6, cloud);
    target.fillCircle(x, y - 4, 8, cloud);
    target.fillCircle(x + 9, y, 6, cloud);
    target.fillRoundRect(x - 14, y, 28, 10, 4, cloud);
  };

  auto drawRain = [&]() {
    drawCloud(centerX, centerY - 2);
    for (int dx = -8; dx <= 8; dx += 8) {
      target.drawLine(centerX + dx, centerY + 10, centerX + dx - 2, centerY + 16, rain);
    }
  };

  auto drawStorm = [&]() {
    drawCloud(centerX, centerY - 2);
    target.drawLine(centerX - 2, centerY + 8, centerX + 3, centerY + 8, storm);
    target.drawLine(centerX + 3, centerY + 8, centerX - 1, centerY + 16, storm);
    target.drawLine(centerX - 1, centerY + 16, centerX + 4, centerY + 16, storm);
  };

  if (code == 0 || code == 1) {
    drawSun();
  } else if (code == 2) {
    drawSun();
    drawCloud(centerX + 4, centerY + 2);
  } else if (code == 3 || code == 45 || code == 48) {
    drawCloud(centerX, centerY);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    drawRain();
  } else if (code >= 95) {
    drawStorm();
  } else {
    drawCloud(centerX, centerY);
  }
}

void drawBottomBar(const String &clockText, bool showWeather,
                   const String &weatherTempText, const String &humidityText) {
  // Render the full bottom strip off-screen and push it in one shot to reduce
  // the visible blinking that direct redraws caused on the TFT.
  bottomBarSprite.fillSprite(TFT_BLACK);
  bottomBarSprite.setTextPadding(0);
  bottomBarSprite.setTextDatum(TL_DATUM);
  bottomBarSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  bottomBarSprite.setTextSize(2);
  bottomBarSprite.drawString(clockText, 10, 8, 2);

  if (showWeather) {
    bottomBarSprite.setTextSize(1);
    bottomBarSprite.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    drawWeatherIcon(bottomBarSprite, 124, CLOCK_BAR_H / 2, weather.codeToday);
    bottomBarSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    bottomBarSprite.drawString(String(static_cast<int>(roundf(weather.maxTodayC))), 140, 4, 2);
    bottomBarSprite.drawString(String(static_cast<int>(roundf(weather.minTodayC))), 140, 20, 2);
    bottomBarSprite.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    bottomBarSprite.drawString(weatherTempText, 184, 12, 4);
    bottomBarSprite.fillCircle(235, 16, 2, TFT_SKYBLUE);
    bottomBarSprite.drawString(humidityText, 248, 12, 4);
    drawWeatherIcon(bottomBarSprite, SCREEN_W - 126, CLOCK_BAR_H / 2, weather.codeDay1);
    bottomBarSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    bottomBarSprite.drawString(String(static_cast<int>(roundf(weather.maxDay1C))), SCREEN_W - 108, 4, 2);
    bottomBarSprite.drawString(String(static_cast<int>(roundf(weather.minDay1C))), SCREEN_W - 108, 20, 2);
    drawWeatherIcon(bottomBarSprite, SCREEN_W - 62, CLOCK_BAR_H / 2, weather.codeDay2);
    drawWeatherIcon(bottomBarSprite, SCREEN_W - 22, CLOCK_BAR_H / 2, weather.codeDay3);
  }

  bottomBarSprite.pushSprite(0, CLOCK_BAR_Y);
}

// Minimal redrawing to avoid visible flicker on the ILI9488.
void refreshDisplay(bool force = false) {
  if (!displayReady) {
    return;
  }
  if (!displayUpdatesEnabled && !force) {
    return;
  }

  const wl_status_t wifiStatus = WiFi.status();
  String radioTitle = copySharedText(radioStreamTitle, sizeof(radioStreamTitle));
  String btTitle = copySharedText(btTrackTitle, sizeof(btTrackTitle));
  String btArtist = copySharedText(btTrackArtist, sizeof(btTrackArtist));
  String clockText = currentClockText();
  bool showWeather = (currentMode == PlaybackMode::Radio);
  String weatherTempText = showWeather ? currentTemperatureText() : "";
  String humidityText = showWeather ? currentHumidityText() : "";
  String weatherIconState = showWeather ? weatherIconsKey() : "";
  String titleToShow;
  if (currentMode == PlaybackMode::Radio) {
    titleToShow = radioTitle.length() > 0 ? radioTitle : String(STATIONS[stationIndex].name);
  } else if (btTitle.length() > 0 && btArtist.length() > 0) {
    titleToShow = btArtist + " - " + btTitle;
  } else if (btTitle.length() > 0) {
    titleToShow = btTitle;
  } else {
    titleToShow = "Waiting for title...";
  }

  // Trim to fit the screen width instead of a fixed character count.
  const int statusMaxWidth = SCREEN_W - 12; // leave small margin
  while (titleToShow.length() > 3 && display.textWidth(titleToShow, 2) > statusMaxWidth) {
    titleToShow.remove(titleToShow.length() - 1);
  }

  bool stationChanged = (stationIndex != displayCache.stationIndex);
  size_t prevStationIndex = displayCache.stationIndex;
  // Skip redraws when the visible state did not change; this keeps the UI calm
  // and avoids repainting large regions every loop iteration.
  if (!force && currentMode == displayCache.mode && !stationChanged &&
      volumeLevel == displayCache.volume && wifiStatus == displayCache.wifi &&
      titleToShow == displayCache.title && clockText == displayCache.clock &&
      weatherTempText + humidityText == displayCache.weatherText &&
      weatherIconState == displayCache.weatherIcons) {
    return;
  }
  display.setTextFont(2);
  display.setTextSize(1);

  // Status strip between the slider and the station / BT controls.
  display.setTextDatum(TL_DATUM);
  display.setTextPadding(SCREEN_W);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.fillRect(0, STATUS_LINE_Y, SCREEN_W, STATUS_LINE_H, TFT_BLACK);
  String status;
  if (currentMode == PlaybackMode::Bluetooth) {
    const bool btConnected = a2dpSink.is_connected();
    if (btConnected) {
      status = titleToShow.length() ? titleToShow : "BT connected";
    } else {
      status = "BT connecting...";
    }
  } else { // Radio
    if (wifiStatus == WL_CONNECTED) {
      status = titleToShow.length() ? titleToShow : "Connected";
    } else {
      status = "WiFi connecting...";
    }
  }
  display.drawString(status, 6, STATUS_LINE_Y + 8, 2);

  drawBottomBar(clockText, showWeather, weatherTempText, humidityText);
  // Reset padding so subsequent button labels don't overpaint horizontal bars.
  display.setTextPadding(0);

  // Keep volume slider and buttons in sync only when needed.
  if (force || volumeLevel != displayCache.volume || ui.sliderPressed) {
    drawVolumeSlider(ui.sliderPressed);
  }

  // Radio mode: redraw station buttons when selection changes.
  if (currentMode == PlaybackMode::Radio && stationChanged) {
    for (auto &btn : ui.buttons) {
      if (btn.action == ButtonAction::StationBase &&
          (btn.stationOffset == stationIndex || btn.stationOffset == prevStationIndex)) {
        drawButton(btn);
      }
    }
  }
  // BT mode: clear grid area and draw centered BT icon when connected.
  if (currentMode == PlaybackMode::Bluetooth) {
    // Leave transport controls untouched; clear below them.
    const int ctrlY = 62 + BTN_H + 12; // matches buildButtons BT controls Y
    const int ctrlH = 44;
    int areaY = ctrlY + ctrlH + 12;
    int areaH = CLOCK_BAR_Y - areaY - 4;
    display.fillRect(0, areaY, SCREEN_W, areaH, TFT_BLACK);
    if (a2dpSink.is_connected()) {
      int cx = SCREEN_W / 2;
      int cy = areaY + areaH / 2;
      uint16_t ico = TFT_WHITE; // draw BT icon in white
      for (int o = -1; o <= 1; ++o) {
        display.drawLine(cx - 12 + o, cy - 28, cx + 12 + o, cy - 6, ico);
        display.drawLine(cx + 12 + o, cy - 6, cx - 12 + o, cy + 16, ico);
        display.drawLine(cx - 12 + o, cy - 28, cx - 12 + o, cy + 28, ico);
        display.drawLine(cx - 12 + o, cy + 28, cx + 12 + o, cy + 6, ico);
        display.drawLine(cx + 12 + o, cy + 6, cx - 12 + o, cy - 16, ico);
      }
    }
  }
  // Refresh mode button visuals (color/icon) if needed.
  if (force || currentMode != displayCache.mode) {
    for (auto &btn : ui.buttons) {
      if (btn.action == ButtonAction::ToggleMode) {
        drawButton(btn);
      }
    }
  }

  displayCache.mode = currentMode;
  displayCache.stationIndex = stationIndex;
  displayCache.volume = volumeLevel;
  displayCache.wifi = wifiStatus;
  displayCache.title = titleToShow;
  displayCache.clock = clockText;
  displayCache.weatherText = weatherTempText + humidityText;
  displayCache.weatherIcons = weatherIconState;
}

// Queue a UI redraw from any context; actual drawing runs in loop().
void requestDisplayRefresh(bool force = false) {
  if (force) {
    displayForceRefreshPending = true;
  }
  // When refreshDisplay is called in loop(), it will update status bar and dynamic labels.
}

// Probe PSRAM and print diagnostics once at boot.
bool probePsram() {
  const bool initOk = psramInit();
  const bool found = psramFound();
  const uint32_t size = ESP.getPsramSize();
  const uint32_t freeSize = ESP.getFreePsram();
  Serial.printf("PSRAM status: init=%s found=%s size=%lu free=%lu\n",
                initOk ? "yes" : "no",
                found ? "yes" : "no",
                static_cast<unsigned long>(size),
                static_cast<unsigned long>(freeSize));
  return initOk && found && size > 0;
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_UNKNOWN:
    return "unknown";
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt watchdog";
  case ESP_RST_TASK_WDT:
    return "task watchdog";
  case ESP_RST_WDT:
    return "other watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  default:
    return "unmapped";
  }
}

// Load the boot mode from NVS.
PlaybackMode loadModeFromNvs() {
  prefs.begin(PREF_NS, true);
  uint8_t raw = prefs.getUChar(PREF_MODE, static_cast<uint8_t>(PlaybackMode::Radio));
  prefs.end();
  return raw == static_cast<uint8_t>(PlaybackMode::Bluetooth) ? PlaybackMode::Bluetooth
                                                               : PlaybackMode::Radio;
}

uint8_t loadVolumeFromNvs() {
  prefs.begin(PREF_NS, true);
  uint8_t raw = prefs.getUChar(PREF_VOLUME, START_VOLUME);
  prefs.end();
  return raw <= 21 ? raw : START_VOLUME;
}

size_t loadStationFromNvs() {
  prefs.begin(PREF_NS, true);
  uint32_t raw = prefs.getUInt(PREF_STATION, 0);
  prefs.end();
  return raw < STATION_COUNT ? static_cast<size_t>(raw) : 0;
}

// Persist the selected mode to NVS.
void saveModeToNvs(PlaybackMode mode) {
  prefs.begin(PREF_NS, false);
  prefs.putUChar(PREF_MODE, static_cast<uint8_t>(mode));
  prefs.end();
}

// Connect to Wi-Fi with bounded retries.
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(80);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi SSID '%s'", WIFI_SSID);
  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 60) {
    delay(500);
    Serial.print('.');
    retries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("Wi-Fi connection failed.");
  }
}

// Start playback of one station by index.
bool startStation(size_t idx) {
  if (idx >= STATION_COUNT) {
    return false;
  }

  stationIndex = idx;
  statePersistencePending = true;
  portENTER_CRITICAL(&metadataMux);
  setSharedText(radioStreamTitle, sizeof(radioStreamTitle), "");
  portEXIT_CRITICAL(&metadataMux);
  Serial.printf("\nNow playing [%u/%u]: %s\n", static_cast<unsigned>(stationIndex + 1),
                static_cast<unsigned>(STATION_COUNT), STATIONS[stationIndex].name);
  Serial.printf("URL: %s\n", STATIONS[stationIndex].url);
  audio.setIcyMetadata(STATIONS[stationIndex].enableIcyMetadata);
  requestDisplayRefresh(false); // allow incremental refresh
  return audio.connecttohost(STATIONS[stationIndex].url);
}

// Request station switch handled by the radio task.
void requestStation(size_t idx) {
  if (idx >= STATION_COUNT) {
    return;
  }
  requestedStationIndex = idx;
  stationChangePending = true;
}

// AVRCP metadata callback for Bluetooth source title/artist.
void btMetadataCallback(uint8_t attrId, const uint8_t *text) {
  if (text == nullptr) {
    return;
  }
  if (attrId == 0x1) {
    portENTER_CRITICAL(&metadataMux);
    setSharedText(btTrackTitle, sizeof(btTrackTitle), reinterpret_cast<const char *>(text));
    portEXIT_CRITICAL(&metadataMux);
    Serial.printf("BT title      %s\n", text);
    requestDisplayRefresh(true);
  } else if (attrId == 0x2) {
    portENTER_CRITICAL(&metadataMux);
    setSharedText(btTrackArtist, sizeof(btTrackArtist), reinterpret_cast<const char *>(text));
    portEXIT_CRITICAL(&metadataMux);
    Serial.printf("BT artist     %s\n", text);
    requestDisplayRefresh(true);
  }
}

// Bluetooth connection state callback.
void btConnectionStateCallback(esp_a2d_connection_state_t state, void *obj) {
  (void)obj;
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    Serial.println("BT connected.");
  }
}

// Ensure BT namespace exists to avoid NVS open errors in A2DP library.
void ensureBluetoothNamespace() {
  nvs_handle handle;
  esp_err_t err = nvs_open("connected_bda", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    nvs_close(handle);
  } else {
    Serial.printf("BT namespace init failed: %s\n", esp_err_to_name(err));
  }
}

// Initialize Internet radio resources and spawn radio task.
void startRadioMode() {
  WiFi.mode(WIFI_STA);
  connectToWiFi();

  const bool hasPsram = probePsram();

  // Must be called before first connecttohost()/loop() so the input buffer gets
  // allocated with the requested size. Keep RAM usage conservative so decoder
  // buffers can still be allocated when PSRAM is not available.
  if (hasPsram) {
    audio.setBufsize(16 * 1024, 512 * 1024);
    Serial.println("Audio buffer config: PSRAM mode (16KB RAM + 512KB PSRAM)");
  } else {
    audio.setBufsize(12 * 1024, 0);
    Serial.println("Audio buffer config: RAM-only mode (12KB)");
  }
  audio.setConnectionTimeout(2500, 5500);
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_PIN_NO_CHANGE, I2S_MCLK);
  audio.setVolume(0);

  if (WiFi.status() == WL_CONNECTED) {
    requestedStationIndex = stationIndex;
    stationChangePending = true;
    fetchWeather();
  }

  if (radioTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(
        [](void *param) {
          (void)param;
          while (true) {
            if (currentMode != PlaybackMode::Radio) {
              vTaskDelay(10 / portTICK_PERIOD_MS);
              continue;
            }

            if (stationChangePending && WiFi.status() == WL_CONNECTED) {
              stationChangePending = false;
              radioPrebuffering = true;
              radioRecoveringUnderrun = false;
              audio.stopSong();
              audio.setVolume(0);
              startStation(requestedStationIndex);
            }

            audio.loop();

            const uint32_t filled = audio.inBufferFilled();
            const uint32_t total = filled + audio.inBufferFree();

            if (radioPrebuffering) {
              uint32_t threshold = RADIO_RECOVERY_UNMUTE_BYTES;
              if (!radioRecoveringUnderrun) {
                threshold = RADIO_PREBUFFER_MIN_BYTES;
                if (total > 0) {
                  const uint32_t percentThreshold =
                      static_cast<uint32_t>((static_cast<uint64_t>(total) * RADIO_PREBUFFER_PERCENT) / 100ULL);
                  if (percentThreshold < threshold) {
                    threshold = percentThreshold;
                  }
                }
              }

              if (filled >= threshold) {
                audio.setVolume(volumeLevel);
                radioVolumeDirty = false;
                radioPrebuffering = false;
                radioRecoveringUnderrun = false;
              }
            } else if (filled > 0 && filled <= RADIO_CRITICAL_REBUFFER_BYTES) {
              // Emergency rebuffer only when near-empty to suppress tiny crackles.
              audio.setVolume(0);
              radioPrebuffering = true;
              radioRecoveringUnderrun = true;
            } else if (radioVolumeDirty) {
              audio.setVolume(volumeLevel);
              radioVolumeDirty = false;
            }

            if (WiFi.status() != WL_CONNECTED) {
              unsigned long now = millis();
              if (now - lastWifiRetryMs > 5000UL) {
                lastWifiRetryMs = now;
                Serial.println("Wi-Fi lost. Reconnecting...");
                connectToWiFi();
                if (WiFi.status() == WL_CONNECTED) {
                  stationChangePending = true;
                }
              }
            }

            // Yield to lower-priority tasks (incl. IDLE) once per loop to satisfy the task watchdog
            // while still keeping a tight audio.loop() cadence.
            vTaskDelay(1);
          }
        },
        "radio-audio-task",
        6144,
        nullptr,
        tskIDLE_PRIORITY + 3,
        &radioTaskHandle,
        0);
  }
}

// Initialize Bluetooth sink and I2S output.
void startBluetoothMode() {
  if (WiFi.getMode() & WIFI_MODE_STA) {
    WiFi.disconnect(true, false);
  }
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Audio object may already have installed I2S on this port during static init.
  // Ensure the BT sink can install its own I2S driver cleanly.
  i2s_driver_uninstall(AUDIO_I2S_PORT);

  ensureBluetoothNamespace();

  a2dpSink.set_i2s_port(AUDIO_I2S_PORT);
  a2dpSink.set_bits_per_sample(16);
  a2dpSink.set_auto_reconnect(true, 1000);
  a2dpSink.set_reconnect_delay(1000);

  // Tune I2S buffering/clocking for smoother A2DP output.
  i2s_config_t btI2SConfig = {};
  btI2SConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  btI2SConfig.sample_rate = 44100;
  btI2SConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  btI2SConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  btI2SConfig.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  btI2SConfig.intr_alloc_flags = 0;
  btI2SConfig.dma_buf_count = 20;
  btI2SConfig.dma_buf_len = 512;
  btI2SConfig.use_apll = true;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
  btI2SConfig.tx_desc_auto_clear = true;
  btI2SConfig.fixed_mclk = 11289600;
#endif
  a2dpSink.set_i2s_config(btI2SConfig);

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRCK;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  pins.mck_io_num = I2S_MCLK;

  a2dpSink.set_task_core(1);
  a2dpSink.set_pin_config(pins);
  a2dpSink.set_on_connection_state_changed(btConnectionStateCallback, nullptr);
  a2dpSink.set_avrc_metadata_callback(btMetadataCallback);
  a2dpSink.set_volume(toBtVolume(volumeLevel));
  a2dpSink.start(BT_DEVICE_NAME, true);
  btSinkStarted = true;

  Serial.printf("Bluetooth mode active. Pair with '%s'.\n", BT_DEVICE_NAME);
}

// Store a new mode and reboot to apply cleanly.
void requestModeAndRestart(PlaybackMode mode) {
  if (mode == currentMode) {
    Serial.printf("Already in %s mode.\n", modeName(mode));
    return;
  }

  if (statePersistencePending &&
      (volumeLevel != lastSavedVolume || stationIndex != lastSavedStationIndex)) {
    saveRuntimeStateToNvs();
  }
  saveModeToNvs(mode);
  Serial.printf("Switching to %s mode. Rebooting...\n", modeName(mode));
  if (displayReady) {
    display.fillScreen(TFT_BLACK);
    display.setTextFont(2);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.drawString("Switching to " + String(modeName(mode)), SCREEN_W / 2, SCREEN_H / 2 - 10, 2);
    display.drawString("Rebooting...", SCREEN_W / 2, SCREEN_H / 2 + 12, 2);
  }
  delay(250);
  ESP.restart();
}

// Print serial command help.
void printHelp() {
  Serial.println("\nCommands:");
  Serial.println("  m           -> toggle mode (reboot)");
  Serial.println("  r           -> switch to radio mode (reboot)");
  Serial.println("  b           -> switch to bluetooth mode (reboot)");
  Serial.println("  n           -> next station (radio mode)");
  Serial.println("  p           -> previous station (radio mode)");
  Serial.println("  l           -> list stations");
  Serial.println("  1..9        -> select station index (radio mode)");
  Serial.println("  + / -       -> volume up/down");
  Serial.println("  d           -> toggle display updates");
  Serial.println("  h           -> help");
}

// Print station preset list.
void listStations() {
  Serial.println("\nStation list:");
  for (size_t i = 0; i < STATION_COUNT; i++) {
    Serial.printf("  %u: %s\n", static_cast<unsigned>(i + 1), STATIONS[i].name);
  }
}

// Apply volume change to currently active mode.
void setCurrentVolume(uint8_t value) {
  volumeLevel = value;
  statePersistencePending = true;
  if (currentMode == PlaybackMode::Radio) {
    radioVolumeDirty = true;
  } else if (btSinkStarted) {
    a2dpSink.set_volume(toBtVolume(volumeLevel));
  }
  Serial.printf("Volume: %u\n", volumeLevel);
  // Lightweight UI refresh (status bar + slider) without forcing full redraw.
  requestDisplayRefresh(false);
  // Update slider + +/- buttons.
  drawVolumeSlider(ui.sliderPressed);
}

bool hitButton(const UiButton &btn, int tx, int ty, int margin = 0) {
  return tx >= btn.x - margin && tx <= btn.x + btn.w + margin &&
         ty >= btn.y - margin && ty <= btn.y + btn.h + margin;
}

// Parse and process serial commands.
void handleSerial() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n' || c == ' ') {
      continue;
    }

    if (c == 'm') {
      requestModeAndRestart(currentMode == PlaybackMode::Radio ? PlaybackMode::Bluetooth
                                                               : PlaybackMode::Radio);
    } else if (c == 'r') {
      requestModeAndRestart(PlaybackMode::Radio);
    } else if (c == 'b') {
      requestModeAndRestart(PlaybackMode::Bluetooth);
    } else if (c == 'n') {
      if (currentMode == PlaybackMode::Radio) {
        requestStation((stationIndex + 1) % STATION_COUNT);
      }
    } else if (c == 'p') {
      if (currentMode == PlaybackMode::Radio) {
        requestStation((stationIndex + STATION_COUNT - 1) % STATION_COUNT);
      }
    } else if (c == 'l') {
      listStations();
    } else if (c == 'h') {
      printHelp();
    } else if (c == 'd') {
      displayUpdatesEnabled = !displayUpdatesEnabled;
      Serial.printf("Display updates: %s\n", displayUpdatesEnabled ? "enabled" : "disabled");
    } else if (c == '+') {
      if (volumeLevel < 21) {
        setCurrentVolume(static_cast<uint8_t>(volumeLevel + 1));
      }
    } else if (c == '-') {
      if (volumeLevel > 0) {
        setCurrentVolume(static_cast<uint8_t>(volumeLevel - 1));
      }
    } else if (c >= '1' && c <= '9') {
      size_t idx = static_cast<size_t>(c - '1');
      if (currentMode == PlaybackMode::Radio && idx < STATION_COUNT) {
        requestStation(idx);
      }
    }
  }
}

// Board initialization and mode dispatch.
void setup() {
  Serial.begin(115200);
  delay(500);
  esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("Reset reason: %s (%d)\n", resetReasonName(resetReason), static_cast<int>(resetReason));
  initDisplay();
  // Lower I2C speed for touch controller to improve reliability on longer wires.
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 100000);
  if (ENABLE_DIAGNOSTICS) {
    scanI2C();
  }
  uint8_t touchAddr = 0;
  if (probeTouchController(touchAddr)) {
    touchReady = touch.begin(40, &Wire); // threshold, pass bus explicitly
  } else {
    touchReady = false;
  }
  Serial.println(touchReady ? "FT6236 touch ready." : "FT6236 not detected; touch disabled.");

  currentMode = loadModeFromNvs();
  volumeLevel = loadVolumeFromNvs();
  stationIndex = loadStationFromNvs();
  requestedStationIndex = stationIndex;
  lastSavedVolume = volumeLevel;
  lastSavedStationIndex = stationIndex;
  statePersistencePending = false;
  buildButtons();
  drawVolumeSlider(false);

  Serial.println("\nDAC-radio-3 (single active mode)");
  Serial.printf("Boot mode: %s\n", modeName(currentMode));
  Serial.printf("Boot station: %s\n", STATIONS[stationIndex].name);
  Serial.printf("Boot volume: %u\n", volumeLevel);
  requestDisplayRefresh(true);

  if (ENABLE_DIAGNOSTICS) {
    printHelp();
    listStations();
  }

  if (currentMode == PlaybackMode::Radio) {
    startRadioMode();
  } else {
    startBluetoothMode();
  }
  // Draw initial slider and its buttons.
  drawVolumeSlider(false);
  drawButton(ui.sliderMinus);
  drawButton(ui.sliderPlus);
}

// Main loop: serial handling + lightweight mode-specific actions.
void loop() {
  handleSerial();
  unsigned long now = millis();
  if (now - lastUiMaintenanceMs >= UI_MAINTENANCE_INTERVAL_MS) {
    lastUiMaintenanceMs = now;
    if (currentMode == PlaybackMode::Radio && (WiFi.getMode() & WIFI_MODE_STA) && WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
    }
    if (currentMode == PlaybackMode::Radio && WiFi.status() == WL_CONNECTED &&
        (!weather.valid || now - weather.lastFetchMs >= WEATHER_REFRESH_INTERVAL_MS)) {
      if (fetchWeather()) {
        requestDisplayRefresh(false);
      }
    }
    requestDisplayRefresh(false);
    if (statePersistencePending &&
        (volumeLevel != lastSavedVolume || stationIndex != lastSavedStationIndex)) {
      saveRuntimeStateToNvs();
    }
  }
  if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL_MS) {
    bool forceRefresh = displayForceRefreshPending;
    displayForceRefreshPending = false;
    refreshDisplay(forceRefresh);
    lastDisplayRefreshMs = now;
  }
  // Poll touch roughly at display cadence.
  if (touchReady && touch.touched()) {
    TS_Point p = touch.getPoint();
    // Map raw touch to screen coordinates for rotation 3 (landscape 480x320).
    int tx = map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, SCREEN_W);
    int ty = map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, SCREEN_H);
    if (TOUCH_INVERT_X) {
      tx = SCREEN_W - 1 - tx;
    }
    if (TOUCH_INVERT_Y) {
      ty = SCREEN_H - 1 - ty;
    }
    tx = constrain(tx, 0, SCREEN_W - 1);
    ty = constrain(ty, 0, SCREEN_H - 1);
    if (ENABLE_DIAGNOSTICS) {
      static unsigned long lastTouchLogMs = 0;
      if (now - lastTouchLogMs > 250) {
        Serial.printf("Touch raw x=%d y=%d -> screen x=%d y=%d\n", p.x, p.y, tx, ty);
        lastTouchLogMs = now;
      }
    }

    // Slider drag (volume); buttons (including +/-) handled separately below.
    int trackX = SLIDER_X + SLIDER_BTN_W + SLIDER_GAP;
    int sliderTouchYMin = SLIDER_Y - 12;
    int sliderTouchYMax = max(SLIDER_Y + SLIDER_H + 14, (SLIDER_Y - 8) + SLIDER_BTN_H + 6);
    bool minusHit = hitButton(ui.sliderMinus, tx, ty, 6);
    bool plusHit = hitButton(ui.sliderPlus, tx, ty, 6);
    bool inSliderTrack =
        !minusHit && !plusHit &&
        (tx >= trackX && tx <= trackX + SLIDER_W && ty >= sliderTouchYMin && ty <= sliderTouchYMax);
    if (inSliderTrack) {
      // Dragging the slider takes ownership of the touch so the +/- buttons
      // do not also react while the finger is moving across the top area.
      ui.sliderPressed = true;
      ui.sliderPressedAtMs = now;
      ui.volumeButtonsTouchActive = false;
      if (ui.sliderMinus.pressed) {
        ui.sliderMinus.pressed = false;
      }
      if (ui.sliderPlus.pressed) {
        ui.sliderPlus.pressed = false;
      }
      int newVol = map(tx, trackX, trackX + SLIDER_W - 1, 0, 21);
      newVol = constrain(newVol, 0, 21);
      if (newVol != volumeLevel) {
        setCurrentVolume(static_cast<uint8_t>(newVol));
      } else {
        drawVolumeSlider(true);
      }
    } else if (minusHit || plusHit) {
      ui.sliderPressed = false;
      bool currentTouchActive = minusHit || plusHit;
      // Only step volume on the transition into the button hit zone; holding a
      // finger down should not generate repeated writes or rapid volume jumps.
      if (currentTouchActive && !ui.volumeButtonsTouchActive) {
        if (minusHit && volumeLevel > 0) {
          ui.sliderMinus.pressed = true;
          ui.sliderMinus.pressedAtMs = now;
          setCurrentVolume(static_cast<uint8_t>(volumeLevel - 1));
        } else if (plusHit && volumeLevel < 21) {
          ui.sliderPlus.pressed = true;
          ui.sliderPlus.pressedAtMs = now;
          setCurrentVolume(static_cast<uint8_t>(volumeLevel + 1));
        } else {
          drawVolumeSlider(false);
        }
      } else {
        if (minusHit) {
          ui.sliderMinus.pressed = true;
        }
        if (plusHit) {
          ui.sliderPlus.pressed = true;
        }
        drawVolumeSlider(false);
      }
      ui.volumeButtonsTouchActive = currentTouchActive;
    } else {
      ui.volumeButtonsTouchActive = false;
      if (ui.sliderMinus.pressed || ui.sliderPlus.pressed) {
        ui.sliderMinus.pressed = false;
        ui.sliderPlus.pressed = false;
        drawVolumeSlider(false);
      }
      for (auto &btn : ui.buttons) {
        // Slightly enlarge hit area for volume +/- buttons.
        bool hit = hitButton(btn, tx, ty, 0);
        if (hit && !btn.pressed) {
          btn.pressed = true;
          btn.pressedAtMs = now;
          drawButton(btn);
          // Execute action
          if (btn.action == ButtonAction::ToggleMode) {
            requestModeAndRestart(currentMode == PlaybackMode::Radio ? PlaybackMode::Bluetooth
                                                                     : PlaybackMode::Radio);
          } else if (btn.action == ButtonAction::StationBase) {
            requestStation(btn.stationOffset);
          } else if (btn.action == ButtonAction::VolDown) {
            if (volumeLevel > 0) setCurrentVolume(static_cast<uint8_t>(volumeLevel - 1));
          } else if (btn.action == ButtonAction::VolUp) {
            if (volumeLevel < 21) setCurrentVolume(static_cast<uint8_t>(volumeLevel + 1));
          } else if (btn.action == ButtonAction::BtPrev) {
            if (currentMode == PlaybackMode::Bluetooth) {
              a2dpSink.previous();
            }
          } else if (btn.action == ButtonAction::BtNext) {
            if (currentMode == PlaybackMode::Bluetooth) {
              a2dpSink.next();
            }
          } else if (btn.action == ButtonAction::BtPlayPause) {
            if (currentMode == PlaybackMode::Bluetooth) {
              if (a2dpSink.is_connected()) {
                static bool btPlaying = true;
                btPlaying ? a2dpSink.pause() : a2dpSink.play();
                btPlaying = !btPlaying;
              }
            }
          }
        } else if (!hit && btn.pressed) {
          // Auto-release handled below; keep state until timeout.
        }
      }
    }
  }

  // Auto-release buttons after a short delay so they can be tapped again quickly.
  if (touchReady) {
    if (ui.sliderPressed && now - ui.sliderPressedAtMs >= 300) {
      ui.sliderPressed = false;
      drawVolumeSlider(false);
    }
    if (!touch.touched() && ui.volumeButtonsTouchActive) {
      ui.volumeButtonsTouchActive = false;
      if (ui.sliderMinus.pressed || ui.sliderPlus.pressed) {
        ui.sliderMinus.pressed = false;
        ui.sliderPlus.pressed = false;
        drawVolumeSlider(false);
      }
    }
    for (auto &btn : ui.buttons) {
      if (btn.pressed && now - btn.pressedAtMs >= 300) {
        btn.pressed = false;
        drawButton(btn); // redraw with default/selected styling
      }
    }
  }

  if (currentMode == PlaybackMode::Radio) {
    vTaskDelay(2 / portTICK_PERIOD_MS);
  } else if (currentMode == PlaybackMode::Bluetooth) {
  }
}

// Optional callbacks from ESP32-audioI2S (verbose logs only).
void audio_info(const char *info) {
  if (VERBOSE_AUDIO_LOGS) {
    Serial.print("info        ");
    Serial.println(info);
  }
}

void audio_showstation(const char *info) {
  if (VERBOSE_AUDIO_LOGS) {
    Serial.print("station     ");
    Serial.println(info);
  }
}

void audio_showstreamtitle(const char *info) {
  if (info != nullptr) {
    portENTER_CRITICAL(&metadataMux);
    setSharedText(radioStreamTitle, sizeof(radioStreamTitle), info);
    portEXIT_CRITICAL(&metadataMux);
    requestDisplayRefresh(false);
  }
  if (VERBOSE_AUDIO_LOGS) {
    Serial.print("title       ");
    Serial.println(info);
  }
}

void audio_bitrate(const char *info) {
  if (VERBOSE_AUDIO_LOGS) {
    Serial.print("bitrate     ");
    Serial.println(info);
  }
}

void audio_eof_mp3(const char *info) {
  if (VERBOSE_AUDIO_LOGS) {
    Serial.print("eof_mp3     ");
    Serial.println(info);
  }
}
