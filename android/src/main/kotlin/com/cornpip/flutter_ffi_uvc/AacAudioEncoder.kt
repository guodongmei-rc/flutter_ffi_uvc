package com.cornpip.flutter_ffi_uvc

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * PCM → AAC encoder fed from the native UAC capture ([NativeAudio]).
 *
 * A feeder thread polls [NativeAudio.read] and queues 16-bit PCM into the
 * codec; a drain thread forwards the encoded output through the callbacks.
 * Presentation timestamps are a monotonic clock starting at [start], so the
 * first sample sits near zero like the video track's.
 */
internal class AacAudioEncoder(
    private val sampleRate: Int,
    private val channelCount: Int,
    /**
     * Fixed linear gain applied to the PCM before encoding, or 0 (the
     * default) for automatic gain control. Camera mics often run at a very
     * low level (observed peaks of ~100 on a 32767 full scale) and many
     * expose no UAC volume control to raise it at the source, so the AGC
     * amplifies active speech up to [AGC_MAX_GAIN] and relaxes toward
     * [AGC_MIN_GAIN] during silence to avoid amplifying pure noise floor.
     */
    private val pcmGain: Float = 0f,
) {
    companion object {
        private const val TAG = "flutter_ffi_uvc"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_AUDIO_AAC
        private const val PCM_CHUNK_BYTES = 4096
        private const val READ_TIMEOUT_MS = 200
        private const val QUEUE_TIMEOUT_US = 10_000L
        private const val JOIN_TIMEOUT_MS = 3_000L

        private const val AGC_TARGET_PEAK = 12000f  // ≈ -8.7 dBFS
        private const val AGC_MAX_GAIN = 32f
        private const val AGC_MIN_GAIN = 2f
        private const val AGC_NOISE_FLOOR = 350
        private const val AGC_RELEASE = 0.05f  // per ~21ms chunk toward target
        private const val AGC_SILENCE_DECAY = 0.995f  // per chunk while silent
    }

    /** Called on the drain thread when the codec output format is known. */
    var onFormatChanged: ((MediaFormat) -> Unit)? = null

    /**
     * Called on the drain thread for each encoded AAC sample. The buffer is
     * only valid until the callback returns.
     */
    var onEncodedSample: ((ByteBuffer, MediaCodec.BufferInfo) -> Unit)? = null

    private var codec: MediaCodec? = null
    private var feederThread: Thread? = null
    private var drainThread: Thread? = null
    @Volatile private var stopRequested = false
    private var startNanos = 0L

    /**
     * Absolute monotonic time (µs, same clock as System.nanoTime) when [start]
     * ran. Encoded sample PTS are relative to this base; muxers use it to
     * align the audio track with the video track's zero point.
     */
    var startTimeUs = 0L
        private set

    fun start() {
        val format = MediaFormat.createAudioFormat(MIME_TYPE, sampleRate, channelCount).apply {
            setInteger(
                MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC,
            )
            setInteger(MediaFormat.KEY_BIT_RATE, 96_000 * channelCount)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, PCM_CHUNK_BYTES)
        }
        val encoder = MediaCodec.createEncoderByType(MIME_TYPE)
        encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        encoder.start()
        codec = encoder
        startNanos = System.nanoTime()
        startTimeUs = startNanos / 1_000
        drainThread = Thread({ drainLoop(encoder) }, "uvc-audio-drain").also { it.start() }
        feederThread = Thread({ feedLoop(encoder) }, "uvc-audio-feed").also { it.start() }
    }

    /** Queues end-of-stream and waits for the encoder pipeline to drain. */
    fun stop() {
        stopRequested = true
        try { feederThread?.join(JOIN_TIMEOUT_MS) } catch (_: InterruptedException) {}
        try { drainThread?.join(JOIN_TIMEOUT_MS) } catch (_: InterruptedException) {}
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        feederThread = null
        drainThread = null
    }

    private fun nowUs(): Long = (System.nanoTime() - startNanos) / 1_000

    /** Applies the effective gain to little-endian 16-bit PCM in place, saturating. */
    private fun applyGain(pcm: ByteBuffer, bytes: Int) {
        pcm.order(ByteOrder.LITTLE_ENDIAN)
        val gain = if (pcmGain > 0f) pcmGain else updateAgcGain(pcm, bytes)
        var i = 0
        while (i + 1 < bytes) {
            val s = pcm.getShort(i)
            val amplified = (s * gain).toInt().coerceIn(-32768, 32767)
            pcm.putShort(i, amplified.toShort())
            i += 2
        }
    }

    // AGC state for the default (pcmGain == 0) mode.
    private var agcGain = 8f

    /**
     * Adapts [agcGain] from this chunk's peak: fast attack when speech would
     * clip, slow release upward, and a slow decay toward [AGC_MIN_GAIN] while
     * the input sits at the noise floor so silence stays quiet.
     */
    private fun updateAgcGain(pcm: ByteBuffer, bytes: Int): Float {
        var peak = 0
        var i = 0
        while (i + 1 < bytes) {
            val a = pcm.getShort(i).toInt().let { if (it < 0) -it else it }
            if (a > peak) peak = a
            i += 2
        }
        if (peak > AGC_NOISE_FLOOR) {
            val desired = (AGC_TARGET_PEAK / peak).coerceIn(AGC_MIN_GAIN, AGC_MAX_GAIN)
            agcGain = if (desired < agcGain) {
                desired // attack immediately: clipping sounds worse than a dip
            } else {
                agcGain + (desired - agcGain) * AGC_RELEASE
            }
        } else {
            agcGain = (agcGain * AGC_SILENCE_DECAY).coerceAtLeast(AGC_MIN_GAIN)
        }
        return agcGain
    }

    private fun feedLoop(encoder: MediaCodec) {
        val pcm = ByteBuffer.allocateDirect(PCM_CHUNK_BYTES)
        try {
            while (!stopRequested) {
                pcm.clear()
                val bytes = NativeAudio.read(pcm, READ_TIMEOUT_MS)
                if (bytes < 0) break // capture stopped: end of stream
                if (bytes == 0) continue // poll timeout
                val index = encoder.dequeueInputBuffer(QUEUE_TIMEOUT_US)
                if (index < 0) continue
                val input = encoder.getInputBuffer(index)
                if (input == null || input.remaining() < bytes) {
                    // Codec input buffers are at least KEY_MAX_INPUT_SIZE, so
                    // this only fires on a misbehaving codec; drop the chunk.
                    Log.w(TAG, "AAC input buffer too small, dropping $bytes PCM bytes")
                    encoder.queueInputBuffer(index, 0, 0, nowUs(), 0)
                    continue
                }
                applyGain(pcm, bytes)
                pcm.limit(bytes)
                pcm.position(0)
                input.clear()
                input.put(pcm)
                encoder.queueInputBuffer(index, 0, bytes, nowUs(), 0)
            }
        } catch (e: Exception) {
            Log.e(TAG, "AAC feed loop failed", e)
        } finally {
            try {
                val index = encoder.dequeueInputBuffer(JOIN_TIMEOUT_MS * 1_000L)
                if (index >= 0) {
                    encoder.queueInputBuffer(
                        index, 0, 0, nowUs(), MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                    )
                }
            } catch (e: Exception) {
                Log.w(TAG, "Failed to queue audio EOS", e)
            }
        }
    }

    private fun drainLoop(encoder: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (true) {
                val index = encoder.dequeueOutputBuffer(info, QUEUE_TIMEOUT_US)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        onFormatChanged?.invoke(encoder.outputFormat)
                    }
                    index >= 0 -> {
                        val buffer = encoder.getOutputBuffer(index)
                        if (buffer != null &&
                            info.size > 0 &&
                            (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                        ) {
                            onEncodedSample?.invoke(buffer, info)
                        }
                        encoder.releaseOutputBuffer(index, false)
                        if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            break
                        }
                    }
                    // INFO_TRY_AGAIN_LATER: keep polling; EOS arrives via the
                    // buffer flag after stop() signals end of input.
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "AAC drain loop failed", e)
        }
    }
}
