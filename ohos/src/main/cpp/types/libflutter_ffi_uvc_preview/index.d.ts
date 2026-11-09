// Native half of the HarmonyOS camera path. See the headers in src/main/cpp.

/**
 * Opens the producer side of the surface with this id (decimal string).
 * Returns a window handle, or 0n when the surface could not be opened.
 */
export const createWindow: (surfaceId: string) => bigint;

/** Closes a handle returned by createWindow. */
export const destroyWindow: (window: bigint) => void;

// USB requests on a descriptor from usbManager.getFileDescriptor(). Each
// returns a negative errno value on failure.

export const usbClaimInterface: (fd: number, interfaceNumber: number) => number;

export const usbReleaseInterface: (fd: number, interfaceNumber: number) => number;

export const usbSetInterface: (fd: number, interfaceNumber: number, altSetting: number) => number;

/**
 * One control transfer; `data` is sent or filled depending on the direction
 * bit of `requestType`, and its size is the transfer's wLength. Returns the
 * number of bytes transferred. Blocks for at most `timeoutMs`.
 */
export const usbControlTransfer: (
  fd: number, requestType: number, request: number, value: number, index: number, data: ArrayBuffer,
  timeoutMs: number) => number;

/** Closes the descriptor. */
export const usbClose: (fd: number) => void;

/**
 * Starts receiving the isochronous stream on `endpoint`, with packets of up
 * to `bytesPerPacket` bytes, and showing its MJPEG frames in `window` (0n:
 * decode only). Frames larger than `maxFrameBytes` are dropped. The streaming
 * interface must already be claimed and on its streaming alt setting.
 *
 * Returns a positive stream id, or a negative errno value.
 */
export const streamStart: (
  fd: number, endpoint: number, bytesPerPacket: number, maxFrameBytes: number, window: bigint) => number;

/** Changes the window a running stream draws into; 0n stops drawing. */
export const streamSetWindow: (stream: number, window: bigint) => void;

/** Cancels the transfers and frees the stream. Blocks until they are cancelled. */
export const streamStop: (stream: number) => void;

/**
 * Counters of a stream, empty for an unknown id:
 *  [0] running (1/0)             [1] errno that stopped it, or 0
 *  [2] transfers completed       [3] packets
 *  [4] failed packets            [5] empty packets
 *  [6] rejected submits          [7] frames assembled
 *  [8] frame ends by EOF         [9] frame ends by FID toggle
 * [10] frames dropped incomplete [11] bytes of the last frame
 * [12] frames that were not JPEG [13] frames shown
 * [14] decode/draw failures      [15] frames replaced before being decoded
 * [16] decode ms, total          [17] draw ms, total
 * [18] ms to the first frame (-1 before it)
 * [19] average, [20] 95th percentile, [21] maximum ms between shown frames
 * [22] elapsed ms
 * [23..27] frames arriving and [28..32] frames shown, counted by the gap to
 *          the previous one: <=22, <=30, <=45, <=70, longer (ms)
 * [33] 1 when frames are decoded to NV12, 0 for RGBA
 */
export const streamStats: (stream: number) => number[];
