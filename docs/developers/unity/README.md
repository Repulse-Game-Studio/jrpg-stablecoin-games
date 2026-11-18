# For Unity developers

Stablecoin for Games for **Unity 6.3 LTS or newer**: accept **USDC** and **USDT** tips and unlock purchases in
your game, paid **directly to your own wallet**. There is no server, no account and no middleman: the package
watches the blockchain itself. It is a UPM package, `com.repulse.scfg`, in plain C# with no
native plugins and nothing beyond Unity's built-in modules, and works in desktop builds (Windows, macOS, Linux)
and WebGL builds (mobile is best effort and unverified). It is a **preview**: tested on Unity 6000.6.9f1, in Linux players (Mono and IL2CPP)
and in a WebGL build in Chromium, against local test chains. 6.3 LTS is the declared minimum but nobody has compiled
the package on it yet. A real-wallet testnet payment is the last check before it leaves preview.

- [Install](install.md): add the package to your project and take your first payment, in code or with the
  **SCFG Settings** asset, or [import the samples](install.md#try-the-samples-first) to see every call in action
  first.
- [API reference](api.md): every `SCFG` call, the result objects and events, customising the screens, input,
  WebGL builds, IL2CPP and mobile.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.
