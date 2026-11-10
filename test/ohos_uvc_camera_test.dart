import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_ffi_uvc/flutter_ffi_uvc.dart';
import 'package:flutter_ffi_uvc/src/ohos/ohos_uvc_camera.dart';
import 'package:flutter_test/flutter_test.dart';

const MethodChannel _channel = MethodChannel('flutter_ffi_uvc/ohos');
const EventChannel _events = EventChannel('flutter_ffi_uvc/ohos_events');

const Map<String, Object?> _mode1080p60 = <String, Object?>{
  'format': 7,
  'formatName': 'MJPEG',
  'width': 1920,
  'height': 1080,
  'fps': 60,
};

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final TestDefaultBinaryMessenger messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  late List<MethodCall> calls;
  late Map<String, Object? Function(MethodCall call)> replies;
  MockStreamHandlerEventSink? eventSink;
  late OhosUvcCamera camera;

  setUp(() {
    calls = <MethodCall>[];
    replies = <String, Object? Function(MethodCall call)>{
      'openUsbDevice': (_) => <String, Object?>{
        'result': 0,
        'lastError': '',
        'modes': <Object?>[_mode1080p60],
      },
      'startPreview': (_) => <String, Object?>{
        'success': true,
        'validFrameCount': 3,
        'consecutiveValidFrames': 3,
        'errorCount': 0,
        'elapsedMs': 420,
        'lastError': '',
        'nativeErrorCode': 0,
      },
    };
    messenger.setMockMethodCallHandler(_channel, (MethodCall call) async {
      calls.add(call);
      return replies[call.method]?.call(call);
    });
    eventSink = null;
    messenger.setMockStreamHandler(
      _events,
      MockStreamHandler.inline(
        onListen: (Object? arguments, MockStreamHandlerEventSink sink) {
          eventSink = sink;
        },
      ),
    );
    camera = OhosUvcCamera();
  });

  tearDown(() {
    messenger.setMockMethodCallHandler(_channel, null);
    messenger.setMockStreamHandler(_events, null);
  });

  test('lists the devices reported by the platform', () async {
    replies['listUsbDevices'] = (_) => <Object?>[
      <String, Object?>{
        'deviceId': 1003,
        'deviceName': '1-3',
        'vendorId': 0x1d6b,
        'productId': 4,
        'productName': 'Camera',
        'manufacturerName': 'Vendor',
        'serialNumber': '',
        'hasPermission': true,
      },
    ];

    final List<UvcUsbDevice> devices = await camera.listUsbDevices();

    expect(devices, hasLength(1));
    expect(devices.single.deviceId, 1003);
    expect(devices.single.productName, 'Camera');
    expect(devices.single.hasPermission, isTrue);
  });

  test('opening a device makes its modes available synchronously', () async {
    expect(camera.supportedModes(), isEmpty);

    final int result = await camera.openUsbDevice(1003);

    expect(result, 0);
    expect(calls.single.method, 'openUsbDevice');
    expect(calls.single.arguments, <String, Object?>{'deviceId': 1003});
    expect(camera.supportedModes(), <UvcCameraMode>[
      UvcCameraMode.fromJson(Map<String, dynamic>.from(_mode1080p60)),
    ]);
  });

  test('a failed open reports the platform error and no modes', () async {
    replies['openUsbDevice'] = (_) => <String, Object?>{
      'result': -3,
      'lastError': 'USB access was not granted',
      'modes': <Object?>[],
    };

    expect(await camera.openUsbDevice(1003), -3);
    expect(camera.lastError, 'USB access was not granted');
    expect(camera.supportedModes(), isEmpty);
  });

  test('closing the device clears the cached modes', () async {
    await camera.openUsbDevice(1003);
    await camera.closeUsbDevice();

    expect(calls.last.method, 'closeUsbDevice');
    expect(camera.supportedModes(), isEmpty);
    expect(camera.isPreviewing, isFalse);
  });

  test('startPreview forwards the mode and verification settings', () async {
    await camera.openUsbDevice(1003);
    final UvcCameraMode mode = camera.supportedModes().single;

    final UvcPreviewStartResult result = await camera.startPreview(
      mode,
      consecutiveValidFrames: 5,
      timeout: const Duration(seconds: 4),
    );

    expect(calls.last.method, 'startPreview');
    expect(calls.last.arguments, <String, Object?>{
      'format': 7,
      'width': 1920,
      'height': 1080,
      'fps': 60,
      'consecutiveValidFrames': 5,
      'timeoutMs': 4000,
      'sequenceOnly': false,
    });
    expect(result.success, isTrue);
    expect(result.mode, mode);
    expect(result.validFrameCount, 3);
    expect(result.elapsed, const Duration(milliseconds: 420));
    expect(result.lastError, isNull);
    expect(camera.isPreviewing, isTrue);
  });

  test('the result reports the frame rate the device committed to', () async {
    replies['startPreview'] = (_) => <String, Object?>{
      'success': true,
      'validFrameCount': 3,
      'consecutiveValidFrames': 3,
      'errorCount': 0,
      'elapsedMs': 400,
      'lastError': '',
      'nativeErrorCode': 0,
      'negotiatedFps': 60,
    };
    await camera.openUsbDevice(1003);
    final UvcCameraMode listed = camera.supportedModes().single;
    final UvcCameraMode requested = UvcCameraMode(
      frameFormat: listed.frameFormat,
      formatName: listed.formatName,
      width: listed.width,
      height: listed.height,
      fps: 25,
    );

    final UvcPreviewStartResult result = await camera.startPreview(requested);

    expect(result.success, isTrue);
    expect(result.mode.fps, 60);
    expect(result.mode.width, requested.width);
    expect(result.mode.formatName, requested.formatName);
  });

  test('queryAudioInterface reports the microphone format or null', () async {
    await camera.openUsbDevice(1003);
    expect(await camera.queryAudioInterface(), isNull);
    expect(calls.last.method, 'queryAudioInterface');

    replies['queryAudioInterface'] = (_) => <String, Object?>{
      'sampleRate': 48000,
      'channels': 1,
      'bitsPerSample': 16,
    };
    final UvcAudioInfo? audio = await camera.queryAudioInterface();
    expect(audio, isNotNull);
    expect(audio!.sampleRate, 48000);
    expect(audio.channels, 1);
    expect(audio.bitsPerSample, 16);
  });

  test('a preview that fails verification is reported as not running', () async {
    replies['startPreview'] = (_) => <String, Object?>{
      'success': false,
      'validFrameCount': 0,
      'consecutiveValidFrames': 0,
      'errorCount': 1,
      'elapsedMs': 2000,
      'lastError': 'no valid frame within 2000 ms',
      'nativeErrorCode': 0,
    };
    await camera.openUsbDevice(1003);

    final UvcPreviewStartResult result = await camera.startPreview(
      camera.supportedModes().single,
    );

    expect(result.success, isFalse);
    expect(result.lastError, 'no valid frame within 2000 ms');
    expect(camera.lastError, 'no valid frame within 2000 ms');
    expect(camera.isPreviewing, isFalse);
  });

  test('startPreviewAuto stops at the first mode that verifies', () async {
    int attempt = 0;
    replies['startPreview'] = (MethodCall call) {
      attempt++;
      return <String, Object?>{
        'success': attempt == 2,
        'validFrameCount': attempt == 2 ? 3 : 0,
        'consecutiveValidFrames': attempt == 2 ? 3 : 0,
        'errorCount': 0,
        'elapsedMs': 100,
        'lastError': attempt == 2 ? '' : 'no frames',
        'nativeErrorCode': 0,
      };
    };
    const UvcCameraMode small = UvcCameraMode(
      frameFormat: 7,
      formatName: 'MJPEG',
      width: 1280,
      height: 720,
      fps: 30,
    );
    const UvcCameraMode large = UvcCameraMode(
      frameFormat: 7,
      formatName: 'MJPEG',
      width: 1920,
      height: 1080,
      fps: 60,
    );
    const UvcCameraMode unused = UvcCameraMode(
      frameFormat: 7,
      formatName: 'MJPEG',
      width: 3840,
      height: 2160,
      fps: 30,
    );

    final UvcAutoPreviewResult result = await camera.startPreviewAuto(
      candidates: <UvcCameraMode>[small, large, unused],
    );

    expect(result.success, isTrue);
    expect(result.mode, large);
    expect(result.attempts, hasLength(2));
    expect(camera.isPreviewing, isTrue);
  });

  test('stopPreview stops the platform stream', () async {
    await camera.openUsbDevice(1003);
    await camera.startPreview(camera.supportedModes().single);

    camera.stopPreview();
    await pumpEventQueue();

    expect(camera.isPreviewing, isFalse);
    expect(calls.last.method, 'stopPreview');
  });

  test('stats events update the frame sequence and stream statistics', () async {
    await camera.openUsbDevice(1003);
    await camera.startPreview(camera.supportedModes().single);
    expect(camera.latestFrameSequence(), 0);

    eventSink!.success(<String, Object?>{
      'type': 'stats',
      'sequence': 42,
      'inputFrameCount': 45,
      'deliveredFrameCount': 42,
      'decodeSuccessCount': 42,
      'decodeFailureCount': 1,
      'callbackLockDropCount': 2,
      'undersizedFrameCount': 0,
      'invalidMjpegCount': 1,
      'inputFps': 36,
      'deliveredFps': 33.5,
      'avgInterFrameGapMs': 29.8,
      'p95InterFrameGapMs': 40,
      'maxInterFrameGapMs': 55,
      'firstFrameLatencyMs': 180,
      'elapsedMs': 1250,
    });
    await pumpEventQueue();

    expect(camera.latestFrameSequence(), 42);
    final UvcStreamStats stats = camera.getStreamStats();
    expect(stats.deliveredFrameCount, 42);
    expect(stats.callbackLockDropCount, 2);
    expect(stats.inputFps, 36.0);
    expect(stats.deliveredFps, 33.5);
    expect(stats.elapsed, const Duration(milliseconds: 1250));
  });

  test('platform stream errors are delivered on streamErrors', () async {
    final List<UvcStreamError> errors = <UvcStreamError>[];
    final StreamSubscription<UvcStreamError> sub = camera.streamErrors.listen(
      errors.add,
    );
    await pumpEventQueue();

    eventSink!.success(<String, Object?>{
      'type': 'streamError',
      'message': 'MJPEG decode failed',
    });
    await pumpEventQueue();

    expect(errors.single.message, 'MJPEG decode failed');
    expect(camera.lastError, 'MJPEG decode failed');
    await sub.cancel();
  });

  test('a preview stopped by the platform is no longer previewing', () async {
    await camera.openUsbDevice(1003);
    await camera.startPreview(camera.supportedModes().single);

    eventSink!.success(<String, Object?>{
      'type': 'previewStopped',
      'reason': 'device disconnected',
    });
    await pumpEventQueue();

    expect(camera.isPreviewing, isFalse);
    expect(camera.lastError, 'device disconnected');
  });

  test('USB attach and detach broadcasts arrive on deviceEvents', () async {
    final List<UvcDeviceEvent> events = <UvcDeviceEvent>[];
    final StreamSubscription<UvcDeviceEvent> sub = camera.deviceEvents.listen(
      events.add,
    );
    await pumpEventQueue();

    eventSink!.success(<String, Object?>{
      'type': 'device',
      'event': 'attached',
      'device': <String, Object?>{
        'deviceId': 1003,
        'deviceName': '1-3',
        'vendorId': 0x1d6b,
        'productId': 4,
        'productName': 'Camera',
        'manufacturerName': 'Vendor',
        'serialNumber': '',
        'hasPermission': true,
      },
    });
    eventSink!.success(<String, Object?>{
      'type': 'device',
      'event': 'detached',
      'device': <String, Object?>{'deviceId': 1003, 'deviceName': '1-3'},
    });
    await pumpEventQueue();

    expect(events, hasLength(2));
    expect(events[0].type, UvcDeviceEventType.attached);
    expect(events[0].device.deviceId, 1003);
    expect(events[0].device.productName, 'Camera');
    expect(events[1].type, UvcDeviceEventType.detached);
    expect(events[1].device.deviceId, 1003);
    await sub.cancel();
  });

  test('preview textures go through the platform channel', () async {
    replies['createPreviewTexture'] = (_) => 7;

    final int textureId = await camera.createPreviewTexture();
    await camera.attachPreviewTexture(textureId, width: 1920, height: 1080);
    await camera.disposePreviewTexture(textureId);

    expect(textureId, 7);
    expect(calls[1].method, 'attachPreviewTexture');
    expect(calls[1].arguments, <String, Object?>{
      'textureId': 7,
      'width': 1920,
      'height': 1080,
    });
    expect(calls[2].method, 'disposePreviewTexture');
  });

  test('features without a HarmonyOS implementation report notSupported', () async {
    expect(camera.supportedControls(), isEmpty);
    expect(camera.getControl(UvcControlId.brightness), isNull);
    expect(
      camera.setControl(UvcControlId.brightness, 1),
      UvcErrorCode.notSupported.nativeValue,
    );
    expect(camera.copyLatestFrame(), isNull);
    expect(await camera.queryAudioInterface(), isNull);
    expect(camera.openFd(3), UvcErrorCode.notSupported.nativeValue);
  });

  test('photos and recordings are saved through the platform channel', () async {
    replies['takePicture'] = (_) => <String, Object?>{
      'path': '/data/app/files/DCIM/UVC_1.jpg',
      'uri': null,
    };
    replies['startVideoRecording'] = (_) => <String, Object?>{
      'path': '/data/app/files/DCIM/UVC_2.mp4',
      'uri': null,
    };
    replies['stopVideoRecording'] = (_) => <String, Object?>{
      'path': '/data/app/files/DCIM/UVC_2.mp4',
      'uri': null,
    };

    final UvcGalleryMedia photo = await camera.takePicture();
    expect(photo.path, '/data/app/files/DCIM/UVC_1.jpg');
    expect(photo.uri, isNull);

    expect(camera.isVideoRecording, isFalse);
    await camera.startVideoRecording(bitRate: 8000000, frameRate: 30);
    expect(camera.isVideoRecording, isTrue);
    expect(calls.last.arguments, <String, Object?>{
      'bitRate': 8000000,
      'frameRate': 30,
    });
    final UvcGalleryMedia video = await camera.stopVideoRecording();
    expect(camera.isVideoRecording, isFalse);
    expect(video.path, '/data/app/files/DCIM/UVC_2.mp4');
  });

  test('a failed capture surfaces the platform error', () async {
    messenger.setMockMethodCallHandler(_channel, (MethodCall call) async {
      throw PlatformException(code: 'capture', message: 'no preview is running');
    });

    await expectLater(camera.takePicture(), throwsA(isA<PlatformException>()));
    expect(camera.lastError, 'no preview is running');
  });

  test('debug logging is switched on from the debug level up', () async {
    camera.setLogLevel(UvcLogLevel.info);
    camera.setLogLevel(UvcLogLevel.debug);
    await pumpEventQueue();

    expect(calls.map((MethodCall call) => call.arguments), <Object?>[
      <String, Object?>{'verbose': false},
      <String, Object?>{'verbose': true},
    ]);
  });

  test('the preview transform stays at identity', () {
    camera.rotatePreviewClockwise();
    camera.togglePreviewFlipHorizontal();

    expect(camera.previewTransform, UvcPreviewTransform.identity);
  });
}
