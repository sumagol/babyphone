# Desktop multicast babyphone sender (Rust)

A standalone cross-platform desktop application written in Rust that turns any Windows or Linux computer into a multicast babyphone sender. The stream matches the existing M5StickS3 firmware protocol byte for byte, playing directly into the Flutter Android client and the Python desktop receiver, or into VLC when run in unencrypted mode.

---

## 1. Why Rust instead of C for the desktop client

1. **Native audio abstraction with `cpal`.** In C on Windows, WASAPI requires manual COM initialization, event handles, and circular buffer management. On Linux, it requires separate ALSA or PulseAudio code. The `cpal` crate abstracts this into platform-native callbacks (WASAPI on Windows, ALSA on Linux).
2. **Zero runtime dependencies on Windows.** The resulting `.exe` links statically against the Universal C Runtime and bundles Opus. It uses Windows built-in WASAPI and Winsock DLLs. No external DLLs, Python interpreters, or GStreamer installations are required on client machines.
3. **Pure Rust cryptography.** AES-128-CTR runs via the `aes` and `ctr` crates, avoiding OpenSSL or Windows CNG (`bcrypt.h`) C header plumbing.
4. **Cross-compilation.** You can compile the Windows `.exe` directly from Linux using either `cargo-xwin` (targeting `x86_64-pc-windows-msvc`) or MinGW (targeting `x86_64-pc-windows-gnu`).

---

## 2. Technical architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Desktop Sender (Rust)                    │
│                                                             │
│ 1. Audio Ingest (cpal):                                     │
│    - Captures f32 or i16 samples from default or chosen mic │
│    - Downmixes stereo to mono, converts f32 to i16 PCM      │
│    - Validates 48 kHz stream configuration                  │
│    - Accumulates exact 20 ms frames (960 samples / 1920 B)  │
│                                                             │
│ 2. DSP and Level Detection:                                 │
│    - 80 Hz high-pass biquad filter (cuts fan and desk hum)  │
│    - Configurable RMS threshold (--threshold, default 75)   │
│    - Peak dB calculation with live terminal VU meter        │
│                                                             │
│ 3. Noise Guard State Machine:                               │
│    - Modes: 0.5s (infant), 5.0s (toddler), 10.0s (heavy)    │
│    - Hangover hold time: 2.5s (125 frames)                  │
│    - Muted state emits zeroed PCM frames to keep RTP alive  │
│                                                             │
│ 4. Compression (audiopus / opus):                          │
│    - 48 kHz, mono, 24 kbps VBR VoIP profile                 │
│                                                             │
│ 5. Packet Framing and Encryption:                           │
│    - 12-byte RTP header (PT 96, seq, timestamp += 960)      │
│    - 8-byte Extension Header (0xBABB profile, telemetry)    │
│    - AES-128-CTR payload encryption (Ctr128BE)              │
│    - Optional plain Opus payload (--unencrypted) for VLC    │
│                                                             │
│ 6. Network Transmission (socket2):                          │
│    - Outbound IP_MULTICAST_IF bound to selected network     │
│    - Multicast to 239.255.0.1:5004 (TTL = 1 or 2)           │
│    - Pacing driven directly by sound card sample arrival    │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Protocol compatibility specifications

The Rust sender matches the firmware wire format:

* **Audio format:** 48,000 Hz, 16-bit signed PCM, mono, 20 ms frames (960 samples).
* **Opus bitrate:** 24 kbps, VoIP application mode.
* **RTP header (12 bytes):**
  * Byte 0: `0x90` (Version 2, Extension flag set).
  * Byte 1: `0x60` (Payload Type 96).
  * Bytes 2-3: 16-bit incrementing sequence number.
  * Bytes 4-7: 32-bit timestamp (advances by 960 per packet).
  * Bytes 8-11: 32-bit SSRC identifier.
* **RTP extension header (8 bytes):**
  * Bytes 12-13: Profile ID `0xBABB`.
  * Bytes 14-15: Length `0x0001` (1 word = 4 payload bytes).
  * Byte 16: Telemetry byte. Bit 7 is the charging flag, bits 0-6 are battery percentage. When running on AC power, bit 7 must be set, giving `100% charging` as `0xE4` (`0x80 | 0x64`). On battery power, bit 7 is cleared when discharging.
  * Byte 17: Temperature byte. Fixed default (`35` °C) or read from system sensors.
  * Byte 18: Guard filter duration in tenths of a second (`5` for 0.5s, `50` for 5.0s, `100` for 10.0s).
  * Byte 19: Guard mode index (`0`, `1`, or `2`).
