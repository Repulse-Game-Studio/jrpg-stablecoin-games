## cryptopay demo game. Run `uv run python tools/sync_demo.py` first: it copies the
## plugin from plugin/game/ into this game's folder.

define e = Character("Eileen")

default supporter_thanked = False

init python:
    cryptopay.configure(
        # Replace with YOUR wallet address. This one is the public EIP-55 example address.
        wallet="0x52908400098527886E0F7030069857D2E4169EE7",
        testnet=True,   # Sepolia / Arbitrum Sepolia / Amoy / Base Sepolia: no real money
    )
    cryptopay.add_item("chapter2", code=1, price="1.00", token="USDC", title="Chapter 2: The Fall")

    def on_payment(result):
        # Side effects only: never jump or call from a callback.
        if result.status == "paid":
            store.supporter_thanked = True

    cryptopay.on_result = on_payment


label start:
    e "Welcome to the cryptopay demo. This build runs on testnets, so no real money is involved."
    e "Get free test USDC from Circle's faucet and a little test ETH for network fees."

label hub:
    menu:
        "Unlock Chapter 2 (1.00 USDC)" if not cryptopay.owns("chapter2"):
            $ result = cryptopay.pay("unlock", item="chapter2", callback=on_payment)
            if result.status == "paid":
                e "Payment confirmed. Thank you!"
            elif result.status == "pending":
                e "No problem, keep playing. Chapter 2 unlocks by itself when the payment arrives."
            else:
                e "Maybe later. (Status: [result.status])"
            jump hub

        "Play Chapter 2" if cryptopay.owns("chapter2"):
            jump chapter2

        "Tip the developer":
            $ result = cryptopay.pay("tip", amount="1.00", token="USDC", callback=on_payment)
            if result.status == "paid":
                e "Thank you for the [result.amount] [result.token] tip!"
            jump hub

        "Restore purchases":
            $ cryptopay.restore()
            jump hub

        "Quit":
            return


label chapter2:
    e "This is Chapter 2. Thank you for supporting the game!"
    jump hub
