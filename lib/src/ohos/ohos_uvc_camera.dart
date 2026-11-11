import 'dart:async';

import 'package:flutter/services.dart';

import '../uvc_camera_api.dart';

/// [UvcCamera] for HarmonyOS / OpenHarmony.
///
/// The camera is driven by the platform side of the plugin instead of the
/// libuvc FFI library used on Android: the system USB API (`usbManager`)
/// finds the device and asks for permission, and native code then streams
/// from it, decodes the frames and draws them into the preview texture. Every
/// operation here is therefore a platform-channel call. The synchronous
/// getters of [UvcCamera] are answered from state cached here: modes are
/// cached when a device is opened, and the frame sequence and stream
/// statistics are refreshed by periodic events while a preview is running.
///
/// Implemented so far: device listing, attach/detach events, open/close,
/// MJPEG mode listing, preview into a Flutter texture with frame
/// verification, photos and video recording without audio (saved to the
/// system gallery with the user's consent, else to the app's files
/// directory). Camera controls, frame
/// copies, preview transforms and stall detection are not available yet and
/// report [UvcErrorCode.notSupported] or an empty result.
class OhosUvcCamera implements UvcCamera {
  OhosUvcCamera();

  static const MethodChannel _channel = MethodChannel('flutter_ffi_uvc/ohos');
  static const EventChannel _eventChannel = EventChannel(
    'flutter_ffi_uvc/ohos_events',
  );

  static final int _notSupported = UvcErrorCode.notSupported.nativeValue;

  final StreamController<UvcStreamError> _streamErrorController =
      StreamController<UvcStreamError>.broadcast();
  final StreamController<UvcDeviceEvent> _deviceEventController =
      StreamController<UvcDeviceEvent>.broadcast();
  final StreamController<UvcStallEvent> _stallEventController =
      StreamController<UvcStallEvent>.broadcast();
  StreamSubscription<Object?>? _eventSubscription;

  List<UvcCameraMode> _modes = const <UvcCameraMode>[];
  bool _previewing = false;
  bool _recording = false;
  String _lastError = '';
  int _sequence = 0;
  UvcStreamStats _stats = const UvcStreamStats.zero();

  void _ensureEvents() {
    _eventSubscription ??= _eventChannel.receiveBroadcastStream().listen(
      _onEvent,
      onError: (Object error) {
        _lastError = error.toString();
      },
    );
  }

  void _onEvent(Object? event) {
    if (event is! Map<Object?, Object?>) {
      return;
    }
    switch (event['type']) {
      case 'stats':
        _sequence = _int(event['sequence']);
        _stats = _statsFromMap(event);
      case 'streamError':
        final String message = event['message'] as String? ?? '';
        _lastError = message;
        _streamErrorController.add(UvcStreamError(message: message));
      case 'previewStopped':
        _previewing = false;
        final String reason = event['reason'] as String? ?? '';
        if (reason.isNotEmpty) {
          _lastError = reason;
        }
      case 'device':
        _deviceEventController.add(UvcDeviceEvent.fromMap(event));
    }
  }

  static int _int(Object? value) => (value as num?)?.toInt() ?? 0;

  static double _double(Object? value) => (value as num?)?.toDouble() ?? 0;

  static UvcStreamStats _statsFromMap(Map<Object?, Object?> map) {
    return UvcStreamStats(
      inputFrameCount: _int(map['inputFrameCount']),
      deliveredFrameCount: _int(map['deliveredFrameCount']),
      decodeSuccessCount: _int(map['decodeSuccessCount']),
      decodeFailureCount: _int(map['decodeFailureCount']),
      callbackLockDropCount: _int(map['callbackLockDropCount']),
      warmupDropCount: 0,
      staleFrameCount: 0,
      undersizedFrameCount: _int(map['undersizedFrameCount']),
      invalidMjpegCount: _int(map['invalidMjpegCount']),
      bufferAllocationFailureCount: 0,
      previewSurfaceFailureCount: 0,
      conversionFailureCount: 0,
      inputFps: _double(map['inputFps']),
      deliveredFps: _double(map['deliveredFps']),
      avgInterFrameGapMs: _double(map['avgInterFrameGapMs']),
      p95InterFrameGapMs: _double(map['p95InterFrameGapMs']),
      maxInterFrameGapMs: _double(map['maxInterFrameGapMs']),
      firstFrameLatencyMs: _double(map['firstFrameLatencyMs']),
      elapsed: Duration(
        microseconds: (_double(map['elapsedMs']) * 1000).round(),
      ),
    );
  }

  // ---------------------------------------------------------------------------
  // Devices
  // ---------------------------------------------------------------------------

