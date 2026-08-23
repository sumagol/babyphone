# Architecture & Implementation Plan: Low-Latency ESP32-S3 Multicast Babyphone (SRTP/Opus)

## 1. Executive Summary & Core Concept
This document specifies the technical architecture, pinouts, display UX design, and implementation roadmap for an ultra-low-latency, resilient baby monitor built around the **M5Stack M5StickS3** development kit.

The system bypasses heavy application-layer protocols (WebRTC, SIP, Cloud WebSockets) in favor of **L2/L3 UDP Multicast (IGMP) with SRTP (Static Pre-Shared Key / SDES) and Opus audio encoding**.

### Key System Characteristics:
* **Dedicated Hardware Platform:** M5Stack M5StickS3 (ESP32-S3, ES8311 low-noise audio codec + high-SNR MEMS mic, 8MB PSRAM, integrated enclosure, internal battery backup).
* **Stateless Broadcast Sender:** On boot, the device connects to Wi-Fi, initializes the ES8311 codec via I2C, starts I2S DMA capture, encodes to Opus, and streams SRTP packets to multicast group `239.255.0.1:5004`.
* **Zero Cloud Dependency:** 100% local network execution for maximum privacy and resilience against WAN outages.
* **Target Latency:** **< 80 ms** end-to-end on local Wi-Fi.
* **Multi-Client Architecture:** Any number of clients (Desktop VLC, custom Flutter Android/iOS app, or secondary ESP32 hardware receiver like M5Stack ATOM Echo) can join or leave via IGMP without sender overhead.
* **Integrated Glanceable UI:** 1.14″ LCD provides instant network status, battery level, live audio VU-meter, and nursery night-mode auto-dimming.

---

## 2. Hardware Specification: M5Stack M5StickS3

| Parameter | Specification | Project Role / Significance |
| :--- | :--- | :--- |
| **SoC** | ESP32-S3-PICO-1-N8R8 | Dual-Core Xtensa LX7 @ 240 MHz with Vector Instructions |
| **Memory** | 8 MB Octal PSRAM + 8 MB Flash | High headroom for Opus encoding buffers & ringbuffers |
| **Audio Codec** | Everest ES8311 | Low-power 24-bit audio ADC/DAC, I2C register config |
| **Microphone** | High-SNR MEMS Mic (onboard) | Direct acoustic port on enclosure; hardware gain (PGA) tunable via ES8311 |
| **Speaker / Amp** | AW8737 Amp + 1W Cavity Speaker | Enables optional reverse-intercom / talkback in future |
| **Display** | 1.14″ TFT LCD (ST7789v2, 240×135) | Standalone visual status (Wi-Fi, IP, VU-meter, streaming state) |
| **Power / Battery** | 250 mAh LiPo + USB-C | Prevents drops during socket shifts; native USB-JTAG/CDC flashing |
| **Dimensions** | 48 × 24 × 15 mm (20g) | Finished injection-molded enclosure with magnetic base |

---

## 3. Hardware Pinout & Bus Mapping (M5StickS3 Internal)

The M5StickS3 routes its peripherals to the following internal ESP32-S3 GPIOs:

### A. I2C Bus (ES8311 Audio Codec Control & AXP Power)
* **`I2C_SDA`:** `GPIO 47`
* **`I2C_SCL`:** `GPIO 48`
* **ES8311 I2C Address:** `0x18` (7-bit)

### B. I2S Audio Interface (ES8311 Codec Data)
* **`I2S_MCLK`:** `GPIO 18` (Master Clock to Codec)
* **`I2S_BCLK`:** `GPIO 17` (Bit Clock)
* **`I2S_LRCK`:** `GPIO 15` (Word Select / Left-Right Clock)
* **`I2S_SDIN`:** `GPIO 16` (Microphone ADC Data into ESP32)
* **`I2S_SDOUT`:** `GPIO 14` (Speaker DAC Data out of ESP32)

### C. Display (ST7789 SPI) & Controls
* **`TFT_MOSI`:** `GPIO 21` | **`TFT_CLK`:** `GPIO 36` | **`TFT_DC`:** `GPIO 37` | **`TFT_RST`:** `GPIO 38` | **`TFT_CS`:** `GPIO 39`
* **`TFT_BL` (Backlight):** `GPIO 40` (PWM dimming support)
* **`BTN_A` (Main Front Button):** `GPIO 35` (Wake display / Toggle status screen)
* **Native USB-CDC/JTAG:** `GPIO 19 (D-)` / `GPIO 20 (D+)` (Flashing & serial monitoring)

---

## 4. Display UI & UX Specification (1.14″ ST7789 LCD)

