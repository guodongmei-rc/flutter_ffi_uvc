package com.cornpip.flutter_ffi_uvc

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.media.MediaCodecList
import android.media.AudioManager
import android.media.MediaFormat
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.util.Log
import android.view.Surface
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.PluginRegistry
import io.flutter.view.TextureRegistry

class FlutterFfiUvcPlugin :
    FlutterPlugin,
    MethodChannel.MethodCallHandler,
    ActivityAware,
    PluginRegistry.RequestPermissionsResultListener {

    companion object {
        private const val CAMERA_PERMISSION_REQUEST_CODE = 9001
        private const val GALLERY_PERMISSION_REQUEST_CODE = 9002
        private const val AUDIO_PERMISSION_REQUEST_CODE = 9003
        private const val TAG = "flutter_ffi_uvc"

        init {
            System.loadLibrary("flutter_ffi_uvc")
        }
    }

    // Texture
    private lateinit var textureChannel: MethodChannel
    private lateinit var textureRegistry: TextureRegistry
    private val textures = mutableMapOf<Long, TextureRegistry.SurfaceTextureEntry>()
    private var attachedTextureId: Long? = null

    // USB
    private lateinit var usbChannel: MethodChannel
    private lateinit var deviceEventChannel: EventChannel
    private var deviceEventSink: EventChannel.EventSink? = null
    private var deviceEventReceiverRegistered = false
    private var appContext: Context? = null
    private var activity: Activity? = null
    private var usbManager: UsbManager? = null
    private var currentConnection: UsbDeviceConnection? = null
    private var currentDevice: UsbDevice? = null
    private var usbPermissionResult: MethodChannel.Result? = null
    private var cameraPermissionResult: MethodChannel.Result? = null
    private var galleryPermissionResult: MethodChannel.Result? = null

    // Recording
    private var videoRecorder: VideoRecorder? = null
    private var rawRecTempFile: File? = null
    private var aacFileRecorder: AacFileRecorder? = null
    private var rawRecAudioTempFile: File? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    private val usbPermissionAction: String
        get() = "${appContext?.packageName}.flutter_ffi_uvc.USB_PERMISSION"

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != usbPermissionAction) return
            val result = usbPermissionResult ?: return

            val device: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            }

            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            usbPermissionResult = null

            if (!granted || device == null) {
                result.error("permission_denied", "USB permission denied", null)
                return
            }
            openDevice(device, result)
        }
    }

    private val deviceEventReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val eventType = when (intent?.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> "attached"
                UsbManager.ACTION_USB_DEVICE_DETACHED -> "detached"
                else -> return
            }
            val device: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            }
            if (device == null || !isVideoDevice(device)) return
            deviceEventSink?.success(
                mapOf("event" to eventType, "device" to deviceToMap(device)),
            )
        }
    }

    private fun registerDeviceEventReceiver() {
        val context = appContext ?: return
        if (deviceEventReceiverRegistered) return
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        // ATTACHED/DETACHED are protected system broadcasts, so an exported
        // receiver cannot be spoofed by other apps.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(deviceEventReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            context.registerReceiver(deviceEventReceiver, filter)
        }
        deviceEventReceiverRegistered = true
    }

    private fun unregisterDeviceEventReceiver() {
        if (!deviceEventReceiverRegistered) return
        try { appContext?.unregisterReceiver(deviceEventReceiver) } catch (_: Exception) {}
        deviceEventReceiverRegistered = false
    }

    // ── FlutterPlugin ────────────────────────────────────────────────────────

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        appContext = binding.applicationContext
        usbManager = binding.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager
        textureRegistry = binding.textureRegistry

        textureChannel = MethodChannel(binding.binaryMessenger, "flutter_ffi_uvc/texture")
        textureChannel.setMethodCallHandler(this)

        usbChannel = MethodChannel(binding.binaryMessenger, "flutter_ffi_uvc/usb")
        usbChannel.setMethodCallHandler(this)

        deviceEventChannel = EventChannel(binding.binaryMessenger, "flutter_ffi_uvc/device_events")
        deviceEventChannel.setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                deviceEventSink = events
                registerDeviceEventReceiver()
            }

            override fun onCancel(arguments: Any?) {
                unregisterDeviceEventReceiver()
                deviceEventSink = null
            }
        })
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        nativeDetachRecordingSurface()
        videoRecorder?.abort()
        videoRecorder = null
        nativeRawRecStop()
        try { aacFileRecorder?.stop() } catch (_: Exception) {}
        aacFileRecorder = null
        stopAudioCaptureQuietly()
        rawRecTempFile?.delete()
        rawRecTempFile = null
        rawRecAudioTempFile?.delete()
        rawRecAudioTempFile = null
        nativeDetachSurface()
        attachedTextureId = null
        textures.values.forEach { it.release() }
        textures.clear()
        textureChannel.setMethodCallHandler(null)
        usbChannel.setMethodCallHandler(null)
        unregisterDeviceEventReceiver()
        deviceEventChannel.setStreamHandler(null)
        deviceEventSink = null
        closeCurrentConnection()
        appContext = null
        usbManager = null
    }

    // ── ActivityAware ────────────────────────────────────────────────────────

    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        activity = binding.activity
        binding.addRequestPermissionsResultListener(this)
        val filter = IntentFilter(usbPermissionAction)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            binding.activity.registerReceiver(
                permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED
            )
        } else {
            @Suppress("DEPRECATION")
            binding.activity.registerReceiver(permissionReceiver, filter)
        }
    }

    override fun onDetachedFromActivity() {
        try { activity?.unregisterReceiver(permissionReceiver) } catch (_: Exception) {}
        activity = null
    }

    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
        onAttachedToActivity(binding)
    }

    override fun onDetachedFromActivityForConfigChanges() {
        onDetachedFromActivity()
    }

    // ── RequestPermissionsResultListener ─────────────────────────────────────

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ): Boolean {
        if (requestCode == AUDIO_PERMISSION_REQUEST_CODE) {
            // Fire-and-forget: the current recording already fell back to
            // the libusb path; the grant (or denial) applies to the next one.
            val granted =
                grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
            Log.i(TAG, "RECORD_AUDIO permission ${if (granted) "granted" else "denied"}")
            return true
        }
        val result = when (requestCode) {
            CAMERA_PERMISSION_REQUEST_CODE -> {
                val pending = cameraPermissionResult ?: return false
                cameraPermissionResult = null
                pending
            }
            GALLERY_PERMISSION_REQUEST_CODE -> {
                val pending = galleryPermissionResult ?: return false
                galleryPermissionResult = null
                pending
            }
            else -> return false
        }
        val granted = grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
        result.success(granted)
        return true
    }

    // ── MethodCallHandler ────────────────────────────────────────────────────

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {

            // Texture ─────────────────────────────────────────────────────────

            "createPreviewTexture" -> {
                val entry = textureRegistry.createSurfaceTexture()
                textures[entry.id()] = entry
                result.success(entry.id())
            }

            "disposePreviewTexture" -> {
                val textureId = call.argument<Number>("textureId")?.toLong()
                if (textureId == null) {
                    result.error("invalid_args", "textureId is required.", null)
                    return
                }
                if (attachedTextureId == textureId) {
                    nativeDetachSurface()
                    attachedTextureId = null
                }
                textures.remove(textureId)?.release()
                result.success(null)
            }

            "attachPreviewTexture" -> {
                val textureId = call.argument<Number>("textureId")?.toLong()
                val width = call.argument<Number>("width")?.toInt()
                val height = call.argument<Number>("height")?.toInt()
                if (textureId == null) {
                    result.error("invalid_args", "textureId is required.", null)
                    return
                }
                val entry = textures[textureId]
                if (entry == null) {
                    result.error("missing_texture", "Unknown textureId=$textureId", null)
                    return
                }
                if (width != null && height != null && width > 0 && height > 0) {
                    entry.surfaceTexture().setDefaultBufferSize(width, height)
                }
                val surface = Surface(entry.surfaceTexture())
                try {
                    val attachResult = nativeAttachSurface(surface)
                    if (attachResult != 0) {
                        result.error(
                            "attach_failed",
                            "nativeAttachSurface failed with code $attachResult",
                            attachResult,
                        )
                        return
                    }
                    attachedTextureId = textureId
                    result.success(null)
                } finally {
                    surface.release()
                }
            }

            // USB ─────────────────────────────────────────────────────────────

            "listUsbDevices" -> {
                val manager = usbManager ?: run {
                    result.error("unavailable", "UsbManager not available", null)
                    return
                }
                result.success(
                    manager.deviceList.values
                        .filter { isVideoDevice(it) }
                        .map { deviceToMap(it) },
                )
            }

            "openUsbDevice" -> {
                val manager = usbManager ?: run {
                    result.error("unavailable", "UsbManager not available", null)
                    return
                }
                val deviceId = call.argument<Int>("deviceId") ?: run {
                    result.error("bad_args", "deviceId is required", null)
                    return
                }
                val device = manager.deviceList.values.firstOrNull { it.deviceId == deviceId }
                if (device == null) {
                    result.error("not_found", "USB device $deviceId not found", null)
                    return
                }
                if (manager.hasPermission(device)) {
                    openDevice(device, result)
                } else {
                    if (usbPermissionResult != null) {
                        result.error("busy", "Another USB permission request is in progress", null)
                        return
                    }
                    val act = activity ?: run {
                        result.error("no_activity", "Activity not available for USB permission", null)
                        return
                    }
                    usbPermissionResult = result
                    val pendingIntent = PendingIntent.getBroadcast(
                        act,
                        deviceId,
                        Intent(usbPermissionAction).apply { `package` = act.packageName },
                        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
                    )
                    manager.requestPermission(device, pendingIntent)
                }
            }

            "closeUsbDevice" -> {
                closeCurrentConnection()
                result.success(null)
            }

            "ensureCameraPermission" -> {
                val act = activity ?: run {
                    result.error("no_activity", "Activity not available", null)
                    return
                }
                if (ContextCompat.checkSelfPermission(act, android.Manifest.permission.CAMERA)
                    == PackageManager.PERMISSION_GRANTED
                ) {
                    result.success(true)
                } else {
                    cameraPermissionResult = result
                    ActivityCompat.requestPermissions(
                        act,
                        arrayOf(android.Manifest.permission.CAMERA),
                        CAMERA_PERMISSION_REQUEST_CODE,
                    )
                }
            }

            "ensureGalleryPermission" -> {
                if (hasGalleryPermission()) {
                    result.success(true)
                    return
                }
                val act = activity ?: run {
                    result.error("no_activity", "Activity not available", null)
                    return
                }
                if (galleryPermissionResult != null) {
                    result.error("busy", "Another gallery permission request is in progress", null)
                    return
                }
                galleryPermissionResult = result
                ActivityCompat.requestPermissions(
                    act,
                    arrayOf(android.Manifest.permission.WRITE_EXTERNAL_STORAGE),
                    GALLERY_PERMISSION_REQUEST_CODE,
                )
            }

            // Gallery capture ─────────────────────────────────────────────────

            "saveImageToGallery" -> {
                val bytes = call.argument<ByteArray>("bytes")
                if (bytes == null || bytes.isEmpty()) {
                    result.error("invalid_args", "bytes is required.", null)
                    return
                }
                if (!hasGalleryPermission()) {
                    result.error("gallery_permission_denied", "Gallery permission not granted", null)
                    return
                }
                Thread({
                    try {
                        val saved = saveJpegToGallery(bytes)
                        mainHandler.post { result.success(saved) }
                    } catch (e: Exception) {
                        Log.e(TAG, "saveImageToGallery failed", e)
                        mainHandler.post {
                            result.error("save_failed", e.message ?: "Failed to save image", null)
                        }
                    }
                }, "uvc-save-image").start()
            }

            "startVideoRecording" -> {
                val width = call.argument<Number>("width")?.toInt()
                val height = call.argument<Number>("height")?.toInt()
                if (width == null || height == null || width <= 0 || height <= 0) {
                    result.error("invalid_args", "width and height are required.", null)
                    return
                }
                if (!hasGalleryPermission()) {
                    result.error("gallery_permission_denied", "Gallery permission not granted", null)
                    return
                }
                if (videoRecorder != null || rawRecTempFile != null) {
                    result.error("already_recording", "A recording is already in progress", null)
                    return
                }
                val context = appContext ?: run {
                    result.error("unavailable", "Context not available", null)
                    return
                }
                // Passthrough (currently unused — Dart always re-encodes):
                // the native layer writes the camera's own NAL stream to a
                // temp file and stopVideoRecording remuxes it, no re-encode.
                if (call.argument<Boolean>("passthrough") == true) {
                    val tempFile = File(context.cacheDir, "uvc_raw_${System.currentTimeMillis()}.bin")
                    val startResult = nativeRawRecStart(tempFile.absolutePath)
                    if (startResult != 0) {
                        tempFile.delete()
                        result.error(
                            "start_failed",
                            "nativeRawRecStart failed with code $startResult",
                            startResult,
                        )
                        return
                    }
                    rawRecTempFile = tempFile
                    if (call.argument<Boolean>("withAudio") ?: true) {
                        startPassthroughAudio(context)
                    }
                    result.success(null)
                    return
                }
                val bitRate = call.argument<Number>("bitRate")?.toInt()
                val frameRate = call.argument<Number>("frameRate")?.toInt() ?: 30
                // The output codec follows the camera stream: H.265 stays
                // H.265 when the device has an HEVC encoder, otherwise the
                // recording falls back to H.264.
                val cameraFormat = call.argument<String>("cameraFormat")
                val videoMime =
                    if (cameraFormat == "H265" && hevcEncoderAvailable()) {
                        MediaFormat.MIMETYPE_VIDEO_HEVC
                    } else {
                        MediaFormat.MIMETYPE_VIDEO_AVC
                    }
                // Feed mode follows the camera stream too: H.264/H.265 keep
                // the decoder-to-Surface path; uncompressed/MJPEG previews
                // are fed as YUV byte buffers from the native recording
                // queue (no GL, no input Surface on that path).
                val useBufferInput = cameraFormat != "H264" && cameraFormat != "H265"
                val audioEncoder = if (call.argument<Boolean>("withAudio") ?: true) {
                    maybeRequestAudioPermission()
                    createAudioEncoder()
                } else {
                    null
                }
                try {
                    val recorder = VideoRecorder(
                        context, width, height, bitRate, frameRate, audioEncoder,
                        videoMime, useBufferInput,
                    )
                    val surface = recorder.start()
                    if (!useBufferInput) {
                        val attachResult = nativeAttachRecordingSurface(surface!!)
                        if (attachResult != 0) {
                            recorder.abort()
                            result.error(
                                "attach_failed",
                                "nativeAttachRecordingSurface failed with code $attachResult",
                                attachResult,
                            )
                            return
                        }
                    }
                    videoRecorder = recorder
                    result.success(null)
                } catch (e: Exception) {
                    Log.e(TAG, "startVideoRecording failed", e)
                    // The capture is also stopped inside VideoRecorder's
                    // failure paths; NativeAudio.stop is idempotent.
                    if (audioEncoder != null) stopAudioCaptureQuietly()
                    result.error("start_failed", e.message ?: "Failed to start recording", null)
                }
            }

            "stopVideoRecording" -> {
                // Passthrough finalization: stop the native recorder (a
                // no-op if a preview stop already ended it) and remux the
                // temp file into the gallery.
                val rawFile = rawRecTempFile
                if (rawFile != null) {
                    rawRecTempFile = null
                    val audioRecorder = aacFileRecorder
                    val audioFile = rawRecAudioTempFile
                    aacFileRecorder = null
                    rawRecAudioTempFile = null
                    Thread({
                        try {
                            nativeRawRecStop()
                            // Stop the capture before finalizing: the AAC
                            // encoder drains its tail once nativeAudioRead
                            // reports end-of-stream.
                            if (audioRecorder != null) {
                                stopAudioCaptureQuietly()
                                audioRecorder.stop()
                            }
                            val context = appContext
                                ?: throw IllegalStateException("Context not available")
                            val muxResult = RawVideoMuxer.mux(context, rawFile, audioFile)
                            rawFile.delete()
                            audioFile?.delete()
                            mainHandler.post {
                                result.success(mapOf("uri" to muxResult.uri, "path" to muxResult.path))
                            }
                        } catch (e: Exception) {
                            Log.e(TAG, "stopVideoRecording(passthrough) failed", e)
                            rawFile.delete()
                            audioFile?.delete()
                            mainHandler.post {
                                result.error("stop_failed", e.message ?: "Failed to finish recording", null)
                            }
                        }
                    }, "uvc-raw-video-mux").start()
                    return
                }
                val recorder = videoRecorder ?: run {
                    result.error("not_recording", "No recording in progress", null)
                    return
                }
                videoRecorder = null
                // Detach first so no frame is rendered after end-of-stream.
                nativeDetachRecordingSurface()
                recorder.stop(object : VideoRecorder.StopCallback {
                    override fun onComplete(uri: String?, path: String?) {
                        mainHandler.post {
                            result.success(mapOf("uri" to uri, "path" to path))
                        }
                    }

                    override fun onError(message: String) {
                        mainHandler.post { result.error("stop_failed", message, null) }
                    }
                })
            }

            "probeAudioInterface" -> {
                val info = try {
                    NativeAudio.probe()
                } catch (e: Throwable) {
                    Log.w(TAG, "probeAudioInterface failed", e)
                    null
                }
                result.success(
                    info?.let {
                        mapOf(
                            "sampleRate" to it[0],
                            "channels" to it[1],
                            "bitsPerSample" to it[2],
                        )
                    },
                )
            }

            else -> result.notImplemented()
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    // Audio capture for recordings. Every failure logs a warning and degrades
    // to video-only — audio must never break a recording.
    //
    // Source selection: the Android USB audio path (AudioRecord) is tried
    // first — the kernel USB audio driver does the isochronous streaming
    // itself, which is immune to the userspace usbfs starvation some phones
    // show on raw isoc reads (a steady 1/16 of the nominal rate on a Huawei
    // nova 12). Without mic permission or a USB audio input device, the
    // native libusb UAC capture is used instead; startDetected() measures
    // the real capture format while starting (the descriptor rate is only
    // an estimate).
    private fun createAudioEncoder(): AacAudioEncoder? {
        val context = appContext
        if (context != null && hasRecordAudioPermission()) {
            val audioManager =
                context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            val usbInput = UsbMicCapture.findUsbInput(audioManager)
            if (usbInput != null) {
                val capture = UsbMicCapture.open(usbInput)
                if (capture != null) {
                    Log.i(TAG, "Audio source: AudioRecord USB mic @ ${capture.sampleRate} Hz")
                    // The kernel path already applies sane gain; keep PCM
                    // as-is (fixed unity gain, no AGC).
                    return AacAudioEncoder(
                        capture.sampleRate, capture.channelCount, 1f, capture,
                    )
                }
                Log.w(TAG, "USB audio input found but AudioRecord failed; using libusb UAC")
            } else {
                Log.i(TAG, "No USB audio input device; using libusb UAC capture")
            }
        }
        val info = try {
            NativeAudio.startDetected()
        } catch (e: Throwable) {
            Log.w(TAG, "Audio capture start threw; recording video-only", e)
            null
        }
        if (info == null) {
            Log.w(TAG, "No UAC audio interface; recording video-only")
            return null
        }
        if (info.size < 3 || info[0] <= 0 || info[1] <= 0 || info[2] != 16) {
            Log.w(TAG, "Unsupported UAC format ${info.contentToString()}; recording video-only")
            stopAudioCaptureQuietly()
            return null
        }
        Log.i(TAG, "Audio source: libusb UAC @ ${info[0]} Hz")
        return AacAudioEncoder(info[0], info[1])
    }

    private fun hasRecordAudioPermission(): Boolean {
        val context = appContext ?: return false
        return ContextCompat.checkSelfPermission(
            context, android.Manifest.permission.RECORD_AUDIO,
        ) == PackageManager.PERMISSION_GRANTED
    }

    /**
     * Triggers the runtime mic-permission prompt when a USB audio input is
     * present but RECORD_AUDIO is not granted. Fire-and-forget: the current
     * recording falls back to the libusb path; the next one can use
     * AudioRecord.
     */
    private fun maybeRequestAudioPermission() {
        val context = appContext ?: return
        val currentActivity = activity ?: return
        if (hasRecordAudioPermission()) return
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        if (UsbMicCapture.findUsbInput(audioManager) == null) return
        Log.i(TAG, "Requesting RECORD_AUDIO for the USB mic path")
        ActivityCompat.requestPermissions(
            currentActivity,
            arrayOf(android.Manifest.permission.RECORD_AUDIO),
            AUDIO_PERMISSION_REQUEST_CODE,
        )
    }

    private fun stopAudioCaptureQuietly() {
        try {
            NativeAudio.stop()
        } catch (e: Throwable) {
            Log.w(TAG, "Audio capture stop failed", e)
        }
    }

    /** Starts the AAC temp-file recorder for the passthrough recording path. */
    private fun startPassthroughAudio(context: Context) {
        maybeRequestAudioPermission()
        val encoder = createAudioEncoder() ?: return
        try {
            val audioFile =
                File(context.cacheDir, "uvc_audio_${System.currentTimeMillis()}.bin")
            val recorder = AacFileRecorder(audioFile, encoder)
            recorder.start()
            rawRecAudioTempFile = audioFile
            aacFileRecorder = recorder
        } catch (e: Exception) {
            Log.w(TAG, "Audio recorder failed to start; recording video-only", e)
            stopAudioCaptureQuietly()
            // Safe on a never-started encoder; releases the AudioRecord when
            // the kernel path was selected.
            encoder.stop()
        }
    }

    // Saving media through MediaStore needs no runtime permission on
    // Android 10+ (scoped storage); earlier releases need WRITE_EXTERNAL_STORAGE.
    private fun hasGalleryPermission(): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return true
        val context = appContext ?: return false
        return ContextCompat.checkSelfPermission(
            context,
            android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
        ) == PackageManager.PERMISSION_GRANTED
    }

    /** HEVC encoding is optional in the Android CDD; probe before selecting it. */
    private fun hevcEncoderAvailable(): Boolean =
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            info.isEncoder &&
                info.supportedTypes.any {
                    it.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, ignoreCase = true)
                }
        }

    /** Writes JPEG bytes into the device gallery. Returns uri/path of the entry. */
    private fun saveJpegToGallery(bytes: ByteArray): Map<String, String?> {
        val context = appContext ?: throw IllegalStateException("Context not available")
        val name = "UVC_" +
            SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date()) + ".jpg"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val resolver = context.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, name)
                put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_DCIM)
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
            val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("Failed to create MediaStore image entry")
            try {
                resolver.openOutputStream(uri)?.use { it.write(bytes) }
                    ?: throw IllegalStateException("Failed to open MediaStore image for writing")
                values.clear()
                values.put(MediaStore.Images.Media.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
            } catch (e: Exception) {
                resolver.delete(uri, null, null)
                throw e
            }
            return mapOf("uri" to uri.toString(), "path" to null)
        }

        @Suppress("DEPRECATION")
        val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM)
        if (!dir.exists() && !dir.mkdirs()) {
            throw IllegalStateException("Cannot create output directory ${dir.absolutePath}")
        }
        val file = File(dir, name)
        file.writeBytes(bytes)
        MediaScannerConnection.scanFile(context, arrayOf(file.absolutePath), null, null)
        return mapOf("uri" to null, "path" to file.absolutePath)
    }

    private fun openDevice(device: UsbDevice, result: MethodChannel.Result) {
        closeCurrentConnection()
        val connection = usbManager?.openDevice(device)
        if (connection == null) {
            result.error("open_failed", "Unable to open USB device", null)
            return
        }
        logUsbDeviceLayout(device, connection)
        currentDevice = device
        currentConnection = connection
        result.success(mapOf("fileDescriptor" to connection.fileDescriptor))
    }

    private fun closeCurrentConnection() {
        currentConnection?.close()
        currentConnection = null
        currentDevice = null
    }

    private fun deviceToMap(device: UsbDevice): Map<String, Any> = mapOf(
        "deviceId" to device.deviceId,
        "deviceName" to device.deviceName,
        "vendorId" to device.vendorId,
        "productId" to device.productId,
        "productName" to (device.productName ?: ""),
        "manufacturerName" to (device.manufacturerName ?: ""),
        "serialNumber" to safeSerialNumber(device),
        // hasPermission can be queried for a device that is already gone
        // (detach events), so treat failures as "no permission".
        "hasPermission" to runCatching { usbManager?.hasPermission(device) == true }
            .getOrDefault(false),
    )

    private fun safeSerialNumber(device: UsbDevice): String = try {
        device.serialNumber ?: ""
    } catch (_: SecurityException) {
        ""
    }

    private fun isVideoDevice(device: UsbDevice): Boolean {
        if (device.deviceClass == 14) return true
        for (index in 0 until device.interfaceCount) {
            if (device.getInterface(index).interfaceClass == 14) return true
        }
        return false
    }

    private fun logUsbDeviceLayout(device: UsbDevice, connection: UsbDeviceConnection) {
        Log.d(
            TAG,
            "@@@@UVC_ANDROID/D openDevice id=${device.deviceId} name=${device.deviceName} " +
                "vendor=${device.vendorId} product=${device.productId} " +
                "fd=${connection.fileDescriptor} configs=${device.configurationCount} " +
                "interfaces=${device.interfaceCount}",
        )
        for (configIndex in 0 until device.configurationCount) {
            val config = device.getConfiguration(configIndex)
            Log.d(
                TAG,
                "@@@@UVC_ANDROID/D config index=$configIndex id=${config.id} " +
                    "name=${config.name ?: ""} interfaces=${config.interfaceCount}",
            )
            for (interfaceIndex in 0 until config.interfaceCount) {
                val usbInterface = config.getInterface(interfaceIndex)
                Log.d(
                    TAG,
                    "@@@@UVC_ANDROID/D interface config=$configIndex index=$interfaceIndex " +
                        "id=${usbInterface.id} alt=${usbInterface.alternateSetting} " +
                        "class=${usbInterface.interfaceClass} subclass=${usbInterface.interfaceSubclass} " +
                        "protocol=${usbInterface.interfaceProtocol} endpoints=${usbInterface.endpointCount}",
                )
                for (endpointIndex in 0 until usbInterface.endpointCount) {
                    val endpoint = usbInterface.getEndpoint(endpointIndex)
                    Log.d(
                        TAG,
                        "@@@@UVC_ANDROID/D endpoint interface=${usbInterface.id} " +
                            "alt=${usbInterface.alternateSetting} index=$endpointIndex " +
                            "address=0x${endpoint.address.toString(16)} " +
                            "type=${usbEndpointTypeName(endpoint.type)} " +
                            "direction=${if (endpoint.direction == UsbConstants.USB_DIR_IN) "IN" else "OUT"} " +
                            "maxPacket=${endpoint.maxPacketSize} interval=${endpoint.interval}",
                    )
                }
            }
        }
    }

    private fun usbEndpointTypeName(type: Int): String = when (type) {
        UsbConstants.USB_ENDPOINT_XFER_CONTROL -> "CONTROL"
        UsbConstants.USB_ENDPOINT_XFER_ISOC -> "ISOC"
        UsbConstants.USB_ENDPOINT_XFER_BULK -> "BULK"
        UsbConstants.USB_ENDPOINT_XFER_INT -> "INT"
        else -> type.toString()
    }

    // ── JNI ──────────────────────────────────────────────────────────────────

    private external fun nativeAttachSurface(surface: Surface): Int
    private external fun nativeDetachSurface()
    private external fun nativeAttachRecordingSurface(surface: Surface): Int
    private external fun nativeDetachRecordingSurface()
    private external fun nativeRawRecStart(path: String): Int
    private external fun nativeRawRecStop(): Int
}
