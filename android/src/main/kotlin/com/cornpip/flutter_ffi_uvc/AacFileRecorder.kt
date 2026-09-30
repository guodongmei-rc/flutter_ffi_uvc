package com.cornpip.flutter_ffi_uvc

import android.media.MediaCodec
import android.util.Log
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer

/**
 * Passthrough-path audio recorder: PCM from the native UAC capture is
 * encoded to AAC and appended to a temp file, which [RawVideoMuxer] later
 * remuxes alongside the video temp file (h26x_rawrec counterpart).
 *
 * File layout (little-endian):
 *   header: [8B magic "UVCAUD02"][4B sample_rate][1B channels][1B reserved]
 *           [2B asc_len][asc_len B AudioSpecificConfig]
 *           [8B base_us — absolute monotonic µs of encoder start]
 *   per sample: [8B pts_us — relative to base_us][4B payload_len][payload]
 */
internal class AacFileRecorder(
    file: File,
    private val encoder: AacAudioEncoder,
) {
    companion object {
        private const val TAG = "flutter_ffi_uvc"
        private const val MAGIC = "UVCAUD02"
    }

    private val sampleRate = encoder.sampleRate
    private val channelCount = encoder.channelCount
    private val out = BufferedOutputStream(FileOutputStream(file))
    private var sampleCount = 0L

    fun start() {
        encoder.onEncodedSample = { buffer, info -> onSample(buffer, info) }
        // The header carries the encoder's absolute start base, so start the
        // encoder first and write the header once the base is known.
        encoder.start()
        val asc = buildAudioSpecificConfig(sampleRate, channelCount)
        out.write(MAGIC.toByteArray(Charsets.US_ASCII))
        out.writeIntLE(sampleRate)
        out.write(channelCount and 0xff)
        out.write(0) // reserved
        out.writeShortLE(asc.size)
        out.write(asc)
        out.writeLongLE(encoder.startTimeUs)
    }

    /** Stops the encoder and closes the file. Returns the sample count. */
    fun stop(): Long {
        encoder.stop()
        out.flush()
        out.close()
        Log.i(TAG, "Audio temp recording stopped: $sampleCount samples")
        return sampleCount
    }

    private fun onSample(buffer: ByteBuffer, info: MediaCodec.BufferInfo) {
        val payload = ByteArray(info.size)
        buffer.position(info.offset)
        buffer.limit(info.offset + info.size)
        buffer.get(payload)
        out.writeLongLE(info.presentationTimeUs)
        out.writeIntLE(info.size)
        out.write(payload)
        sampleCount += 1
    }

    private fun BufferedOutputStream.writeShortLE(value: Int) {
        write(value and 0xff)
        write((value shr 8) and 0xff)
    }

    private fun BufferedOutputStream.writeIntLE(value: Int) {
        write(value and 0xff)
        write((value shr 8) and 0xff)
        write((value shr 16) and 0xff)
        write((value shr 24) and 0xff)
    }

    private fun BufferedOutputStream.writeLongLE(value: Long) {
        for (i in 0 until 8) {
            write((value shr (8 * i)).toInt() and 0xff)
        }
    }

    private fun buildAudioSpecificConfig(sampleRate: Int, channels: Int): ByteArray {
        val frequencyIndex = when (sampleRate) {
            96000 -> 0
            88200 -> 1
            64000 -> 2
            48000 -> 3
            44100 -> 4
            32000 -> 5
            24000 -> 6
            22050 -> 7
            16000 -> 8
            12000 -> 9
            11025 -> 10
            8000 -> 11
            else -> 4
        }
        val profile = 2 // AAC-LC
        val asc = (profile shl 11) or (frequencyIndex shl 7) or (channels shl 3)
        return byteArrayOf((asc shr 8).toByte(), asc.toByte())
    }
}
