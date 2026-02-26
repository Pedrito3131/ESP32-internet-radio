#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <WiFi.h>
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

// Output volume range for ESP32-audioI2S is 0..21.
// A slightly lower default helps avoid analog clipping/blurry sound.
static constexpr uint8_t START_VOLUME = 8;
static constexpr bool VERBOSE_AUDIO_LOGS = false;

// NVS keys used to persist selected playback mode.
static const char *PREF_NS = "dacradio3";
static const char *PREF_MODE = "mode";

// Radio station model.
struct Station {
  const char *name;
  const char *url;
};

// Built-in station presets.
static const Station STATIONS[] = {

    {"France Inter","icecast.radiofrance.fr/franceinter-midfi.mp3"}, // "icecast.radiofrance.fr/franceinter-hifi.aac",
    {"Radio Classique","radioclassique.ice.infomaniak.ch/radioclassique-high.mp3"},
    {"FIP","icecast.radiofrance.fr/fip-midfi.mp3"}, // "icecast.radiofrance.fr/fip-hifi.aac",
    {"France Culture","icecast.radiofrance.fr/franceculture-midfi.mp3"}, // "icecast.radiofrance.fr/franceculture-hifi.aac",
    {"France Musique","icecast.radiofrance.fr/francemusique-midfi.mp3"}, // "icecast.radiofrance.fr/francemusique-hifi.aac",
    {"Oui FM","ouifm.ice.infomaniak.ch/ouifm-high.mp3"},
    {"Radio Nova","novazz.ice.infomaniak.ch/novazz-128.mp3"},
    {"RTL2","streaming.radio.rtl2.fr/rtl2-1-44-128?listen=webCwsBCggNCQgLDQUGBAcGBg"}};

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

// Mutable runtime state.
PlaybackMode currentMode = PlaybackMode::Radio;
size_t stationIndex = 0;
volatile size_t requestedStationIndex = 0;
uint8_t volumeLevel = START_VOLUME;
unsigned long lastWifiRetryMs = 0;
bool btSinkStarted = false;
volatile bool btConnectedChimePending = false;
volatile bool stationChangePending = false;
volatile bool radioVolumeDirty = true;
volatile bool radioPrebuffering = false;
volatile bool radioRecoveringUnderrun = false;
TaskHandle_t radioTaskHandle = nullptr;

// Bluetooth and audio constants.
// Final radio tuning:
// - Use I2S_NUM_1 (same stable path as Bluetooth)
// - Larger PSRAM input buffer
// - Startup prebuffer + critical underrun recovery
static constexpr uint8_t RADIO_PREBUFFER_PERCENT = 80;
static constexpr uint32_t RADIO_PREBUFFER_MIN_BYTES = 64UL * 1024UL;
static constexpr uint32_t RADIO_CRITICAL_REBUFFER_BYTES = 8UL * 1024UL;
static constexpr uint32_t RADIO_RECOVERY_UNMUTE_BYTES = 32UL * 1024UL;
static constexpr float PI_F = 3.14159265f;

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

// Load the boot mode from NVS.
PlaybackMode loadModeFromNvs() {
  prefs.begin(PREF_NS, true);
  uint8_t raw = prefs.getUChar(PREF_MODE, static_cast<uint8_t>(PlaybackMode::Radio));
  prefs.end();
  return raw == static_cast<uint8_t>(PlaybackMode::Bluetooth) ? PlaybackMode::Bluetooth
                                                               : PlaybackMode::Radio;
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
  Serial.printf("\nNow playing [%u/%u]: %s\n", static_cast<unsigned>(stationIndex + 1),
                static_cast<unsigned>(STATION_COUNT), STATIONS[stationIndex].name);
  Serial.printf("URL: %s\n", STATIONS[stationIndex].url);
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
    Serial.printf("BT title      %s\n", text);
  } else if (attrId == 0x2) {
    Serial.printf("BT artist     %s\n", text);
  }
}

