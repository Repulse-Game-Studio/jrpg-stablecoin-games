"""The restore screen (rpgmaker/src/mz/ui/restore.ts) on the MZ shim, in Chrome: Ren'Py's
scfg_restore screen by the RestoreController's state, finished with a gamepad alone (MZ polls it
through navigator.getGamepads) and with the mouse through an ambiguous payment.
"""
TX = "0x" + "ab" * 32
RAW = "() => Object.keys(Input._currentState).filter((k) => Input._currentState[k])"

# The standard gamepad mapping, as MZ's Input.gamepadMapper reads it.
A, B, UP, DOWN, LEFT, RIGHT = 0, 1, 12, 13, 14, 15


def test_gamepad_only_restore(make_game):
    game = make_game()
    game.setup()
    game.js("(tx) => scfgTest.receiptOnArbitrum('4993742', tx)", TX)  # chapter2's code: no question to ask
    game.js("(tx) => { window.__clip.text = tx; }", TX)
    game.js("() => scfgTest.restore()")
    game.frames(2)
    assert game.screen("restore").is_visible()
    assert game.focused_key() == "tx_input", "the hash field has the focus"
    game.pad(DOWN)
    assert game.focused_key() == "paste", "down from the field: Paste (a web build)"
    game.pad(A)
    game.page.wait_for_function("(tx) => scfgTest.restoreView().txInput === tx", arg=TX)
    game.frames(2)
    assert game.key("tx_input").input_value() == TX, "the pasted hash is in the field"
    game.pad(DOWN)
    assert game.focused_key() == "check"
    game.pad(A)
    assert game.restore_view()["state"] == "checking"
    assert game.key("status").inner_text() == "Checking the blockchain..."
    game.pass_()  # the lookup: one candidate, chapter2, saved at once
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'done'")
    game.frames(2)
    assert game.key("restored").inner_text() == "Restored:"
    assert game.focused_key() == "close"
    assert game.js("() => SCFG.owns('chapter2')")
    owned = game.page.locator("#scfg-host .screen [data-key^='receipt_']")
    assert owned.count() == 1 and "Chapter 2" in owned.first.inner_text()
    game.pad(B)  # cancel closes the restore screen
    game.frames(3)
    assert game.screen("restore").count() == 0
    box = game.box()
    assert box["done"] and box["result"]["status"] == "restored"
    assert game.page.locator("#scfg-host .toast").inner_text() == "Purchases restored."


def test_gamepad_up_and_down_stay_on_the_screen(make_game):
    game = make_game()
    game.setup()
    game.js("() => scfgTest.restore()")
    game.frames(2)
    for _ in range(6):
        game.pad(DOWN)
    assert game.focused_key() == "close", "the last control keeps the focus"
    for _ in range(8):
        game.pad(axes=[0, -1, 0, 0])  # the stick up
    assert game.focused_key() == "tx_input", "the first one too"
    game.pad(A)  # OK on the empty field: Check, which asks for a hash
    assert game.restore_view()["error"] == "invalid_hash"
    assert game.key("message").inner_text() == "That doesn't look like a transaction hash or signature."
    assert game.js("() => [$gamePlayer.x, $gamePlayer.y]") == game.js("() => [$dataSystem.startX, $dataSystem.startY]")


def test_restore_choose_and_confirm(make_game):
    game = make_game()
    game.setup()
    game.js("(tx) => scfgTest.receiptOnArbitrum('5000000', tx)", TX)  # whole cents: an item or a tip?
    game.js("() => scfgTest.restore()")
    game.frames(2)
    game.key("tx_input").fill(TX)
    game.key("check").click()
    game.frames(2)
    assert game.key("tx_input").get_attribute("readonly") is not None, "no typing while checking"
    assert game.key("check").is_disabled()
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'choose'")
    game.frames(2)
    assert game.key("candidate_0").inner_text() == "Payment of 5.000000 USDC on Arbitrum. What was it for?"
    assert game.key("choice_0_chapter2").inner_text() == "Chapter 2"
    assert game.key("choice_0_tip").inner_text() == "Keep as a tip"
    assert game.key("restore").is_disabled(), "Restore waits for a choice"
    assert game.focused_key() == "choice_0_chapter2"
    game.key("choice_0_chapter2").click()
    game.frames(2)
    assert "selected" in game.key("choice_0_chapter2").get_attribute("class").split()
    assert game.restore_view()["choices"] == {"0": "chapter2"}
    assert not game.key("restore").is_disabled()
    game.key("restore").click()
    game.frames(2)
    assert game.restore_view()["state"] == "saving"
    assert game.key("status").inner_text() == "Saving..."
    game.pass_()
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'done'")
    game.frames(2)
    assert game.key("added_0").inner_text() == "Chapter 2: 5.000000 USDC on Arbitrum"
    assert game.js("() => SCFG.owns('chapter2')")
    game.key("copy_receipt_0").click()
    assert game.js("() => window.__clip.writes") == [TX]
    game.key("close").click()
    game.frames(3)
    assert game.box()["result"]["status"] == "restored"


def test_restore_opened_under_a_held_enter(make_game):
    game = make_game()
    game.setup()
    game.js("(tx) => scfgTest.receiptOnArbitrum('4993742', tx)", TX)
    start = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    game.page.keyboard.down("Enter")  # the OK that talked to the NPC
    game.frames(2)
    game.js("() => scfgTest.restore()")
    game.frames(2)
    assert game.focused_key() == "tx_input"
    game.page.keyboard.up("Enter")  # released in the field: MZ must see it go up
    game.frames(2)
    assert game.js(RAW) == [], "MZ's OK is released"
    assert game.restore_view()["state"] == "input", "the held OK pressed nothing"
    game.key("tx_input").fill(TX)
    game.hold("ArrowDown")  # out of the field: Paste
    game.hold("ArrowDown")  # Check
    assert game.focused_key() == "check"
    game.hold("Enter")
    assert game.restore_view()["state"] == "checking", "one Enter on Check checks"
    game.pass_()
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'done'")
    game.key("close").click()
    game.frames(3)
    assert game.screen("restore").count() == 0
    assert game.js(RAW) == []
    game.hold("ArrowRight")
    x, y = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    assert x > start[0] and y == start[1], "the player moves after closing"
    game.js("() => scfgTest.probe.start()")
    game.hold("Enter")
    log = game.js("() => scfgTest.probe.stop()")
    assert any("ok" in f["game"] for f in log), "the game's OK works on the next press"
