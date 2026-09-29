package com.cornpip.flutter_ffi_uvc

import java.nio.ByteBuffer

/**
 * JNI bridge to the native recording frame queue (src/flutter_ffi_uvc.c).
 * The newest preview frame is converted RGBA -> YUV (BT.601 limited range)
 * straight into a MediaCodec input buffer, so the re-encode recording path
 * never touches GL or an encoder input Surface.
 *
 * Used for uncompressed/MJPEG previews only; H.264/H.265 streams keep the
 * hardware-decoder-to-Surface path (nativeAttachRecordingSurface).
 */
internal object NativeRecording {

    /** Y plane + interleaved UV chroma (COLOR_FormatYUV420SemiPlanar). */
    const val YUV_FORMAT_NV12 = 0

    /** Y plane + U plane + V plane (COLOR_FormatYUV420Planar). */
    const val YUV_FORMAT_I420 = 1

    /**
     * Starts the queue. [stride]/[sliceHeight] describe the encoder input
     * buffer layout in pixels (from the codec's input format, defaulting to
     * the frame width/height when absent). Returns 0 on success, negative
     * on failure (invalid params or a queue already active).
     */
    external fun startQueue(yuvFormat: Int, stride: Int, sliceHeight: Int): Int

    /**
     * Waits up to [timeoutMs] for a new frame and converts it into [buffer]
     * (must be direct, typically a MediaCodec input buffer). Returns the
     * byte count with the frame PTS (monotonic µs, strictly increasing) in
     * [outPtsUs][0], 0 on timeout or a dropped frame, or -1 once the queue
     * is stopped (end of stream).
     */
    external fun readFrameYuv(buffer: ByteBuffer, outPtsUs: LongArray, timeoutMs: Int): Int

    /** Stops the queue and wakes any blocked reader. Idempotent. */
    external fun stopQueue()
}
