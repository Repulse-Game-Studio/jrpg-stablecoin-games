# Wallets that work

Which networks and tokens you can use depends on the game. The developer picks them. The
payment sheet only lists what that game accepts.

| Network | Tokens | Fee paid in |
|---|---|---|
| Ethereum | USDC, USDT | ETH |
| Arbitrum | USDC, USDT | ETH |
| Polygon | USDC, USDT | POL |
| Base | USDC | ETH |
| Solana | USDC, USDT | SOL |

Some games also add other networks.

## Browser wallet

- **Ethereum, Arbitrum, Polygon, Base:** MetaMask, Rabby, Coinbase Wallet or another
  wallet extension.
- **Solana:** Phantom, Solflare, Backpack or another wallet extension.

In a downloaded game, the checkout page opens in your default web browser, so the
extension has to be installed there. In a game played in the browser (on itch.io, for
example), the option only appears when the page can see a wallet for the network you
picked. A wallet app's built-in browser on a phone counts.

## Phone wallet

Any wallet app that can scan a payment QR code. On Ethereum-style networks the code is a
standard payment link that carries the network, token and amount. On Solana it is a
Solana Pay code, which Phantom and Solflare can scan.

Some phone wallets round the amount or let you edit it. If yours did, see
[Why the amount has extra digits](paying.md#why-the-amount-has-extra-digits).

## Exchanges and other wallets

Use **Manual transfer** and send the exact amount to the address shown.

- Withdraw on the network you picked in the game. For Solana, choose **Solana (SPL)** in
  the exchange's withdrawal form.
- Many exchanges round amounts or take their fee out of what you send. Afterwards, find
  the transaction hash in your withdrawal history and paste it on the Manual transfer
  step.
