# Customizing the look

The payment and restore screens are a light sheet on the right of the screen, so your game stays visible beside it. The browser checkout page uses the same look.

The payment sheet shows one step at a time, and each step slides in:

| Step | Shown when | Moves on when |
|---|---|---|
| Amount | Tips only | The player presses **Next** |
| Network | More than one network is enabled | The player picks one and the payment has started |
| Payment method | More than one method is available | The player picks Browser wallet, Phone wallet or Manual transfer |
| Browser / QR / Manual | Always, for the chosen method | **Open checkout in browser**, **I've sent it**, a pasted hash, or the game sees the transaction |
| Waiting | After the method page | The payment is confirmed, too small, or expires |
| Done | Final result | The player presses **Continue** |

- **Skipped steps:** a step with only one choice is skipped in both directions. A game with one network and one method goes straight to that method's page.
- **Going back:** the ← arrow (and Esc or right-click) goes back one step. It is hidden on the first step, and it disappears once the game has seen a transaction for this payment, so the player can't start a second payment by accident. Going back keeps the payment that has already started. Picking a different network or amount starts a new one.

```renpy
init python:
    scfg.configure(wallet="0x...", logo="images/logo.png", accent=gui.accent_color)
```

- **Logo and accent:** the `logo` and `accent` options above apply to both the in-game screens and the browser page.
- **Colors:** the sheet's palette lives in `scfg.SHEET`, `scfg.INK`, `scfg.MUTED`, `scfg.LINE`, `scfg.DIM` (the shade over your game) and friends. Set them in an `init python` block.
- **Styles:** all screen styles start with `scfg_`: `scfg_sheet`, `scfg_title`, `scfg_amount`, `scfg_option` (network, method and amount choices), `scfg_button`, `scfg_primary_button`, `scfg_text`, … Override them in your own `.rpy`. Text uses your game's default font.
- **Replacing a screen:** define `screen scfg_payment(cid)` or `screen scfg_restore(rid)` yourself, using `scfg/scfg_screens.rpy` as the reference. To change a single step, define its screen instead: `scfg_step_amount`, `scfg_step_network`, `scfg_step_method`, `scfg_step_browser`, `scfg_step_qr`, `scfg_step_manual`, `scfg_step_waiting` or `scfg_step_done`. Each takes `(ctl, cid)`. The header (`scfg_payment_header`) and footer (`scfg_payment_footer`) are screens too, and the slide is the `scfg_step_in` transform.
- **Sizing:** the screens scale with your game's resolution. In portrait or narrow games, the sheet takes the full width. A step's body scrolls when needed. Its forward button, the status line and the footer buttons stay pinned at the bottom.
