# Solana

Add `solana_wallet` to accept USDC and USDT on Solana (on devnet, USDC only). You can use it
alone, or next to `wallet` so players choose between EVM chains and Solana. Call
`configure()` once; these are two alternatives.

Solana only, on devnet:

```renpy
init python:
    scfg.configure(solana_wallet="YourSolanaWalletAddress", testnet=True)
```

EVM chains and Solana, with your own Solana endpoint:

```renpy
init python:
    scfg.configure(
        wallet="0xYourWallet",
        solana_wallet="YourSolanaWalletAddress",
        rpc_urls={"solana": "https://mainnet.helius-rpc.com/?api-key=YOUR_KEY"},
    )
```

- **Use a wallet address.** `solana_wallet` is the address you see in Phantom or Solflare. A
  token-account or program-derived address is refused with an error that says so. Multisig
  vaults (for example a Squads vault) aren't supported in v1.
- **Your wallet must already hold a token account.** Receive any amount of USDC (and of USDT,
  if you offer it) once. At the first check after the game starts, the game looks for your
  token account for each enabled token. If one is missing, the console prints (in developer
  mode only, never in a distributed game) `scfg: your
  wallet can't receive USDC on solana yet (no token account). Receive any amount of USDC there
  once; until then players aren't offered it.`, the DEV MODE banner says "Your Solana wallet
  can't receive USDC yet: receive any amount once.", and that token isn't offered on Solana.
  A network error never hides it. The check runs only when the game starts: once the token
  account exists, restart the game and Solana is offered again.
- **Your own endpoint.** `rpc_urls={"solana": "https://…"}` works like on other chains and is
  tried before the built-in public ones. The public mainnet endpoints rate-limit, and
  `solana-rpc.publicnode.com` keeps only about two days of history, so Restore for older
  payments uses a fallback endpoint or yours.
- **No `confirmations` for Solana** (the option is refused for `"solana"`). A payment counts
  once the network has finalized it, about 13 to 20 seconds.
- **Wallets.** In the browser checkout, Phantom, Solflare and Backpack connect through the
  Wallet Standard. The game builds the transaction and the wallet only signs it. On a phone,
  the player scans a Solana Pay QR code (a `solana:` link with a reference).
- **Exchanges and manual transfers** must withdraw on the **Solana (SPL)** network. The game
  finds the exact amount by scanning your token account. Pasting the transaction signature
  also works.
- **Restore** accepts an EVM `0x…` hash or a Solana signature (base58).
