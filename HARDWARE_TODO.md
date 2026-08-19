# Hardware Arrival TODO List

When your M5Stack M5StickS3 arrives, use this list to pick up right where we left off and validate the initial C firmware draft.

## 1. Environment Setup
- [x] Install the ESP-IDF toolchain on your Linux machine (either via the VS Code "Espressif IDF" extension or via the terminal).
- [ ] Connect the M5StickS3 to your PC via USB-C.
- [ ] Build and flash the current firmware: `idf.py build flash monitor`.

## 2. Hardware Validation (Milestone 1)
- [ ] **Verify Display:** Does the 1.14" LCD screen turn bright green when the device boots? (This validates the SPI bus, ST7789 panel driver, and backlight PWM).
- [ ] **Verify I2C:** Check the serial monitor output. Does the `hw_codec` successfully find the ES8311 on I2C address `0x18`? If it says "Failed to add ES8311", the I2C pins or addressing might need tweaking.

## 3. Open Engineering Tasks (Next Steps)
- [x] **Wi-Fi Integration:** Add the standard ESP-IDF Wi-Fi station code so the device can connect to your local network.
- [ ] **ES8311 Register Configuration:** The I2C bus is ready, but we still need to write the specific setup sequence to the ES8311 codec (powering it up, setting the PGA gain for the microphone, and configuring the I2S clock mode).
- [ ] **UI Rendering:** We need to decide if we want to import a full graphics library like LVGL to draw the UI (VU-meters, text, battery icons), or if we want to manually draw rectangles and pixels using the basic `esp_lcd` functions.

## 4. Streaming Validation (Milestone 2)
- [ ] **Test Unencrypted Stream in VLC:** Create a temporary unencrypted SDP file (`RTP/AVP` without the `crypto` line) and open it in VLC on your PC. Speak into the M5StickS3 microphone and confirm the Opus stream plays with low latency.
