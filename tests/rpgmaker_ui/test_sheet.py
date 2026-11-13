"""The payment sheet (rpgmaker/src/mz/ui/sheet.ts) on the MZ shim, in Chrome: it shows what the
CheckoutController says, step for step as Ren'Py's scfg_payment screen; keyboard- and
gamepad-only players can finish a payment through MZ's Input; typing into it never reaches the
game; its texts come from SCFG.text(); a replaced SCFG.sheetHost is used instead.

The harness is a web deployment (no browser wallet in the page), so the methods are the phone
wallet (QR) and the manual transfer.
"""
from scfg_core.paylinks import qr_rects

TX = "0x" + "ab" * 32
RAW = "() => Object.keys(Input._currentState).filter((k) => Input._currentState[k])"


def _dots(game):
    return game.page.evaluate("""() => {
        const s = document.getElementById("scfg-host").shadowRoot;
        const dots = [...s.querySelectorAll(".screen[data-screen=payment] .dots .dot")];
        return [dots.filter((d) => d.classList.contains("on")).length, dots.length];
    }""")


def _assert_matches(game):
    """The sheet shows the controller's step, the back arrow exactly while it can go back, and its dots."""
    v = game.view()
    assert game.step() == v["step"], v
    assert game.key("back").is_visible() == v["canGoBack"], v
    index, total = v["dots"]
    if v["step"] != "done" and total > 1:
        assert _dots(game) == [index + 1, total], v
    else:
        assert _dots(game) == [0, 0], v
    return v


def _to_manual(game):
    game.pay()
    game.key("network_arbitrum").click()
    game.pass_()
    game.wait_step("method")
    game.key("method_manual").click()
    game.wait_step("manual")


def test_steps_follow_the_controller(make_game):
    game = make_game()
    game.setup()
    game.pay()
    sheet = game.screen()
    assert sheet.is_visible()
    v = _assert_matches(game)
    assert v["step"] == "network"
    assert game.key("title").inner_text() == "Chapter 2"
    assert game.key("price").inner_text() == "4.99"
    assert game.key("token").inner_text() == "USDC"
    shown = [game.key(f"network_{k}").inner_text().splitlines()[0] for k, _name, _fee in v["chainOptions"]]
    assert shown == [name for _k, name, _fee in v["chainOptions"]]
    assert game.key("cancel").is_visible() and game.key("keep_playing").count() == 0

    game.key("network_arbitrum").click()
    game.frames(2)
    assert game.view()["state"] == "starting"
    assert game.key("status").inner_text() == "Preparing the payment..."
    game.pass_()
    game.wait_step("method")
    v = _assert_matches(game)
    assert v["methods"] == ["qr", "manual"]
    assert game.key("method_qr").get_attribute("class").split().count("selected") == 1
    assert "Your wallet will ask for " + v["payAmount"] + " USDC" in game.key("note").inner_text()
    assert game.key("keep_playing").is_visible(), "Continue playing once a session exists"

    game.key("method_manual").click()
    game.wait_step("manual")
    v = _assert_matches(game)
    assert game.key("value_amount").inner_text() == v["payAmount"] + " USDC"
    assert game.key("value_recipient").inner_text() == v["recipient"]
    assert game.key("value_token").inner_text() == v["tokenAddress"]
    assert game.key("forward").inner_text() == "I've sent it"
    assert game.key("note").count() == 0, "no exact-amount note on the manual step (it shows the amount)"

    game.key("back").click()
    game.wait_step("method")
    _assert_matches(game)
    game.key("method_manual").click()
    game.wait_step("manual")
    game.key("forward").click()
    game.wait_step("waiting")
    v = _assert_matches(game)
    assert game.key("heading").inner_text() == "Waiting for your payment on Arbitrum..."

    game.js("() => scfgTest.payOnChain()")
    game.wait_step("done")
    v = _assert_matches(game)
    assert v["state"] == "paid"
    assert game.key("heading").inner_text() == "Payment confirmed. Thank you!"
    assert game.key("tx").inner_text() == TX
    assert game.key("cancel").count() == 0 and game.key("continue").is_visible()
    assert game.page.locator("#scfg-host .toast").inner_text() == "Payment confirmed: Chapter 2. Thank you!"
    assert game.focused_key() == "continue"
    game.key("continue").click()
    game.frames(3)
    assert game.screen().count() == 0, "the sheet is gone"
    box = game.box()
    assert box["done"] and box["result"]["status"] == "paid"