  /// Debug and trace turn on a once-per-second line of transport and
  /// rendering counters in the system log; other levels leave it off.
  @override
  void setLogLevel(UvcLogLevel level) {
    unawaited(
      _channel
          .invokeMethod<void>('setLogLevel', <String, Object?>{
            'verbose': level.index >= UvcLogLevel.debug.index,
          })
          .catchError((Object _) {}),
    );
  }

  /// USB access is requested when a device is opened; there is no separate
  /// camera permission for external USB cameras.
  @override
  Future<bool> ensureCameraPermission() async => true;

  /// The gallery asks for consent when a capture is saved (once, then for
  /// five minutes), so there is nothing to request up front.
  @override
  Future<bool> ensureGalleryPermission() async => true;

  @override
  Future<List<UvcUsbDevice>> listUsbDevices() async {
    _ensureEvents();
    final List<Object?>? devices = await _channel.invokeListMethod<Object?>(
      'listUsbDevices',
    );
    return (devices ?? const <Object?>[])
        .whereType<Map<Object?, Object?>>()
        .map(UvcUsbDevice.fromMap)
        .toList();
  }

  /// Attach and detach broadcasts for USB video devices, as the system
  /// reports them. A detach of the open device also ends the preview, which
  /// [streamErrors] reports before this event arrives.
  @override
  Stream<UvcDeviceEvent> get deviceEvents {
    _ensureEvents();
    return _deviceEventController.stream;
  }

  @override
  Future<int> openUsbDevice(int deviceId) async {
    _ensureEvents();
    _previewing = false;
    _modes = const <UvcCameraMode>[];
    final Map<Object?, Object?>? reply = await _channel
        .invokeMapMethod<Object?, Object?>('openUsbDevice', <String, Object?>{
          'deviceId': deviceId,
        });
    final int result = _int(reply?['result'] ?? _notSupported);
    _lastError = reply?['lastError'] as String? ?? '';
    if (result == 0) {
      _modes = ((reply?['modes'] as List<Object?>?) ?? const <Object?>[])
          .whereType<Map<Object?, Object?>>()
          .map(
            (Map<Object?, Object?> mode) =>
                UvcCameraMode.fromJson(Map<String, dynamic>.from(mode)),
          )
          .toList();
    }
    return result;
  }

  @override
  Future<void> closeUsbDevice() async {
    _previewing = false;
    _modes = const <UvcCameraMode>[];
    await _channel.invokeMethod<void>('closeUsbDevice');
  }

  /// HarmonyOS opens the camera through the system USB service, not through a
  /// caller-supplied file descriptor. Use [openUsbDevice].
  @override
  int openFd(int fd) => _notSupported;

  @override
  void closeFd() {
    unawaited(closeUsbDevice().catchError((Object _) {}));
  }

  @override
  @Deprecated('Use closeFd() instead.')
  void closeDevice() => closeFd();

  @override
  List<UvcCameraMode> supportedModes() => List<UvcCameraMode>.of(_modes);

  // ---------------------------------------------------------------------------
  // Preview
  // ---------------------------------------------------------------------------

  /// Starting a stream is asynchronous on HarmonyOS. Use [startPreview].
  @override
  int openPreview(UvcCameraMode mode) => _notSupported;

  @override
  Future<UvcPreviewStartResult> startPreview(
    UvcCameraMode mode, {
    UvcPreviewPolicy policy = UvcPreviewPolicy.stableFrames,
    int consecutiveValidFrames = 3,
    Duration timeout = const Duration(seconds: 2),
  }) async {
    if (consecutiveValidFrames <= 0) {
      throw ArgumentError.value(
        consecutiveValidFrames,
        'consecutiveValidFrames',
        'Must be greater than 0.',
      );
    }
    _ensureEvents();
    _previewing = false;
    final Map<Object?, Object?>? reply = await _channel
        .invokeMapMethod<Object?, Object?>('startPreview', <String, Object?>{
          'format': mode.frameFormat,
          'width': mode.width,
          'height': mode.height,
          'fps': mode.fps,
          'consecutiveValidFrames': consecutiveValidFrames,
          'timeoutMs': timeout.inMilliseconds,
          'sequenceOnly': policy == UvcPreviewPolicy.sequenceOnly,
        });
    final bool success = reply?['success'] as bool? ?? false;
    final String error = reply?['lastError'] as String? ?? '';
    _previewing = success;
    if (!success) {
      _lastError = error;
    }
    // UVC lets the device rewrite the requested frame interval during
    // PROBE; report the rate it actually committed to so callers can show
    // the mode that is streaming rather than the one they asked for.
    final int negotiatedFps = _int(reply?['negotiatedFps']);
    final UvcCameraMode streaming =
        success && negotiatedFps > 0 && negotiatedFps != mode.fps
            ? UvcCameraMode(
                frameFormat: mode.frameFormat,
                formatName: mode.formatName,
                width: mode.width,
                height: mode.height,
                fps: negotiatedFps,
              )
            : mode;
    return UvcPreviewStartResult(
      mode: streaming,
      success: success,
      validFrameCount: _int(reply?['validFrameCount']),
      consecutiveValidFrames: _int(reply?['consecutiveValidFrames']),
      errorCount: _int(reply?['errorCount']),
      elapsed: Duration(milliseconds: _int(reply?['elapsedMs'])),
      lastError: error.isEmpty ? null : error,
      nativeErrorCode: _int(reply?['nativeErrorCode']),
    );
  }

