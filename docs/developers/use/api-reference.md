# API reference

| Call | Returns |
|---|---|
| `scfg.add_chain(key, name, chain_id, rpc, tokens, confirmations, explorer_tx, ...)` | Nothing. Adds your own EVM chain; call before `configure()` ([details](../configure/README.md#your-own-rpc-endpoints-and-chains)) |
| `scfg.configure(wallet=None, ..., solana_wallet=None)` | Nothing. Needs `wallet` or `solana_wallet` (or both); see [All `configure()` options](../configure/README.md#all-configure-options) |
| `scfg.pay(kind, item=None, amount=None, token=None, callback=None)` | `PaymentResult`, when the payment screen closes |
| `scfg.owns(item_id)` | `True` if a stored, verified payment unlocks the item. Instant, works offline |
| `scfg.has_tipped()` / `scfg.tip_total()` | Supporter status / total tipped (USD) |
| `scfg.restore(callback=None)` | `RestoreResult`; shows the restore screen |
| `scfg.receipts()` / `scfg.pending()` | Stored receipts / payments still being watched |
| `scfg.export_receipts(path=None)` | Writes receipts as JSON (for support requests) |
| `scfg.supported()` | Always `True` (the web build included). If you used it to keep your web build free, check `renpy.emscripten` instead |
| `scfg.clear_local_data()` | Testing helper: forgets receipts and pending payments on this device, so you can see your paywall again. Payments stay on the chain, and Restore brings them back |
| `scfg.on_result = fn` | Handler for results that arrive with no live callback (e.g. after a restart) |
| `scfg.notify = True` | Show a "Payment confirmed" notification |

`PaymentResult` fields:
- `status`: `"paid"`, `"already_owned"`, `"cancelled"`, `"expired"`, `"underpaid"`, `"failed"` or `"pending"`
- `kind`, `item`, `amount` (actually paid), `token`, `chain`, `tx_hash`, `payer`, `session_id`, `error`

`"pending"` means the player chose **Continue playing**. The plugin keeps watching in the background, across restarts, for up to 24 hours.
