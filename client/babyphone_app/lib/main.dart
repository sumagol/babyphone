import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:math';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_pcm_sound/flutter_pcm_sound.dart';
import 'package:opus_codec_dart/opus_codec_dart.dart';
import 'package:opus_codec/opus_codec.dart' as opus_codec;
import 'package:encrypt/encrypt.dart' as encrypt;
import 'package:wakelock_plus/wakelock_plus.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  
  try {
    initOpus(await opus_codec.load());
    print("Opus initialized. Version: ${getOpusVersion()}");
  } catch (e) {
    print("Failed to load Opus: $e");
  }

  runApp(const BabyphoneApp());
}

class BabyphoneApp extends StatelessWidget {
  const BabyphoneApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Babyphone Receiver',
      theme: ThemeData(
        brightness: Brightness.dark,
        primarySwatch: Colors.cyan,
      ),
      home: const UdpDiagnosticScreen(),
    );
  }
}

class UdpDiagnosticScreen extends StatefulWidget {
  const UdpDiagnosticScreen({super.key});

  @override
  State<UdpDiagnosticScreen> createState() => _UdpDiagnosticScreenState();
}

class _UdpDiagnosticScreenState extends State<UdpDiagnosticScreen> {
  bool _isListening = false;
  String _statusMessage = "Ready.";
  int _packetCount = 0;
  int _lastPacketSize = 0;
  
  int _batteryLevel = 100;
  bool _isCharging = false;
  double _currentRms = 0.0;
  
  bool _alarmEnabled = true;
  double _beepPhase = 0.0;
  int _beepSampleCounter = 0;
  
  RawDatagramSocket? _socket;
  Timer? _uiTimer;
  Timer? _igmpRefreshTimer;
  SimpleOpusDecoder? _decoder;
  final String _multicastIp = '239.255.0.1';

  @override
  void initState() {
    super.initState();
    _acquireMulticastLock();
    _uiTimer = Timer.periodic(const Duration(milliseconds: 60), (timer) {
      if (mounted && _isListening) {
        setState(() {}); // trigger rebuild to show UI changes smoothly
      }
    });
  }

  Future<void> _acquireMulticastLock() async {
    const platform = MethodChannel('com.babyphone/multicast');
    try {
      await platform.invokeMethod('acquireMulticastLock');
      setState(() {
        _statusMessage = "Multicast Lock Acquired.";
      });
    } on PlatformException catch (e) {
      setState(() {
        _statusMessage = "Multicast Lock Error: ${e.message}";
      });
    }
  }

  Future<void> _joinMulticastGroup() async {
    if (_socket == null) return;
    final interfaces = await NetworkInterface.list(type: InternetAddressType.IPv4);
    bool joined = false;
    for (var interface in interfaces) {
      try {
        _socket!.joinMulticast(InternetAddress(_multicastIp), interface);
        joined = true;
      } catch (e) {}
    }
    if (!joined) {
      _socket!.joinMulticast(InternetAddress(_multicastIp));
    }
  }

  double _calculateRms(List<int> pcmData) {
    if (pcmData.isEmpty) return 0.0;
    double sum = 0;
    for (int sample in pcmData) {
      sum += (sample * sample);
    }
    return sqrt(sum / pcmData.length) / 32768.0;
  }

