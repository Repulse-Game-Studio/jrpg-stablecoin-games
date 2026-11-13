# For RPG Maker MZ developers

Stablecoin for Games for **RPG Maker MZ**: accept **USDC** and **USDT** tips and unlock purchases in your
game, paid **directly to your own wallet**. There is no server, no account and no middleman: the plugin
watches the blockchain itself. It is one plugin file, `js/plugins/SCFG.js`, with no dependencies. It works
in desktop builds (NW.js) and web deployments. Mobile is best effort and unverified.

> **Preview: tested on a stand-in, not yet in real RPG Maker MZ.** The plugin is tested on an in-repo
> stand-in for MZ's public plugin API, in headless Chrome and in Node, against local test chains, for both
> the web and the desktop (NW.js) checkout. Nobody has run it inside the real MZ editor or runtime yet. Its
> plugin header, the demo kit and the NW.js parts follow MZ's public documentation and may need small fixes
> there. The checklist for that first real run is in
> [the plugin's README](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/blob/main/rpgmaker/README.md).
> If something does not work in your project, please
> [open an issue](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/issues).

- [Install](install.md): add the plugin, fill in its parameters and take your first payment with the
  **Unlock Item** command.
- [API reference](api.md): every plugin parameter and command, the owned switches, the `window.SCFG` script
  API, customising the sheet, texts, desktop, web and mobile.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.
