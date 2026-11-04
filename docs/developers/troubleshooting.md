# Troubleshooting

| Symptom | What to do |
|---|---|
| "Can't reach the network. Retrying…" | All RPC endpoints for that chain are failing. Add your own endpoint via `rpc_urls` (Alchemy, Infura, QuickNode, …). Note that keys inside a game build are visible to anyone, so use a key restricted to read-only calls with a spending cap |
| Payment sent but nothing happens | The wallet may have changed the amount. Use **Manual transfer → paste transaction hash** |
| Paid on the wrong network | Funds sent to a normal wallet on another EVM chain are still in your wallet on that chain. Honour the purchase manually or refund |
| Paid from an exchange that rounds amounts | Pasting the transaction hash works for any amount ≥ the price |
| "Transaction not found" on restore | Some public nodes don't index old hashes. Try again later, or add your own RPC |
| Solana isn't offered | Your wallet has no token account for that token yet. The console says which one: receive any amount of it once, then restart the game |
| "Transaction not found" on restore (Solana) | A very new transaction needs about 15 seconds to finalize. Try again shortly |
| Startup error `chain 'arc' is built in`, or `has the same chain_id (5042) as 'arc'` | Your game added Arc itself before it was built in. Delete that `add_chain()` call (see [Upgrading](configure/arc.md#upgrading-to-the-version-with-arc-built-in)) |
| Arc payment stays "pending" in the wallet and never arrives | Arc silently drops transactions priced under 20 gwei. The browser checkout always pays enough; for a wallet Send, raise the fee (or "speed up") and resend |
| Wallet shows the Arc balance as "ETH" | It is USDC: Arc's native coin is USDC, and some wallets label every native coin ETH |
| Player needs a refund or manual unlock | Look up their transaction on a block explorer; refunds are sent from your wallet |

`scfg.export_receipts()` writes a readable JSON file you can ask players to send you.
