# Architecture & Implementation Concept: Low-Latency ESP32 Multicast Babyphone (SRTP/Opus)

## 1. Executive Summary & Core Concept
This document serves as the architectural foundation and implementation blueprint for an ultra-low-latency, resilient, headless Babyphone system.
Instead of relying on heavy application-layer stacks (e.g., WebRTC, SIP, or cloud gateways), this system operates purely on **L2/L3 UDP Multicast (IGMP) with SRTP (Static Pre-Shared Key / SDES) and Opus audio encoding**.

### Key Architectural Tenets:
* **Stateless & Headless Sender:** The ESP32 boots, establishes Wi-Fi, and immediately broadcasts audio packets to a multicast group. No handshake, no signaling server, zero incoming connection handling.
* **Zero Cloud / Local-Only:** High privacy, zero internet dependency.
* **Low Latency & High Performance:** Target latency is **< 80 ms** on LAN.
* **Multi-Client Support:** Any number of clients (VLC on PC, custom Android/Flutter app) can independently join (`IGMP JOIN`) or leave (`IGMP LEAVE`) the stream without altering sender state.

---

## 2. System Architecture & Data Flow

```
┌────────────────────────────────────────────────────────┐
│                   ESP32-S3 SENDER                      │
│                                                        │
│  [ I2S MEMS Mic ] (e.g. INMP441 / SPH0645)             │
│        │ (16-bit PCM @ 16 kHz / 48 kHz Mono)           │
│        ▼                                               │
│  [ Task: audio_capture_task (Core 0, High Prio) ]      │
│        │ DMA Ringbuffer -> 20ms Frame Queue            │
│        ▼                                               │
│  [ Task: audio_dsp_encode_task (Core 1, Med-High) ]    │
│        │ Software High-Pass (80Hz) + VAD (Optional)    │
│        │ Opus Encoder (VoIP mode, 16-24 kbps)          │
│        │ RTP Packetization (PT 96, Seq, TS, SSRC)      │
│        ▼                                               │
│  [ Task: network_tx_task (Core 0, Med Prio) ]          │
│        │ SRTP Encryption via libsrtp (AES-CTR / HMAC)  │
│        │ UDP Multicast Socket (sendto)                 │
└────────────────────────┬───────────────────────────────┘
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

---

## 3. Hardware Bill of Materials (BOM)

| Component | Function / Spec | Typical Price (Est.) |
| :--- | :--- | :--- |
| **ESP32-S3 Dev Board** | Dual-core Xtensa LX7 @ 240MHz, 4MB/8MB Flash, 2MB+ PSRAM, USB-C | ~$15 - $20 / CHF 15-22 |
| **I2S MEMS Mic Module** | INMP441 or Adafruit SPH0645LM4H (Digital I2S, high sensitivity) | ~$6 - $9 / CHF 7-10 |
| **Power Supply** | 5V / 2A USB Power Adapter + 1-2m USB-C Cable | ~$10 - $15 / CHF 12-18 |
| **Enclosure** | 3D printed custom enclosure with acoustic port & USB-C cutout | Custom / < CHF 5 |

---

## 4. Hardware Pinout & Wiring (ESP32-S3 standard mapping)

| INMP441 / SPH0645 Pin | ESP32-S3 GPIO | Note |
| :--- | :--- | :--- |
| **VDD / 3V3** | 3V3 | Clean 3.3V power rail |
| **GND** | GND | Common Ground |
| **SD / DOUT** | GPIO 4 (I2S_DATA_IN) | Serial Data Out from Mic |
| **WS / LRCLK** | GPIO 5 (I2S_WS) | Word Select / Frame Clock |
| **SCK / BCLK** | GPIO 6 (I2S_BCLK) | Bit Clock |
| **L/R** | GND (or VDD) | Tie to GND for Left channel mono |

---

## 5. Software Architecture: ESP32-S3 Firmware (ESP-IDF)

### 5.1 FreeRTOS Task Layout & Priorities
1. **`audio_capture_task` (Priority: 10, Affinity: Core 0):**
   * Reads I2S samples in chunks of 20 ms (at 16 kHz = 320 samples / 640 bytes).
   * Pushes raw PCM buffer to `xQueueAudioFrames` (Queue length: 4 to prevent backlog/latency build-up).
2. **`audio_dsp_encode_task` (Priority: 8, Affinity: Core 1):**
   * Pops PCM frame.
   * Simple IIR High-Pass Filter (cutoff 80–100 Hz) to eliminate DC offset and low-frequency rumble.
   * Opus encoding using `libopus` (Voice profile, 16–24 kbps).
   * Generates RTP Header (12 bytes: `V=2`, `PT=96`, Sequence Number increment, Timestamp `+= 320`).
3. **`network_tx_task` (Priority: 6, Affinity: Core 0):**
   * Applies SRTP protection (`srtp_protect()` with static Master Key + Salt).
   * `sendto()` packet over raw UDP socket to `239.255.0.1:5004`.

### 5.2 Flash Wear & Logging Hygiene (Critical!)
* **No persistent flash logging:** The ESP32 must never log runtime events or audio metrics to SPIFFS/NVS/LittleFS to prevent flash wear and I/O blocking.
* **Production Log Level:** Set `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` or `CONFIG_LOG_DEFAULT_LEVEL_ERROR=y`.
* **UART Debug Only:** When debugging, logs must strictly stream over USB-CDC/UART ringbuffer without blocking tasks.
* **Watchdog Integration:** FreeRTOS Task Watchdog Timer (TWDT) enabled on audio and network tasks. If network freezes for > 3 seconds, reset the Wi-Fi stack or soft-reboot.

---

## 6. Testing & Validation: Desktop PoC with VLC

Before developing any mobile client, the sender is verified on a PC/Mac using a static SDP file.

### `babyphone.sdp`
```text
v=0
o=- 0 0 IN IP4 239.255.0.1
s=ESP32 Multicast Babyphone
c=IN IP4 239.255.0.1/1
t=0 0
m=audio 5004 RTP/SAVP 96
a=rtpmap:96 OPUS/48000/1
a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:W1NkZmIzZDRlN2Y4ZzloMWoyazNsNG01bjZvN3A4
```
*Note: The inline base64 string consists of 16-byte Master Key + 14-byte Master Salt.*

**Execution:**
```bash
vlc babyphone.sdp
```

---

## 7. Software Architecture: Flutter Client (Android)

### 7.1 Android Low-Level Network Pitfalls & Workarounds
1. **Multicast Packet Filtering by OS:**
   * Android drops multicast packets when the screen is off or in deep doze mode by default.
   * **Fix:** Acquire `WifiManager.MulticastLock` and `PowerManager.WakeLock` (Partial) via native Kotlin method channel or plugin (`wifi_iot` / custom platform code).
   * **Manifest Permissions:**
     * `android.permission.CHANGE_WIFI_MULTICAST_STATE`
     * `android.permission.INTERNET`
     * `android.permission.ACCESS_WIFI_STATE`
     * `android.permission.WAKE_LOCK`
     * `android.permission.FOREGROUND_SERVICE`
     * `android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK`
2. **Wi-Fi AP IGMP Snooping:**
   * Ensure local Wi-Fi router / Access Points have **IGMP Snooping** enabled or properly forward local multicast traffic between 2.4 GHz and 5 GHz bands.

### 7.2 Dart/Flutter Processing Pipeline
```dart
// 1. Setup RawDatagramSocket
final socket = await RawDatagramSocket.bind(
  InternetAddress.anyIPv4, 
  5004, 
  reuseAddress: true, 
  reusePort: true
);