### 4.1 Screen Layout
The display operates in landscape orientation ($240 	imes 135	ext{ px}$) or vertical orientation, divided into 4 clear diagnostic zones:

```text
┌────────────────────────────────────────────────────────┐
│ [● REC] SRTP: ON      WiFi: -58dBm   BAT: 100% (USB)   │  <-- Header: Stream Status & Power
├────────────────────────────────────────────────────────┤
│ MULTICAST: 239.255.0.1:5004                            │  <-- Join Target Info
│ LOCAL IP : 192.168.1.142                               │
├────────────────────────────────────────────────────────┤
│ AUDIO LEVEL (VU):                                      │
│ [████████████████░░░░░░░░░░░░░░░░] -18 dB              │  <-- Dynamic Bar: Green / Yellow / Red
├────────────────────────────────────────────────────────┤
│ CODEC: OPUS 24kbps Mono (16kHz)     UPTIME: 02h:14m    │  <-- Stream Specs & Uptime
└────────────────────────────────────────────────────────┘
```

### 4.2 UI Elements & Color Coding
* **Stream & Security Indicator:** Green dot `[● REC]` when audio is actively captured and streamed; Lock icon `[🔒 SRTP]` indicating AES-128 encryption.
* **Network & Multicast Info:** Explicitly displays the target IGMP group `239.255.0.1:5004` so any device in the house can be configured instantly without checking documentation.
* **Dynamic Audio VU-Meter:**
  * `< -30 dB` (Green): Normal nursery background noise / silence.
  * `-30 dB to -15 dB` (Yellow): Baby stirring, breathing sounds, light rustling.
  * `> -15 dB` (Red): Crying / loud disturbance.
* **Nursery Night-Mode (Auto-Dimming):**
  * Display backlight auto-dims to **0% (Off)** or **5%** after 30 seconds of quiet.
  * **Wake Triggers:** Front button press (`BTN_A`) or automatic display wake-up when audio level crosses threshold (e.g. `> -18 dB`).
* **Refresh Rate:** Low UI refresh rate (**5–10 Hz**) to prevent SPI bus starvation and keep Core 1 free for Opus DSP.

---

## 5. Software Architecture: ESP32-S3 Sender Firmware (ESP-IDF)

```
┌─────────────────────────────────────────────────────────┐
│                   M5StickS3 SENDER                      │
│                                                         │
│  [ ES8311 Codec via I2C (0x18) - Set PGA Gain & Clocks] │
│        │                                                │
│  [ Task: audio_capture_task (Core 0, High Prio) ]       │
│        │ I2S DMA Ingest (48 kHz / 16-bit Mono / 20ms)   │
│        ▼                                                │
│  [ Task: audio_encode_task (Core 1, Med-High Prio) ]    │
│        │ High-Pass Filter (80Hz)                        │
│        │ Opus Encoder (libopus, VoIP mode, 16-24 kbps)  │
│        │ RTP Header Generation (PT=96, Seq, TS)         │
│        ▼                                                │
│  [ Task: network_tx_task (Core 0, Med Prio) ]           │
│        │ SRTP Encryption (libsrtp AES-CTR + HMAC-SHA1)  │
│        │ UDP Multicast Socket sendto()                  │
│        ▼                                                │
│  [ Task: ui_display_task (Core 1, Low Prio) ]           │
│        │ ST7789 LCD: Status, IP, VU-meter, Night-Mode   │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
         Network: 239.255.0.1:5004 (TTL=1)
                         │
         ┌───────────────┴───────────────┐
         ▼                               ▼
┌──────────────────┐           ┌──────────────────┐
│   VLC Client     │           │ Flutter Android  │
│   (PC / Mac)     │           │ App (Custom)     │
│                  │           │                  │
│ - Reads .sdp     │           │ - WifiLock       │
│ - Pre-Shared Key │           │ - IGMP Join      │
│ - HW Playback    │           │ - AES-CTR Decrypt│
│                  │           │ - Opus Decode    │
└──────────────────┘           └──────────────────┘
```

### 5.1 Audio Processing Pipeline
1. **Boot Initialization:**
   * Configure I2C master on `GPIO 47/48`.
   * Configure ES8311 codec registers: set input to onboard mic, configure ADC PGA (+3 dB), enable `MCLK`.
   * Initialize standard ESP-IDF `i2s_driver` (Philips format, 48 kHz sample rate, 16-bit depth).
2. **Audio Capture Task:**
   * Reads 20 ms frames (960 samples / 1920 bytes) via DMA into FreeRTOS Ringbuffer.
