# Vendored: Project Nayuki QR Code generator

`qrcodegen.js` and `qrcodegen.d.ts` are the unmodified output of compiling Nayuki's
`typescript-javascript/qrcodegen.ts` with `tsc`, plus one trailing line each so they are ES modules.
No upstream logic is edited. The MIT notice is kept at the top of both files.

- Upstream: https://github.com/nayuki/QR-Code-generator, tag `v1.8.0` (commit `720f62bddb7226106071d4728c292cb1df519ceb`)
- Source file: `typescript-javascript/qrcodegen.ts`, sha256 `c4749095a91bf9696e3a303998b9905e467094f53041e64393e65e6d887737fd`

Why compiled output: upstream declares `namespace qrcodegen { ... }`, which Node's type stripping
and `erasableSyntaxOnly` do not allow, so a `.ts` copy cannot run in this repo. A `.js` plus `.d.ts`
pair keeps `node --test` (no build step) and `tsc` both working.

## Regenerate

```sh
curl -sSfL https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/typescript-javascript/qrcodegen.ts -o qrcodegen.ts
tsc --strict --target ES2017 --lib ES2017 --types "" --declaration --outDir out qrcodegen.ts   # tsc 7.0.2
cp out/qrcodegen.js out/qrcodegen.d.ts .
printf '\nexport { qrcodegen };\n' | tee -a qrcodegen.js >> qrcodegen.d.ts
```

## Relation to the Python twin

`scfg_core/_qrcodegen.py` is Nayuki's Python library between v1.7.0 and v1.8.0. It computes the
alignment-pattern step as `(ver * 8 + numalign * 3 + 5) // (numalign * 4 - 4) * 2`, which gives
every version the positions of ISO/IEC 18004 Table E.1; v1.8.0 here writes
`(ver == 32) ? 26 : ceil((ver * 4 + 4) / (numalign * 2 - 2)) * 2`, which gives the same step for all
40 versions (version 32 included: 26). Every other difference is typing or naming, so the two agree on
every input. The `qr` vectors pass bit for bit, `version_32` (1453 bytes) included.
