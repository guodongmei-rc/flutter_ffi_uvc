// Synthetic descriptor builder for host tests.
//
// This is NOT a capture from a real camera. It is assembled field by field to
// match what the research note recorded for the RunCam in UVC mode (interface
// 0 = VideoControl with interrupt endpoint 0x83, interface 1 = VideoStreaming
// with isochronous endpoint 0x81 and wMaxPacketSize 800 / 3040 / 4992 / 5120).
// Format and frame contents are invented so the parser has something to read.
// Replace with a real getRawDescriptor() dump once the probe has run on device.

export function le16(v: number): number[] {
  return [v & 0xff, (v >> 8) & 0xff];
}

export function le32(v: number): number[] {
  return [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];
}

function withLength(body: number[]): number[] {
  return [body.length + 1, ...body];
}

export function deviceDescriptor(): number[] {
  return withLength([
    0x01, ...le16(0x0200), 0xef, 0x02, 0x01, 64,
    ...le16(0x1d6b), ...le16(0x0004), ...le16(0x0100), 1, 2, 0, 1,
  ]);
}

export function interfaceDescriptor(
  num: number, alt: number, numEndpoints: number, subClass: number,
): number[] {
  return withLength([0x04, num, alt, numEndpoints, 0x0e, subClass, 0x00, 0]);
}

export function endpointDescriptor(
  address: number, attributes: number, wMaxPacketSize: number, interval: number,
): number[] {
  return withLength([0x05, address, attributes, ...le16(wMaxPacketSize), interval]);
}

export function vcHeader(bcdUVC: number): number[] {
  // bLength 13: one streaming interface (number 1) in the collection.
  return withLength([
    0x24, 0x01, ...le16(bcdUVC), ...le16(13), ...le32(48000000), 1, 1,
  ]);
}

export function vsInputHeader(numFormats: number, endpointAddress: number): number[] {
  return withLength([
    0x24, 0x01, numFormats, ...le16(0), endpointAddress, 0, 2, 0, 0, 0, 1, 0,
  ]);
}

export function mjpegFormat(formatIndex: number, numFrames: number): number[] {
  return withLength([0x24, 0x06, formatIndex, numFrames, 1, 1, 0, 0, 0, 0]);
}

/** Frame descriptor shared by MJPEG (0x07) and uncompressed (0x05) formats. */
export function mjpegFrame(
  frameIndex: number, width: number, height: number, intervals: number[],
  subtype: number = 0x07,
): number[] {
  const body = [
    0x24, subtype, frameIndex, 0, ...le16(width), ...le16(height),
    ...le32(1000000), ...le32(50000000), ...le32(width * height * 2),
    ...le32(intervals[0]), intervals.length,
  ];
  for (const interval of intervals) {
    body.push(...le32(interval));
  }
  return withLength(body);
}

export function guidFromFourcc(fourcc: string): number[] {
  const tail = [0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71];
  return [
    fourcc.charCodeAt(0), fourcc.charCodeAt(1), fourcc.charCodeAt(2), fourcc.charCodeAt(3),
    ...tail,
  ];
}

export function frameBasedFormat(formatIndex: number, numFrames: number, fourcc: string): number[] {
  return withLength([
    0x24, 0x10, formatIndex, numFrames, ...guidFromFourcc(fourcc), 16, 1, 0, 0, 0, 0, 1,
  ]);
}

export function frameBasedFrame(
  frameIndex: number, width: number, height: number, intervals: number[],
): number[] {
  const body = [
    0x24, 0x11, frameIndex, 0, ...le16(width), ...le16(height),
    ...le32(1000000), ...le32(50000000), ...le32(intervals[0]), intervals.length, ...le32(0),
  ];
  for (const interval of intervals) {
    body.push(...le32(interval));
  }
  return withLength(body);
}

/** Device descriptor followed by one configuration, as a flat byte array. */
export function runcamLikeDescriptor(bcdUVC: number = 0x0100): Uint8Array {
  const functions: number[] = [
    // IAD: first interface 0, two interfaces, video class.
    ...withLength([0x0b, 0, 2, 0x0e, 0x03, 0x00, 0]),
    ...interfaceDescriptor(0, 0, 1, 0x01),
    ...vcHeader(bcdUVC),
    ...endpointDescriptor(0x83, 0x03, 16, 8),
    ...interfaceDescriptor(1, 0, 0, 0x02),
    ...vsInputHeader(2, 0x81),
    ...mjpegFormat(1, 2),
    ...mjpegFrame(1, 1920, 1080, [333333]),
    ...mjpegFrame(2, 1280, 720, [333333, 166666]),
    ...frameBasedFormat(2, 1, 'H264'),
    ...frameBasedFrame(1, 1280, 720, [333333]),
    ...interfaceDescriptor(1, 1, 1, 0x02),
    ...endpointDescriptor(0x81, 0x05, 800, 1),
    ...interfaceDescriptor(1, 2, 1, 0x02),
    ...endpointDescriptor(0x81, 0x05, 3040, 1),
    ...interfaceDescriptor(1, 3, 1, 0x02),
    ...endpointDescriptor(0x81, 0x05, 4992, 1),
    ...interfaceDescriptor(1, 4, 1, 0x02),
    ...endpointDescriptor(0x81, 0x05, 5120, 1),
  ];
  const config = [
    9, 0x02, ...le16(9 + functions.length), 2, 1, 0, 0x80, 250, ...functions,
  ];
  return new Uint8Array([...deviceDescriptor(), ...config]);
}
