# Babyphone Desktop Sender

Turns a Linux or Windows computer into a babyphone sender. It captures the microphone, runs the same Noise Guard as the M5StickS3 firmware, encodes Opus at 24 kbps, encrypts with AES-128-CTR and multicasts RTP to `239.255.0.1:5004`. The Flutter app, `client/desktop/babyphone_receiver.py` and (with `--unencrypted`) VLC can all play it.

See [desktop_sender_plan.md](../../desktop_sender_plan.md) for the design and wire format.

## Build (Debian/Ubuntu)

```bash
sudo apt install build-essential pkg-config libasound2-dev libopus-dev
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh   # if cargo is missing

cd client/desktop_sender
cargo build --release
```

## Build for Windows

Cross-compile from Linux with MinGW. libopus is built from source by CMake, so the result is a single `.exe` with no extra DLLs:

```bash
sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
rustup target add x86_64-pc-windows-gnu
cargo build --release --target x86_64-pc-windows-gnu
# -> target/x86_64-pc-windows-gnu/release/babyphone_sender.exe
```

Or build natively on Windows with Rust, Visual Studio Build Tools and CMake installed: `cargo build --release`.

Copy `babyphone_key.env` next to the `.exe`. On Windows, `--iface` is most reliable with the adapter's IP address (see `--list`).

## Run

```bash
# Show mics and network interfaces
./target/release/babyphone_sender --list

# Stream with defaults (default mic, 0.5 s guard, threshold 75)
./target/release/babyphone_sender

# Pick a NIC, slower guard, more sensitive gate
./target/release/babyphone_sender --iface wlan0 --guard 5 --threshold 40

# Plain Opus for VLC (client/vlc/stream.sdp)
./target/release/babyphone_sender --unencrypted
```

The terminal shows a live VU meter with the current RMS against the threshold. Watch the RMS value while the room is quiet and set `--threshold` a bit above it. Press Enter to cycle the Noise Guard (0.5 s → 5 s → 10 s), like the side button on the device.

The key is read from `babyphone_key.env` in the current directory, next to the executable, or in the repository root. Use `--key-file` to point elsewhere.

Mics that can't run at 48 kHz (common on Windows, where shared mode only offers the rate set in Sound settings) are resampled automatically. `--list` shows which devices are resampled.

## Telemetry

- **Battery:** read from `/sys/class/power_supply` on Linux and `GetSystemPowerStatus` on Windows. Machines without a battery report 100 % on AC.
- **Temperature:** always reports 35 °C. Desktop CPUs often run hotter than the app's 68 °C warning level, which is set for the M5StickS3.
- **SSRC:** random per run, so the AES-CTR counter never overlaps the firmware's stream under the same key.
