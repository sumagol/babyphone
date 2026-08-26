#!/usr/bin/env python3
import socket
import struct
import subprocess
import sys

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.backends import default_backend
except ImportError:
    print("Error: The 'cryptography' module is required.")
    print("Please install it using: pip install cryptography")
    sys.exit(1)

import os

MULTICAST_GROUP = '239.255.0.1'
PORT = 5004

def load_key_from_env():
    env_path = os.path.join(os.path.dirname(__file__), '..', '..', 'babyphone_key.env')
    try:
        with open(env_path, 'r') as f:
            for line in f:
                if line.startswith('AES_KEY='):
                    return line.strip().split('=', 1)[1].encode('utf-8')
    except FileNotFoundError:
        pass
    print("[*] Warning: babyphone_key.env not found, using default key.")
    return b"BabyPhoneKey2026"

KEY = load_key_from_env()

def main():
    # 1. Setup UDP Multicast Socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', PORT))
    
    # Join Multicast Group
    group = socket.inet_aton(MULTICAST_GROUP)
    mreq = struct.pack('4sL', group, socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    print(f"[*] Listening for Babyphone stream on {MULTICAST_GROUP}:{PORT}...")
    print("[*] Launching GStreamer for audio playback...")

    # 2. Setup GStreamer Subprocess
    # We pipe the unencrypted RTP packets into GStreamer via stdin (fdsrc)
    gst_cmd = [
        "gst-launch-1.0", "-v", "fdsrc", 
        "!", "application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=96",
        "!", "rtpjitterbuffer", "latency=200",
        "!", "rtpopusdepay",
        "!", "opusdec",
        "!", "audioconvert",
        "!", "audioresample",
        "!", "pulsesink"
    ]
    
    process = subprocess.Popen(gst_cmd, stdin=subprocess.PIPE)

    try:
        while True:
            # Receive UDP packet
            data, addr = sock.recvfrom(2048)
            if len(data) < 12:
                continue

            # 3. Parse RTP Header for Telemetry Extensions
            has_extension = (data[0] & 0x10) != 0
            payload_offset = 12
            
            if has_extension and len(data) >= 20:
                ext_len = (data[14] << 8) | data[15]
                payload_offset = 12 + 4 + (ext_len * 4)
            
            if len(data) <= payload_offset:
                continue

            opus_payload = data[payload_offset:]

            # 4. Reconstruct Custom AES-128-CTR IV
            # IV = SSRC (4) + Sequence (2) + Timestamp (4) + Padding (6)
            iv = bytearray(16)
            iv[0:4] = data[8:12]   # SSRC
            iv[4:6] = data[2:4]    # Sequence
            iv[6:10] = data[4:8]   # Timestamp

            # 5. Decrypt Payload
            cipher = Cipher(algorithms.AES(KEY), modes.CTR(iv), backend=default_backend())
            decryptor = cipher.decryptor()
            decrypted_payload = decryptor.update(opus_payload) + decryptor.finalize()

            # 6. Reconstruct Unencrypted RTP Packet
            # Combine the original header (and extension) with the decrypted payload
            unencrypted_rtp = data[:payload_offset] + decrypted_payload

            # 7. Pipe to GStreamer
            process.stdin.write(unencrypted_rtp)
            process.stdin.flush()
            
    except KeyboardInterrupt:
        print("\n[*] Stopping Babyphone Receiver...")
    finally:
        if process.stdin:
            process.stdin.close()
        process.wait()
        sock.close()

if __name__ == '__main__':
    main()
