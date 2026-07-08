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
                if (videoRecorder != null) {
                    result.error("already_recording", "A recording is already in progress", null)
                    return
                }
                val context = appContext ?: run {
                    result.error("unavailable", "Context not available", null)
                    return
                }
                val bitRate = call.argument<Number>("bitRate")?.toInt()
                val frameRate = call.argument<Number>("frameRate")?.toInt() ?: 30
                try {
                    val recorder = VideoRecorder(context, width, height, bitRate, frameRate)
                    val surface = recorder.start()
                    val attachResult = nativeAttachRecordingSurface(surface)
                    if (attachResult != 0) {
                        recorder.abort()
                        result.error(
                            "attach_failed",
                            "nativeAttachRecordingSurface failed with code $attachResult",
                            attachResult,
                        )
                        return
                    }
                    videoRecorder = recorder
                    result.success(null)
                } catch (e: Exception) {
                    Log.e(TAG, "startVideoRecording failed", e)
                    result.error("start_failed", e.message ?: "Failed to start recording", null)
                }
            }

            "stopVideoRecording" -> {
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

            else -> result.notImplemented()
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

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
}
