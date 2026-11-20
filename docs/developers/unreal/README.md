# For Unreal developers

Stablecoin for Games for **Unreal Engine 5.6 or newer**: accept **USDC** and **USDT** tips and unlock purchases in
your game, paid **directly to your own wallet**. There is no server, no account and no middleman in the payment: the plugin watches
the blockchain itself. It is a code plugin, `Scfg`, in C++ (its payment core is engine-independent C++20) with no
`.uasset` content, usable from C++ and from Blueprint. It is a **preview**: tested only on UE 5.8.4, in Linux
desktop builds, against local test chains. UE 5.6 is the minimum: each release has one zip per engine version (5.6,
5.7 and 5.8), and each declares its own version, but only 5.8.4 has been tested. Nobody has compiled the plugin on 5.6
or 5.7 yet; the plugin builds with MSVC on Windows (UE 5.8) but is not yet fully tested there, macOS is expected to work but
not yet built or tested, and mobile is best effort. A
real-wallet testnet payment is the last check before it leaves preview.

- [Install](install.md): add the plugin to your project and take your first payment, in C++, in Blueprint or from
  **Project Settings > Plugins > SCFG**, or [try the showcase project](install.md#try-the-showcase-first) to see
  every call in action first.
- [API reference](api.md): every `UScfgSubsystem` call and Blueprint node, the result structs and events, the
  payment sheet and custom sheet hosts, what the game opens on the network, platform notes and troubleshooting.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.
