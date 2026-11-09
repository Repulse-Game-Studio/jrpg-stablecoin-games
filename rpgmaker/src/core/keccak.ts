/**
 * Keccak-256 as used by Ethereum (original Keccak padding 0x01, not NIST SHA3-256).
 * Lanes are 64-bit BigInts; the permutation follows the Keccak team's compact reference.
 */

const MASK = (1n << 64n) - 1n;
const RATE_BYTES = 136; // 1088 bits
const KECCAK_SUFFIX = 0x01; // NIST SHA3 uses 0x06; Ethereum uses the original Keccak 0x01

const rol64 = (a: bigint, n: number): bigint => {
  const k = BigInt(n % 64);
  return ((a << k) | (a >> (64n - k))) & MASK;
};

function keccakF1600(lanes: bigint[]): void {
  // lanes[x + 5 * y]
  let R = 1;
  for (let round = 0; round < 24; round++) {
    const C: bigint[] = [];
    for (let x = 0; x < 5; x++) C.push(lanes[x]! ^ lanes[x + 5]! ^ lanes[x + 10]! ^ lanes[x + 15]! ^ lanes[x + 20]!);
    for (let x = 0; x < 5; x++) {
      const d = C[(x + 4) % 5]! ^ rol64(C[(x + 1) % 5]!, 1);
      for (let y = 0; y < 5; y++) lanes[x + 5 * y] = lanes[x + 5 * y]! ^ d;
    }
    // rho and pi
    let x = 1, y = 0;
    let current = lanes[x + 5 * y]!;
    for (let t = 0; t < 24; t++) {
      const nx = y, ny = (2 * x + 3 * y) % 5;
      x = nx; y = ny;
      const tmp = lanes[x + 5 * y]!;
      lanes[x + 5 * y] = rol64(current, ((t + 1) * (t + 2)) / 2);
      current = tmp;
    }
    // chi
    for (let yy = 0; yy < 5; yy++) {
      const T = [0, 1, 2, 3, 4].map((xx) => lanes[xx + 5 * yy]!);
      for (let xx = 0; xx < 5; xx++) lanes[xx + 5 * yy] = T[xx]! ^ (~T[(xx + 1) % 5]! & T[(xx + 2) % 5]!);
    }
    // iota
    for (let j = 0; j < 7; j++) {
      R = ((R << 1) ^ ((R >> 7) * 0x71)) % 256;
      if (R & 2) lanes[0] = lanes[0]! ^ (1n << BigInt((1 << j) - 1));
    }
  }
}

function xorBlock(lanes: bigint[], block: Uint8Array): void {
  for (let i = 0; i < RATE_BYTES / 8; i++) {
    let v = 0n;
    for (let b = 7; b >= 0; b--) v = (v << 8n) | BigInt(block[8 * i + b]!);
    lanes[i] = lanes[i]! ^ v;
  }
}

export function keccak256(data: Uint8Array): Uint8Array {
  const lanes: bigint[] = new Array(25).fill(0n);
  let off = 0;
  while (data.length - off >= RATE_BYTES) {
    xorBlock(lanes, data.subarray(off, off + RATE_BYTES));
    keccakF1600(lanes);
    off += RATE_BYTES;
  }
  const last = new Uint8Array(RATE_BYTES);
  last.set(data.subarray(off));
  last[data.length - off] ^= KECCAK_SUFFIX;
  last[RATE_BYTES - 1] ^= 0x80;
  xorBlock(lanes, last);
  keccakF1600(lanes);
  const out = new Uint8Array(32);
  for (let i = 0; i < 32; i++) out[i] = Number((lanes[i >> 3]! >> BigInt(8 * (i & 7))) & 0xffn);
  return out;
}
