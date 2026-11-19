package com.cornpip.flutter_ffi_uvc

import android.content.Context
import android.hardware.usb.UsbDevice

/**
 * Remembers on which camera the native libusb UAC capture ([NativeAudio])
 * was measured as starved on this phone.
 *
 * Which audio path works cannot be found out by trying both in one session:
 * on some phones (a OnePlus) the Android USB audio path yields only digital
 * silence while this app holds the camera, and merely opening it leaves the
 * libusb capture silent as well until the camera is re-plugged. So the
 * libusb capture always goes first, and only a camera whose capture the
 * native start-up measurement reports as starved (a steady 1/16 of the
 * nominal rate on a Huawei nova 12) is remembered here. From then on it is
 * recorded through the Android USB audio path, or treated as having no
 * microphone when the platform exposes none for it.
 *
 * The verdict is kept per camera model: a slow delivery can also be a trait
 * of one camera firmware (an iCatch V37 answers one poll in four with a
 * quarter-size packet), and must not reroute every other camera.
 */
internal object UacCaptureHealth {
    private const val PREFS_NAME = "flutter_ffi_uvc_audio"
    private const val KEY_PREFIX = "libusb_capture_starved_"

    /** Whether an earlier recording measured [device]'s libusb capture as starved. */
    fun isLibusbStarved(context: Context, device: UsbDevice): Boolean =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(keyFor(device), false)

    fun markLibusbStarved(context: Context, device: UsbDevice) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(keyFor(device), true)
            .apply()
    }

    private fun keyFor(device: UsbDevice): String =
        "$KEY_PREFIX${device.vendorId}_${device.productId}"
}