3. **Opus Encoding Task:**
   * Applies IIR High-Pass Filter (80 Hz cutoff) to suppress low-frequency hum.
   * Computes peak/RMS level and sends metric to UI task.
   * Encodes raw PCM to Opus frame (40–80 bytes at 16–24 kbps, VBR).
   * Constructs 12-byte RTP Header (`Payload Type 96`, incrementing Sequence Number and Timestamp `+= 960`).
4. **SRTP Network Task:**
   * Encrypts payload with `libsrtp` (`AES_CM_128_HMAC_SHA1_80`) using Pre-Shared Key + Salt.
   * Transmits via BSD UDP Socket to multicast group `239.255.0.1:5004`.

### 5.2 Logging, Flash Wear & Reliability Constraints
* **Zero Flash Logging:** No runtime logging to SPIFFS/NVS/LittleFS during streaming.
* **Log Level in Production:** `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` or `CONFIG_LOG_DEFAULT_LEVEL_ERROR=y`.
* **Watchdog Integration:** FreeRTOS Task Watchdog Timer (TWDT) attached to audio and network tasks. Auto-reset Wi-Fi stack or soft-reboot if tasks stall for > 3000 ms.

---

## 6. PoC Validation: Desktop VLC Playback

To verify the sender before building the mobile app, open a static SDP file on PC/Mac:

### `babyphone.sdp`
```text
v=0
o=- 0 0 IN IP4 239.255.0.1
s=M5StickS3 Multicast Babyphone
c=IN IP4 239.255.0.1/1
t=0 0
m=audio 5004 RTP/SAVP 96
a=rtpmap:96 OPUS/48000/1
a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:W1NkZmIzZDRlN2Y4ZzloMWoyazNsNG01bjZvN3A4
```
*(Base64 key contains 16-byte Master Key + 14-byte Master Salt).*

**Command:**
```bash
vlc babyphone.sdp
```

### 6.1 GStreamer Playback
For low-latency Linux testing without VLC, you can use the following GStreamer pipeline. Note the inclusion of `rtpjitterbuffer` which is required for raw UDP:

```bash
gst-launch-1.0 -v udpsrc multicast-group=239.255.0.1 port=5004 \
  caps="application/x-rtp, media=audio, clock-rate=48000, encoding-name=OPUS, payload=96" \
  ! rtpjitterbuffer latency=200 \
  ! rtpopusdepay \
  ! opusdec \
  ! audioconvert \
  ! audioresample \
  ! pulsesink
```

---

## 7. Software Architecture: Flutter Client (Android)

### 7.1 Low-Level Networking Requirements
1. **Multicast Lock & Wake Lock:**
   * Android drops multicast packets in doze/screen-off mode.
   * App must acquire `WifiManager.MulticastLock` and run a `ForegroundService` with `FOREGROUND_SERVICE_MEDIA_PLAYBACK`.
2. **Socket Handling in Dart:**
   ```dart
   final socket = await RawDatagramSocket.bind(
     InternetAddress.anyIPv4, 
     5004, 
     reuseAddress: true, 
     reusePort: true
   );
   socket.joinMulticast(InternetAddress('239.255.0.1'));
   
   socket.listen((event) {
     if (event == RawSocketEvent.read) {
       final dg = socket.receive();
       if (dg != null) {
         // 1. Strip RTP & Decrypt SRTP (AES-CTR PSK)
         // 2. Decode Opus frame via FFI libopus
         // 3. Push to AudioTrack / OpenSL ES buffer
       }
     }
   });
   ```

---

## 8. Implementation Roadmap & Milestones

- [ ] **Milestone 1: Toolchain, ES8311 & Display Bring-up**
  - [x] Configure ESP-IDF project for ESP32-S3 with native USB-CDC.
  - [x] Initialize I2C and configure ES8311 codec registers (Drafted).
  - [x] Initialize ST7789 LCD driver; display IP, Wi-Fi RSSI, and dynamic audio VU-meter bar (Drafted).
- [ ] **Milestone 2: Unencrypted Multicast & VLC Validation**
  - [x] Integrate `libopus` encoder into firmware.
  - [x] Stream unencrypted RTP/Opus to `239.255.0.1:5004`.
  - [ ] Verify playback and latency with VLC (`RTP/AVP`).
- [ ] **Milestone 3: SRTP Encryption (Pre-Shared Key)**
  - [ ] Integrate `libsrtp` on ESP32-S3 using hardware AES acceleration.
  - [ ] Validate encrypted playback with `babyphone.sdp` in VLC (`RTP/SAVP`).
- [ ] **Milestone 4: Flutter Android Client**
  - [ ] Implement Kotlin Foreground Service with `MulticastLock`.
  - [ ] Build Dart FFI pipeline (SRTP decrypt -> Opus decode -> AudioTrack playback).
  - [ ] Add background listening, audio level VU-meter, and threshold notification alerts.
  - 