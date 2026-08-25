# Captive Portal Wi-Fi Manager (SoftAP Provisioning)

We will replace the hardcoded Wi-Fi credentials with a robust Captive Portal system. On boot, the ESP32 will attempt to connect to the last saved network. If it fails (e.g., you are at a new hotel), it will spin up a temporary Wi-Fi network (`Babyphone-Setup`). Connecting to this network will pop up a webpage to enter new Wi-Fi credentials. 

To ensure this doesn't impact the highly-tuned Opus streaming performance, **the Captive Portal (HTTP Server, DNS Server, and SoftAP interface) will be completely destroyed and their memory freed** before the audio pipeline is ever allowed to start.

## Performance Guarantee 
The ESP32 will run in distinct "phases". 
- **Phase 1:** Provisioning (SoftAP, HTTP, DNS). If needed, this runs *alone*.
- **Phase 2:** Cleanup. We call `httpd_stop()`, destroy the DNS socket, and switch the Wi-Fi mode from `APSTA` to pure `STA`.
- **Phase 3:** Streaming. Audio tasks are created. The ESP32 operates exactly as it does now, with zero background web server overhead!

## Proposed Changes

### ESP32 Firmware

#### [MODIFY] `firmware/main/main.c`
- Move the `audio_capture_init`, `audio_encoder_init`, and `network_tx_init` calls out of the main boot sequence. They will only be called *after* a successful Wi-Fi Station connection is established.
- Add NVS (Non-Volatile Storage) initialization to load saved SSID/PW.

#### [NEW] `firmware/main/wifi_manager.c` & `firmware/main/wifi_manager.h`
Create a dedicated component to handle the Wi-Fi state machine:
- **State 1 (Try STA):** Read credentials from NVS and attempt connection. Timeout after 10 seconds.
- **State 2 (SoftAP Mode):** If STA fails, switch to `WIFI_MODE_APSTA`. Start the SoftAP (`Babyphone-Setup`).
- **State 3 (Captive Portal):** 
  - Start a lightweight UDP DNS Server on port 53 that resolves all domains to `192.168.4.1`.
  - Start `esp_http_server` on port 80. Serve a simple HTML form with `<input name="ssid">` and `<input name="pass">`.
- **State 4 (Save & Reboot):** When the user submits the form, save the new credentials to NVS via `nvs_set_string()`, then call `esp_restart()` to cleanly reboot the chip and apply the new configuration.

#### [MODIFY] `firmware/main/CMakeLists.txt`
- Add `wifi_manager.c` to the build sources.
- Add the `esp_http_server` component dependency.
