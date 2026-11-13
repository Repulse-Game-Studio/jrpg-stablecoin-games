/**
 * Utils (rmmz_core.js), from MZ's documented behaviour. `Utils.isNwjs()` and
 * `Utils.isMobileDevice()` answer from the shim's options (MZ looks for `require`/`process` and at
 * the user agent; in Node the real check would always say NW.js).
 *
 * Encrypted deployments (the editor's "Encrypt image files"): Scene_Boot hands the database's
 * settings to `setEncryptionInfo`; `hasEncryptedImages()` answers from them, and
 * `decryptArrayBuffer(data)` turns a shipped `<file>_` back into the file. Modelled format, written
 * from the format's public descriptions (UNVERIFIED against a real deployment): a 16-byte header
 * (ENCRYPTION_HEADER: "RPGMV", zero padding, a version) before the file, whose first 16 bytes are
 * XORed with the 16 bytes of the 32-hex-digit key; the rest is stored as it is. A wrong header
 * throws. `encryptForDeployment` (not part of MZ) makes such a file for tests.
 */
import type { Ctx } from "./context.ts";

/** The 16 bytes an encrypted MZ file starts with (modelled; see the header). */
export const ENCRYPTION_HEADER: readonly number[] = [
  0x52, 0x50, 0x47, 0x4d, 0x56, 0, 0, 0, 0, 0x03, 0x01, 0, 0, 0, 0, 0,
];

/** The key's 16 bytes from its 32 hex digits. */
function keyBytes(key: string): number[] {
  const pairs = String(key).match(/[0-9a-fA-F]{2}/g) ?? [];
  if (pairs.length < 16) throw new Error("Decryption error: no encryption key");
  return pairs.slice(0, 16).map((h) => parseInt(h, 16));
}

/** TEST ONLY (not MZ): `file` as the editor's encrypted deployment ships it (`<file>_`). */
export function encryptForDeployment(file: Uint8Array, key: string): Uint8Array {
  const k = keyBytes(key);
  const out = new Uint8Array(ENCRYPTION_HEADER.length + file.length);
  out.set(ENCRYPTION_HEADER);
  out.set(file, ENCRYPTION_HEADER.length);
  for (let i = 0; i < 16 && i < file.length; i++) out[ENCRYPTION_HEADER.length + i]! ^= k[i]!;
  return out;
}

export function createUtils(ctx: Ctx) {
  const g = ctx.g;
  return {
    // Utils.RPGMAKER_NAME
    RPGMAKER_NAME: "MZ",
    // Utils.RPGMAKER_VERSION
    RPGMAKER_VERSION: ctx.opts.version ?? "1.9.0",
    // Utils.isOptionValid(name): MZ reads "?test&btest" from the URL (and NW.js's argv)
    isOptionValid(name: string): boolean {
      if (ctx.opts.options) return ctx.opts.options.includes(name);
      const search = typeof g.location?.search === "string" ? g.location.search.slice(1) : "";
      return search.split("&").includes(name);
    },
    // Utils.isNwjs()
    isNwjs(): boolean {
      return !!ctx.opts.nwjs;
    },
    // Utils.isMobileDevice()
    isMobileDevice(): boolean {
      return !!ctx.opts.mobile;
    },
    // Utils.isMobileSafari() / Utils.isAndroidChrome()
    isMobileSafari(): boolean {
      return false;
    },
    isAndroidChrome(): boolean {
      return false;
    },
    // Utils.extractFileName(filename)
    extractFileName(filename: string): string {
      return filename.split("/").pop()!;
    },
    // Utils.encodeURI(str): like encodeURIComponent but keeps "/"
    encodeURI(str: string): string {
      return encodeURIComponent(str).replace(/%2F/g, "/");
    },
    _hasEncryptedImages: false,
    _hasEncryptedAudio: false,
    _encryptionKey: "",
    // Utils.hasEncryptedImages() / hasEncryptedAudio()
    hasEncryptedImages(): boolean {
      return this._hasEncryptedImages;
    },
    hasEncryptedAudio(): boolean {
      return this._hasEncryptedAudio;
    },
    // Utils.setEncryptionInfo(hasImages, hasAudio, key)
    setEncryptionInfo(hasImages: boolean, hasAudio: boolean, key: string): void {
      this._hasEncryptedImages = !!hasImages;
      this._hasEncryptedAudio = !!hasAudio;
      this._encryptionKey = String(key ?? "");
    },
    // Utils.decryptArrayBuffer(source) -> ArrayBuffer (modelled format: see the header)
    decryptArrayBuffer(source: ArrayBuffer): ArrayBuffer {
      const bytes = new Uint8Array(source);
      const n = ENCRYPTION_HEADER.length;
      if (bytes.length < n || ENCRYPTION_HEADER.some((b, i) => bytes[i] !== b)) throw new Error("Decryption error");
      const body = bytes.slice(n);
      const k = keyBytes(this._encryptionKey);
      for (let i = 0; i < 16 && i < body.length; i++) body[i]! ^= k[i]!;
      return body.buffer;
    },
  };
}