  @override
  Future<UvcAutoPreviewResult> startPreviewAuto({
    List<UvcCameraMode>? candidates,
    UvcAutoPreviewPreference preference = UvcAutoPreviewPreference.reliability,
    UvcPreviewPolicy policy = UvcPreviewPolicy.stableFrames,
    int consecutiveValidFrames = 3,
    Duration perModeTimeout = const Duration(seconds: 2),
    int maxCandidates = 8,
  }) async {
    if (maxCandidates <= 0) {
      throw ArgumentError.value(
        maxCandidates,
        'maxCandidates',
        'Must be greater than 0.',
      );
    }
    final List<UvcCameraMode> modes =
        (candidates ?? _defaultAutoCandidates(preference))
            .take(maxCandidates)
            .toList();
    final List<UvcPreviewStartResult> attempts = <UvcPreviewStartResult>[];
    for (final UvcCameraMode mode in modes) {
      final UvcPreviewStartResult result = await startPreview(
        mode,
        policy: policy,
        consecutiveValidFrames: consecutiveValidFrames,
        timeout: perModeTimeout,
      );
      attempts.add(result);
      if (result.success) break;
    }
    return UvcAutoPreviewResult(attempts: attempts);
  }

  /// Same ordering as the Android implementation: by resolution and frame
  /// rate, ascending for reliability and descending for quality.
  List<UvcCameraMode> _defaultAutoCandidates(
    UvcAutoPreviewPreference preference,
  ) {
    final int direction = preference == UvcAutoPreviewPreference.reliability
        ? 1
        : -1;
    return supportedModes()..sort((UvcCameraMode a, UvcCameraMode b) {
      final int byArea =
          direction * (a.width * a.height).compareTo(b.width * b.height);
      if (byArea != 0) return byArea;
      return direction * a.fps.compareTo(b.fps);
    });
  }

  @override
  void stopPreview() {
    _previewing = false;
    unawaited(
      _channel.invokeMethod<void>('stopPreview').catchError((Object error) {
        _lastError = error.toString();
      }),
    );
  }

  @override
  bool get isPreviewing => _previewing;

  @override
  String get lastError => _lastError;

  @override
  Stream<UvcStreamError> get streamErrors {
    _ensureEvents();
    return _streamErrorController.stream;
  }

  @override
  int latestFrameSequence() => _sequence;

  @override
  UvcStreamStats getStreamStats() => _stats;

  @override
  Future<int> createPreviewTexture() async {
    final int? textureId = await _channel.invokeMethod<int>(
      'createPreviewTexture',
    );
    if (textureId == null) {
      throw PlatformException(
        code: 'texture',
        message: 'The platform did not return a preview texture id.',
      );
    }
    return textureId;
  }

  @override
  Future<void> disposePreviewTexture(int textureId) async {
    await _channel.invokeMethod<void>('disposePreviewTexture', <String, Object?>{
      'textureId': textureId,
    });
  }

  @override
  Future<void> attachPreviewTexture(
    int textureId, {
    int? width,
    int? height,
  }) async {
    await _channel.invokeMethod<void>('attachPreviewTexture', <String, Object?>{
      'textureId': textureId,
      'width': width ?? 0,
      'height': height ?? 0,
    });
  }

  // ---------------------------------------------------------------------------
  // Not available on HarmonyOS yet
  // ---------------------------------------------------------------------------

  @override
  void enableStallDetection([
    UvcStallDetectionConfig config = const UvcStallDetectionConfig(),
  ]) {}

  @override
  void disableStallDetection() {}

  @override
  Stream<UvcStallEvent> get stallEvents => _stallEventController.stream;

  @override
  UvcPreviewFrame? copyLatestFrame() => null;

  @override
  UvcPreviewFrame? copyLatestFrameTransformed(UvcPreviewTransform transform) =>
      null;

