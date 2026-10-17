# Mobile multicast babyphone sender (Android / Flutter)

A design plan to add transmitter mode to the Flutter mobile application (`client/babyphone_app/`), allowing a spare Android smartphone to function as the babyphone sender. The stream matches the M5StickS3 firmware wire protocol and communicates over local Wi-Fi multicast.

---

## 1. Mobile architecture overview

Running a continuous real-time audio transmitter on Android requires handling aggressive OS power management and background limitations:

```
┌─────────────────────────────────────────────────────────────┐
│                 Android Transmitter Pipeline                │
│                                                             │
│ 1. Audio Capture (record or native audio plugin):           │
│    - 48 kHz, 16-bit PCM, mono                               │
│    - Pushes 20 ms frames (960 samples / 1920 bytes)         │
│                                                             │
│ 2. Noise Guard & Silence Generator:                         │
│    - RMS level calculation against threshold                │
│    - 0.5s, 5.0s, or 10.0s continuous trigger requirement    │
│    - 2.5s hangover unmuted window                           │
│    - Zeroed PCM frames when muted to keep watchdog alive    │
│                                                             │
│ 3. Compression & Encryption:                                │
│    - Opus encoder at 24 kbps VoIP                           │
│    - 12-byte RTP header + 8-byte 0xBABB extension           │
│    - AES-128-CTR encryption via Dart encrypt package        │
│                                                             │
│ 4. Multicast Transmission (RawDatagramSocket):              │
│    - Sends to 239.255.0.1:5004                              │
│                                                             │
│ 5. Android OS Background Enforcement:                       │
│    - Foreground service with microphone type                │
│    - High-performance Wi-Fi lock + Multicast lock           │
│    - Battery optimization (Doze) exemption prompt           │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Key Android challenges and solutions

1. **Background microphone restrictions (Android 14+):**
   Android strictly limits background microphone access. The application must run an Android Foreground Service declaring `foregroundServiceType="microphone"` in `AndroidManifest.xml` with an active ongoing notification, and request `RECORD_AUDIO` runtime permissions.

2. **Wi-Fi multicast sleep policies:**
   Many Android devices drop or throttle outbound multicast packets when the screen turns off. The app must acquire `WifiManager.MulticastLock` and `WifiManager.WIFI_MODE_FULL_HIGH_PERF`, along with wake lock protection via `wakelock_plus`.

3. **Audio recording pipeline:**
   While `flutter_pcm_sound` handles low-latency audio playback on the receiver side, microphone capture requires an input plugin such as `record` or a native Kotlin audio recording bridge configured for raw 48 kHz 16-bit PCM streaming.

4. **Battery and thermal telemetry:**
   Query Android battery percentage and charging state via `battery_plus` or Android intent `ACTION_BATTERY_CHANGED`. On AC power, report bit 7 set with 100% (`0xE4`).

---

## 3. UI and mode selection

Add a mode toggle on the main screen:
* **Parent Mode (Receiver):** Current behavior, listening to multicast stream and graphing audio levels.
* **Baby Mode (Transmitter):** Switches to microphone ingest, displays live VU meter with noise gate threshold calibration, and broadcasts encrypted multicast packets.
