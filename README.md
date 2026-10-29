# cryptopay for Ren'Py

Accept **USDC/USDT tips and unlock purchases** in Ren'Py games, paid straight to the
developer's wallet, on Ethereum, Arbitrum, Polygon, Base and **Solana**. No server, no
accounts, no middleman: the game watches the blockchain itself. Works in desktop builds and
in Ren'Py's web (HTML5) build.

```renpy
init python:
    cryptopay.configure(wallet="0xYourWallet", testnet=True)
    cryptopay.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")

label end_of_chapter1:
    $ result = cryptopay.pay("unlock", item="chapter2")
    if cryptopay.owns("chapter2"):
        jump chapter2
```

## Using it in your game

1. Build the drop-in zip with `uv run python tools/package.py`, which writes `dist/cryptopay-renpy-<version>.zip`.
2. Unzip it into your Ren'Py project folder. You get `game/cryptopay/` and
   `game/python-packages/cryptopay_core/`.
3. Follow the [developer guide](docs/developer-guide.md). It covers configuration, the API,
   testing on testnets, customizing the look, web builds and the security model.

Requires Ren'Py 8.2 or newer. Read the guide's "Security and limitations" section before
selling anything: without a server, unlocks are honour-grade.

## Repository layout

| Path | Contents |
|---|---|
| `plugin/game/cryptopay/` | Ren'Py glue: API (`cryptopay_api.rpy`), screens, UI images (`ui/`), browser checkout page and web-build bridge (`web/`) |
| `plugin/game/python-packages/cryptopay_core/` | Engine-independent core (Python 3.9, stdlib only) |
| `examples/demo_game/` | Demo game (run `tools/sync_demo.py` first) |
| `docs/developer-guide.md` | The guide for game developers (shipped in the zip as `cryptopay-README.md`) |
| `tests/unit/` | Fast unit tests |
| `tests/integration/` | Local Anvil chain, local Solana validator and headless Chrome tests (`-m integration`) |
| `tests/renpy_e2e/`, `tests/renpy_web_e2e/` | Real Ren'Py runs (desktop and web build) driving a payment through the screens |
| `tools/` | `package.py` (release zip), `sync_demo.py`, `check_rpcs.py`, `run_renpy_e2e.py`, `run_renpy_web_e2e.py`, `make_ui_assets.py` (screen corner images) |
| `contracts/MockToken.sol` | 6-decimal mock token used by integration tests |

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

# real web build in headless Chrome (needs an SDK with web support, anvil and Playwright's Chromium)
uv run python tools/run_renpy_web_e2e.py --sdk /path/to/renpy-8.5.3-sdk

uv run python tools/check_rpcs.py         # are the built-in public RPCs alive?
uv run python tools/package.py            # build dist/cryptopay-renpy-<version>.zip
```

## License

MIT, see [`LICENSE`](LICENSE).

Third-party code vendored unmodified: `cryptopay_core/_xkcp.py` (Keccak team, CC0) and
`cryptopay_core/_qrcodegen.py` (Project Nayuki, MIT).
