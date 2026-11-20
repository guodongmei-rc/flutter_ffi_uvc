package com.cornpip.flutter_ffi_uvc

/**
 * Remembers, for the camera connection currently open, that the native
 * libusb UAC capture ([NativeAudio]) was measured as starved.
 *
 * Which audio path works cannot be found out by trying both in any order:
 * on some phones (a OnePlus) the Android USB audio path yields only digital
 * silence while this app holds the camera, and merely opening it leaves the
 * libusb capture silent as well until the camera is re-plugged. So the
 * libusb capture always goes first, and only a capture the native start-up
 * measurement reports as starved (a steady 1/16 of the nominal rate on a
 * Huawei nova 12, one quarter-size packet per four polls on an iCatch V37,
 * nothing at all on a RunCam firmware) falls back to the Android USB audio
 * path. This flag lets the following recordings skip the measurement.
 *
 * The verdict lives in memory and is cleared with the connection. It is not
 * kept per camera model: units of one model differ by firmware, and a
 * verdict stored for one would reroute a healthy unit as well.
 */
internal object UacCaptureHealth {
    @Volatile
    private var libusbStarved = false

    /** Whether this connection's libusb capture was measured as starved. */
    fun isLibusbStarved(): Boolean = libusbStarved

    fun markLibusbStarved() {
        libusbStarved = true
    }

    /** Forgets the verdict; call whenever the camera connection changes. */
    fun reset() {
        libusbStarved = false
    }
}
