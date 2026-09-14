package com.cornpip.flutter_ffi_uvc

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaFormat
import android.media.MediaMuxer
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.util.Log
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Remuxes a passthrough recording (interleaved temp file written by the
 * native layer, see h26x_rawrec.c) into an MP4 published to the gallery.
 * Samples are the camera's own H.264/H.265 access units — no re-encoding.
 */
internal object RawVideoMuxer {
    private const val TAG = "flutter_ffi_uvc"
    private const val MAGIC = "UVCRAW01"
    private const val AUDIO_MAGIC = "UVCAUD02"
    private const val CODEC_H264 = 0
    private const val CODEC_H265 = 1
    private const val FLAG_KEYFRAME = 0x01

    class Result(val uri: String?, val path: String?)

    private class AudioSample(val ptsUs: Long, val payload: ByteArray)

    private class AudioHeader(
        val file: RandomAccessFile,
        val sampleRate: Int,
        val channels: Int,
        val asc: ByteArray,
        // Absolute monotonic µs when the audio encoder started; sample PTS in
        // the file are relative to this base.
        val baseUs: Long,
    )

    /**
     * Opens an AacFileRecorder temp file and validates its header. Returns
     * null (and logs) on any inconsistency so a broken audio side never
     * fails the video mux.
     */
    private fun readAudioHeader(input: File): AudioHeader? = try {
        val raf = RandomAccessFile(input, "r")
        val magic = ByteArray(8)
        raf.readFully(magic)
        val sampleRate = raf.readIntLE() ?: -1
        val channels = raf.read()
        raf.read() // reserved
        val ascLen = raf.readUnsignedShortLE()
        val asc = ByteArray(ascLen.coerceAtLeast(0))
        raf.readFully(asc)
        val baseUs = raf.readLongLE() ?: -1L
        if (String(magic, Charsets.US_ASCII) != AUDIO_MAGIC ||
            sampleRate <= 0 || channels <= 0 || ascLen <= 0 || baseUs < 0
        ) {
            Log.w(TAG, "Bad audio recording header; muxing video-only")
            raf.close()
            null
        } else {
            AudioHeader(raf, sampleRate, channels, asc, baseUs)
        }
    } catch (e: Exception) {
        Log.w(TAG, "Failed to read audio temp file; muxing video-only", e)
        null
    }

    /** Reads one AAC sample record, or null at end of file. */
    private fun readAudioSample(header: AudioHeader?): AudioSample? {
        if (header == null) return null
        val ptsUs = header.file.readLongLE() ?: return null
        val payloadLen = header.file.readIntLE() ?: return null
        if (payloadLen <= 0 || payloadLen > 1024 * 1024) return null
        val payload = ByteArray(payloadLen)
        header.file.readFully(payload)
        return AudioSample(ptsUs, payload)
    }

    /**
     * Audio sample PTS on the normalized video timeline: absolute monotonic
     * (base + relative) minus the first video frame's PTS, clamped to 0 for
     * audio that predates the first frame.
     */
    private fun audioPtsUs(header: AudioHeader?, sample: AudioSample, videoBaseUs: Long): Long {
        val base = header?.baseUs ?: 0L
        return (base + sample.ptsUs - videoBaseUs).coerceAtLeast(0L)
    }