def test_filled_buttons_show_no_border_and_keep_the_outlined_size(make_game):
    """The primary and accent buttons are filled, with no visible border of their own colour
    (theme.gd's ScfgPrimary and ScfgAccent draw none), and they are exactly as large as the
    outlined buttons, as in Godot, where a border never adds to a button's size: the border's
    width is kept, transparent, with the fill painted under it."""
    game = make_game()
    game.setup()
    _to_manual(game)
    color = "(el) => getComputedStyle(el).borderTopColor"
    box = "(el) => { const r = el.getBoundingClientRect(); return [r.width, r.height]; }"
    assert "accent" in game.key("forward").get_attribute("class").split()
    assert "primary" in game.key("keep_playing").get_attribute("class").split()
    for key in ("forward", "keep_playing"):
        assert game.key(key).evaluate(color) == "rgba(0, 0, 0, 0)", f"{key}: no visible border"
    assert game.key("cancel").evaluate(color) != "rgba(0, 0, 0, 0)", "an outlined button shows its border"
    cancel_w, cancel_h = game.key("cancel").evaluate(box)
    keep_w, keep_h = game.key("keep_playing").evaluate(box)
    forward_h = game.key("forward").evaluate(box)[1]
    assert keep_w == cancel_w and keep_h == cancel_h, "primary and outlined: the same size in their row"
    assert forward_h == cancel_h, "accent and outlined: the same height"


def test_back_arrow_hidden_once_tx_seen(make_game):
    game = make_game()
    game.setup()
    _to_manual(game)
    assert game.key("back").is_visible() and game.view()["canGoBack"]
    game.key("tx_input").fill(TX)
    game.key("check").click()
    game.wait_step("waiting")
    v = game.view()
    assert v["hint"] == TX and v["txSeen"] and not v["canGoBack"]
    assert not game.key("back").is_visible(), "no way back once a transaction was seen"
    assert game.key("hint").inner_text() == TX
    # cancel (Escape) neither goes back nor closes once a transaction is seen
    game.hold("Escape")
    assert game.step() == "waiting" and game.screen().count() == 1
    assert game.focused_key() == "explorer"


def test_keyboard_only_manual_payment(make_game):
    game = make_game()
    game.setup()
    game.pay()
    first = game.view()["chainOptions"][0][0]
    assert game.focused_key() == f"network_{first}", "the first network has the focus"
    # walk to Arbitrum with the arrow keys, then Enter (MZ's "ok")
    keys = [k for k, _n, _f in game.view()["chainOptions"]]
    for _ in range(keys.index("arbitrum")):
        game.hold("ArrowDown")
    assert game.focused_key() == "network_arbitrum"
    game.hold("Enter")
    game.pass_()
    game.wait_step("method")
    assert game.focused_key() == "method_qr", "the selected method"
    game.hold("ArrowDown")
    assert game.focused_key() == "method_manual"
    game.hold("Escape")  # cancel goes back a step: the method step is not the first
    game.wait_step("network")
    assert game.focused_key() == "network_arbitrum", "the chosen network keeps its selection"
    game.hold("Enter")
    game.wait_step("method")
    game.hold("ArrowDown")
    game.hold("Enter")
    game.wait_step("manual")
    assert game.focused_key() == "copy_amount"
    game.hold("Enter")
    v = game.view()
    assert game.js("() => window.__clip.writes") == [v["payAmount"]], "Copy copies the amount"
    for _ in range(3):
        game.hold("ArrowDown")
    assert game.focused_key() == "tx_input", "down from the last Copy reaches the hash field"
    game.page.keyboard.type(TX)
    assert game.key("tx_input").input_value() == TX
    game.page.keyboard.press("Enter")  # Enter in the field: Check
    game.wait_step("waiting")
    assert game.view()["hint"] == TX
    game.js("() => scfgTest.payOnChain()")
    game.wait_step("done")
    assert game.focused_key() == "continue"
    game.hold("Enter")
    game.frames(3)
    assert game.screen().count() == 0
    assert game.box()["result"]["status"] == "paid"
    assert game.js(RAW) == [], "no key left held in MZ's Input"