// 2. Join Multicast Group
final multicastGroup = InternetAddress('239.255.0.1');
socket.joinMulticast(multicastGroup);

// 3. Packet Handling Loop
socket.listen((event) {
  if (event == RawSocketEvent.read) {
    Datagram? dg = socket.receive();
    if (dg != null) {
      // Step A: Parse RTP & Decrypt SRTP (AES-CTR with PSK)
      // Step B: Strip Header -> Opus Payload
      // Step C: Decode Opus -> PCM
      // Step D: Feed Audio Track / RingBuffer (AAudio / OpenSL ES)
    }
  }
});
```

---

## 8. Critical Review & Open Design Questions for Future Iterations

### A. Wi-Fi Multicast Performance & Packet Drop (Crucial!)
* **Issue:** Wi-Fi routers transmit 802.11 Multicast at basic/lowest beacon rates (often 1–6 Mbps) to ensure all clients receive it, causing high airtime consumption and potential packet drops in crowded RF environments.
* **Mitigation / Considerations:**
  * Keep Opus payload very small (16 kbps = ~40-60 byte payloads per 20ms packet).
  * If packet drop occurs: Consider enabling Multicast-to-Unicast conversion on modern APs (e.g., igmp-snooping with unicast helper), or fallback to a lightweight Unicast UDP stream if only 1 receiver is needed.

### B. SRTP Replay Protection & Sequence Rollover (ROC)
* **Issue:** Because there is no initial handshake, when the ESP32 restarts, its RTP Sequence Number resets to 0 (or random). A listening client might reject packets as replay attacks if the SRTP replay window is active.
* **Mitigation:**
  * Configure the client SRTP unprotect layer with a sliding replay window or reset the SRTP session context if timestamps jump backwards.

### C. Voice Activity Detection (VAD) vs. Continuous Stream
* For a baby monitor, parents often want to hear background room ambient noise (confirmation that the stream is alive).
* **Recommendation:** Keep continuous stream running, but transmit Comfort Noise Frames (CNG) or drop bitrate to 6–8 kbps during silence rather than cutting the stream entirely.

---

## 9. Implementation Roadmap & To-Dos

- [ ] **Milestone 1: Hardware Assembly & Basic I2S Testing**
  - [ ] Solder headers / connect INMP441 to ESP32-S3.
  - [ ] Write minimal ESP-IDF app reading I2S samples and logging peak amplitude to UART.
- [ ] **Milestone 2: Unencrypted UDP Multicast Streaming**
  - [ ] Integrate `libopus` into ESP-IDF.
  - [ ] Stream unencrypted RTP/Opus to `239.255.0.1:5004`.
  - [ ] Verify playback in VLC (`RTP/AVP`).
- [ ] **Milestone 3: SRTP Encryption Integration**
  - [ ] Integrate `libsrtp` on ESP-IDF with fixed PSK.
  - [ ] Update SDP file for VLC (`RTP/SAVP`) and verify hardware-accelerated playback.
- [ ] **Milestone 4: Flutter Android Client Development**
  - [ ] Implement Foreground Service + MulticastLock in Android module.
  - [ ] Implement UDP multicast receiver + SRTP decrypt + Opus decode in Dart/FFI.
  - [ ] Add visual VU-meter, noise-threshold alerts, and background audio playback.
