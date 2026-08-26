---
name: m5sticks3-hardware
description: Hardware quirks, pinouts, and initialization constraints for the M5StickS3 board. Use this skill whenever interacting with the display, PMIC, or I2C bus on the M5StickS3.
---

# M5StickS3 Hardware Cheatsheet

When writing or modifying hardware initialization code for the M5StickS3, strictly adhere to these verified constraints. Many of these learnings were derived from the official M5Stack repositories: [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX).

## 1. I2C and PMIC (M5PM1) Quirks
- **I2C Panic**: The M5PM1 PMIC (I2C Address `0x6E`) is extremely sensitive. Using standard `i2c_master_transmit_receive` or read-modify-write operations on its registers causes the ESP32 I2C hardware to lock up, resulting in an `Interrupt wdt timeout on CPU0` panic.
- **Solution**: You MUST use blind, "fire-and-forget" writes (`i2c_master_transmit`) when configuring the PMIC. Assume default register values and overwrite them directly.
- **LCD Power**: The LCD panel's logic power is gated by **GPIO2 on the PMIC**. You must configure PMIC registers `0x16`, `0x10`, `0x13`, and `0x11` to enable this power, or the screen will remain physically dark regardless of SPI commands.

## 2. Display (ST7789) Quirks
- **Pinout**: MOSI=39, SCLK=40, DC=45, CS=41, RST=21, Backlight=38.
- **Memory Gaps & Rotation**: The physical panel is 135x240 with memory offsets `(X=52, Y=40)`. If you rotate the screen to landscape using `esp_lcd_panel_swap_xy(true)`, you **MUST** swap the gaps to `(X=40, Y=52)`. Failure to do so results in uninitialized memory being drawn on the edges ("pixel snow").
- **Color Endianness**: The ESP32 SPI peripheral sends 16-bit colors in Little-Endian, but the ST7789 expects Big-Endian. A pure Green (`0x07E0`) will be interpreted as mostly Red (`0xE007`). You must swap the byte endianness of your color data before transmitting it via SPI.
- **Backlight Power Draw**: The backlight is controlled by GPIO 38. However, on USB power, leaving the LCD backlight ON while transmitting over Wi-Fi can trigger a **hardware brownout**. In low-power scenarios, disable the backlight.

## 3. ES8311 Codec & Audio Capture
- **I2C Address**: The ES8311 is at `0x18`.
- **I2S Configuration (Mono)**: When using `I2S_SLOT_MODE_MONO`, the default Philips slot configuration captures *both* the left and right slots into the DMA buffer, effectively doubling the sample rate in memory (e.g., 16000 samples/sec on an 8000 Hz clock) and causing audio playback speed issues. 
- **Solution**: You MUST explicitly set `std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT` when capturing mono audio to ensure the DMA drops the unused channel and maintains true timing.

## 4. Wi-Fi Power and Multicast
- **Brownouts**: The combination of Wi-Fi calibration (TX), LCD Backlight, and PMIC can drop the USB voltage enough to trigger continuous brownout resets. 
- **Solution**: Lower the Wi-Fi TX power (`esp_wifi_set_max_tx_power(52)` for 13dBm).
- **Multicast UDP (errno 118)**: Default ESP-IDF Wi-Fi power save (`WIFI_PS_MIN_MODEM`) causes UDP multicast packets to fail with `errno 118 (ENETUNREACH)` shortly after connection. You **MUST** disable Wi-Fi power saving (`esp_wifi_set_ps(WIFI_PS_NONE)`) to maintain a stable multicast stream.

## 5. Flashing & Monitoring Protocol
Due to the USB CDC download mode quirks on this board, follow this strict interactive protocol when flashing:
1. **Bootloader Mode Request**: Before initiating the flash command, explicitly ask the user to manually set the device into bootloader mode (usually holding the button while plugging it in or pressing it).
2. **Execute**: Once the user confirms the device is in bootloader mode, run the build/flash/monitor command, making sure to kill any process currently using the port and source the ESP-IDF export script: `fuser -k /dev/ttyACM0 ; . ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash monitor`
3. **Manual Reboot Request**: The device will typically hang at `boot:0x0 (DOWNLOAD(USB/UART0))` after flashing. As soon as the flash completes and the monitor attaches, explicitly ask the user to single-press the Reset button to reboot into the new firmware.
4. **Monitor and Fix**: Keep an eye on the background task monitor output. If a crash (e.g., stack overflow, panic) or issue occurs, immediately fix the code and inform the user of the fix, then restart this protocol for the next flashing round.

## 6. Full Hardware Pinout (from Back Sticker)
**Display (1.14" IPS ST7789P3)**:
- MOSI: G39, SCK: G40, CS: G41, RS/DC: G45, RST: G21, BL: G38

**Audio (ES8311 & AW8737)**:
- MCLK: G18, DOUT: G14, BCLK: G17, LRCK: G15, DIN: G16

**IMU (BMI270)**:
- SCL: G48, SDA: G47

**Buttons**:
- KEY1: G11 (Hold: Boot, Double: Off, Press: On)
- KEY2: G12

**IR & USB**:
- IRTX: G46, IRRX: G42
- USB DM: G19, USB DP: G20

**Port A (Grove)**:
- GND, EXT 5V, G9, G10

**Top Header**:
- Row 1: G5, G4, G6, G7, G43, G44, G2, G3
- Row 2: GND, EXT 5V, G0, G1, G8, BAT, 3V3_L2, 5VIN
