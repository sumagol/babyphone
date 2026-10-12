# Desktop Multicast Babyphone Sender (Rust)

A standalone cross-platform desktop application written in Rust that turns any Windows or Linux computer into an encrypted multicast babyphone sender. The stream is 100% compatible with the existing M5StickS3 firmware protocol, the Flutter Android client, the Python desktop receiver, and VLC.

---

## 1. Why Rust instead of C for the desktop client

1. **Native audio abstraction via `cpal`.** In C on Windows, WASAPI requires manual COM initialization, event handles, and circular buffer management. On Linux, it requires separate ALSA or PulseAudio code. The `cpal` crate abstracts this safely into clean, platform-native callbacks (WASAPI on Windows, ALSA on Linux).
2. **Zero runtime dependencies on Windows.** The resulting `.exe` links statically against the Universal C Runtime and bundles Opus. It uses Windows built-in WASAPI and Winsock DLLs. No external DLLs, Python interpreters, or GStreamer installations are required on client machines.
3. **Pure Rust cryptography.** AES-128-CTR runs via the `aes` and `ctr` crates, avoiding OpenSSL or Windows CNG (`bcrypt.h`) C header plumbing.
4. **Cross-compilation.** You can compile the Windows `.exe` directly from Linux using the `x86_64-pc-windows-gnu` target without booting Windows.

---

## 2. Technical architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Desktop Sender (Rust)                    │
│                                                             │
│ 1. Audio Ingest (cpal):                                     │
│    Windows: WASAPI event callback (48 kHz / 16-bit mono)    │
│    Linux:   ALSA / PulseAudio bridge                        │
│    Pushes 20 ms frames (960 samples / 1920 bytes)           │
│                                                             │
│ 2. DSP & Level Detection:                                   │
│    - 80 Hz high-pass biquad filter                          │
│    - RMS & peak dB calculation                              │
│                                                             │
│ 3. Noise Guard State Machine:                               │
│    - Modes: 0.5s (infant), 5.0s (toddler), 10.0s (heavy)    │
│    - Hangover hold time: 2.5s (125 frames)                  │
│    - Comfort silence generator (keeps mobile watchdogs calm)│
│                                                             │
│ 4. Compression (audiopus / libopus):                        │
│    - 48 kHz, mono, 24 kbps VBR VoIP profile                 │
│                                                             │
│ 5. Packet Framing & Encryption:                             │
│    - 12-byte RTP header (PT 96, seq, timestamp += 960)      │
│    - 8-byte Extension Header (0xBABB profile, telemetry)    │
│    - AES-128-CTR encrypted payload                          │
│                                                             │
│ 6. Network Transmission (std::net::UdpSocket):              │
│    - Multicast to 239.255.0.1:5004 (TTL = 1)                │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Protocol compatibility specifications

To maintain full compatibility with the existing clients, the Rust sender matches the firmware byte-for-byte:

* **Audio format:** 48,000 Hz, 16-bit signed PCM, mono, 20 ms frames (960 samples).
* **Opus bitrate:** 24 kbps, VoIP application mode.
* **RTP header (12 bytes):**
  * Byte 0: `0x90` (Version 2, Extension flag set).
  * Byte 1: `0x60` (Payload Type 96).
  * Bytes 2-3: 16-bit incrementing Sequence Number.
  * Bytes 4-7: 32-bit Timestamp (advances by 960 per packet).
  * Bytes 8-11: 32-bit SSRC identifier.
* **RTP extension header (8 bytes):**
  * Bytes 12-13: Profile ID `0xBABB`.
  * Bytes 14-15: Length `0x0001` (1 word = 4 payload bytes).
  * Byte 16: Telemetry byte (bit 7: charging status, bits 0-6: battery level percentage). On AC power, reports `100%` (`0x64`).
  * Byte 17: Temperature byte. Fixed default (`35` °C) or read from system sensors.
  * Byte 18: Guard filter duration in tenths of a second (`5` for 0.5s, `50` for 5.0s, `100` for 10.0s).
  * Byte 19: Guard mode index (`0`, `1`, or `2`).
