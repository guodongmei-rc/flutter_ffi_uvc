import 'dart:async';
import 'dart:io';

import 'package:flutter_ffi_uvc/flutter_ffi_uvc.dart';
import 'package:flutter_test/flutter_test.dart';

// ---------------------------------------------------------------------------
// Fake camera that records the withAudio argument of startVideoRecording.
// ---------------------------------------------------------------------------

class _FakeCamera implements UvcCamera {
  bool? lastWithAudio;

  @override
  Future<void> startVideoRecording({
    int? bitRate,
    int? frameRate,
    bool withAudio = true,
  }) async {
    lastWithAudio = withAudio;
  }

  // --- unused stubs ---
  @override
  Stream<UvcStreamError> get streamErrors => const Stream.empty();
  @override
  void setLogLevel(UvcLogLevel level) {}
  @override
  Future<bool> ensureCameraPermission() async => false;
  @override
  Future<bool> ensureGalleryPermission() async => false;
  @override
  Future<UvcGalleryMedia> takePicture({int quality = 90}) async =>
      const UvcGalleryMedia();
  @override
  Future<UvcGalleryMedia> stopVideoRecording() async =>
      const UvcGalleryMedia();
  @override
  bool get isVideoRecording => false;
  @override
  Future<UvcAudioInfo?> queryAudioInterface() async => null;
  @override
  int startImuCapture() => -1;
  @override
  void stopImuCapture() {}
  @override
  bool get isImuCaptureRunning => false;
  @override
  Future<List<UvcUsbDevice>> listUsbDevices() async => const [];
  @override
  Future<int> openUsbDevice(int deviceId) async => -1;
  @override
  Future<void> closeUsbDevice() async {}
  @override
  int openFd(int fd) => -1;
  @override
  int openPreview(UvcCameraMode mode) => -1;
  @override
  Future<UvcPreviewStartResult> startPreview(
    UvcCameraMode mode, {
    UvcPreviewPolicy policy = UvcPreviewPolicy.stableFrames,
    int consecutiveValidFrames = 3,
    Duration timeout = const Duration(seconds: 2),
  }) async => UvcPreviewStartResult(
    mode: mode,
    success: false,
    validFrameCount: 0,
    consecutiveValidFrames: 0,
    errorCount: 0,
    elapsed: Duration.zero,
  );
  @override
  Future<UvcAutoPreviewResult> startPreviewAuto({
    List<UvcCameraMode>? candidates,
    UvcAutoPreviewPreference preference = UvcAutoPreviewPreference.reliability,
    UvcPreviewPolicy policy = UvcPreviewPolicy.stableFrames,
    int consecutiveValidFrames = 3,
    Duration perModeTimeout = const Duration(seconds: 2),
    int maxCandidates = 8,
  }) async => const UvcAutoPreviewResult(attempts: []);
  @override
  Stream<UvcDeviceEvent> get deviceEvents => const Stream.empty();
  @override
  Stream<UvcStallEvent> get stallEvents => const Stream.empty();
  @override
  void enableStallDetection([
    UvcStallDetectionConfig config = const UvcStallDetectionConfig(),
  ]) {}
  @override
  void disableStallDetection() {}
  @override
  void stopPreview() {}
  @override
  void closeFd() {}
  @override
  @Deprecated('Use closeFd() instead.')
  void closeDevice() {}
  @override
  bool get isPreviewing => false;
  @override
  String get lastError => '';
  @override
  UvcPreviewFrame? copyLatestFrame() => null;
  @override
  UvcPreviewFrame? copyLatestFrameTransformed(UvcPreviewTransform transform) => null;
  @override
  int latestFrameSequence() => 0;
  @override
  UvcStreamStats getStreamStats() => const UvcStreamStats.zero();
  @override
  Future<int> createPreviewTexture() async => -1;
  @override
  Future<void> disposePreviewTexture(int textureId) async {}
  @override
  Future<void> attachPreviewTexture(int textureId,
      {int? width, int? height}) async {}
  @override
  List<UvcCameraControl> supportedControls() => const [];
  @override
  List<UvcBmControlInfo> debugBmControls() => const [];
  @override
  int? getControl(UvcControlId controlId) => null;
  @override
  int setControl(UvcControlId controlId, int value) => -1;
  @override
  UvcWhiteBalanceComponent? getWhiteBalanceComponent() => null;
  @override
  int setWhiteBalanceComponent(UvcWhiteBalanceComponent value) => -1;
  @override
  UvcFocusRelativeControl? getFocusRelativeControl() => null;
  @override
  int setFocusRelativeControl(UvcFocusRelativeControl value) => -1;
  @override
  UvcZoomRelativeControl? getZoomRelativeControl() => null;
  @override
  int setZoomRelativeControl(UvcZoomRelativeControl value) => -1;
  @override
  UvcPanTiltAbsoluteControl? getPanTiltAbsoluteControl() => null;
  @override
  int setPanTiltAbsoluteControl(UvcPanTiltAbsoluteControl value) => -1;
  @override
  UvcPanTiltRelativeControl? getPanTiltRelativeControl() => null;
  @override
  int setPanTiltRelativeControl(UvcPanTiltRelativeControl value) => -1;
  @override
  UvcRollRelativeControl? getRollRelativeControl() => null;
  @override
  int setRollRelativeControl(UvcRollRelativeControl value) => -1;
  @override
  UvcDigitalWindowControl? getDigitalWindowControl() => null;
  @override
  int setDigitalWindowControl(UvcDigitalWindowControl value) => -1;
  @override
  UvcRegionOfInterestControl? getRegionOfInterestControl() => null;
  @override
  int setRegionOfInterestControl(UvcRegionOfInterestControl value) => -1;
  @override
  List<UvcCameraMode> supportedModes() => const [];
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void main() {
  group('startVideoRecording withAudio', () {
    test('defaults to true and passes explicit values through', () async {
      final _FakeCamera camera = _FakeCamera();

      await camera.startVideoRecording();
      expect(camera.lastWithAudio, isTrue);

      await camera.startVideoRecording(withAudio: false);
      expect(camera.lastWithAudio, isFalse);

      await camera.startVideoRecording(withAudio: true);
      expect(camera.lastWithAudio, isTrue);
    });

    test('is accepted on the shared service and still platform-guarded', () {
      if (Platform.isAndroid) {
        return;
      }

      expect(
        () => uvcCamera.startVideoRecording(withAudio: false),
        throwsA(isA<UnsupportedError>()),
      );
      expect(
        () => uvcCamera.startVideoRecording(withAudio: true),
        throwsA(isA<UnsupportedError>()),
      );
    });
  });

  group('queryAudioInterface', () {
    test('is platform-guarded on the shared service', () {
      if (Platform.isAndroid) {
        return;
      }

      expect(
        () => uvcCamera.queryAudioInterface(),
        throwsA(isA<UnsupportedError>()),
      );
    });
  });
}
