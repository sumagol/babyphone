import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:opus_codec_dart/opus_codec_dart.dart';

import 'noise_gate.dart';
import 'rtp_packetizer.dart';

/// Snapshot for the UI, published every 100 ms while transmitting.
class SenderStats {
  final double rms;
  final bool gateOpen;
  final int packets;
  final PowerStatus power;

  const SenderStats({required this.rms, required this.gateOpen, required this.packets, required this.power});

  static const idle = SenderStats(rms: 0, gateOpen: false, packets: 0, power: PowerStatus.unknown);
}

/// Phone-as-babyphone pipeline: mic (native AudioRecord) -> high-pass -> Noise Guard
/// -> Opus 24 kbps -> RTP + 0xBABB telemetry -> AES-128-CTR -> multicast.
class BabySender {
  static const _control = MethodChannel('com.babyphone/multicast');
  static const _mic = EventChannel('com.babyphone/mic');
  static final _group = InternetAddress('239.255.0.1');
  static const _port = 5004;
  static const _opusSetBitrateRequest = 4002; // OPUS_SET_BITRATE_REQUEST

  final stats = ValueNotifier<SenderStats>(SenderStats.idle);
  final _highPass = HighPass(80, 48000);
  final NoiseGate _gate;
  GuardMode guard;

  RawDatagramSocket? _socket;
  BufferedOpusEncoder? _encoder;
  RtpPacketizer? _packetizer;
  StreamSubscription<dynamic>? _micSub;
  Timer? _powerTimer;
  PowerStatus _power = PowerStatus.unknown;
  int _frameCount = 0;

  /// Called when capture fails after start (mic taken by a call, device error).
  void Function(String message)? onError;

  BabySender({required double threshold, required this.guard}) : _gate = NoiseGate(threshold);

  bool get running => _micSub != null;

  double get threshold => _gate.threshold;
  set threshold(double v) => _gate.threshold = v;

  Future<void> start() async {
    if (running) return;

    final granted = await _control.invokeMethod<bool>('requestMicPermission');
    if (granted != true) throw StateError('Microphone permission denied');

    // Needs the mic permission first: Android 14+ rejects a microphone FGS without it.
    await _control.invokeMethod('startForegroundService', {'mode': 'sender'});
    await _control.invokeMethod('requestBatteryExemption');

    try {
      const envKey = String.fromEnvironment('AES_KEY', defaultValue: 'BabyPhoneKey2026');
      _packetizer = RtpPacketizer(key: Uint8List.fromList(utf8.encode(envKey)));

      _encoder = BufferedOpusEncoder(
        sampleRate: 48000,
        channels: 1,
        application: Application.voip,
        maxInputBufferSizeBytes: frameSamples * 2,
      )..encoderCtl(request: _opusSetBitrateRequest, value: 24000);

      _socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0)
        ..multicastHops = 1
        ..multicastLoopback = false;

      await _refreshPower();
      // Same 5 s cadence the firmware uses for PMIC polling.
      _powerTimer = Timer.periodic(const Duration(seconds: 5), (_) => _refreshPower());

      _micSub = _mic.receiveBroadcastStream().listen(
        _onFrame,
        onError: (Object e) {
          final message = e is PlatformException ? (e.message ?? e.code) : '$e';
          stop();
          onError?.call(message);
        },
      );
    } catch (_) {
      await stop();
      rethrow;
    }
  }

  Future<void> stop() async {
    await _micSub?.cancel();
    _micSub = null;
    _powerTimer?.cancel();
    _socket?.close();
    _socket = null;
    _encoder?.destroy();
    _encoder = null;
    _packetizer = null;
    stats.value = SenderStats.idle;
    try {
      await _control.invokeMethod('stopForegroundService');
    } on PlatformException catch (_) {}
  }

  Future<void> _refreshPower() async {
    try {
      final m = await _control.invokeMapMethod<String, dynamic>('getPowerStatus');
      if (m != null) {
        _power = PowerStatus(
          battery: (m['level'] as int).clamp(0, 100),
          charging: m['charging'] as bool,
          temperatureC: m['temperatureC'] as int,
        );
      }
    } on PlatformException catch (_) {}
  }

  void _onFrame(dynamic data) {
    final encoder = _encoder, packetizer = _packetizer, socket = _socket;
    if (encoder == null || packetizer == null || socket == null) return;

    // Kotlin delivers exactly one 20 ms frame of s16le per event.
    final bytes = ByteData.sublistView(data as Uint8List);
    final pcm = Int16List(frameSamples);
    for (var i = 0; i < frameSamples; i++) {
      pcm[i] = bytes.getInt16(i * 2, Endian.little);
    }

    _highPass.process(pcm);
    final level = _gate.process(pcm, guard);

    encoder.inputBuffer.setAll(0, pcm.buffer.asUint8List());
    encoder.inputBufferIndex = frameSamples * 2;
    final opus = encoder.encode();

    socket.send(packetizer.build(opus, _power, guard), _group, _port);

    if (++_frameCount % 5 == 0) {
      stats.value = SenderStats(
        rms: level,
        gateOpen: _gate.isOpen,
        packets: packetizer.packetsSent,
        power: _power,
      );
    }
  }
}
