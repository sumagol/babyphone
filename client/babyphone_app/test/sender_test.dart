import 'dart:convert';
import 'dart:typed_data';

import 'package:babyphone_app/sender/noise_gate.dart';
import 'package:babyphone_app/sender/rtp_packetizer.dart';
import 'package:flutter_test/flutter_test.dart';

Int16List loud() => Int16List.fromList(List.generate(frameSamples, (i) => i.isEven ? 1000 : -1000));
Int16List silent() => Int16List(frameSamples);

const power = PowerStatus(battery: 100, charging: true, temperatureC: 35);

void main() {
  group('NoiseGate', () {
    test('opens after the guard time and holds for the hangover', () {
      final gate = NoiseGate(75);
      for (var i = 0; i < 24; i++) {
        gate.process(loud(), GuardMode.halfSecond);
      }
      expect(gate.isOpen, isFalse);

      final f = loud();
      gate.process(f, GuardMode.halfSecond);
      expect(gate.isOpen, isTrue);
      expect(f[0], 1000);

      for (var i = 0; i < 124; i++) {
        gate.process(silent(), GuardMode.halfSecond);
      }
      expect(gate.isOpen, isTrue);
      gate.process(silent(), GuardMode.halfSecond);
      expect(gate.isOpen, isFalse);
    });

    test('closed gate zeroes audio', () {
      final f = loud();
      NoiseGate(75).process(f, GuardMode.tenSeconds);
      expect(f.every((s) => s == 0), isTrue);
    });
  });

  test('high-pass removes DC', () {
    final hp = HighPass(80, 48000);
    final f = Int16List(frameSamples);
    for (var i = 0; i < 10; i++) {
      f.fillRange(0, frameSamples, 5000);
      hp.process(f);
    }
    expect(f.every((s) => s.abs() < 5), isTrue);
  });

  group('RtpPacketizer', () {
    test('header layout matches the firmware', () {
      final p = RtpPacketizer(ssrc: 0x12345678);
      p.build(Uint8List.fromList([1, 2, 3]), power, GuardMode.halfSecond);
      final pkt = p.build(Uint8List.fromList([1, 2, 3]), power, GuardMode.fiveSeconds);
      expect(pkt, [
        0x90, 96, 0, 1, 0, 0, 0x03, 0xC0, 0x12, 0x34, 0x56, 0x78, //
        0xBA, 0xBB, 0, 1, 0xE4, 35, 50, 1, //
        1, 2, 3,
      ]);
    });

    test('encryption matches the Python receiver (cryptography AES-CTR)', () {
      final p = RtpPacketizer(
        key: Uint8List.fromList(utf8.encode('BabyPhoneKey2026')),
        ssrc: 0xDEADBEEF,
        sequence: 7,
        timestamp: 960 * 7,
      );
      final pkt = p.build(Uint8List.fromList(List.generate(40, (i) => i)), power, GuardMode.halfSecond);
      // Generated with babyphone_receiver.py's cipher setup.
      expect(pkt.sublist(20), [
        134, 160, 47, 243, 160, 107, 216, 64, 236, 105, 135, 234, 116, 138, 174, 11, 93, 152, 216, 20, //
        105, 232, 32, 208, 122, 152, 38, 115, 211, 39, 245, 234, 61, 218, 218, 66, 59, 51, 185, 112,
      ]);
    });

    test('sequence and timestamp wrap', () {
      final p = RtpPacketizer(ssrc: 1, sequence: 0xFFFF, timestamp: 0xFFFFFFFF - 959);
      p.build(Uint8List(1), power, GuardMode.halfSecond);
      final pkt = p.build(Uint8List(1), power, GuardMode.halfSecond);
      final h = ByteData.sublistView(pkt);
      expect(h.getUint16(2), 0);
      expect(h.getUint32(4), 0);
    });
  });
}
