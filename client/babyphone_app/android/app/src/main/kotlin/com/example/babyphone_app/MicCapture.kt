package com.example.babyphone_app

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import io.flutter.plugin.common.EventChannel

/**
 * Streams 20 ms frames (960 samples, s16le, 48 kHz mono) to Dart over an EventChannel.
 * Runs in the app process; the foreground service in microphone mode keeps it alive
 * and permitted while the screen is off.
 */
object MicCapture : EventChannel.StreamHandler {
    private const val TAG = "MicCapture"
    private const val SAMPLE_RATE = 48000
    private const val FRAME_BYTES = 960 * 2

    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile private var running = false
    private var thread: Thread? = null

    override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
        start(events)
    }

    override fun onCancel(arguments: Any?) {
        stop()
    }

    @SuppressLint("MissingPermission") // Dart requests RECORD_AUDIO before listening.
    private fun start(sink: EventChannel.EventSink) {
        stop()
        val minBuffer = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        if (minBuffer <= 0) {
            sink.error("UNSUPPORTED", "This phone can't record 48 kHz mono audio", null)
            return
        }

        // VOICE_RECOGNITION skips AGC and noise suppression, which would pump the
        // noise floor and could swallow quiet whimpers.
        val record = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_RECOGNITION,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                maxOf(minBuffer, FRAME_BYTES * 4)
            )
        } catch (e: Exception) {
            sink.error("MIC_UNAVAILABLE", e.message, null)
            return
        }
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            sink.error("MIC_UNAVAILABLE", "Microphone is in use or unavailable", null)
            return
        }

        running = true
        thread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            try {
                record.startRecording()
                while (running) {
                    val frame = ByteArray(FRAME_BYTES)
                    var filled = 0
                    while (filled < FRAME_BYTES && running) {
                        val n = record.read(frame, filled, FRAME_BYTES - filled)
                        if (n < 0) {
                            Log.e(TAG, "AudioRecord.read failed: $n")
                            running = false
                            mainHandler.post { sink.error("MIC_READ_FAILED", "Microphone stopped ($n)", null) }
                            break
                        }
                        filled += n
                    }
                    if (filled == FRAME_BYTES) {
                        mainHandler.post { if (running) sink.success(frame) }
                    }
                }
            } finally {
                try { record.stop() } catch (_: IllegalStateException) {}
                record.release()
            }
        }, "BabyphoneMic").apply { start() }
    }

    fun stop() {
        running = false
        thread?.join(500)
        thread = null
    }
}
