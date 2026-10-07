package com.example.babyphone_app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.annotation.NonNull
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.babyphone/multicast"

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "startForegroundService", "acquireMulticastLock" -> {
                    startStreamingService()
                    result.success(null)
                }
                "stopForegroundService", "releaseMulticastLock" -> {
                    stopStreamingService()
                    result.success(null)
                }
                else -> {
                    result.notImplemented()
                }
            }
        }

        // Request POST_NOTIFICATIONS permission on Android 13+ (API 33+) if not yet granted
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 101)
            }
        }
    }

    private fun startStreamingService() {
        val serviceIntent = Intent(this, BabyphoneForegroundService::class.java).apply {
            action = BabyphoneForegroundService.ACTION_START
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
        stopStreamingService()
    }
}
