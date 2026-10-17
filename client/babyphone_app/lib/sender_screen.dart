import 'dart:async';
import 'dart:math';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter/material.dart';

import 'glass_container.dart';
import 'sender/baby_sender.dart';
import 'sender/noise_gate.dart';

/// Baby Mode: turns this phone into the babyphone transmitter.
class SenderScreen extends StatefulWidget {
  final ValueChanged<bool>? onActiveChanged;

  const SenderScreen({super.key, this.onActiveChanged});

  @override
  State<SenderScreen> createState() => _SenderScreenState();
}

class _SenderScreenState extends State<SenderScreen> {
  // Slider spans RMS 10..3000 on a log scale; phone mics vary a lot.
  static const double _minThreshold = 10, _maxThreshold = 3000;

  final _sender = BabySender(threshold: 75, guard: GuardMode.halfSecond);
  String _status = "Ready to transmit.";
  bool _starting = false;

  @override
  void initState() {
    super.initState();
    _sender.onError = (message) {
      if (!mounted) return;
      setState(() => _status = "Stopped: $message");
      widget.onActiveChanged?.call(false);
    };
  }

  @override
  void dispose() {
    // stats isn't disposed: the async stop() still publishes the idle snapshot.
    _sender.stop();
    super.dispose();
  }

  Future<void> _start() async {
    final net = await Connectivity().checkConnectivity();
    if (!net.contains(ConnectivityResult.wifi) && !net.contains(ConnectivityResult.ethernet)) {
      setState(() => _status = "Connect to Wi-Fi first.");
      return;
    }

    setState(() {
      _starting = true;
      _status = "Starting microphone...";
    });
    try {
      await _sender.start();
      _status = "Transmitting to 239.255.0.1:5004";
      widget.onActiveChanged?.call(true);
    } catch (e) {
      _status = "Start Error: ${e is StateError ? e.message : e}";
    }
    if (mounted) setState(() => _starting = false);
  }

  Future<void> _stop() async {
    await _sender.stop();
    widget.onActiveChanged?.call(false);
    if (mounted) setState(() => _status = "Stopped.");
  }

  double _toSlider(double threshold) => log(threshold / _minThreshold) / log(_maxThreshold / _minThreshold);

  double _fromSlider(double v) => _minThreshold * pow(_maxThreshold / _minThreshold, v);

