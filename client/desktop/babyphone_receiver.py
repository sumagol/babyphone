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
import argparse
import fcntl

def get_ip_address(ifname):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        return socket.inet_ntoa(fcntl.ioctl(
            s.fileno(),
            0x8915,  # SIOCGIFADDR
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24])
    except Exception as e:
        print(f"Error getting IP for interface {ifname}: {e}")
        sys.exit(1)

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
    parser = argparse.ArgumentParser(description="Babyphone Receiver")
    parser.add_argument("--iface", default=None, help="Network interface name (e.g., enp195s0f0 or wlan0)")
    args = parser.parse_args()

    # 1. Setup UDP Multicast Socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)

    try:
        sock.bind((MULTICAST_GROUP, PORT))
    except OSError:
        sock.bind(('', PORT))
    
    # Join Multicast Group
    group = socket.inet_aton(MULTICAST_GROUP)
    
    if args.iface:
        iface_ip = get_ip_address(args.iface)
        iface_aton = socket.inet_aton(iface_ip)
    else:
        iface_aton = socket.INADDR_ANY
        
    mreq = struct.pack('4s4s', group, iface_aton)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    print(f"[*] Listening for Babyphone stream on {MULTICAST_GROUP}:{PORT}...")
    if args.iface:
        print(f"[*] Bound to specific interface: {args.iface} (IP: {iface_ip})")

    print("[*] Launching GStreamer for audio playback...")
    print("[*] No stream received yet? verify your iptables!")

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
    
    import sys
    process = subprocess.Popen(gst_cmd, stdin=subprocess.PIPE, stderr=sys.stderr)

    packet_count = 0
    try:
        while True:
            # Receive UDP packet
            data, addr = sock.recvfrom(2048)
            
            if packet_count == 0:
                print(f"[DEBUG] Received first raw UDP packet of {len(data)} bytes from {addr}!")

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
            
            packet_count += 1
            if packet_count == 1:
                print(f"[*] Received first packet successfully!")
            elif packet_count % 50 == 0:
                print(f"[*] Successfully decrypted and piped {packet_count} audio frames...")
            
    except KeyboardInterrupt:
        print("\n[*] Stopping Babyphone Receiver...")
    finally:
        if process.stdin:
            process.stdin.close()
        process.wait()
        sock.close()

if __name__ == '__main__':
    main()