    /**
     * Muxes [input] into the gallery and deletes the temp file on success.
     * When [audioInput] (an AacFileRecorder temp file) is given, an AAC audio
     * track is interleaved by presentation time.
     */
    fun mux(context: Context, input: File, audioInput: File? = null): Result {
        var contentUri: Uri? = null
        var outputFile: File? = null
        try {
            RandomAccessFile(input, "r").use { raf ->
                // ── Header ───────────────────────────────────────────────
                val magic = ByteArray(8)
                raf.readFully(magic)
                require(String(magic, Charsets.US_ASCII) == MAGIC) { "Bad raw recording magic" }
                val codec = raf.read()
                val csdLen = raf.readUnsignedShortLE()
                val width = raf.readUnsignedShortLE()
                val height = raf.readUnsignedShortLE()
                raf.read() // reserved
                val csd = ByteArray(csdLen)
                raf.readFully(csd)
                require(width > 0 && height > 0 && csdLen > 0) { "Bad raw recording header" }

                // ── Optional audio temp file (AAC samples + header) ──────
                // A broken audio side never fails the mux: log and continue
                // video-only.
                val audioHeader = audioInput?.let { readAudioHeader(it) }
                var audioTrackIndex = -1
                var audioSampleCount = 0L

                val mime = when (codec) {
                    CODEC_H264 -> MediaFormat.MIMETYPE_VIDEO_AVC
                    CODEC_H265 -> MediaFormat.MIMETYPE_VIDEO_HEVC
                    else -> throw IllegalArgumentException("Unknown codec id $codec")
                }

                // ── Output target ────────────────────────────────────────
                val name = "UVC_" +
                    SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date()) + ".mp4"
                var pfd: ParcelFileDescriptor? = null
                val muxer: MediaMuxer
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    val values = ContentValues().apply {
                        put(MediaStore.Video.Media.DISPLAY_NAME, name)
                        put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                        put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_DCIM)
                        put(MediaStore.Video.Media.IS_PENDING, 1)
                    }
                    val resolver = context.contentResolver
                    contentUri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                        ?: throw IllegalStateException("Failed to create MediaStore video entry")
                    pfd = resolver.openFileDescriptor(contentUri, "rw")
                        ?: throw IllegalStateException("Failed to open MediaStore video for writing")
                    muxer = MediaMuxer(pfd.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
                } else {
                    @Suppress("DEPRECATION")
                    val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM)
                    if (!dir.exists() && !dir.mkdirs()) {
                        throw IllegalStateException("Cannot create output directory ${dir.absolutePath}")
                    }
                    outputFile = File(dir, name)
                    muxer = MediaMuxer(outputFile.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
                }

                var sampleCount = 0L
                try {
                    val format = MediaFormat.createVideoFormat(mime, width, height).apply {
                        // Android's MPEG4Writer convention: for AVC the SPS
                        // lives in csd-0 and the PPS in csd-1; feeding them
                        // concatenated in csd-0 fails with "Missing codec
                        // specific data". HEVC keeps all parameter sets in
                        // csd-0.
                        if (codec == CODEC_H264) {
                            val nals = splitAnnexB(csd)
                            require(nals.size >= 2) { "Missing SPS/PPS in CSD (${nals.size} nals)" }
                            setByteBuffer("csd-0", ByteBuffer.wrap(nals[0]))
                            setByteBuffer("csd-1", ByteBuffer.wrap(nals[1]))
                        } else {
                            setByteBuffer("csd-0", ByteBuffer.wrap(csd))
                        }
                    }
                    val trackIndex = muxer.addTrack(format)
                    if (audioHeader != null) {
                        val audioFormat = MediaFormat.createAudioFormat(
                            MediaFormat.MIMETYPE_AUDIO_AAC,
                            audioHeader.sampleRate,
                            audioHeader.channels,
                        ).apply {
                            // AudioSpecificConfig goes into csd-0.
                            setByteBuffer("csd-0", ByteBuffer.wrap(audioHeader.asc))
                        }
                        audioTrackIndex = muxer.addTrack(audioFormat)
                    }
                    muxer.start()

                    val info = MediaCodec.BufferInfo()
                    var nextAudio = readAudioSample(audioHeader)
                    // Video PTS in the temp file are absolute monotonic µs;
                    // normalize the track so the first frame sits at 0, and
                    // shift audio (base_us + relative pts) onto the same zero
                    // so the two tracks play in sync.
                    var videoBaseUs = -1L
                    while (true) {
                        val ptsUs = raf.readLongLE() ?: break
                        val payloadLen = raf.readIntLE() ?: break
                        val flags = raf.read()
                        if (payloadLen <= 0 || payloadLen > 64 * 1024 * 1024) break
                        val payload = ByteArray(payloadLen)
                        raf.readFully(payload)
                        if (videoBaseUs < 0) videoBaseUs = ptsUs
                        // Flush every audio sample up to this video PTS first
                        // so the two tracks stay interleaved in time.
                        while (nextAudio != null &&
                            audioPtsUs(audioHeader, nextAudio!!, videoBaseUs) <= ptsUs - videoBaseUs
                        ) {
                            info.set(
                                0,
                                nextAudio!!.payload.size,
                                audioPtsUs(audioHeader, nextAudio!!, videoBaseUs),
                                0,
                            )
                            muxer.writeSampleData(
                                audioTrackIndex,
                                ByteBuffer.wrap(nextAudio!!.payload),
                                info,
                            )
                            audioSampleCount += 1
                            nextAudio = readAudioSample(audioHeader)
                        }
                        info.set(
                            0,
                            payloadLen,
                            ptsUs - videoBaseUs,
                            if (flags and FLAG_KEYFRAME != 0) {
                                MediaCodec.BUFFER_FLAG_KEY_FRAME
                            } else {
                                0
                            },
                        )
                        muxer.writeSampleData(trackIndex, ByteBuffer.wrap(payload), info)
                        sampleCount += 1
                    }
                    // Audio tail past the last video frame.
                    while (nextAudio != null) {
                        info.set(
                            0,
                            nextAudio!!.payload.size,
                            audioPtsUs(audioHeader, nextAudio!!, videoBaseUs),
                            0,
                        )
                        muxer.writeSampleData(
                            audioTrackIndex,
                            ByteBuffer.wrap(nextAudio!!.payload),
                            info,
                        )
                        audioSampleCount += 1
                        nextAudio = readAudioSample(audioHeader)
                    }
                    require(sampleCount > 0) { "No access units in raw recording" }

                    muxer.stop()
                } finally {
                    try { audioHeader?.file?.close() } catch (_: Exception) {}
                    try { muxer.release() } catch (_: Exception) {}
                    try { pfd?.close() } catch (_: Exception) {}
                }

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && contentUri != null) {
                    val values = ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }
                    context.contentResolver.update(contentUri, values, null, null)
                } else {
                    outputFile?.let {
                        MediaScannerConnection.scanFile(context, arrayOf(it.absolutePath), null, null)
                    }
                }
                Log.i(
                    TAG,
                    "Raw recording muxed: $sampleCount video + $audioSampleCount audio " +
                        "samples -> ${contentUri ?: outputFile}",
                )
                return Result(contentUri?.toString(), outputFile?.absolutePath)
            }
        } catch (e: Exception) {
            // A leftover IS_PENDING entry is invisible in the gallery
            // forever — discard it on any failure.
            try { contentUri?.let { context.contentResolver.delete(it, null, null) } } catch (_: Exception) {}
            outputFile?.let { if (it.exists()) it.delete() }
            throw e
        }
    }

    /** Splits an Annex B buffer into its NAL units (start codes retained). */
    private fun splitAnnexB(data: ByteArray): List<ByteArray> {
        val starts = mutableListOf<Int>()
        var i = 0
        while (i + 3 < data.size) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 && data[i + 2].toInt() == 1) {
                starts.add(i)
                i += 3
            } else if (i + 4 < data.size && data[i].toInt() == 0 && data[i + 1].toInt() == 0 &&
                data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1
            ) {
                starts.add(i)
                i += 4
            } else {
                i++
            }
        }
        return starts.mapIndexed { index, start ->
            val end = starts.getOrNull(index + 1) ?: data.size
            data.copyOfRange(start, end)
        }
    }

    // ── Little-endian readers (RandomAccessFile is big-endian) ───────────

    private fun RandomAccessFile.readUnsignedShortLE(): Int {
        val b0 = read()
        val b1 = read()
        return b0 or (b1 shl 8)
    }

    private fun RandomAccessFile.readIntLE(): Int? {
        val b0 = read(); if (b0 < 0) return null
        val b1 = read(); if (b1 < 0) return null
        val b2 = read(); if (b2 < 0) return null
        val b3 = read(); if (b3 < 0) return null
        return b0 or (b1 shl 8) or (b2 shl 16) or (b3 shl 24)
    }

    private fun RandomAccessFile.readLongLE(): Long? {
        var value = 0L
        for (i in 0 until 8) {
            val b = read()
            if (b < 0) return null
            value = value or (b.toLong() and 0xff shl (8 * i))
        }
        return value
    }
}