  /// Saves the latest camera frame as it was sent (MJPEG, so [quality] is not
  /// used). The gallery asks the user for consent the first time and then
  /// allows saving for five minutes; the result carries the gallery URI. When
  /// the user declines, the file stays in the app's files directory and the
  /// result carries its path instead.
  @override
  Future<UvcGalleryMedia> takePicture({int quality = 90}) async {
    return _capture('takePicture', const <String, Object?>{});
  }

  /// Records H.264 video, with the camera's USB microphone as an AAC track
  /// when [withAudio] is true and the device has one (see
  /// [queryAudioInterface]); [stopVideoRecording] saves the file like
  /// [takePicture] does. A microphone that fails to stream makes the
  /// recording video-only rather than failing it, as on Android.
  @override
  Future<void> startVideoRecording({
    int? bitRate,
    int? frameRate,
    bool withAudio = true,
  }) async {
    await _capture('startVideoRecording', <String, Object?>{
      'bitRate': bitRate ?? 0,
      'frameRate': frameRate ?? 0,
      'withAudio': withAudio,
    });
    _recording = true;
  }

  @override
  Future<UvcGalleryMedia> stopVideoRecording() async {
    _recording = false;
    return _capture('stopVideoRecording', const <String, Object?>{});
  }

  @override
  bool get isVideoRecording => _recording;

  Future<UvcGalleryMedia> _capture(
    String method,
    Map<String, Object?> arguments,
  ) async {
    try {
      final Map<Object?, Object?>? reply = await _channel
          .invokeMapMethod<Object?, Object?>(method, arguments);
      return UvcGalleryMedia.fromMap(reply ?? const <Object?, Object?>{});
    } on PlatformException catch (error) {
      _lastError = error.message ?? error.code;
      rethrow;
    }
  }

  /// Reports the device's UAC microphone format from its descriptors, using
  /// the same candidate rule as Android. Capturing it is not implemented on
  /// HarmonyOS yet, so a non-null answer means "the camera has a microphone",
  /// not "recordings will carry audio".
  @override
  Future<UvcAudioInfo?> queryAudioInterface() async {
    final Map<Object?, Object?>? reply =
        await _channel.invokeMapMethod<Object?, Object?>('queryAudioInterface');
    return reply == null ? null : UvcAudioInfo.fromMap(reply);
  }

  @override
  List<UvcCameraControl> supportedControls() => <UvcCameraControl>[];

  @override
  List<UvcBmControlInfo> debugBmControls() => <UvcBmControlInfo>[];

  @override
  int? getControl(UvcControlId controlId) => null;

  @override
  int setControl(UvcControlId controlId, int value) => _notSupported;

  @override
  UvcWhiteBalanceComponent? getWhiteBalanceComponent() => null;

  @override
  int setWhiteBalanceComponent(UvcWhiteBalanceComponent value) => _notSupported;

  @override
  UvcFocusRelativeControl? getFocusRelativeControl() => null;

  @override
  int setFocusRelativeControl(UvcFocusRelativeControl value) => _notSupported;

  @override
  UvcZoomRelativeControl? getZoomRelativeControl() => null;

  @override
  int setZoomRelativeControl(UvcZoomRelativeControl value) => _notSupported;

  @override
  UvcPanTiltAbsoluteControl? getPanTiltAbsoluteControl() => null;

  @override
  int setPanTiltAbsoluteControl(UvcPanTiltAbsoluteControl value) =>
      _notSupported;

  @override
  UvcPanTiltRelativeControl? getPanTiltRelativeControl() => null;

  @override
  int setPanTiltRelativeControl(UvcPanTiltRelativeControl value) =>
      _notSupported;

  @override
  UvcRollRelativeControl? getRollRelativeControl() => null;

  @override
  int setRollRelativeControl(UvcRollRelativeControl value) => _notSupported;

  @override
  UvcDigitalWindowControl? getDigitalWindowControl() => null;

  @override
  int setDigitalWindowControl(UvcDigitalWindowControl value) => _notSupported;

  @override
  UvcRegionOfInterestControl? getRegionOfInterestControl() => null;

  @override
  int setRegionOfInterestControl(UvcRegionOfInterestControl value) =>
      _notSupported;

  /// The preview texture is shown unrotated; transforms are not applied yet,
  /// so this stays at [UvcPreviewTransform.identity].
  @override
  UvcPreviewTransform get previewTransform => UvcPreviewTransform.identity;

  @override
  void setPreviewTransform(UvcPreviewTransform transform) {}

  @override
  void rotatePreviewClockwise() {}

  @override
  void rotatePreviewCounterClockwise() {}

  @override
  void togglePreviewFlipHorizontal() {}

  @override
  void togglePreviewFlipVertical() {}
}
