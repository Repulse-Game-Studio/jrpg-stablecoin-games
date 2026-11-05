# Stablecoin for Games

Accept **USDC/USDT tips and unlock purchases** in Ren'Py games, paid straight to the
developer's wallet, on Ethereum, Arbitrum, Polygon, Base, **Arc** and **Solana**. No server, no
accounts, no middleman: the game watches the blockchain itself. Works in desktop builds and
in Ren'Py's web (HTML5) build.

![A player tips 3 USDC on Arc in Ruby Dreams: pick an amount, pick a network, scan the QR code, payment confirmed](.github/media/tip-arc.gif)

```renpy
init python:
    scfg.configure(wallet="0xYourWallet", testnet=True)
    scfg.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")

label end_of_chapter1:
    $ result = scfg.pay("unlock", item="chapter2")
    if scfg.owns("chapter2"):
        jump chapter2
```

## Using it in your game

1. From your Ren'Py project folder, run `uvx scfg-renpy install .` (or `pipx run scfg-renpy install .`, or `npx scfg-renpy install .`),
   and `uvx scfg-renpy update .` later for new versions. Or download `scfg-renpy-<version>.zip`
   from the [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest).
   To build it yourself instead, clone the repo and run `python tools/package.py` (Python 3.9+,
   no dependencies), which writes the same zip to `dist/`. Don't copy the repo itself into your
   game: only `plugin/game/` ships, and the zip holds exactly that.
2. Unzip it into your Ren'Py project folder, the one that contains `game/`. You get `game/scfg/` and
   `game/python-packages/scfg_core/`. See [Install](docs/developers/install.md) for the layout and for updating.
3. Follow the [developer guide](docs/developers/README.md). It covers configuration, the
   API, testing on testnets, customizing the look, web builds and the security model.

**Updating an existing game:** Arc is now built in and offered by default. Read the
guide's [upgrade notes](docs/developers/configure/arc.md#upgrading-to-the-version-with-arc-built-in)
first, especially if your payout wallet is a smart-contract wallet or your game added Arc
itself with `add_chain()`.

Requires Ren'Py 8.2 or newer. Read [Security and limitations](docs/developers/security/README.md)
before selling anything: without a server, unlocks are honour-grade.

The docs are published at https://jrpg.com/stablecoinforgames/docs/.
Players can read how paying works, which wallets work and how to restore a purchase in
[docs/players/](docs/players/paying.md).

## Repository layout

| Path | Contents |
|---|---|
| `plugin/game/scfg/` | Ren'Py glue: API (`scfg_api.rpy`), screens, UI images (`ui/`), browser checkout page and web-build bridge (`web/`) |
| `plugin/game/python-packages/scfg_core/` | Engine-independent core (Python 3.9, stdlib only) |
| `examples/demo_game/` | Demo game (run `tools/sync_demo.py` first) |
| `docs/` | The docs: `players/` for players, `developers/` for the developer guide (joined into one `scfg-README.md` in the zip) |
| `website/` | Docusaurus site built from `docs/`, sidebar in `sidebars.json`. `.github/workflows/docs.yml` checks that it builds |
| `tests/unit/` | Fast unit tests |
| `tests/integration/` | Local Anvil chain (also dressed as Arc), local Solana validator and headless Chrome tests (`-m integration`) |
| `tests/live/` | Read-only checks of pinned real Arc mainnet payments (`-m live`, needs internet) |
| `tests/renpy_e2e/`, `tests/renpy_web_e2e/` | Real Ren'Py runs (desktop and web build) driving a payment through the screens |
| `tools/` | `package.py` (release zip), `sync_demo.py`, `check_rpcs.py`, `run_renpy_e2e.py`, `run_renpy_web_e2e.py`, `make_ui_assets.py` (screen corner images) |
| `contracts/` | `MockToken.sol` (6-decimal mock token) and the Arc stand-ins `ArcMockUsdc.sol` / `ArcSystemEmitter.sol`, used by integration tests |