def test_arrow_held_into_the_field_is_released_in_mz(make_game):
    game = make_game()
    game.setup()
    start = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    _to_manual(game)
    game.key("copy_token").focus()
    game.page.keyboard.down("ArrowDown")  # on a button: MZ's Input gets it and moves the focus into the field
    game.frames(3)
    assert game.focused_key() == "tx_input"
    game.page.keyboard.up("ArrowDown")  # released in the field: MZ must see it go up
    game.frames(2)
    assert game.js(RAW) == [], "MZ's down is released"
    game.frames(60)
    assert game.focused_key() == "tx_input", "no auto-repeat out of the field"
    game.key("cancel").click()
    game.frames(3)
    assert game.js(RAW) == []
    game.hold("ArrowRight")
    x, y = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    assert x > start[0] and y == start[1], "the player moves after closing"


def test_desktop_branches(make_game):
    game = make_game()
    game.js("() => scfgTest.desktop()")
    game.setup()
    game.pay()
    game.key("network_arbitrum").click()
    game.pass_()
    game.wait_step("method")
    assert game.view()["methods"] == ["browser", "qr", "manual"], "a computer's browser can always open the page"
    assert game.focused_key() == "method_browser"
    browser = game.key("method_browser").inner_text().splitlines()
    assert browser == ["Browser wallet", "MetaMask, Rabby or another wallet extension"]
    assert game.key("method_qr").inner_text().splitlines()[1] == "Scan a code with your phone wallet"
    game.key("method_browser").click()
    game.wait_step("browser")
    body = game.page.locator("#scfg-host .screen .sheet-body").inner_text()
    assert "Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions." in body
    assert game.key("forward").inner_text() == "Open checkout in browser"
    game.key("forward").click()
    game.wait_step("waiting")
    assert game.js("() => scfgTest.opened") == ["http://127.0.0.1:1234/c/secret/"], "nw.Shell.openExternal"
    assert game.key("reopen").inner_text() == "Reopen checkout page"
    body = game.page.locator("#scfg-host .screen .sheet-body").inner_text()
    assert "Finish the payment on the checkout page in your browser." in body
    assert game.focused_key() == "reopen"
    game.key("reopen").click()
    game.frames(2)
    assert len(game.js("() => scfgTest.opened")) == 2
    game.key("back").click()
    game.wait_step("browser")
    game.key("back").click()
    game.wait_step("method")
    game.key("method_manual").click()
    game.wait_step("manual")
    assert game.key("tx_input").is_visible() and game.key("check").is_visible()
    assert game.key("paste").count() == 0, "no Paste button on desktop (Ren'Py: web only)"


def test_typing_a_hash_keeps_backspace_and_blocks_the_game(make_game):
    game = make_game()
    # a WASD plugin: the letters are the game's direction keys
    game.js("() => Object.assign(Input.keyMapper, { 87: 'up', 65: 'left', 83: 'down', 68: 'right' })")
    game.setup()
    start = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    _to_manual(game)
    field = game.key("tx_input")
    field.click()
    game.js("() => scfgTest.probe.start()")
    kb = game.page.keyboard
    for k in ["0", "x", "a", "b", "w", "s", "Backspace", "Backspace", "w"]:
        kb.down(k)
        game.frames(2)
        kb.up(k)
        game.frames(1)
    assert field.input_value() == "0xabw", "Backspace deletes in the field"
    kb.down("ArrowLeft")
    game.frames(2)
    kb.up("ArrowLeft")
    kb.type("s")
    game.frames(2)
    assert field.input_value() == "0xabsw", "the arrows move the caret in the field"
    assert game.focused_key() == "tx_input", "letters the game binds to directions stay in the field"
    assert game.view()["txInput"] == "0xabsw"
    log = game.js("() => scfgTest.probe.stop()")
    assert log, "frames ran"
    seen = [f for f in log if f["raw"] or f["game"] or f["dir4"] or f["dir8"] or f["touch"]]
    assert not seen, f"MZ's Input saw the typing: {seen[:3]}"
    assert all(f["scene"] == "Scene_Map" for f in log), "no menu opened"
    assert game.js("() => [$gamePlayer.x, $gamePlayer.y]") == start, "the player did not move"
    # ArrowDown leaves the field (the sheet moves the focus; MZ never sees the key)
    game.js("() => scfgTest.probe.start()")
    game.hold("ArrowDown")
    assert game.focused_key() == "paste", "the next control (Paste: a web build)"
    log = game.js("() => scfgTest.probe.stop()")
    assert not any(f["raw"] for f in log), "MZ's Input did not see the arrow in the field"
    # on a button (not a field) the keys reach MZ's Input, which the sheet reads, and the game still sees nothing
    game.js("() => scfgTest.probe.start()")
    game.hold("ArrowDown")
    assert game.focused_key() == "check"
    log = game.js("() => scfgTest.probe.stop()")
    assert any("down" in f["raw"] for f in log), "MZ's Input had the key"
    assert not any(f["game"] or f["dir4"] for f in log), "the game did not"
    assert game.js("() => [$gamePlayer.x, $gamePlayer.y]") == start
    # closed: the game has its input back
    game.key("cancel").click()
    game.frames(3)
    assert game.screen().count() == 0
    game.hold("ArrowRight")
    x, y = game.js("() => [$gamePlayer.x, $gamePlayer.y]")
    assert x > start[0] and y == start[1], "the player moves again once the sheet is closed"


