package com.example.babyphone_app

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.annotation.NonNull
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.babyphone/multicast"
    private val MIC_CHANNEL = "com.babyphone/mic"
    private val MIC_PERMISSION_REQUEST = 102

    private var pendingMicResult: MethodChannel.Result? = null

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "startForegroundService", "acquireMulticastLock" -> {
                    startStreamingService(call.argument<String>("mode") ?: BabyphoneForegroundService.MODE_RECEIVER)
                    result.success(null)
                }
                "stopForegroundService", "releaseMulticastLock" -> {
                    stopStreamingService()
                    result.success(null)
                }
                "requestMicPermission" -> requestMicPermission(result)
                "getPowerStatus" -> result.success(powerStatus())
                "requestBatteryExemption" -> {
                    requestBatteryExemption()
                    result.success(null)
                }
                else -> {
                    result.notImplemented()
                }
            }
        }

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, MIC_CHANNEL).setStreamHandler(MicCapture)

        // Request POST_NOTIFICATIONS permission on Android 13+ (API 33+) if not yet granted
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 101)
            }
        }
    }

    private fun requestMicPermission(result: MethodChannel.Result) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M ||
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            result.success(true)
            return
        }
        pendingMicResult?.success(false)
        pendingMicResult = result
        requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MIC_PERMISSION_REQUEST)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == MIC_PERMISSION_REQUEST) {
            pendingMicResult?.success(grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED)
            pendingMicResult = null
        }
    }

    /** Battery level, plugged-in state and battery temperature for the RTP telemetry bytes. */
    private fun powerStatus(): Map<String, Any> {
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        val tenthsC = battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
        return mapOf(
            "level" to if (level >= 0 && scale > 0) level * 100 / scale else 100,
            "charging" to (plugged != 0),
            "temperatureC" to (tenthsC + 5) / 10
        )
    }

    /** Asks once to exempt the app from Doze so the sender isn't throttled overnight. */
    @SuppressLint("BatteryLife") // Sideloaded app, not subject to Play's policy on this intent.
    private fun requestBatteryExemption() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        val power = getSystemService(PowerManager::class.java) ?: return
        if (power.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (_: Exception) {
            // Some OEM builds lack this screen; the user can still set "Unrestricted" manually.
        }
    }

    private fun startStreamingService(mode: String) {
        val serviceIntent = Intent(this, BabyphoneForegroundService::class.java).apply {
            action = BabyphoneForegroundService.ACTION_START
            putExtra(BabyphoneForegroundService.EXTRA_MODE, mode)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent)
        } else {
            startService(serviceIntent)
        }
    }

    private fun stopStreamingService() {
        val serviceIntent = Intent(this, BabyphoneForegroundService::class.java).apply {
            action = BabyphoneForegroundService.ACTION_STOP
        }
        startService(serviceIntent)
    }

    override fun onDestroy() {
        super.onDestroy()
        MicCapture.stop()
        stopStreamingService()
    }
}
