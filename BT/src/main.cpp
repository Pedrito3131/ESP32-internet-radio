
#define BLUETOOTH_NAME "BT_Lavigne"

#include <SPI.h>
#include <VS1053.h>
#include "player.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "BluetoothA2DPSink.h"
#include <cbuf.h>
#include "bluetoothsink.h"

BluetoothA2DPSink a2dp_sink;

#define VOLUME  100 // Default volume

static bool init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }

  if (err != ESP_OK) {
    Serial.printf("NVS init failed: %s\n", esp_err_to_name(err));
    return false;
  }

  return true;
}

static void ensure_bt_nvs_namespace() {
  nvs_handle handle;
  esp_err_t err = nvs_open("connected_bda", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    nvs_close(handle);
  } else {
    Serial.printf("BT NVS namespace init failed: %s\n", esp_err_to_name(err));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Bluetooth Audio Receiver");
  delay(500);

  init_nvs();
  ensure_bt_nvs_namespace();

  SPI.begin();

  player.begin();
  player.loadDefaultVs1053Patches(); 
  
  player.switchToMp3Mode();
  player.setVolume(VOLUME);

  circBuffer.flush();

  a2dp_sink.set_stream_reader(read_data_stream, false);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.start(BLUETOOTH_NAME);
  delay(10000);
  circBuffer.write((char *)bt_wav_header, 44);
  delay(100);

}

void loop() {
  handle_stream();
}
