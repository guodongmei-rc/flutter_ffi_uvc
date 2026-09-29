package com.cornpip.flutter_ffi_uvc

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.util.Log
import android.view.Surface
import java.io.File
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Hardware H.264/H.265 recorder fed by the native layer.
 *
 * Two feed modes exist:
 *  - Surface mode (H.264/H.265 camera streams): [start] returns the encoder
 *    input [Surface]; the caller attaches it to the native frame pipeline,
 *    whose hardware decoder renders every frame into it.
 *  - Buffer mode ([useBufferInput], uncompressed/MJPEG camera streams): the
 *    encoder gets YUV byte buffers — a feed thread pulls the newest preview
 *    frame from the native recording queue (NativeRecording), which
 *    converts RGBA -> YUV straight into the dequeued input buffer. No GL
 *    and no input Surface on this path.
 *
 * Encoded output is muxed into an MP4 published to the device gallery
 * (MediaStore). [mimeType] selects the encoder
 * ([MediaFormat.MIMETYPE_VIDEO_AVC] or [MediaFormat.MIMETYPE_VIDEO_HEVC]);
 * the caller probes availability first.
 *
 * When [audioEncoder] is given, an AAC audio track (PCM from the camera's
 * UAC interface, see AacAudioEncoder) is muxed alongside; the muxer then
 * starts only after both tracks reported their output format. Audio startup
 * failures degrade to a video-only recording — audio never breaks a
 * recording.
 */
