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

`scfg_core/_qrcodegen.py` is Nayuki's Python library between v1.7.0 and v1.8.0. Compared with v1.8.0 it
lacks one fix: the alignment-pattern step of version 32 (`26 if ver == 32`). Every other difference is
typing or naming. Version 32 needs a payload of roughly 1000 bytes at ECC medium, far beyond any
payment link (the longest is under 300 bytes), so the two agree on every input the core produces.
The `qr` vectors pass bit for bit.
