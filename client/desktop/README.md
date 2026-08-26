# Babyphone Desktop Receiver

This directory contains a standalone Python script designed to let you listen to the encrypted Babyphone multicast stream directly from your Linux, macOS, or Windows desktop.

Because the ESP32-S3 firmware uses a highly-optimized, custom AES-128-CTR encryption cipher directly over the Opus audio payload (to save CPU cycles), standard media players like VLC or GStreamer cannot decrypt the stream natively using standard `libsrtp`.

This script acts as an intelligent bridge to solve this problem.

## How it works

1. **Multicast Ingest:** The script binds to the local network and joins the UDP Multicast group `239.255.0.1:5004`.
2. **RTP Parsing:** It parses incoming RTP packets and seamlessly steps over our custom battery telemetry extensions.
3. **Dynamic Decryption:** It dynamically reconstructs the AES-128-CTR Initialization Vector (IV) exactly like the ESP32 does, and decrypts the Opus audio using the shared key (`BabyPhoneKey2026`).
4. **GStreamer Piping:** It instantly pipes the raw, decrypted RTP packets into a background `gst-launch-1.0` subprocess.

## Requirements

You must have **Python 3**, **GStreamer**, and the **Python Cryptography** library installed.

On Debian/Ubuntu Linux:
```bash
# Install GStreamer and the Good Plugins (which contain rtpopusdepay and opusdec)
sudo apt update
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-base python3-pip

# Install the Python cryptography module
pip3 install cryptography
```

## Usage

Simply run the script from your terminal:

```bash
./babyphone_receiver.py
```

The script will launch GStreamer in the background and block. As soon as the babyphone starts streaming (e.g. it detects noise or is manually woken up), the audio will automatically play out of your desktop speakers with ultra-low latency. 

Press `Ctrl+C` to gracefully terminate the stream and close the GStreamer pipeline.
