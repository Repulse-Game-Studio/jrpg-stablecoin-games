# Arc

[Arc](https://docs.arc.io) is Circle's own chain. **USDC is its gas coin**, so a player who
holds USDC on Arc needs nothing else to pay: no ETH, no second token. Arc is built in and
**on by default** next to the other EVM chains, for the same `wallet=` address. Blocks are
final as soon as they're made, so a payment counts within about a second.

```renpy
init python:
    scfg.configure(wallet="0xYourWallet", testnet=True)    # offers Arc Testnet too

    # Or leave Arc out:
    scfg.configure(wallet="0xYourWallet", chains=["arbitrum", "base", "polygon", "ethereum"])
```

- **Your `wallet` must be usable on Arc.** A normal wallet address (MetaMask, Rabby, a
  hardware wallet…) is the same on every EVM chain, Arc included. A **smart-contract wallet**
  (a Safe or another multisig, or a smart account) exists only on the chains it was deployed
  to. If yours isn't deployed on Arc, payments there land at an address you can't control.
  Leave Arc out with `chains=` until it is.
- **USDC only.** USDT isn't on Arc, so items priced in USDT simply don't offer it.
  `tokens=["USDT"]` leaves Arc out entirely.
- **Every way of paying is found.** A wallet's "Send" button moves USDC as Arc's native
  coin. A browser checkout or phone QR calls USDC's ERC-20 interface
  (`0x3600000000000000000000000000000000000000`). An exchange withdrawal does either. The
  game reads every one from the same place (Arc's USDC event log), so each payment counts
  exactly once.
- **Only six decimals count.** A native send can carry up to 18 decimals; the game reads
  the first 6, as for every USDC (`4.9937421…` counts as `4.993742`). A move of less than
  a millionth of a USDC is never a payment.
- **Wallets.** MetaMask, Rabby, Coinbase Wallet and Rainbow can add Arc (chain id 5042;
  testnet 5042002). The browser checkout adds it for the player when the wallet asks. Some
  wallets show the native balance labelled "ETH": on Arc it is USDC. The checkout page
  warns the player when paying would leave less than 0.01 USDC for the fee.
- **Fee floor.** Arc drops transactions priced under 20 gwei without an error, so the
  browser checkout always sets at least that. A fee costs about 0.001 USDC.
- **If the wallet won't quote a fee,** the browser checkout can't build the transaction:
  it reads the network's gas price from the wallet before it sends. The player sees the
  wallet's error and can pay by QR or manual transfer instead.
- **Testing.** `testnet=True` uses Arc Testnet. Get testnet USDC from
  [faucet.circle.com](https://faucet.circle.com) (choose Arc Testnet). It pays the fee too.
  Before release, pay once with your wallet's own **Send** button (a native send) and once
  through the browser checkout (an ERC-20 transfer): the two arrive in different ways.

## Upgrading to the version with Arc built in

- **Arc appears on its own.** If you don't pass `chains=`, the update adds Arc to your
  payment screen. Check that your `wallet` works on Arc (see above), or list your chains
  with `chains=` to keep the old set.
- **Did your game add Arc itself?** Delete that `add_chain()` call. With the key `"arc"`,
  the game otherwise stops at startup with `chain 'arc' is built in`. Purchases it recorded
  stay valid and are still re-checked at each launch.
- **Under another key** (say `"arc_mainnet"`), the game stops with `chain 'arc_mainnet'
  has the same chain_id (5042) as 'arc'`. Delete that call too. Purchases stored under the
  old key stay owned, but the game no longer re-checks them on-chain (it keeps receipts of
  chains it doesn't know). New purchases are stored under `"arc"`.
