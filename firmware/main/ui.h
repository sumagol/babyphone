#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize the LVGL UI subsystem
 */
esp_err_t ui_init(void);

/**
 * @brief Update the Wi-Fi connection status text
 * @param connected true if connected, false otherwise
 */
void ui_set_wifi_status(bool connected);
void ui_set_provisioning_mode(void);

/**
 * @brief Update the IP address text
 * @param ip IP address string
 */
void ui_set_ip_address(const char* ip);

/**
 * @brief Update the streaming status text
 * @param streaming true if streaming, false otherwise
 */
void ui_set_streaming_status(bool streaming);

/**
 * @brief Update the RSSI value
 * @param rssi RSSI value in dBm
 */
void ui_set_rssi(int rssi);

/**
 * @brief Update the audio level
 * @param db Audio level in dB
 */
void ui_set_audio_level(int db);

/**
 * @brief Update the smart sleep state (crying or quiet)
 * @param crying true if noise gate is open for sustained period
 */
void ui_set_smart_sleep_state(bool crying);

/**
 * @brief Update the battery level text
 * @param percent Battery percentage (0-100)
 */
void ui_set_battery_level(uint8_t percent);

#ifdef __cplusplus
}
#endif