def test_qr_matches_core_matrix(make_game):
    game = make_game()
    game.setup()
    game.pay()
    game.key("network_arbitrum").click()
    game.pass_()
    game.wait_step("method")
    game.key("method_qr").click()
    game.wait_step("qr")
    uri = game.view()["uri"]
    assert uri.startswith("ethereum:")
    drawn = game.page.evaluate("""() => {
        const svg = document.getElementById("scfg-host").shadowRoot.querySelector("[data-key=qr] svg");
        const [, , w, h] = svg.getAttribute("viewBox").split(" ").map(Number);
        const rects = [...svg.querySelectorAll("rect")].slice(1)
            .map((r) => [+r.getAttribute("x"), +r.getAttribute("y"), +r.getAttribute("width")]);
        return [w, h, rects];
    }""")
    size, runs = qr_rects(uri, 2)
    assert drawn[0] == drawn[1] == size
    assert [tuple(r) for r in drawn[2]] == [tuple(r) for r in runs], "the Python core's QR code"
    assert game.key("forward").inner_text() == "I've sent it"


def test_replacement_sheet_host_is_used(make_game):
    game = make_game()
    game.setup()
    game.js("""() => {
        window.__opened = [];
        SCFG.sheetHost = {
            openPayment(ctl) { window.__opened.push(["payment", ctl.spec.item]); return { closed: Promise.resolve("cancel") }; },
            openRestore(ctl) { window.__opened.push(["restore"]); return { closed: Promise.resolve("close") }; },
            close() { window.__opened.push(["close"]); },
            notify(t) { window.__opened.push(["notify", t]); },
        };
    }""")
    game.pay()
    game.frames(3)
    assert game.js("() => window.__opened") == [["payment", "chapter2"], ["close"]]
    assert game.page.locator("#scfg-host .screen").count() == 0, "the default sheet is not shown"
    assert game.box()["result"]["status"] == "cancelled"


def test_default_sheet_host_freed_counts_as_cancel(make_game):
    game = make_game()
    game.setup()
    game.pay()
    assert game.screen().count() == 1
    game.js("() => document.getElementById('scfg-host').shadowRoot.querySelector('.screen').remove()")
    game.frames(3)
    assert game.box()["result"]["status"] == "cancelled"


def _static_texts(game):
    """Visible text nodes of the host that are not data (amounts, addresses, names), and labels."""
    return game.page.evaluate("""() => {
        const root = document.getElementById("scfg-host").shadowRoot;
        const out = [];
        const walk = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        for (let n = walk.nextNode(); n; n = walk.nextNode()) {
            const t = n.textContent.trim();
            const el = n.parentElement;
            if (!t || el.closest("style") || el.closest("[data-value]")) continue;
            if (!el.checkVisibility()) continue;
            out.push(t);
        }
        for (const el of root.querySelectorAll("[aria-label], [title]")) {
            if (el.closest("[data-value]")) continue;
            for (const a of ["aria-label", "title"]) if (el.hasAttribute(a)) out.push(el.getAttribute(a));
        }
        return out;
    }""")


def _assert_pseudo(game, where):
    texts = _static_texts(game)
    assert texts, where
    bad = [t for t in texts if not (t.startswith("⟦") and t.endswith("⟧"))]
    assert not bad, f"{where}: not from SCFG.text(): {bad}"


