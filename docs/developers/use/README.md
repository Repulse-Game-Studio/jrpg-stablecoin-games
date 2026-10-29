# Use it

```renpy
label end_of_chapter1:
    if not scfg.owns("chapter2"):
        "Chapter 2 is available for 4.99 USDC."
        $ result = scfg.pay("unlock", item="chapter2")

    if scfg.owns("chapter2"):
        jump chapter2
    "Maybe later, then."
```

Tips (the player can change the suggested amount on the payment screen):

```renpy
$ result = scfg.pay("tip", amount="3.00", token="USDC")
```

Buttons in screens use non-blocking actions:

```renpy
textbutton "Support the developer" action ScfgPay("tip", amount="3", token="USDC")
textbutton "Restore purchases" action CryptoRestore()
```

Every call and the `PaymentResult` fields are listed in the [API reference](api-reference.md).

## Callbacks

```renpy
init python:
    def on_payment(result):
        if result.status == "paid":
            achievement.grant("supporter")
    scfg.on_result = on_payment          # also catches payments finished after a restart

    # per call:
    # $ scfg.pay("unlock", item="chapter2", callback=on_payment)
```

Rules:
- Callbacks run on the main thread, for **side effects only**. Never call `jump`, `call` or `renpy.call_screen` inside one; change the story using `pay()`'s return value or `owns()`.
- The receipt is saved **before** your callback runs. Ownership never depends on the callback.
- A session can report `"cancelled"` or `"expired"` and **later** `"paid"` (the player paid late). Always treat `"paid"` as final.
