# Changelog

Each release's section heads its GitHub release notes (`release.yml` copies the section whose heading is the tagged
version). The generated list of changes follows it there.

## 0.3.0

- **New engines (Preview):** a Unity package (`com.repulse.scfg`, Unity 6.3 LTS+, tested on 6000.6.9f1 in Linux and
  WebGL players) and an Unreal Engine plugin (`Scfg`, UE 5.6+, tested on UE 5.8.4 on Linux). Windows and macOS are
  supported but unverified on both.
- **Buy link, on by default on mainnet.** Players who have no USDC or USDT yet see "No USDC yet? Buy some with a card"
  on the payment sheet. It opens thirdweb's hosted buy page (card payment only) for the network they picked, with the
  payment as the suggested amount; they buy into their own wallet and then pay as usual, straight to your wallet. The
  link carries only the network, the token and the amount. A game that upgrades shows it without a configuration
  change. To turn it off: `onramp={"enabled": False}` in Ren'Py, `{"onramp": {"enabled": false}}` in Godot,
  `{"enabled": false}` in RPG Maker MZ's **Buy Link** parameter, `ScfgOptions.Onramp = new ScfgOnramp { Enabled =
  false }` in Unity, `FScfgOptions.Onramp.bEnabled = false` in Unreal. See
  [the buy link](docs/developers/configure/README.md#the-buy-link).
- Web builds open every outside link (the buy page, the explorer) with `noopener,noreferrer`.
- **Faster "can't reach the network".** Every engine's RPC request timeout now covers the name lookup, a call spends
  at most 15 s across a chain's endpoints (not 10 s per endpoint), and picking another network while a payment is
  starting cancels the first start instead of waiting behind it. A start that cannot reach the network says "Can't
  reach the network. Pick a network to try again." (the waiting screen's "Retrying..." is the one that retries).
- A custom chain's `explorer_tx` may be `http://127.0.0.1` / `http://localhost` (a local devnet's explorer).
- Unity: the samples pay your wallet (their component's, or the SCFG Settings asset's) and refuse when none is set;
  an asset without a wallet is not applied and is no longer an error; `settings.Apply(wallet)` applies it with a
  wallet supplied at runtime; WebGL builds from any active platform; the package's tests ship (run them through
  `testables`). Unreal: `ConfigureWithSettings(Wallet, SolanaWallet)` does the same for the project settings, and
  the showcase ships as an optional sample, `scfg-unreal-showcase-<version>.zip`.