def test_strings_come_from_texts(make_game):
    game = make_game(test_play=True)
    game.setup()
    game.js("() => { for (const [id, en] of Object.entries(scfgTest.texts)) SCFG.texts[id] = '⟦' + en + '⟧'; }")
    game.pay("tip", amount="2", token="USDC")
    game.wait_step("amount")
    _assert_pseudo(game, "amount")
    game.key("custom_amount").fill("abc")
    game.key("set_amount").click()
    game.frames(2)
    game.key("forward").click()
    game.wait_step("network")
    _assert_pseudo(game, "network")
    game.key("network_arbitrum").click()
    game.frames(2)
    _assert_pseudo(game, "network, starting")
    game.pass_()
    game.wait_step("method")
    _assert_pseudo(game, "method")
    game.key("method_qr").click()
    game.wait_step("qr")
    _assert_pseudo(game, "qr")
    game.key("back").click()
    game.wait_step("method")
    game.key("method_manual").click()
    game.wait_step("manual")
    _assert_pseudo(game, "manual")
    game.key("tx_input").fill("zz")
    game.key("check").click()
    game.frames(2)
    assert game.view()["error"] == "invalid_hash"
    _assert_pseudo(game, "manual, invalid hash")
    game.key("tx_input").fill(TX)
    game.key("check").click()
    game.wait_step("waiting")
    _assert_pseudo(game, "waiting")
    game.js("() => scfgTest.payOnChain()")
    game.wait_step("done")
    _assert_pseudo(game, "done (and the toast)")
    game.key("continue").click()
    game.frames(3)
    # the restore screen
    game.js("() => scfgTest.receiptOnArbitrum('5000000', '0x' + 'cd'.repeat(32))")
    game.js("() => scfgTest.restore()")
    game.frames(2)
    _assert_pseudo(game, "restore")
    game.key("tx_input").fill("0x" + "cd" * 32)
    game.key("check").click()
    game.frames(2)
    _assert_pseudo(game, "restore, checking")
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'choose'")
    _assert_pseudo(game, "restore, choose")
    game.key("choice_0_tip").click()
    game.key("restore").click()
    game.pass_()
    game.pass_()
    game.page.wait_for_function("() => scfgTest.restoreView().state === 'done'")
    _assert_pseudo(game, "restore, done")


def test_a_key_held_in_the_field_is_forgotten_once_the_sheet_closes(make_game):
    """A key held in the hash field (kept from MZ, its release too), the sheet closed by mouse and
    the key released outside the sheet: the next sheet hands that key's release to MZ again."""
    game = make_game()
    game.setup()
    _to_manual(game)
    game.key("tx_input").click()
    kb = game.page.keyboard
    kb.down("ArrowRight")
    game.frames(2)
    game.key("cancel").click()
    game.frames(3)
    assert game.screen().count() == 0
    kb.up("ArrowRight")  # outside the sheet: MZ sees it there (it never saw it go down)
    game.frames(2)
    assert game.js(RAW) == []
    game.pay()
    game.key("network_arbitrum").focus()
    game.hold("ArrowRight")  # on a button: MZ's Input gets the key, and must get its release
    assert game.js(RAW) == [], "MZ's right is released"
    game.key("cancel").click()
    game.frames(3)
    assert game.js(RAW) == []


def test_a_key_held_in_the_field_is_forgotten_when_the_window_loses_focus(make_game):
    """A key held in the field while the player switches windows: its release happens where the
    page never sees it. Pressed again on a button, MZ must see that release."""
    game = make_game()
    game.setup()
    _to_manual(game)
    game.key("tx_input").click()
    kb = game.page.keyboard
    kb.down("ArrowRight")
    game.frames(2)
    game.js("() => window.dispatchEvent(new Event('blur'))")
    game.key("copy_token").focus()
    kb.down("ArrowRight")  # down again (the browser repeats a key it still thinks held)
    game.frames(3)
    kb.up("ArrowRight")
    game.frames(2)
    assert game.js(RAW) == [], "MZ's right is released"


def test_a_held_enter_in_the_field_checks_once(make_game):
    game = make_game()
    game.setup()
    _to_manual(game)
    game.key("tx_input").click()
    game.page.keyboard.type("0x12")
    game.js("""() => {
        const c = scfgTest.checkout();
        const submit = c.submitHash.bind(c);
        window.__checks = 0;
        c.submitHash = () => { window.__checks++; return submit(); };
    }""")
    kb = game.page.keyboard
    kb.down("Enter")
    game.frames(3)
    for _ in range(3):
        kb.down("Enter")  # the auto-repeat of a held key (repeat = true)
        game.frames(2)
    kb.up("Enter")
    game.frames(2)
    assert game.js("() => window.__checks") == 1, "a held Enter checks once"
    assert game.view()["error"] is not None, "and the sheet says what is wrong with the hash"
    assert game.js(RAW) == [], "MZ never saw the Enter"


