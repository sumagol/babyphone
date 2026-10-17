import 'dart:math';
import 'dart:typed_data';

import 'package:encrypt/encrypt.dart' as encrypt;

import 'noise_gate.dart';

/// Values for RTP extension bytes 0 and 1.
class PowerStatus {
  final int battery;
  final bool charging;
  final int temperatureC;

  const PowerStatus({required this.battery, required this.charging, required this.temperatureC});

  static const unknown = PowerStatus(battery: 100, charging: true, temperatureC: 0);
}

const int _headerLength = 20; // 12 RTP + 8 extension

/// Builds packets byte-identical in layout to firmware/main/audio_encoder.c.
class RtpPacketizer {
  final encrypt.Encrypter? _encrypter;
  final int ssrc;
  int _sequence = 0;
  int _timestamp = 0;
  int packetsSent = 0;

  /// [ssrc] defaults to a random value so the CTR IV never repeats the
  /// firmware's fixed SSRC (0x12345678) under the same key.
  RtpPacketizer({Uint8List? key, int? ssrc, int sequence = 0, int timestamp = 0})
      : _encrypter = key == null
            ? null
            : encrypt.Encrypter(encrypt.AES(encrypt.Key(key), mode: encrypt.AESMode.ctr, padding: null)),
        ssrc = ssrc ?? Random.secure().nextInt(1 << 32),
        _sequence = sequence,
        _timestamp = timestamp;

  bool get encrypted => _encrypter != null;

  Uint8List build(Uint8List opus, PowerStatus power, GuardMode guard) {
    final p = Uint8List(_headerLength + opus.length);
    final h = ByteData.sublistView(p);
    p[0] = 0x90; // V=2, X=1
    p[1] = 96; // PT
    h.setUint16(2, _sequence);
    h.setUint32(4, _timestamp);
    h.setUint32(8, ssrc);

    p[12] = 0xBA; // profile "Baby"
    p[13] = 0xBB;
    h.setUint16(14, 1); // 1 word
    p[16] = (power.battery & 0x7F) | (power.charging ? 0x80 : 0);
    p[17] = power.temperatureC.clamp(0, 255);
    p[18] = guard.tenths;
    p[19] = guard.index;

    final encrypter = _encrypter;
    if (encrypter == null) {
      p.setAll(_headerLength, opus);
    } else {
      // IV = SSRC(4) | seq(2) | timestamp(4) | zero(6)
      final iv = Uint8List(16)
        ..setRange(0, 4, p, 8)
        ..setRange(4, 6, p, 2)
        ..setRange(6, 10, p, 4);
      p.setAll(_headerLength, encrypter.encryptBytes(opus, iv: encrypt.IV(iv)).bytes);
    }

    _sequence = (_sequence + 1) & 0xFFFF;
    _timestamp = (_timestamp + frameSamples) & 0xFFFFFFFF;
    packetsSent++;
    return p;
  }
}