  @override
  Widget build(BuildContext context) {
    final running = _sender.running;
    return Scaffold(
      body: Container(
        decoration: const BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [Color(0xFF0a192f), Color(0xFF020c1b)], // Midnight Deep Blue
          ),
        ),
        child: SafeArea(
          child: Stack(
            children: [
              Center(
                child: SingleChildScrollView(
                  padding: const EdgeInsets.fromLTRB(20, 80, 20, 140),
                  child: Column(
                    children: [
                      Text(
                        _status,
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: Colors.white70, fontSize: 16, letterSpacing: 1.2),
                      ),
                      const SizedBox(height: 30),
                      ValueListenableBuilder<SenderStats>(
                        valueListenable: _sender.stats,
                        builder: (context, stats, _) => _LevelPanel(
                          stats: stats,
                          threshold: _sender.threshold,
                          running: running,
                          thresholdPosition: _toSlider(_sender.threshold),
                        ),
                      ),
                      const SizedBox(height: 20),
                      GlassContainer(
                        child: Padding(
                          padding: const EdgeInsets.all(16),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                "Sensitivity threshold: RMS ${_sender.threshold.round()}",
                                style: const TextStyle(color: Colors.white70),
                              ),
                              Slider(
                                value: _toSlider(_sender.threshold),
                                activeColor: Colors.cyanAccent,
                                onChanged: (v) => setState(() => _sender.threshold = _fromSlider(v)),
                              ),
                              const Text(
                                "Set it just above the level shown in a quiet room.",
                                style: TextStyle(color: Colors.white38, fontSize: 12),
                              ),
                              const SizedBox(height: 16),
                              const Row(
                                children: [
                                  Icon(Icons.shield_outlined, color: Colors.cyanAccent, size: 18),
                                  SizedBox(width: 6),
                                  Text("Noise Guard", style: TextStyle(color: Colors.white70)),
                                ],
                              ),
                              const SizedBox(height: 8),
                              SizedBox(
                                width: double.infinity,
                                child: SegmentedButton<GuardMode>(
                                  segments: [
                                    for (final m in GuardMode.values) ButtonSegment(value: m, label: Text(m.label)),
                                  ],
                                  selected: {_sender.guard},
                                  showSelectedIcon: false,
                                  onSelectionChanged: (s) => setState(() => _sender.guard = s.first),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),

              // Bottom Controls
              Positioned(
                bottom: 40,
                left: 20,
                right: 20,
                child: GlassContainer(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 20.0, vertical: 12.0),
                    child: ElevatedButton.icon(
                      icon: Icon(running ? Icons.stop : Icons.mic, size: 28),
                      label: Text(
                        running ? "STOP" : "TRANSMIT",
                        style: const TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
                      ),
                      style: ElevatedButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 16),
                        backgroundColor:
                            running ? Colors.redAccent.withOpacity(0.8) : Colors.greenAccent.withOpacity(0.8),
                        foregroundColor: Colors.white,
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
                      ),
                      onPressed: _starting ? null : (running ? _stop : _start),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _LevelPanel extends StatelessWidget {
  final SenderStats stats;
  final double threshold;
  final double thresholdPosition;
  final bool running;

  const _LevelPanel({
    required this.stats,
    required this.threshold,
    required this.thresholdPosition,
    required this.running,
  });

  @override
  Widget build(BuildContext context) {
    // Same log scale as the slider, so the marker lines up with the threshold.
    final level = stats.rms <= 10 ? 0.0 : (log(stats.rms / 10) / log(300)).clamp(0.0, 1.0);
    final over = stats.rms >= threshold;
    final gateColor = stats.gateOpen ? Colors.cyanAccent : Colors.white38;

    return GlassContainer(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  running ? "RMS ${stats.rms.round()}" : "Microphone off",
                  style: const TextStyle(color: Colors.white, fontSize: 16, fontWeight: FontWeight.bold),
                ),
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(stats.gateOpen ? Icons.graphic_eq : Icons.volume_off, color: gateColor, size: 18),
                    const SizedBox(width: 4),
                    Text(
                      stats.gateOpen ? "SENDING AUDIO" : "GATE CLOSED",
                      style: TextStyle(color: gateColor, fontWeight: FontWeight.bold),
                    ),
                  ],
                ),
              ],
            ),
            const SizedBox(height: 12),
            LayoutBuilder(
              builder: (context, box) => SizedBox(
                height: 18,
                child: Stack(
                  children: [
                    Container(
                      decoration: BoxDecoration(
                        color: Colors.white.withOpacity(0.08),
                        borderRadius: BorderRadius.circular(9),
                      ),
                    ),
                    AnimatedContainer(
                      duration: const Duration(milliseconds: 100),
                      width: box.maxWidth * level,
                      decoration: BoxDecoration(
                        color: over ? Colors.cyanAccent : Colors.cyan.withOpacity(0.5),
                        borderRadius: BorderRadius.circular(9),
                      ),
                    ),
                    Positioned(
                      left: (box.maxWidth * thresholdPosition).clamp(0.0, box.maxWidth - 2),
                      top: 0,
                      bottom: 0,
                      child: Container(width: 2, color: Colors.amberAccent),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text("${stats.packets} packets", style: const TextStyle(color: Colors.white54)),
                if (running)
                  Text(
                    "${stats.power.battery}%${stats.power.charging ? ' ⚡' : ''}  ${stats.power.temperatureC}°C",
                    style: const TextStyle(color: Colors.white54),
                  ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
