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
- [ ] **ESP32 LCD UI Rendering:** Implement smart sleep mode (screen pitch black while sleeping), faint red microphone glow when transmitting, and retro-pixel boot screen showing IP/Wi-Fi/Battery.
- [ ] **Flutter App UI Overhaul:** Implement "Midnight" dark mode with glassmorphism, activity history chart, telemetry dashboard, and animated visual VU meter.
## 4. Streaming Validation (Milestone 2)
- [x] **Test Unencrypted Stream in VLC:** Implemented Opus conversion and UDP Unicast streaming.
- [x] **Captive Portal Wi-Fi Manager:** Implemented SoftAP fallback, DNS hijacking, and HTTP server for provisioning `ssid` and `pass` into NVS. Solved Header field too long issues. Added "Show Password" button and robust retry/WPA3 support.
- [x] **Stream Encryption (AES-CTR):** Implemented lightweight AES-128 encryption on the Opus payload only, using SSRC/Sequence/Timestamp as IV, to prevent local eavesdropping. Hardcoded key for now.
- [x] **Audio Processing:** Implemented software noise gate with RMS sensitivity tuning to silence electrical hiss while allowing baby murmurs to pass.
- [x] **Battery Management:** Implemented low-battery acoustic alerting (beeps when <= 10%) with a toggle in the Flutter app to disable.