def test_an_arrow_repeating_into_the_field_is_released_in_mz(make_game):
    """An arrow that went down on a button (MZ saw it) and repeats once the focus is in the field:
    the repeats stay out of MZ, but the release is MZ's."""
    game = make_game()
    game.setup()
    _to_manual(game)
    game.key("copy_token").focus()
    kb = game.page.keyboard
    kb.down("ArrowDown")
    game.frames(3)
    assert game.focused_key() == "tx_input"
    kb.down("ArrowDown")  # an auto-repeat in the field (repeat = true): the field moves the focus on
    game.frames(2)
    assert game.focused_key() == "paste"
    kb.up("ArrowDown")
    game.frames(2)
    assert game.js(RAW) == [], "MZ's down is released"


# The data-key of every button the payment sheet shows now, in its order (the hidden back arrow
# of a first step is not shown).
SHEET_BUTTONS = """() => [...document.getElementById("scfg-host").shadowRoot
    .querySelectorAll(".screen[data-screen=payment] button[data-key]")]
    .filter((b) => !b.hidden && b.offsetParent !== null).map((b) => b.getAttribute("data-key"))"""


def test_headless_host_buttons_match_the_sheet(make_game):
    # The desktop e2e's headless host (rpgmaker/test/e2e/headless_host.ts) presses the sheet's
    # buttons by name: on every step it offers exactly the buttons the real (desktop) sheet shows.
    game = make_game()
    game.js("() => scfgTest.desktop()")
    game.setup(config={"chains": ["arbitrum", "base"]})
    seen = []

    def same(step):
        game.wait_step(step)
        game.frames(2)
        shown, headless = game.js(SHEET_BUTTONS), game.js("() => scfgTest.headlessButtons()")
        assert headless == shown, f"{step}: headless {headless} != sheet {shown}"
        seen.append(step)

    game.pay("tip", amount="1.00", token="USDC")
    same("amount")
    game.key("forward").click()
    same("network")
    game.key("network_arbitrum").click()
    game.pass_()
    same("method")
    for method in ("browser", "qr"):
        game.key(f"method_{method}").click()
        same(method)
        game.key("back").click()
        same("method")
    game.key("method_manual").click()
    same("manual")
    game.key("forward").click()  # I've sent it
    same("waiting")
    game.key("cancel").click()
    game.frames(3)
    # the browser's waiting step (Reopen), then the done step of a paid unlock
    game.pay()
    game.key("network_arbitrum").click()
    game.pass_()
    game.key("method_browser").click()
    game.wait_step("browser")
    game.key("forward").click()
    same("waiting")
    game.js("() => scfgTest.payOnChain()")
    same("done")
    assert seen.count("method") == 3 and {"amount", "network", "browser", "qr", "manual", "waiting", "done"} <= set(seen)
    game.key("continue").click()
    game.frames(3)
    assert game.box()["result"]["status"] == "paid"


def test_an_unhandled_rejection_stops_the_game_as_mz(make_game):
    """The shim models MZ's SceneManager.onError/onReject: a rejection nobody handled (or an
    uncaught error) stops the game, so the fixture fails any test where that happens. A script's
    pay() without .catch is not one of them: what is not its mistake resolves "failed"."""
    game = make_game()
    game.setup()
    game.js("""() => {
        SCFG.sheetHost = { openPayment: () => null, openRestore: () => null, close() {}, notify() {} };
        window.__results = [];
        SCFG.pay("unlock", { item: "chapter2" }).then((r) => window.__results.push(r.status + " " + r.error));
    }""")
    game.page.wait_for_function("() => window.__results.length === 1")
    assert game.js("() => window.__results") == ["failed sheet.unavailable"]
    assert game.js("() => mzShim.stopped") is False, "a pay() without .catch never stops the game"
    game.js("() => { Promise.reject(new Error('nobody caught me')); }")
    game.page.wait_for_function("() => mzShim.stopped")
    frame = game.js("() => Graphics.frameCount")
    game.page.wait_for_timeout(100)
    assert game.js("() => Graphics.frameCount") == frame, "no frame runs any more"
    assert "nobody caught me" in game.js("() => String(mzShim.takeErrors()[0])")
    # what the fixture would report: the plugin's own report, the shim's onError line, the page error
    expected = ("sheet.unavailable", "nobody caught me")
    unexpected = [e for e in game.errors if not any(x in e for x in expected)]
    assert unexpected == [], unexpected
    game.errors.clear()