internal class VideoRecorder(
    private val context: Context,
    private val width: Int,
    private val height: Int,
    bitRate: Int?,
    private val frameRate: Int,
    private val audioEncoder: AacAudioEncoder? = null,
    private val mimeType: String = MediaFormat.MIMETYPE_VIDEO_AVC,
    private val useBufferInput: Boolean = false,
) {
    companion object {
        private const val TAG = "flutter_ffi_uvc"
        private const val DRAIN_TIMEOUT_US = 10_000L
        private const val STOP_JOIN_TIMEOUT_MS = 3_000L
        private const val FEED_READ_TIMEOUT_MS = 100
    }

    /** Encoder input layouts tried in order for buffer mode. */
    private enum class BufferInputFormat(val colorFormat: Int, val queueFormat: Int) {
        NV12(
            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar,
            NativeRecording.YUV_FORMAT_NV12,
        ),
        I420(
            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar,
            NativeRecording.YUV_FORMAT_I420,
        ),
    }

    /** Success carries the gallery URI (API 29+) or file path; both when available. */
    interface StopCallback {
        fun onComplete(uri: String?, path: String?)
        fun onError(message: String)
    }

    private val bitRate: Int =
        bitRate ?: (width * height * frameRate / 8).coerceAtLeast(1_000_000)

    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var muxer: MediaMuxer? = null
    private var muxerStarted = false
    private var trackIndex = -1
    private var sampleCount = 0L
    private var drainThread: Thread? = null
    @Volatile private var stopRequested = false

    // Buffer-input (YUV byte buffer) feed state.
    private var feedThread: Thread? = null
    private var bufferInputFormat: BufferInputFormat? = null
    private var lastQueuedPtsUs = 0L
    private var feedSkips = 0

    // Audio track state. muxerLock serializes the video drain thread and the
    // audio drain callback around MediaMuxer (not thread-safe).
    private val muxerLock = Any()
    private var audioStarted = false
    private var tracksExpected = 1
    private var tracksAdded = 0
    private var audioTrackIndex = -1
    private var audioSampleCount = 0L

    // PTS normalization: video samples carry absolute monotonic µs (Surface
    // input timestamps) and audio PTS are relative to the audio encoder's
    // start. Both are shifted so the recording starts at 0.
    private var ptsBaseUs = 0L

    private var contentUri: Uri? = null
    private var pfd: ParcelFileDescriptor? = null
    private var outputFile: File? = null

    /**
     * Configures encoder and muxer. Surface mode returns the encoder input
     * surface for the caller to attach; buffer mode returns null and starts
     * the YUV feed thread instead.
     */
    fun start(): Surface? {
        require(width > 0 && height > 0) { "Invalid recording size ${width}x$height" }
        require(width % 2 == 0 && height % 2 == 0) {
            "Video encoding requires even dimensions, got ${width}x$height"
        }

        try {
            openMuxerTarget()

            ptsBaseUs = System.nanoTime() / 1_000
            audioStarted = startAudioEncoder()
            tracksExpected = if (audioStarted) 2 else 1

            val encoder = MediaCodec.createEncoderByType(mimeType)
            if (useBufferInput) {
                configureBufferInput(encoder)
                encoder.start()
                codec = encoder

                drainThread = Thread({ drainLoop() }, "uvc-video-recorder").also { it.start() }
                feedThread = Thread({ feedLoop() }, "uvc-video-feed").also { it.start() }
                return null
            }

            val format = MediaFormat.createVideoFormat(mimeType, width, height).apply {
                setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface,
                )
                setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
                setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = encoder.createInputSurface()
            encoder.start()
            codec = encoder
            inputSurface = surface

            drainThread = Thread({ drainLoop() }, "uvc-video-recorder").also { it.start() }
            return surface
        } catch (e: Exception) {
            stopAudio()
            if (useBufferInput) {
                // Idempotent and harmless when the queue never started;
                // without this a failed start would leave it active and the
                // next startQueue would report BUSY.
                NativeRecording.stopQueue()
            }
            releaseResources()
            discardOutput()
            throw e
        }
    }

    /**
     * Buffer mode: configures the encoder for YUV byte-buffer input (NV12
     * preferred, I420 fallback) and starts the native recording queue with
     * the codec's actual input layout.
     */
    private fun configureBufferInput(encoder: MediaCodec) {
        var lastError: Exception? = null
        for (candidate in BufferInputFormat.entries) {
            try {
                val format = MediaFormat.createVideoFormat(mimeType, width, height).apply {
                    setInteger(MediaFormat.KEY_COLOR_FORMAT, candidate.colorFormat)
                    setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
                    setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                }
                encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                bufferInputFormat = candidate
                break
            } catch (e: Exception) {
                Log.w(TAG, "Encoder rejected ${candidate.name} byte-buffer input", e)
                lastError = e
                try { encoder.reset() } catch (_: Exception) {}
            }
        }
        val input = bufferInputFormat
            ?: throw lastError ?: IllegalStateException("No usable YUV420 input format")

        // The codec's real input layout (stride/slice-height) can differ
        // from the frame size; absent keys mean tightly packed.
        val inputFormat = encoder.inputFormat
        val stride = runCatching {
            inputFormat.getInteger(MediaFormat.KEY_STRIDE)
        }.getOrDefault(width)
        val sliceHeight = runCatching {
            inputFormat.getInteger("slice-height")
        }.getOrDefault(height)

        val rc = NativeRecording.startQueue(input.queueFormat, stride, sliceHeight)
        if (rc != 0) {
            throw IllegalStateException("NativeRecording.startQueue failed with code $rc")
        }
        Log.i(
            TAG,
            "@@@@UVC_REC/I buffer input: ${input.name} ${width}x$height " +
                "stride=$stride sliceHeight=$sliceHeight",
        )
    }

    /**
     * Finishes the recording. In surface mode it must be called after the
     * surface has been detached from the native pipeline so no frame
     * arrives past EOS; in buffer mode the feed thread queues EOS itself.
     */
    fun stop(callback: StopCallback) {
        val encoder = codec
        if (encoder == null) {
            callback.onError("Recorder is not running")
            return
        }
        Thread({
            try {
                stopRequested = true
                // Stop audio first: ending the native capture makes the audio
                // feeder queue AAC end-of-stream, so the audio track is fully
                // drained before the video EOS.
                stopAudio()
                if (useBufferInput) {
                    // Wakes a feed thread blocked in the native read; it then
                    // queues the video EOS and exits.
                    NativeRecording.stopQueue()
                    feedThread?.join(STOP_JOIN_TIMEOUT_MS)
                } else {
                    try {
                        encoder.signalEndOfInputStream()
                    } catch (e: IllegalStateException) {
                        Log.w(TAG, "signalEndOfInputStream failed", e)
                    }
                }
                drainThread?.join(STOP_JOIN_TIMEOUT_MS)

                // releaseResources() zeroes sampleCount if MediaMuxer.stop()
                // fails, so read the counters only after it runs.
                releaseResources()

                if (!muxerStarted || sampleCount == 0L) {
                    discardOutput()
                    callback.onError("No frames were recorded")
                    return@Thread
                }

                publishOutput()
                callback.onComplete(contentUri?.toString(), outputFile?.absolutePath)
            } catch (e: Exception) {
                Log.e(TAG, "stopRecording failed", e)
                releaseResources()
                discardOutput()
                callback.onError(e.message ?: "Failed to finish recording")
            }
        }, "uvc-video-recorder-stop").start()
    }

    /** Aborts a recording that failed to start; discards any partial output. */
    fun abort() {
        stopRequested = true
        stopAudio()
        if (useBufferInput) {
            NativeRecording.stopQueue()
            feedThread?.join(STOP_JOIN_TIMEOUT_MS)
        } else {
            try { codec?.signalEndOfInputStream() } catch (_: Exception) {}
        }
        drainThread?.join(STOP_JOIN_TIMEOUT_MS)
        releaseResources()
        discardOutput()
    }

    /** Starts the AAC encoder; failures degrade to a video-only recording. */
    private fun startAudioEncoder(): Boolean {
        val audio = audioEncoder ?: return false
        audio.onFormatChanged = { format -> onAudioFormatChanged(format) }
        audio.onEncodedSample = { buffer, info -> onAudioSample(buffer, info) }
        return try {
            audio.start()
            true
        } catch (e: Exception) {
            Log.w(TAG, "Audio encoder failed to start; recording video-only", e)
            stopAudioCapture()
            false
        }
    }

    /** Stops the audio encoder and the native UAC capture. Idempotent. */
    private fun stopAudio() {
        if (!audioStarted) return
        audioStarted = false
        stopAudioCapture()
        audioEncoder?.stop()
    }

    private fun stopAudioCapture() {
        try {
            NativeAudio.stop()
        } catch (e: Exception) {
            Log.w(TAG, "Audio capture stop failed", e)
        }
    }

    private fun onAudioFormatChanged(format: MediaFormat) {
        synchronized(muxerLock) {
            val m = muxer ?: return
            audioTrackIndex = m.addTrack(format)
            tracksAdded += 1
            if (tracksAdded == tracksExpected) {
                m.start()
                muxerStarted = true
            }
        }
    }

    private fun onAudioSample(buffer: ByteBuffer, info: MediaCodec.BufferInfo) {
        synchronized(muxerLock) {
            if (muxerStarted && audioTrackIndex >= 0) {
                // Shift the audio sample (absolute base + relative pts) onto
                // the same zero as the video track.
                info.presentationTimeUs = (
                    (audioEncoder?.startTimeUs ?: ptsBaseUs) +
                        info.presentationTimeUs - ptsBaseUs
                    ).coerceAtLeast(0L)
                muxer?.writeSampleData(audioTrackIndex, buffer, info)
                audioSampleCount += 1
            }
        }
    }

    /**
     * Buffer mode feed loop: dequeues encoder input buffers and has the
     * native recording queue convert the newest preview frame straight into
     * them. A read timeout (or a frame the native side dropped) returns the
     * buffer empty and polls again; the native queue being stopped means no
     * more frames will come and ends the stream.
     */
    private fun feedLoop() {
        val encoder = codec ?: return
        val ptsOut = LongArray(1)
        try {
            while (true) {
                val index = encoder.dequeueInputBuffer(DRAIN_TIMEOUT_US)
                if (index < 0) continue
                if (stopRequested) {
                    encoder.queueInputBuffer(
                        index, 0, 0, lastQueuedPtsUs,
                        MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                    )
                    break
                }
                val buffer = encoder.getInputBuffer(index)
                if (buffer == null) {
                    encoder.queueInputBuffer(index, 0, 0, 0L, 0)
                    continue
                }
                val bytes = NativeRecording.readFrameYuv(buffer, ptsOut, FEED_READ_TIMEOUT_MS)
                when {
                    bytes > 0 -> {
                        encoder.queueInputBuffer(index, 0, bytes, ptsOut[0], 0)
                        lastQueuedPtsUs = ptsOut[0]
                    }
                    bytes == 0 -> {
                        feedSkips += 1
                        encoder.queueInputBuffer(index, 0, 0, 0L, 0)
                    }
                    else -> {
                        // Native queue stopped: queue the video EOS and exit.
                        encoder.queueInputBuffer(
                            index, 0, 0, lastQueuedPtsUs,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                        )
                        break
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Recorder feed loop failed", e)
        }
        if (feedSkips > 0) {
            Log.i(TAG, "@@@@UVC_REC/I feed loop ended, $feedSkips empty/skipped polls")
        }
    }

    private fun drainLoop() {
        val encoder = codec ?: return
        val info = MediaCodec.BufferInfo()
        try {
            while (true) {
                val index = encoder.dequeueOutputBuffer(info, DRAIN_TIMEOUT_US)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val m = muxer ?: break
                        synchronized(muxerLock) {
                            trackIndex = m.addTrack(encoder.outputFormat)
                            tracksAdded += 1
                            // With an audio track, start only after every
                            // track reported its format; video-only keeps the
                            // original single-track behavior.
                            if (tracksAdded == tracksExpected) {
                                m.start()
                                muxerStarted = true
                            }
                        }
                    }
                    index >= 0 -> {
                        val buffer = encoder.getOutputBuffer(index)
                        if (buffer != null &&
                            info.size > 0 &&
                            (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                        ) {
                            synchronized(muxerLock) {
                                if (muxerStarted) {
                                    // Surface input timestamps are absolute
                                    // monotonic µs; rebase to the recording
                                    // start so the track begins at 0.
                                    info.presentationTimeUs =
                                        (info.presentationTimeUs - ptsBaseUs).coerceAtLeast(0L)
                                    muxer?.writeSampleData(trackIndex, buffer, info)
                                    sampleCount += 1
                                }
                            }
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
            Log.e(TAG, "Recorder drain loop failed", e)
        }
    }

    private fun openMuxerTarget() {
        val name = "UVC_" +
            SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date()) + ".mp4"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, name)
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_DCIM)
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
            val resolver = context.contentResolver
            val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("Failed to create MediaStore video entry")
            contentUri = uri
            val fd = resolver.openFileDescriptor(uri, "rw")
                ?: throw IllegalStateException("Failed to open MediaStore video for writing")
            pfd = fd
            muxer = MediaMuxer(fd.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        } else {
            @Suppress("DEPRECATION")
            val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM)
            if (!dir.exists() && !dir.mkdirs()) {
                throw IllegalStateException("Cannot create output directory ${dir.absolutePath}")
            }
            val file = File(dir, name)
            outputFile = file
            muxer = MediaMuxer(file.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        }
    }

    private fun publishOutput() {
        val uri = contentUri
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && uri != null) {
            val values = ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }
            context.contentResolver.update(uri, values, null, null)
        } else {
            outputFile?.let {
                MediaScannerConnection.scanFile(context, arrayOf(it.absolutePath), null, null)
            }
        }
    }

    private fun discardOutput() {
        try {
            contentUri?.let { context.contentResolver.delete(it, null, null) }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to delete pending video entry", e)
        }
        contentUri = null
        outputFile?.let { if (it.exists()) it.delete() }
        outputFile = null
    }

    private fun releaseResources() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        inputSurface?.release()
        inputSurface = null
        if (muxerStarted) {
            try {
                muxer?.stop()
            } catch (e: Exception) {
                Log.w(TAG, "MediaMuxer stop failed", e)
                sampleCount = 0
            }
        }
        try { muxer?.release() } catch (_: Exception) {}
        muxer = null
        try { pfd?.close() } catch (_: Exception) {}
        pfd = null
        drainThread = null
        feedThread = null
    }
}
