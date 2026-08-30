package com.example.babyphone_app

import android.content.Context
import android.net.wifi.WifiManager
import androidx.annotation.NonNull
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.babyphone/multicast"
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            if (call.method == "acquireMulticastLock") {
                val success = acquireMulticastLock()
                if (success) {
                    result.success(null)
                } else {
                    result.error("UNAVAILABLE", "Failed to acquire multicast lock.", null)
                }
            } else {
                result.notImplemented()
            }
        }
    }

    private fun acquireMulticastLock(): Boolean {
        if (multicastLock == null) {
            val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            multicastLock = wifiManager.createMulticastLock("babyphoneMulticastLock")
            multicastLock?.setReferenceCounted(true)
            
            wifiLock = wifiManager.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "babyphoneWifiLock")
            wifiLock?.setReferenceCounted(true)
        }
        
        multicastLock?.let {
            if (!it.isHeld) {
                it.acquire()
            }
        }
        
        wifiLock?.let {
            if (!it.isHeld) {
                it.acquire()
            }
            return true
        }
        return false
    }

    override fun onDestroy() {
        super.onDestroy()
        multicastLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        wifiLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
    }
}
