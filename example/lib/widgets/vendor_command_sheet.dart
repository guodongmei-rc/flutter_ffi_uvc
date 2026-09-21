import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_ffi_uvc/flutter_ffi_uvc.dart';

/// Modal bottom sheet for sending EP0 vendor control transfers.
///
/// Only bRequest and the payload are user-supplied; the remaining transfer
/// parameters follow the reference host script: OUT direction (0x41),
/// wValue = 0x60, wIndex = auto-detected vendor (0xFF) interface.
class VendorCommandSheet extends StatefulWidget {
  const VendorCommandSheet({super.key, required this.camera});

  final UvcCamera camera;

  @override
  State<VendorCommandSheet> createState() => _VendorCommandSheetState();
}

class _VendorCommandSheetState extends State<VendorCommandSheet> {
  static const int _defaultWValue = 0x60;

  final TextEditingController _commandController = TextEditingController();
  final TextEditingController _payloadController = TextEditingController();

  String? _resultText;
  bool _resultIsError = false;
  bool _sending = false;

  @override
  void dispose() {
    _commandController.dispose();
    _payloadController.dispose();
    super.dispose();
  }

  /// Parses a hex field: optional `0x` prefix.
  int? _parseHex(String text) {
    String value = text.trim();
    if (value.startsWith('0x') || value.startsWith('0X')) {
      value = value.substring(2);
    }
    if (value.isEmpty || !RegExp(r'^[0-9a-fA-F]+$').hasMatch(value)) {
      return null;
    }
    return int.tryParse(value, radix: 16);
  }

  String _toHex(List<int> bytes) => bytes
      .map((int b) => b.toRadixString(16).padLeft(2, '0'))
      .join(' ')
      .toUpperCase();

  void _fail(String message) {
    setState(() {
      _resultIsError = true;
      _resultText = message;
    });
  }

  void _send() {
    final int? command = _parseHex(_commandController.text);
    if (command == null || command > 0xFF) {
      _fail('bRequest: enter a hex byte (00-FF), e.g. 26');
      return;
    }
    final List<int> payload = utf8.encode(_payloadController.text);

    debugPrint(
      '[VendorCommand] OUT bmRequestType=0x41 '
      'bRequest=0x${command.toRadixString(16).padLeft(2, '0').toUpperCase()} '
      'wValue=0x${_defaultWValue.toRadixString(16).padLeft(4, '0').toUpperCase()} '
      'wIndex=auto payload="${_payloadController.text}" '
      'hex=[${_toHex(payload)}]',
    );

    setState(() {
      _sending = true;
      _resultText = null;
    });

    try {
      final int transferred = widget.camera.sendVendorCommand(
        command: command,
        wValue: _defaultWValue,
        payload: payload,
      );
      setState(() {
        _resultIsError = false;
        _resultText = payload.isEmpty
            ? 'Sent (no data stage)'
            : 'Sent $transferred byte(s): ${_toHex(payload)}';
      });
    } on UvcException catch (e) {
      final String detail = e.message.isNotEmpty
          ? e.message
          : widget.camera.lastError;
      _fail('Transfer failed (${e.nativeCode}): $detail');
    } catch (e) {
      _fail('$e');
    } finally {
      setState(() => _sending = false);
    }
  }

  InputDecoration _fieldDecoration(String label, String hint) {
    return InputDecoration(
      labelText: label,
      hintText: hint,
      isDense: true,
      border: const OutlineInputBorder(),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        bottom: MediaQuery.of(context).viewInsets.bottom,
      ),
      child: ClipRRect(
        borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
        child: Container(
          color: Theme.of(context).colorScheme.surface,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 10),
                child: Container(
                  width: 40,
                  height: 4,
                  decoration: BoxDecoration(
                    color: Colors.grey[400],
                    borderRadius: BorderRadius.circular(2),
                  ),
                ),
              ),
              const Padding(
                padding: EdgeInsets.symmetric(horizontal: 16),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    'Vendor command',
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                  ),
                ),
              ),
              const Divider(height: 1),
              Flexible(
                child: ListView(
                  shrinkWrap: true,
                  padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
                  children: <Widget>[
                    TextField(
                      controller: _commandController,
                      decoration: _fieldDecoration('bRequest (hex)', '26'),
                    ),
                    const SizedBox(height: 8),
                    TextField(
                      controller: _payloadController,
                      decoration: _fieldDecoration(
                        'Payload (string, may be empty)',
                        ' ',
                      ),
                    ),
                    const SizedBox(height: 16),
                    SizedBox(
                      width: double.infinity,
                      child: ElevatedButton.icon(
                        onPressed: _sending ? null : _send,
                        icon: const Icon(Icons.send, size: 18),
                        label: const Text('Send'),
                      ),
                    ),
                    if (_resultText != null) ...<Widget>[
                      const SizedBox(height: 12),
                      Container(
                        width: double.infinity,
                        padding: const EdgeInsets.all(12),
                        decoration: BoxDecoration(
                          color: _resultIsError
                              ? Colors.red[50]
                              : Colors.green[50],
                          borderRadius: BorderRadius.circular(8),
                          border: Border.all(
                            color: _resultIsError
                                ? Colors.red[200]!
                                : Colors.green[200]!,
                          ),
                        ),
                        child: Text(
                          _resultText!,
                          style: TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 13,
                            color: _resultIsError
                                ? Colors.red[900]
                                : Colors.green[900],
                          ),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
