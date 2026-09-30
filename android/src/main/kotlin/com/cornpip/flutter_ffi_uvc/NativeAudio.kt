package com.cornpip.flutter_ffi_uvc

import java.nio.ByteBuffer

/**
 * JNI bridge to the native UAC microphone capture (src/uac_audio.c). The
 * camera's USB Audio Class interface is claimed and read with isochronous
 * transfers on the same libusb context the preview uses.
 *
 * Every failure is reported by return value only — callers log and degrade
 * to video-only recording; audio must never break a recording.
 */
internal object NativeAudio {

    /** Returns {sampleRate, channels, bits} of the UAC capture interface, or null. */
    external fun probe(): IntArray?

    /** Starts PCM capture. Returns 0 on success, negative on failure. */
    external fun start(): Int

    /**
     * Starts PCM capture like [start], additionally measuring the real
     * capture format: the native layer streams each altsetting briefly and
     * keeps the one that actually delivers data, so the returned rate is the
     * measured truth (descriptor rates are only estimates). Returns
     * {sampleRate, channels, bits}, or null on failure.
     */
    external fun startDetected(): IntArray?

    /**
     * Fills [buffer] (must be direct) with captured 16-bit PCM. Returns the
     * byte count, 0 on [timeoutMs] timeout, or -1 once the capture stopped
     * (end of stream).
     */
    external fun read(buffer: ByteBuffer, timeoutMs: Int): Int

    /** Stops the capture and releases the interface. Idempotent. */
    external fun stop(): Int
}