* **AES-128-CTR encryption:**
  * Key: 16 bytes loaded from `babyphone_key.env`.
  * Counter block (16 bytes):
    `[ SSRC (4 bytes) | Sequence (2 bytes) | Timestamp (4 bytes) | 6 bytes of 0x00 padding ]`
* **Network endpoint:** Multicast group `239.255.0.1`, port `5004`, TTL `1`.

---

## 4. Progression steps

### Step 1. Project workspace and dependencies
Create `client/desktop_sender/` with a cargo configuration:
* `cpal` for cross-platform audio capture.
* `audiopus` or `opus` for statically bundled Opus encoding.
* `aes` and `ctr` for pure Rust AES-128-CTR encryption.
* `clap` for command-line arguments (interface selection, list input devices, guard mode).
* Configure `.cargo/config.toml` with static CRT flags for standalone Windows builds.

### Step 2. Audio ingest and ringbuffer
* Enumerate system audio input devices and select default or user-specified mic.
* Configure stream at 48 kHz mono with 16-bit format (downmix to mono if stereo only).
* Feed raw audio frames into a lock-free ringbuffer or channel.

### Step 3. DSP filter, VU meter, and Noise Guard
* Implement the 80 Hz high-pass IIR filter to eliminate mic rumble.
* Compute RMS levels and dBFS for a live ASCII terminal VU meter.
* Port the Noise Guard state machine:
  * When baby cries continuously for the configured duration, open the audio gate.
  * Hold audio unmuted for 2.5 seconds after sound level drops.
  * When gate is closed, emit synthetic comfort silence frames instead of stopping packets.

### Step 4. Opus encoding and RTP packetizer
* Initialize Opus encoder at 48,000 Hz, 1 channel, 24 kbps.
* Build the 12-byte RTP header and the 8-byte `0xBABB` extension header.
* Query battery state on laptops via system APIs (`battery` crate or Windows power status).

### Step 5. AES-128-CTR encryption and key loading
* Parse `babyphone_key.env` at startup (fallback to default key if missing).
* Construct the 16-byte counter block from the RTP header fields.
* Encrypt the Opus payload in-place.

### Step 6. Multicast UDP transmission
* Bind a UDP socket with multicast TTL set to 1.
* Allow selecting the egress network interface by IP or adapter name.
* Transmit frames at a consistent 20 ms cadence (50 packets per second).

### Step 7. Verification and cross-compilation
* Test stream against `client/desktop/babyphone_receiver.py` on Linux.
* Test stream against the Flutter Android application over Wi-Fi.
* Build Windows release binary via `cargo build --release --target x86_64-pc-windows-gnu` and verify on a Windows machine.

### Step 8. Flutter Android sender mode (Mobile transmitter)
Add a transmitter mode to the Flutter mobile application (`client/babyphone_app/`) so a spare Android phone can act as the babyphone sender:
* **Audio capture:** Ingest raw PCM microphone audio at 48,000 Hz mono in 20 ms frames (960 samples).
* **Opus and crypto pipeline:** Reuse the existing `opus_codec` encoder and `encrypt` package (AES-128-CTR) to assemble identical RTP packets with the `0xBABB` telemetry header.
* **Multicast socket:** Transmit datagrams to `239.255.0.1:5004` using `RawDatagramSocket`.
* **Background service and Wi-Fi locks:** Implement an Android Foreground Service (`foregroundServiceType="microphone"`), high-performance Wi-Fi lock (`WIFI_MODE_FULL_HIGH_PERF`), and Doze mode exemption so the device streams continuously with the screen off.
* **UI toggle:** Add a toggle in the mobile UI between Parent Mode (Receiver) and Baby Mode (Transmitter).