def test_a_press_mz_saw_released_over_the_sheet_is_released_in_mz(make_game):
    """The mouse pressed on the map (MZ's TouchInput saw it), the sheet opened under it, and the
    button released over the sheet: MZ must see that release, or TouchInput stays pressed."""
    game = make_game()
    game.setup()
    mouse = game.page.mouse
    mouse.move(500, 360)
    mouse.down()
    game.frames(2)
    assert game.js("() => TouchInput._mousePressed") is True, "MZ saw the press on the canvas"
    game.pay()
    mouse.move(505, 362)
    assert game.js("() => document.elementFromPoint(505, 362).id") == "scfg-host", "released over the sheet"
    mouse.up()
    game.frames(2)
    assert game.js("() => TouchInput._mousePressed") is False, "MZ saw the release"
    assert game.screen().count() == 1, "the release did nothing on the sheet"
    # a click on the sheet's own button is the sheet's: MZ never sees a click there (after the
    # sheet closed, a menu window under it would run the command under the pointer)
    game.js("""() => {
        window.__clicks = 0;
        const update = TouchInput.update;
        TouchInput.update = function () { update.call(this); if (this._clicked) window.__clicks++; };
    }""")
    game.key("cancel").click()
    game.frames(3)
    assert game.screen().count() == 0
    assert game.js("() => window.__clicks") == 0, "no click reached MZ"
    # a touch MZ saw: the browser sends its touchend to the element it started on (the canvas,
    # past the host), wherever the finger is lifted, so MZ sees it end
    game.js("""() => {
        const t = (type, target) => target.dispatchEvent(new TouchEvent(type, { bubbles: true, cancelable: true, composed: true,
            changedTouches: [new Touch({ identifier: 1, target, pageX: 500, pageY: 360 })],
            touches: type === "touchend" ? [] : [new Touch({ identifier: 1, target, pageX: 500, pageY: 360 })] }));
        window.__touch = t;
        t("touchstart", document.getElementById("gameCanvas"));
    }""")
    game.frames(2)
    assert game.js("() => TouchInput._screenPressed") is True, "MZ saw the touch"
    game.pay()
    game.js("() => window.__touch('touchend', document.getElementById('gameCanvas'))")
    game.frames(2)
    assert game.js("() => TouchInput._screenPressed") is False, "MZ saw the touch end"
    game.key("cancel").click()
    game.frames(3)
    assert game.screen().count() == 0


def test_a_touch_that_began_on_the_sheet_never_reaches_mz(make_game):
    """Only the mouse's releases pass the host while MZ holds a press: a touchend (or
    touchcancel) at the host is of a touch that started on the sheet (the browser sends it to
    the touch's start element), which MZ never saw, even while MZ holds a press of its own."""
    game = make_game()
    game.setup()
    mouse = game.page.mouse
    mouse.move(500, 360)
    mouse.down()
    game.frames(2)
    assert game.js("() => TouchInput._mousePressed") is True, "MZ holds a press it saw on the canvas"
    game.pay()
    game.js("""() => {
        window.__seen = [];
        for (const name of ["_onTouchStart", "_onTouchEnd", "_onTouchCancel"]) {
            const original = TouchInput[name];
            TouchInput[name] = function (...args) { window.__seen.push(name); return original.apply(this, args); };
        }
        const host = document.getElementById("scfg-host");
        for (const type of ["touchstart", "touchend", "touchstart", "touchcancel"]) {
            host.dispatchEvent(new TouchEvent(type, { bubbles: true, cancelable: true, composed: true,
                changedTouches: [new Touch({ identifier: 2, target: host, pageX: 505, pageY: 362 })],
                touches: type === "touchstart" ? [new Touch({ identifier: 2, target: host, pageX: 505, pageY: 362 })] : [] }));
        }
    }""")
    game.frames(2)
    assert game.js("() => window.__seen") == [], "no touch of the sheet reached MZ"
    assert game.js("() => TouchInput._mousePressed") is True, "MZ's own press is untouched"
    assert game.screen().count() == 1
    mouse.up()
    game.frames(2)
    assert game.js("() => TouchInput._mousePressed") is False, "the mouse's release still reaches MZ"
    game.key("cancel").click()
    game.frames(3)
