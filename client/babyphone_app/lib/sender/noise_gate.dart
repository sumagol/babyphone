import 'dart:math';
import 'dart:typed_data';

const int frameSamples = 960; // 20 ms at 48 kHz
const int _hangoverFrames = 125; // 2.5 s hold after noise drops

/// Noise Guard modes; the index matches the firmware enum sent in RTP ext byte 3.
enum GuardMode {
  halfSecond(25, 5, '0.5s'),
  fiveSeconds(250, 50, '5.0s'),
  tenSeconds(500, 100, '10.0s');

  const GuardMode(this.requiredFrames, this.tenths, this.label);

  /// Consecutive noisy frames required before the gate opens.
  final int requiredFrames;

  /// Duration in tenths of a second (RTP ext byte 2).
  final int tenths;
  final String label;
}

/// 2nd-order Butterworth high-pass (RBJ cookbook), cuts handling rumble and hum.
class HighPass {
  late final double _b0, _b1, _b2, _a1, _a2;
  double _x1 = 0, _x2 = 0, _y1 = 0, _y2 = 0;

  HighPass(double cutoffHz, double sampleRate) {
    final w0 = 2 * pi * cutoffHz / sampleRate;
    final alpha = sin(w0) / (2 * sqrt1_2); // Q = 1/sqrt(2)
    final c = cos(w0);
    final a0 = 1 + alpha;
    _b0 = (1 + c) / 2 / a0;
    _b1 = -(1 + c) / a0;
    _b2 = (1 + c) / 2 / a0;
    _a1 = -2 * c / a0;
    _a2 = (1 - alpha) / a0;
  }

  void process(Int16List frame) {
    for (var i = 0; i < frame.length; i++) {
      final x = frame[i].toDouble();
      final y = _b0 * x + _b1 * _x1 + _b2 * _x2 - _a1 * _y1 - _a2 * _y2;
      _x2 = _x1;
      _x1 = x;
      _y2 = _y1;
      _y1 = y;
      frame[i] = y.round().clamp(-32768, 32767);
    }
  }
}

/// Port of the firmware gate in audio_capture.c plus the RMS floor in audio_encoder.c.
class NoiseGate {
  double threshold;
  int _noiseFrames = 0;
  int _silenceFrames = 0;
  bool _open = false;

  NoiseGate(this.threshold);

  bool get isOpen => _open;

  /// Gates the frame in place and returns its pre-gate RMS for the level meter.
  double process(Int16List frame, GuardMode mode) {
    final level = rms(frame);

    if (level >= threshold) {
      _noiseFrames++;
      _silenceFrames = 0;
      if (_noiseFrames >= mode.requiredFrames) _open = true;
    } else {
      _noiseFrames = 0;
      if (_open && ++_silenceFrames >= _hangoverFrames) {
        _open = false;
        _silenceFrames = 0;
      }
    }

    if (!_open) {
      // Closed gate still yields a (silent) frame so receivers' watchdogs stay quiet.
      frame.fillRange(0, frame.length, 0);
      return level;
    }

    if (level < threshold) {
      // Soft gate during the hangover to suppress hiss between cries.
      final factor = pow(level / threshold, 2);
      for (var i = 0; i < frame.length; i++) {
        frame[i] = (frame[i] * factor).truncate();
      }
    }

    // Firmware floor: RMS 50 against threshold 75, kept proportional here.
    if (rms(frame) < threshold * 2 / 3) frame.fillRange(0, frame.length, 0);
    return level;
  }
}

double rms(Int16List frame) {
  var sumSq = 0.0;
  for (final s in frame) {
    sumSq += s * s;
  }
  return sqrt(sumSq / frame.length);
}
