import 'dart:io';

import 'package:flutter_ffi_uvc/flutter_ffi_uvc.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('vendor command argument validation', () {
    test('sendVendorCommand rejects an out-of-range command', () {
      expect(
        () => uvcCamera.sendVendorCommand(command: -1),
        throwsA(isA<ArgumentError>()),
      );
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x100),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('sendVendorCommand rejects an out-of-range wValue', () {
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26, wValue: -1),
        throwsA(isA<ArgumentError>()),
      );
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26, wValue: 0x10000),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('sendVendorCommand rejects an out-of-range wIndex', () {
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26, wIndex: -1),
        throwsA(isA<ArgumentError>()),
      );
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26, wIndex: 0x10000),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('sendVendorCommand rejects non-byte payload values', () {
      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26, payload: <int>[256]),
        throwsA(isA<ArgumentError>()),
      );
      expect(
        () =>
            uvcCamera.sendVendorCommand(command: 0x26, payload: <int>[0, -1]),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('queryVendorCommand rejects an out-of-range command', () {
      expect(
        () => uvcCamera.queryVendorCommand(command: 0x100, length: 1),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('queryVendorCommand rejects an out-of-range length', () {
      expect(
        () => uvcCamera.queryVendorCommand(command: 0x26, length: 0),
        throwsA(isA<ArgumentError>()),
      );
      expect(
        () => uvcCamera.queryVendorCommand(command: 0x26, length: 0x10000),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('boundary values pass argument validation', () {
      if (Platform.isAndroid) {
        return;
      }

      // Valid arguments reach the platform guard, not ArgumentError.
      expect(
        () => uvcCamera.sendVendorCommand(
          command: 0xFF,
          wValue: 0xFFFF,
          wIndex: 0xFFFF,
          payload: const <int>[0, 255],
        ),
        throwsA(isA<UnsupportedError>()),
      );
      expect(
        () => uvcCamera.queryVendorCommand(
          command: 0,
          wIndex: null,
          length: 0xFFFF,
        ),
        throwsA(isA<UnsupportedError>()),
      );
    });
  });

  group('vendor command platform guard', () {
    test('fails explicitly on unsupported host platforms', () {
      if (Platform.isAndroid) {
        return;
      }

      expect(
        () => uvcCamera.sendVendorCommand(command: 0x26),
        throwsA(isA<UnsupportedError>()),
      );
      expect(
        () => uvcCamera.queryVendorCommand(command: 0x26, length: 64),
        throwsA(isA<UnsupportedError>()),
      );
    });
  });
}
