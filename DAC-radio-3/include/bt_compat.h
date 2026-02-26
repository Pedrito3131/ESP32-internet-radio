#pragma once

#include "esp_bt.h"
#include "esp32-hal-bt.h"

#ifndef BT_MODE_CLASSIC_BT
#define BT_MODE_CLASSIC_BT ESP_BT_MODE_CLASSIC_BT
#endif

#ifndef BT_MODE_BLE
#define BT_MODE_BLE ESP_BT_MODE_BLE
#endif

#ifndef BT_MODE_BTDM
#define BT_MODE_BTDM ESP_BT_MODE_BTDM
#endif

// Compatibility shim for cores that expose btStart() but not btStartMode(...)
#ifndef btStartMode
static inline bool btStartMode(esp_bt_mode_t mode) {
  if (mode == ESP_BT_MODE_CLASSIC_BT) {
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  }
  return btStart();
}
#endif
