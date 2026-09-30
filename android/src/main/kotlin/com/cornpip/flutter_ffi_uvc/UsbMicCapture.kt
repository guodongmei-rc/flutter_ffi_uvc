package com.cornpip.flutter_ffi_uvc

import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import java.nio.ByteBuffer

/**
 * USB microphone capture through the Android audio framework. The kernel's
 * USB audio driver handles the isochronous streaming itself, which is the
 * standard path camera apps use — and on some phones the ONLY one that
 * delivers the camera mic at full rate (userspace usbfs isoc reads can be
 * starved by the host scheduler; observed a steady 1/16 of the nominal
 * rate on a Huawei nova 12 while the kernel path delivers full rate).
 *
 * Wraps an [AudioRecord] pinned to the connected USB audio input device.
 */
internal class UsbMicCapture(
    private val audioRecord: AudioRecord,
) {
    val sampleRate: Int get() = audioRecord.sampleRate
    val channelCount: Int get() = audioRecord.channelCount

    fun start() = audioRecord.startRecording()

    /**
     * Reads 16-bit PCM into [buffer] (must be direct). Blocks until data is
     * available. Returns the byte count, 0 when the recording was stopped,
     * or negative on error.
     */
    fun read(buffer: ByteBuffer): Int =
        audioRecord.read(buffer, buffer.remaining(), AudioRecord.READ_BLOCKING)

    /** Stops the recording; a feed thread blocked in [read] wakes up. */
    fun stopCapture() {
        try {
            audioRecord.stop()
        } catch (e: Exception) {
            Log.w(TAG, "AudioRecord stop failed", e)
        }
    }

    fun release() {
        try {
            audioRecord.release()
        } catch (_: Exception) {}
    }

    companion object {
        private const val TAG = "flutter_ffi_uvc"

        // 48 kHz first: USB audio devices almost always support it, and the
        // fewer resample hops AudioFlinger inserts, the better.
        private val CANDIDATE_RATES = intArrayOf(48000, 44100, 32000, 24000, 16000)

        /** First connected USB audio input device, or null. */
        fun findUsbInput(audioManager: AudioManager): AudioDeviceInfo? =
            audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull {
                it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                    it.type == AudioDeviceInfo.TYPE_USB_HEADSET
            }

        /**
         * Opens an AudioRecord pinned to [usbDevice] at the first sample rate
         * the device accepts, or null when no rate works.
         */
        fun open(usbDevice: AudioDeviceInfo): UsbMicCapture? {
            for (rate in CANDIDATE_RATES) {
                val minBuffer = AudioRecord.getMinBufferSize(
                    rate,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                )
                if (minBuffer <= 0) continue
                val record = try {
                    AudioRecord.Builder()
                        .setAudioSource(MediaRecorder.AudioSource.MIC)
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setSampleRate(rate)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                                .build(),
                        )
                        .setBufferSizeInBytes(maxOf(minBuffer, 8192))
                        .build()
                } catch (e: Exception) {
                    Log.w(TAG, "AudioRecord creation failed at $rate Hz", e)
                    null
                } ?: continue
                if (record.state != AudioRecord.STATE_INITIALIZED) {
                    record.release()
                    continue
                }
                if (!record.setPreferredDevice(usbDevice)) {
                    Log.w(TAG, "AudioRecord rejected the USB input at $rate Hz")
                    record.release()
                    continue
                }
                Log.i(
                    TAG,
                    "USB mic via AudioRecord: ${record.sampleRate} Hz " +
                        "ch=${record.channelCount} device=${usbDevice.productName}",
                )
                return UsbMicCapture(record)
            }
            return null
        }
    }
}
