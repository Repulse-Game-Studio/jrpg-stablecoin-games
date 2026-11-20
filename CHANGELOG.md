# Changelog

Each release's section heads its GitHub release notes (`release.yml` copies the section whose heading is the tagged
version). The generated list of changes follows it there, and the release's date is on the GitHub release.

## 0.3.0

- **New engines (Preview):** a Unity package (`com.repulse.scfg`, Unity 6.3 LTS+, tested on 6000.6.9f1 in Linux and
  WebGL players) and an Unreal Engine plugin (`Scfg`, UE 5.6+, tested on UE 5.8.4 on Linux). The Unreal plugin builds
  with MSVC on Windows (UE 5.8); Windows and macOS are expected to work on Unity but not yet tested, and the Unreal
  plugin is not yet built on macOS; mobile is best effort.
- **Buy link, on by default on mainnet.** Players who have no USDC or USDT yet see "No USDC yet? Buy some with a card"
  on the payment sheet. It opens thirdweb's hosted buy page (card payment only) for the network they picked, with the
  payment as the suggested amount; they buy into their own wallet and then pay as usual, straight to your wallet. The
  link carries only the network, the token and the amount. A game that upgrades shows it without a configuration
  change. To turn it off: `onramp={"enabled": False}` in Ren'Py, `{"onramp": {"enabled": false}}` in Godot,
  `{"enabled": false}` in RPG Maker MZ's **Buy Link** parameter, `ScfgOptions.Onramp = new ScfgOnramp { Enabled =
  false }` in Unity, `FScfgOptions.Onramp.bEnabled = false` in Unreal. See
  [the buy link](https://jrpg.com/stablecoinforgames/docs/developers/configure/#the-buy-link).
- Web builds open every outside link (the buy page, the explorer) with `noopener,noreferrer`.
- **Faster "can't reach the network".** Every engine's RPC request timeout now covers the name lookup, a call spends
  at most 15 s across a chain's endpoints (not 10 s per endpoint), and picking another network while a payment is
  starting cancels the first start, its request in flight included, instead of waiting behind it. A start also no
  longer waits behind the background checks of pending payments: the player's command comes first. An endpoint is
  not asked with less than 2 s of the budget left. A start that cannot reach the network says "Can't reach the
  network. Pick a network to try again." (the waiting screen's "Retrying..." is the one that retries). Behind an HTTP
  proxy, Ren'Py and RPG Maker desktop builds no longer fail a request on a local name lookup.
- A custom chain's `explorer_tx` may be `http://127.0.0.1` / `http://localhost` (a local devnet's explorer).
- Unity: the optional **SCFG Settings** asset (**Assets > Create > SCFG Settings**) configures the package without
  code. It applies only when it names a wallet (an asset without one is skipped, not an error), and
  `settings.Apply(wallet)` applies it with a wallet supplied at runtime. Two samples, Showcase and Demo, pay the
  wallet you give them (on their component or in the asset) and configure nothing without one. The package compiles
  for WebGL from any active platform. Its own tests ship with it and compile in a project that lists it under
  `testables`. Unreal: **Project Settings > Plugins > SCFG** configures the plugin without code,
  `ConfigureWithSettings(Wallet, SolanaWallet)` applies those settings with a wallet supplied at runtime, and the
  showcase is an optional sample, `scfg-unreal-showcase-<version>.zip`.