## Development

```bash
uv sync                                   # Python 3.9 venv + dev tools
uv run pytest                             # unit tests
uv run ruff check .                       # lint

# needs Foundry's `anvil` (https://getfoundry.sh) and Agave's `solana-test-validator`
# (https://github.com/anza-xyz/agave/releases) on PATH; browser tests need Chrome/Chromium
uv run pytest -m integration

# real Ren'Py run (opens a small window; needs a Ren'Py SDK, anvil and, for Solana, the validator)
uv run python tools/run_renpy_e2e.py --sdk /path/to/renpy-8.5.3-sdk
uv run python tools/run_renpy_e2e.py --sdk /path/to/renpy-8.5.3-sdk --family solana
uv run python tools/run_renpy_e2e.py --sdk /path/to/renpy-8.5.3-sdk --family arc

# real web build in headless Chrome (needs an SDK with web support, anvil and Playwright's Chromium)
uv run python tools/run_renpy_web_e2e.py --sdk /path/to/renpy-8.5.3-sdk [--family arc]

uv run pytest -m live                     # pinned real Arc payments, read-only

uv run python tools/check_rpcs.py         # are the built-in public RPCs alive?
uv run python tools/package.py            # build dist/scfg-renpy-<version>.zip

# docs site (Node 20+)
cd website && npm ci && npm start         # live preview at http://localhost:3000/stablecoinforgames/docs/
```

### Testing Arc

Arc (USDC as the gas coin) is tested without its network at three levels:

- **Anvil dressed as Arc.** `anvil_setCode` places `contracts/ArcSystemEmitter.sol` at Arc's
  system emitter (`0xffff…fffe`) and `contracts/ArcMockUsdc.sol` at its USDC address
  (`0x3600…0000`). They log in the same order as Arc: a native send logs only at the
  emitter (18 decimals), an ERC-20 `transfer()` logs at the emitter and then at `0x3600`.
  The integration tests (`tests/integration/test_arc_anvil.py`) and both Ren'Py e2e runs
  with `--family arc` use them. The compiled bytecode lives in
  `tests/integration/mock_token.py`, whose docstring has the `forge build` command to
  regenerate it after editing the contracts.
- **Real Arc mainnet, read-only.** `uv run pytest -m live` reads two pinned real payments
  (one native send, one `transfer()`) through the plugin's restore, re-verify and scan code.
  No key or funds are needed; the tests skip when Arc's RPC can't be reached. In CI they
  run with the scheduled RPC check, not on every push.
- **A real wallet, by hand.** Before a release, pay on Arc Testnet once with a wallet's
  Send button and once through the browser checkout (see the guide's
  [Arc page](docs/developers/configure/arc.md)).

The desktop e2e defaults to SDL's offscreen video driver. Where that isn't available, set
`SCFG_E2E_VIDEODRIVER=x11` (or another SDL driver).

## Releasing

1. Set the new version in `plugin/game/python-packages/scfg_core/__init__.py` (`__version__`) and
   merge it to `main`.
2. Tag that commit and push the tag:
   ```bash
   git tag v0.2.0 && git push origin v0.2.0
   ```

`.github/workflows/release.yml` then runs the lint and unit tests, checks that the tag matches
`__version__`, builds the zip with `tools/package.py` and publishes it as a GitHub release with
generated notes. A second job builds the `scfg-renpy` package (`uv build`: the installer in
`installer/` with the plugin files inside it) and uploads it to PyPI through trusted publishing,
from the `pypi` environment. A version with letters in it, such as `0.2.0rc1`, is published as a
prerelease and does not become "latest".

## License

MIT, see [`LICENSE`](LICENSE).

Third-party code vendored unmodified: `scfg_core/_xkcp.py` (Keccak team, CC0) and
`scfg_core/_qrcodegen.py` (Project Nayuki, MIT).
