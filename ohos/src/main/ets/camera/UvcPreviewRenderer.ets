// Flutter textures for the preview.
//
// Each texture is registered with the engine and its surface is opened for
// writing by the native code (src/main/cpp), which decodes the camera's frames
// and draws them there on its own threads. Frames are not handed to the engine
// as PixelMaps (TextureRegistry.setTextureBackGroundPixelMap): at video rate
// that call crashed inside the engine within seconds, on both its zero-copy
// and its copying path, because the raster thread was still reading the
// previous buffer.

import { TextureRegistry } from '@ohos/flutter_ohos';
import native from 'libflutter_ffi_uvc_preview.so';

export const NO_WINDOW: bigint = BigInt(0);

export class UvcPreviewRenderer {
  private registry: TextureRegistry;
  /** Texture id to the native window handle producing into its surface. */
  private windows: Map<number, bigint> = new Map<number, bigint>();
  private attachedTextureId: number = -1;

  constructor(registry: TextureRegistry) {
    this.registry = registry;
  }

  /** Throws when the texture's surface cannot be opened for writing. */
  createTexture(): number {
    const textureId = this.registry.getTextureId();
    const entry = this.registry.registerTexture(textureId);
    const window = native.createWindow(entry.getSurfaceId().toString());
    if (window === NO_WINDOW) {
      this.registry.unregisterTexture(textureId);
      throw new Error(`the surface of texture ${textureId} could not be opened`);
    }
    this.windows.set(textureId, window);
    return textureId;
  }

  /** Makes `textureId` the preview target. Returns its window, or NO_WINDOW for an unknown id. */
  attach(textureId: number): bigint {
    const window = this.windows.get(textureId);
    if (window === undefined) {
      return NO_WINDOW;
    }
    this.attachedTextureId = textureId;
    return window;
  }

  isAttached(textureId: number): boolean {
    return this.attachedTextureId === textureId;
  }

  /** The caller must have stopped drawing into the texture's window first. */
  disposeTexture(textureId: number): void {
    const window = this.windows.get(textureId);
    if (window === undefined) {
      return;
    }
    if (this.attachedTextureId === textureId) {
      this.attachedTextureId = -1;
    }
    this.windows.delete(textureId);
    native.destroyWindow(window);
    this.registry.unregisterTexture(textureId);
  }

  disposeAll(): void {
    const ids: number[] = Array.from(this.windows.keys());
    for (let i = 0; i < ids.length; i++) {
      this.disposeTexture(ids[i]);
    }
  }
}
