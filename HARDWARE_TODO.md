# Hardware Arrival TODO List

When your M5Stack M5StickS3 arrives, use this list to pick up right where we left off and validate the initial C firmware draft.

## 1. Environment Setup
- [x] Install the ESP-IDF toolchain on your Linux machine (either via the VS Code "Espressif IDF" extension or via the terminal).
- [x] Connect the M5StickS3 to your PC via USB-C.
- [x] Build and flash the current firmware: `idf.py build flash monitor`.

## 2. Hardware Validation (Milestone 1)
- [x] **Verify Display:** LCD screen successfully powers on and draws the green test pattern (validated ST7789 SPI driver and PMIC blind-write power gating).
- [x] **Verify I2C:** `hw_codec` successfully found the ES8311 on I2C address `0x18`.

## 3. Open Engineering Tasks (Next Steps)
- [x] **Wi-Fi Integration:** Standard ESP-IDF Wi-Fi station code implemented.
  - *Note:* Disabled Power Save (`WIFI_PS_NONE`) and lowered TX power to prevent `errno 118` drops and brownouts on USB power.
- [x] **ES8311 Register Configuration:** I2C configuration finalized for microphone capture using I2S Mono.
- [ ] **UI Rendering:** We need to decide if we want to import a full graphics library like LVGL to draw the UI (VU-meters, text, battery icons), or if we want to manually draw rectangles and pixels using the basic `esp_lcd` functions.
  - *Note:* Backlight is currently disabled to prevent brownouts during intense UDP streaming.

## 4. Streaming Validation (Milestone 2)
- [x] **Test Unencrypted Stream in VLC:** Implemented G.711a (PCMA) conversion and UDP Multicast streaming (port 5004). 
- [ ] **Fix RTP Playback Issue:** Wait for stream to successfully route into VLC/Wireshark without dropping packets or playing at the wrong speed. The current build reverted to a stable mono capture but requires network debugging.
