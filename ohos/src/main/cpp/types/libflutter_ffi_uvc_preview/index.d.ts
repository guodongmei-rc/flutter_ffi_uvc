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
 * CLEAR_FEATURE(ENDPOINT_HALT) on an endpoint, resetting its data toggle.
 * This is how a bulk UVC camera is told that streaming stopped. Returns 0 or
 * a negative errno value.
 */
export const usbClearHalt: (fd: number, endpoint: number) => number;

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
 * to `bytesPerPacket` bytes, and showing its frames in `window` (0n: receive
 * without showing). Frames larger than `maxFrameBytes` are dropped. The
 * streaming interface must already be claimed and on its streaming alt
 * setting. `codec` says how to decode the frames (0 MJPEG, 1 H.264, 2 H.265,
 * 3 YUY2, 4 UYVY, 5 NV12); `width`
 * and `height` are the mode's picture size, which the H.264/H.265 hardware
 * decoder is configured with (ignored for MJPEG). With `bulk` = 1 the
 * endpoint is a bulk endpoint: `bytesPerPacket` is then the negotiated
 * dwMaxPayloadTransferSize and each transfer carries one payload.
 *
 * MJPEG frames are decoded on the CPU side of the system image decoder and
 * drawn into the window; H.264/H.265 frames go to the hardware video decoder,
 * which renders into the window itself, so they need a window to show
 * anything and only start at a keyframe; uncompressed frames are repacked to
 * NV12 and drawn like decoded MJPEG.
 *
 * Returns a positive stream id, or a negative errno value.
 */
export const streamStart: (
  fd: number, endpoint: number, bytesPerPacket: number, maxFrameBytes: number, window: bigint,
  codec: number, width: number, height: number, bulk: number) => number;

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
 * [34] H.264/H.265: frames the hardware decoder had no free input buffer for
 * [35] H.264/H.265: frames skipped while waiting for parameter sets or a keyframe
 */
export const streamStats: (stream: number) => number[];

/**
 * The last complete frame as JPEG: as the camera sent it for MJPEG streams,
 * encoded from the picture for uncompressed ones; undefined before the first
 * frame and always for H.264/H.265 streams (their pictures only exist inside
 * the hardware decoder).
 */
export const streamLatestJpeg: (stream: number) => ArrayBuffer | undefined;

/**
 * Starts recording the stream's decoded frames to an MP4 file at `path`
 * (H.264, no audio). Needs at least one decoded frame first. Returns 0 or a
 * negative error code: -20 already recording, -21 encoder, -22 muxer,
 * -23 file, -24 no frames yet, -25 frames are not NV12.
 */
export const recordStart: (stream: number, path: string, fps: number, bitRate: number) => number;

/**
 * Like recordStart, with the camera's microphone recorded as an AAC track.
 * The AudioStreaming interface must already be claimed, on its streaming alt
 * setting and configured (sampling frequency, unmute); `endpoint`,
 * `packetSize`, `sampleRate`, `channels` and `bits` describe its isochronous
 * IN endpoint and PCM format. With `knownSampleRate` 0 the real sample rate
 * is measured over the first 300 ms (the call blocks for that long) and used
 * when it snaps to a standard rate; a positive `knownSampleRate` — the value
 * an earlier call returned for this connection — is used as is, without
 * measuring. Returns the sample rate the audio track uses (> 0), or a
 * negative error code without starting the recording: -27 when the
 * microphone is starved (no standard rate and under a quarter of
 * `sampleRate`, or nothing at all), -28 when its capture cannot be started.
 * The caller may then record without audio.
 */
export const recordStartWithAudio: (
  stream: number, path: string, fps: number, bitRate: number,
  fd: number, endpoint: number, packetSize: number, sampleRate: number, channels: number, bits: number,
  knownSampleRate: number) => number;

/**
 * Like recordStart, with the camera's microphone recorded through the system
 * audio framework instead of its USB endpoint: the fallback for a camera
 * whose direct capture is starved. `productName` is the camera's USB product
 * name, used to recognise its audio input when the system reports it under a
 * device type of its own. With `awaitFirstData` 1 the call waits up to a
 * second for PCM. Needs ohos.permission.MICROPHONE. Returns the sample rate
 * the audio track uses (> 0), or a negative error code without starting the
 * recording: -29 when the system lists no USB audio input it would record
 * from, -28 when the capturer cannot be started (also without the
 * permission), -30 when it delivers nothing.
 */
export const recordStartWithSystemMic: (
  stream: number, path: string, fps: number, bitRate: number, productName: string, awaitFirstData: number) => number;

/** 1 when the system lists the camera's microphone as the audio input recordings use, else 0. */
export const systemMicAvailable: (productName: string) => number;

/**
 * The sample rate the system's audio driver streams the camera's microphone
 * at, or 0 when the system lists no audio input for it.
 */
export const systemMicSampleRate: (productName: string) => number;

/**
 * Hands a released interface back to the kernel so that its driver binds
 * again (the system's USB audio driver, for a microphone). Returns 0 or a
 * negative errno.
 */
export const usbAttachKernelDriver: (fd: number, interfaceNumber: number) => number;

/** Finishes the recording. Returns the number of frames written, or a negative error code. */
export const recordStop: (stream: number) => number;
