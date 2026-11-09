/**
 * Is a 32-byte value a valid ed25519 public key (a point on the curve)?
 *
 * Solana uses this to tell wallet addresses (on the curve) from program-derived addresses
 * such as token accounts (deliberately off the curve). Mirrors curve25519-dalek's
 * CompressedEdwardsY::decompress: y is read little-endian with the sign bit masked and reduced
 * mod p (non-canonical encodings accepted), and the point exists iff
 * (y^2 - 1) / (d*y^2 + 1) is a square mod p.
 */

export const P: bigint = 2n ** 255n - 19n;

function powMod(base: bigint, exp: bigint, mod: bigint): bigint {
  let result = 1n;
  let b = ((base % mod) + mod) % mod;
  let e = exp;
  while (e > 0n) {
    if (e & 1n) result = (result * b) % mod;
    b = (b * b) % mod;
    e >>= 1n;
  }
  return result;
}

const mod = (a: bigint): bigint => ((a % P) + P) % P;

export const D: bigint = mod(-121665n * powMod(121666n, P - 2n, P));

export function isOnCurve(point: Uint8Array): boolean {
  if (point.length !== 32) return false;
  let n = 0n;
  for (let i = 31; i >= 0; i--) n = (n << 8n) | BigInt(point[i]!);
  const y = (n & ((1n << 255n) - 1n)) % P;
  const yy = (y * y) % P;
  const u = mod(yy - 1n);
  const v = mod(D * yy + 1n); // never 0: d is not a square mod p
  const x2 = (u * powMod(v, P - 2n, P)) % P;
  return x2 === 0n || powMod(x2, (P - 1n) / 2n, P) === 1n;
}