  Future<void> _startListening() async {
    setState(() {
      _statusMessage = "Initializing Audio Pipeline...";
    });

    try {
      WakelockPlus.enable();
      await FlutterPcmSound.setup(sampleRate: 48000, channelCount: 1);
      _decoder = SimpleOpusDecoder(sampleRate: 48000, channels: 1);

      _socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 5004, reuseAddress: true, reusePort: true);
      await _joinMulticastGroup();
      
      _igmpRefreshTimer = Timer.periodic(const Duration(seconds: 30), (timer) async {
        try { _socket?.leaveMulticast(InternetAddress(_multicastIp)); } catch (e) {}
        await _joinMulticastGroup();
      });
      
      setState(() {
        _isListening = true;
        _packetCount = 0;
        _statusMessage = "Listening & Decoding Audio...";
      });

      _socket!.listen((RawSocketEvent event) {
        print("Socket Event: $event");
        if (event == RawSocketEvent.read) {
          Datagram? d = _socket!.receive();
          print("Datagram received: ${d != null ? d.data.length : 'null'} bytes");
          if (d != null) {
            _packetCount++;
            _lastPacketSize = d.data.length;

            try {
              int payloadOffset = 12;
              int batteryPercent = _batteryLevel;
              bool isCharging = _isCharging;

              // Check if Extension (X) bit is set
              bool hasExtension = (d.data[0] & 0x10) != 0;
              print("First byte: ${d.data[0]}, hasExtension: $hasExtension");
              
              if (hasExtension && d.data.length >= 20) {
                  int extLen = (d.data[14] << 8) | d.data[15];
                  payloadOffset = 12 + 4 + (extLen * 4);
                  print("Extension found! extLen: $extLen, new payloadOffset: $payloadOffset");
                  
                  // Check for our custom "Baby" profile ID (0xBABB)
                  if (d.data[12] == 0xBA && d.data[13] == 0xBB) {
                      int telemetry = d.data[16];
                      isCharging = (telemetry & 0x80) != 0; // Bit 7
                      batteryPercent = telemetry & 0x7F;    // Bits 0-6
                      print("Parsed Telemetry -> Battery: $batteryPercent%, Charging: $isCharging");
                  } else {
                      print("Unknown Extension ID: ${d.data[12]} ${d.data[13]}");
                  }
              }

              if (d.data.length > payloadOffset) {
                var opusPayload = d.data.sublist(payloadOffset);
                try {
                  // --- AES-128-CTR DECRYPTION ---
                  // Reconstruct the 16-byte IV identically to the ESP32
                  final ivBytes = Uint8List(16);
                  // IV = SSRC (4) + Sequence (2) + Timestamp (4) + Padding (6)
                  ivBytes[0] = d.data[8];  ivBytes[1] = d.data[9];  ivBytes[2] = d.data[10]; ivBytes[3] = d.data[11];
                  ivBytes[4] = d.data[2];  ivBytes[5] = d.data[3];
                  ivBytes[6] = d.data[4];  ivBytes[7] = d.data[5];  ivBytes[8] = d.data[6];  ivBytes[9] = d.data[7];

                  final key = encrypt.Key.fromUtf8("BabyPhoneKey2026");
                  final iv = encrypt.IV(ivBytes);
                  final encrypter = encrypt.Encrypter(encrypt.AES(key, mode: encrypt.AESMode.ctr, padding: null));
                  
                  final decrypted = encrypter.decryptBytes(encrypt.Encrypted(Uint8List.fromList(opusPayload)), iv: iv);
                  
                  List<int> pcmData = _decoder!.decode(input: Uint8List.fromList(decrypted));
                  
                  // Acoustic warning for low battery
                  if (_alarmEnabled && batteryPercent < 10 && !isCharging) {
                    _beepSampleCounter += pcmData.length;
                    // Play a 0.5s beep every 3 seconds (48000 * 3 = 144000)
                    if ((_beepSampleCounter % 144000) < 24000) {
                      for (int i = 0; i < pcmData.length; i++) {
                        double sample = sin(_beepPhase) * 16000.0; // 440Hz sine wave, increased volume
                        _beepPhase += 2 * pi * 440.0 / 48000.0;
                        if (_beepPhase > 2 * pi) _beepPhase -= 2 * pi;
                        
                        int mixed = pcmData[i] + sample.toInt();
                        pcmData[i] = mixed.clamp(-32768, 32767);
                      }
                    }
                  } else {
                    _beepPhase = 0.0;
                    _beepSampleCounter = 0;
                  }
                  
                  FlutterPcmSound.feed(PcmArrayInt16.fromList(pcmData));
                  
                  _batteryLevel = batteryPercent;
                  _isCharging = isCharging;
                  _currentRms = _calculateRms(pcmData);
                } catch (e) {
                  print("Opus Decode Error: $e");
                  if (mounted) {
                    setState(() { _statusMessage = "Decode Error: $e"; });
                  }
                }
              }
            } catch (e) {
              print("Packet parse error: $e");
              if (mounted) {
                setState(() { _statusMessage = "Parse Error: $e"; });
              }
            }
          }
        }
      }, onError: (e) {
          print("Socket Error: $e");
          if (mounted) {
            setState(() { _statusMessage = "Socket Error: $e"; });
          }
      }, onDone: () {
          print("Socket Done");
          if (mounted) {
            setState(() { _statusMessage = "Socket Closed"; });
          }
      });
      
    } catch (e) {
      WakelockPlus.disable();
      setState(() {
        _statusMessage = "Start Error: $e";
      });
    }
  }

  void _stopListening() {
    WakelockPlus.disable();
    _igmpRefreshTimer?.cancel();
    _socket?.close();
    _socket = null;
    _decoder?.destroy();
    _decoder = null;
    
    setState(() {
      _isListening = false;
      _statusMessage = "Stopped.";
      _currentRms = 0.0;
    });
  }

  @override
  void dispose() {
    _uiTimer?.cancel();
    _stopListening();
    super.dispose();
  }

  Widget _buildBatteryIcon() {
    IconData icon;
    Color color;
    
    if (_isCharging) {
        icon = Icons.battery_charging_full;
        color = Colors.greenAccent;
    } else {
        if (_batteryLevel > 80) { icon = Icons.battery_full; color = Colors.green; }
        else if (_batteryLevel > 60) { icon = Icons.battery_6_bar; color = Colors.lightGreen; }
        else if (_batteryLevel > 40) { icon = Icons.battery_4_bar; color = Colors.orange; }
        else if (_batteryLevel > 20) { icon = Icons.battery_2_bar; color = Colors.deepOrange; }
        else { icon = Icons.battery_alert; color = Colors.red; }
    }
    
    return Row(
        mainAxisSize: MainAxisSize.min,
        children: [
            Icon(icon, color: color),
            const SizedBox(width: 4),
            Text("$_batteryLevel%", style: TextStyle(color: color, fontWeight: FontWeight.bold, fontSize: 16)),
        ]
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Babyphone Receiver'),
        actions: [
          if (_isListening)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16.0),
              child: _buildBatteryIcon(),
            )
        ],
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              _packetCount > 0 ? Icons.volume_up : Icons.child_care, 
              size: 100, 
              color: _currentRms > 0.01 ? Colors.cyanAccent : Colors.cyan
            ),
            const SizedBox(height: 30),
            Text(_statusMessage, textAlign: TextAlign.center, style: const TextStyle(fontSize: 16)),
            const SizedBox(height: 20),
            if (_isListening) ...[
              Text("Opus Packets Decoded: $_packetCount", style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: Colors.green)),
              const SizedBox(height: 20),
              const Text("Live Audio VU", style: TextStyle(fontSize: 14, color: Colors.grey)),
              const SizedBox(height: 8),
              Container(
                width: 250,
                height: 24,
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(12),
                  color: Colors.grey[800],
                  border: Border.all(color: Colors.grey[700]!),
                ),
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(12),
                  child: LinearProgressIndicator(
                    value: (_currentRms * 8).clamp(0.0, 1.0), // Amplify for visual effect
                    backgroundColor: Colors.transparent,
                    color: _currentRms > 0.08 ? Colors.redAccent : Colors.greenAccent,
                    minHeight: 24,
                  ),
                ),
              ),
            ],
            const SizedBox(height: 30),
            ElevatedButton.icon(
              icon: Icon(_isListening ? Icons.stop : Icons.play_arrow, size: 40),
              label: Text(_isListening ? "STOP" : "LISTEN"),
              style: ElevatedButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 40, vertical: 20),
                backgroundColor: _isListening ? Colors.red : Colors.green,
                foregroundColor: Colors.white,
              ),
              onPressed: _isListening ? _stopListening : _startListening,
            ),
            const SizedBox(height: 40),
            SizedBox(
              width: 300,
              child: SwitchListTile(
                title: const Text('Low Battery Alarm', style: TextStyle(fontSize: 14)),
                subtitle: const Text('Acoustic beep (< 10%)', style: TextStyle(fontSize: 12, color: Colors.grey)),
                value: _alarmEnabled,
                activeColor: Colors.cyanAccent,
                onChanged: (val) => setState(() => _alarmEnabled = val),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