* **AES-128-CTR encryption:**
  * Key: 16 bytes loaded from `babyphone_key.env` (defaults to `BabyPhoneKey2026`).
  * Counter block (16 bytes):
    `[ SSRC (4 bytes) | Sequence (2 bytes) | Timestamp (4 bytes) | 6 bytes of 0x00 padding ]`
  * Counter orientation: Big-endian 128-bit counter incrementing from byte 15 (`ctr::Ctr128BE<aes::Aes128Enc>`), matching mbedTLS and Python cryptography.
* **Network endpoint:** Multicast group `239.255.0.1`, port `5004`, TTL `1` (or configurable up to `2` for networks with Wi-Fi repeaters).
* **Unencrypted mode:** When started with `--unencrypted`, the Opus payload skips AES encryption, allowing VLC to play `stream.sdp` without a decryption proxy.

---

## 4. Progression steps

### Step 1. Project workspace and dependencies
Create `client/desktop_sender/` with a cargo configuration:
* `cpal` for cross-platform audio capture.
* `audiopus` or `opus` for bundled Opus encoding.
* `aes` and `ctr` for pure Rust AES-128-CTR encryption.
* `socket2` for raw socket configuration, specifically setting `IP_MULTICAST_IF`.
* `clap` for command-line arguments (interface selection, list input devices, guard mode, sensitivity threshold, unencrypted mode).
* For Windows builds from Linux: document using `cargo-xwin` targeting `x86_64-pc-windows-msvc`, or installing MinGW (`gcc-mingw-w64-x86-64`) when targeting `x86_64-pc-windows-gnu`.

### Step 2. Audio ingest, format adaptation, and ringbuffer
* Enumerate system audio input devices and select default or user-specified mic.
* Support both `f32` and `i16` input formats from `cpal`. Convert `f32` samples to `i16` by clamping and scaling.
* Downmix stereo channels to mono by taking the channel mean.
* Validate that the input sample rate is 48 kHz. If the hardware only supports 44.1 kHz, integrate a lightweight resampler or exit with a clear message explaining how to adjust device properties.
* Accumulate samples into an internal FIFO buffer, yielding exact 960-sample (20 ms) frames.

### Step 3. DSP filter, VU meter, and Noise Guard
* Implement an 80 Hz high-pass IIR biquad filter to cut out desk vibrations and laptop fan rumble.
* Compute frame RMS and peak dBFS. Render a live ASCII terminal VU meter that displays current sound level relative to the noise gate threshold.
* Add a `--threshold` CLI argument (defaulting to 75.0) so users can calibrate sensitivity to their specific microphone.
* Implement the Noise Guard state machine:
  * When baby sound exceeds the threshold continuously for the configured duration (0.5s, 5.0s, or 10.0s), open the audio gate.
  * Hold audio unmuted for 2.5 seconds (125 frames) after the sound level drops below threshold.
  * When the gate is closed, emit zeroed PCM frames instead of halting the stream, keeping RTP packet flow alive so mobile watchdogs do not alarm.

### Step 4. Opus encoding and RTP packetizer
* Initialize the Opus encoder at 48,000 Hz, 1 channel, 24 kbps in VoIP application mode.
* Construct the 12-byte RTP header with sequence numbers and timestamps advancing by 960 per frame.
* Build the 8-byte `0xBABB` extension header.
* Query system battery state on laptops. Report `0xE4` (100% on AC power) when plugged in, or actual battery percentage with bit 7 cleared when discharging.

### Step 5. AES-128-CTR encryption and key loading
* Parse `babyphone_key.env` at startup, falling back to `BabyPhoneKey2026` if missing.
* Construct the 16-byte counter block from SSRC, sequence number, timestamp, and six zero bytes.
* Encrypt the Opus payload in place using `ctr::Ctr128BE<aes::Aes128Enc>`.
* If `--unencrypted` is passed, skip the encryption pass and leave the Opus payload raw.

### Step 6. Multicast UDP transmission with interface binding
* Create the UDP socket using `socket2`.
* Allow the user to select the egress network interface by IP address or adapter name via `--iface`.
* Set `IP_MULTICAST_IF` on the socket to ensure packets exit through the intended local network interface rather than a VPN or virtual adapter.
* Set socket TTL to 1 (or 2 via `--ttl`).
* Transmit frames as soon as each 960-sample frame is produced by the audio ingest thread. The sound card ADC clock paces the stream at 50 packets per second with zero software timer drift.

### Step 7. Verification and testing
* Verify encrypted stream playback against `client/desktop/babyphone_receiver.py` on Linux.
* Verify encrypted stream playback and telemetry display on the Flutter Android client over Wi-Fi.
* Verify unencrypted stream playback directly in VLC using `client/vlc/stream.sdp` with `--unencrypted`.
* Build and run the Windows binary on a Windows laptop, confirming WASAPI capture and correct multicast adapter routing.
