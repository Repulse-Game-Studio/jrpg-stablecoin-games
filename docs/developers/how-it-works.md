# How it works

1. **The player picks a network**, and the plugin creates a payment session. The amount is your price plus 4 extra digits that encode the item code and a random number: `4.99 → 4.993742`.
2. **The game watches the chain** for a transfer of exactly that amount to your wallet. A transaction hash reported by the browser page, or pasted by the player, is checked directly instead. On Arc it reads the `Transfer` events of Arc's system emitter (`0xffff…fffe`), which logs every USDC move, a native send or an ERC-20 transfer, once each, in 18 decimals.
3. **After enough confirmations** (blocks on top of the payment's block; one on Arc, whose blocks are final; on Solana, once the transaction is finalized), the transfer is stored as a receipt in `persistent`. `owns()` reads receipts, which are re-verified on-chain at each launch.
4. **Restore** reads any transaction hash back from the chain and decodes the item from its amount.