// Write a short generated tone directly to I2S.
void playToneI2S(float frequencyHz, int durationMs, float amplitude) {
  const int sampleRate = 44100;
  const int totalFrames = (sampleRate * durationMs) / 1000;
  const int framesPerChunk = 128;
  int16_t chunk[framesPerChunk * 2];  // stereo

  int framesWritten = 0;
  while (framesWritten < totalFrames) {
    int n = totalFrames - framesWritten;
    if (n > framesPerChunk) {
      n = framesPerChunk;
    }

    for (int i = 0; i < n; ++i) {
      int sampleIndex = framesWritten + i;
      float t = static_cast<float>(sampleIndex) / static_cast<float>(sampleRate);
      float envelope = 1.0f;
      const int fadeFrames = sampleRate / 100;  // 10ms fade in/out
      if (sampleIndex < fadeFrames) {
        envelope = static_cast<float>(sampleIndex) / static_cast<float>(fadeFrames);
      } else if ((totalFrames - sampleIndex) < fadeFrames) {
        envelope = static_cast<float>(totalFrames - sampleIndex) / static_cast<float>(fadeFrames);
      }

      float s = sinf(2.0f * PI_F * frequencyHz * t) * amplitude * envelope;
      int16_t v = static_cast<int16_t>(s * 32767.0f);
      chunk[i * 2] = v;
      chunk[i * 2 + 1] = v;
    }

    size_t bytesWritten = 0;
    i2s_write(AUDIO_I2S_PORT, chunk, n * sizeof(int16_t) * 2, &bytesWritten, 20 / portTICK_PERIOD_MS);
    framesWritten += n;
  }
}

// Connected notification tone for Bluetooth mode.
void playBtConnectedChime() {
  // Short two-tone smooth chime.
  playToneI2S(880.0f, 80, 0.15f);
  delay(15);
  playToneI2S(1320.0f, 110, 0.15f);
}

// Bluetooth connection state callback.
void btConnectionStateCallback(esp_a2d_connection_state_t state, void *obj) {
  (void)obj;
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    btConnectedChimePending = true;
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
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(0);

  if (WiFi.status() == WL_CONNECTED) {
    requestedStationIndex = stationIndex;
    stationChangePending = true;
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

            // Keep audio.loop() cadence as high as possible to avoid underruns.
            taskYIELD();
          }
        },
        "radio-audio-task",
        6144,
        nullptr,
        configMAX_PRIORITIES - 4,
        &radioTaskHandle,
        1);
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
  btI2SConfig.fixed_mclk = 0;
#endif
  a2dpSink.set_i2s_config(btI2SConfig);

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRCK;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

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

  saveModeToNvs(mode);
  Serial.printf("Switching to %s mode. Rebooting...\n", modeName(mode));
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
  if (currentMode == PlaybackMode::Radio) {
    radioVolumeDirty = true;
  } else if (btSinkStarted) {
    a2dpSink.set_volume(toBtVolume(volumeLevel));
  }
  Serial.printf("Volume: %u\n", volumeLevel);
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

  currentMode = loadModeFromNvs();

  Serial.println("\nDAC-radio-3 (single active mode)");
  Serial.printf("Boot mode: %s\n", modeName(currentMode));

  printHelp();
  listStations();

  if (currentMode == PlaybackMode::Radio) {
    startRadioMode();
  } else {
    startBluetoothMode();
  }
}

// Main loop: serial handling + lightweight mode-specific actions.
void loop() {
  handleSerial();

  if (currentMode == PlaybackMode::Radio) {
    vTaskDelay(2 / portTICK_PERIOD_MS);
  } else if (currentMode == PlaybackMode::Bluetooth) {
    if (btConnectedChimePending) {
      btConnectedChimePending = false;
      playBtConnectedChime();
      Serial.println("BT connected.");
    }
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
