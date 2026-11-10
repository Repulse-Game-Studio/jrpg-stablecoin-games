"""The browser wallet checkout of an RPG Maker MZ web deployment, in Chrome on the MZ shim: the
real shared checkout page (plugin/game/scfg/web/*, embedded in the build) is injected into the
game's page and opens as an overlay over the sheet through the real bridge.js, driven by the real
glue (core/overlay.ts over src/mz/web_page.ts). The wallet is the mock EIP-6963 one of the
integration tests; nothing is paid here (no chain), the paying runs in the web e2e.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration"))

from scfg_core.rpc import RpcError  # noqa: E402

from browser_mocks import install_mock_wallet  # noqa: E402

PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
RAW = "() => Object.keys(Input._currentState).filter((k) => Input._currentState[k])"

# The overlay's host: the element bridge.js adds to the body (the sheet's own host is #scfg-host).
OVERLAYS = """() => [...document.body.children].filter((e) => e.shadowRoot && e.id !== "scfg-host")
    .map((e) => ({ shown: e.style.display !== "none", text: e.shadowRoot.textContent }))"""

# The id (else the text) of the focused control inside the overlay, or null.
OVERLAY_FOCUS = """() => {
    const host = [...document.body.children].find((e) => e.shadowRoot && e.id !== "scfg-host");
    const el = host && host.shadowRoot.activeElement;
    return el ? (el.id || el.textContent) : null;
}"""


class NoChain:
    """The mock wallet's RPC: there is no chain in these tests (only opening and closing)."""

    def call(self, method, params):
        raise RpcError(-32601, f"no chain in this test ({method})")


def _with_wallet(page):
    install_mock_wallet(page, NoChain(), PAYER)


def _to_methods(game):
    game.setup()
    game.pay()
    game.key("network_arbitrum").click()
    game.pass_()
    game.wait_step("method")


def test_web_overlay(make_game):
    game = make_game(before_goto=_with_wallet)
    _to_methods(game)
    assert "browser" in game.view()["methods"], game.view()
    browser = game.key("method_browser").inner_text().splitlines()
    assert browser[0] == "Browser wallet", browser
    assert game.js(OVERLAYS) == [], "nothing over the game before the player asks"
    game.key("method_browser").click()
    game.wait_step("browser")
    assert game.key("forward").inner_text() == "Open wallet"
    game.key("forward").click()
    game.wait_step("waiting")
    overlays = game.js(OVERLAYS)
    assert len(overlays) == 1 and overlays[0]["shown"], overlays
    # the shared page, started with the session through the bridge's channel
    pay_button = game.page.get_by_role("button", name="Pay with Mock Wallet")
    pay_button.wait_for()
    game.page.wait_for_function(
        "() => [...document.body.children].some((e) => e.shadowRoot && e.id !== 'scfg-host'"
        " && e.shadowRoot.getElementById('title').textContent === 'Chapter 2')")
    price = game.view()["payAmount"]
    assert price.startswith("4.99"), price
    assert "USDC" in game.js(OVERLAYS)[0]["text"]
    assert game.page.get_by_text("This page is served by the game").count() == 0, "the embedded page's own footer"

    # the keyboard in the overlay is the overlay's: Tab moves its focus (MZ would preventDefault
    # Tab), and an Enter on its close button does not also press the sheet's focused button
    close = game.page.get_by_role("button", name="Back to the game")
    close.focus()  # the first control of the overlay
    game.page.keyboard.down("Tab")
    game.frames(2)
    game.page.keyboard.up("Tab")
    game.frames(2)
    focused = game.js(OVERLAY_FOCUS)
    assert focused is not None and "Mock Wallet" in focused, f"Tab moved the focus to the next control ({focused})"
    assert game.js(RAW) == [], "MZ's Input never saw the Tab"
    # the overlay scrolls: MZ's wheel handler (preventDefault) never sees its wheel
    prevented = game.js("""() => {
        const host = [...document.body.children].find((e) => e.shadowRoot && e.id !== "scfg-host");
        const e = new WheelEvent("wheel", { deltaY: 100, bubbles: true, cancelable: true, composed: true });
        host.shadowRoot.querySelector("main").dispatchEvent(e);
        return e.defaultPrevented;
    }""")
    assert prevented is False
    # an arrow in the overlay is not the sheet's (which would take the focus under the overlay)
    close.focus()
    game.hold("ArrowDown")
    assert game.js(OVERLAY_FOCUS) == "cp-close", "the focus stays in the overlay"
    assert game.focused_key() is None, "the sheet under it did not take the focus"
    game.hold("Enter")
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False], "Back to the game hides the overlay"
    game.frames(5)
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False], "and stays hidden"
    assert game.js(RAW) == [], "no key left held in MZ's Input"

    # the sheet's Reopen shows the same overlay again
    game.key("reopen").click()
    game.frames(2)
    assert [o["shown"] for o in game.js(OVERLAYS)] == [True], "the same overlay, shown again"
    close.click()
    game.frames(2)
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False]

    # closing the sheet closes the overlay
    game.key("cancel").click()
    game.frames(3)
    assert game.screen().count() == 0
    assert game.js(OVERLAYS) == [], "the overlay is gone with the sheet"
    assert game.box()["result"]["status"] == "cancelled"


def test_web_overlay_takes_the_focus_when_it_opens(make_game):
    # a keyboard-only player's keys go to the overlay at once, not to the sheet hidden under it
    game = make_game(before_goto=_with_wallet)
    _to_methods(game)
    game.key("method_browser").click()
    game.wait_step("browser")
    game.key("forward").click()
    game.wait_step("waiting")
    game.page.get_by_role("button", name="Pay with Mock Wallet").wait_for()
    assert game.js(OVERLAY_FOCUS) == "cp-close", "the overlay's first control has the focus"
    assert game.focused_key() is None, "the sheet under it has none"
    game.hold("Enter")
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False], "Enter pressed the overlay's own Back to the game"
    assert game.step() == "waiting", "and not a button of the sheet"
    # shown again from the sheet: the focus goes back into it
    game.key("reopen").click()
    game.frames(2)
    assert [o["shown"] for o in game.js(OVERLAYS)] == [True]
    assert game.js(OVERLAY_FOCUS) == "cp-close", "Reopen focuses the overlay too"
    assert game.focused_key() is None
    game.hold("Tab")
    focused = game.js(OVERLAY_FOCUS)
    assert focused is not None and "Mock Wallet" in focused, f"Tab moves within the overlay ({focused})"
    assert game.js(RAW) == [], "MZ's Input saw none of these keys"


def test_web_overlay_is_offered_only_with_a_wallet_in_the_page(make_game):
    game = make_game()
    _to_methods(game)
    assert "browser" not in game.view()["methods"], game.view()
    assert game.key("method_browser").count() == 0, "no Browser wallet without a wallet in the page"
    assert game.js("() => !!window.scfgBridge"), "the page's bridge is there (it found no wallet)"
    assert game.js(OVERLAYS) == []


def _overlay_open(game):
    _to_methods(game)
    game.key("method_browser").click()
    game.wait_step("browser")
    game.key("forward").click()
    game.wait_step("waiting")
    game.page.get_by_role("button", name="Pay with Mock Wallet").wait_for()
    assert game.js(OVERLAY_FOCUS) == "cp-close", "the overlay has the focus"


def test_the_sheet_takes_the_focus_back_when_the_overlay_hides(make_game):
    # a keyboard-only player: once the overlay is hidden, one held key acts on the sheet (not a
    # first press that only finds the focus)
    game = make_game(before_goto=_with_wallet)
    _overlay_open(game)
    game.hold("Enter")  # the overlay's Back to the game
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False]
    assert game.focused_key() == "reopen", "the sheet's primary control has the focus again"
    game.hold("Enter")
    assert [o["shown"] for o in game.js(OVERLAYS)] == [True], "one held Enter pressed Reopen"
    assert game.js(OVERLAY_FOCUS) == "cp-close", "and the overlay has the focus again"
    assert game.focused_key() is None
    assert game.js(RAW) == [], "no key left held in MZ's Input"


def test_the_sheet_takes_the_focus_back_when_the_paid_overlay_hides(make_game):
    # paid while the overlay had the focus: the sheet rebuilt to its done step without taking the
    # focus; once the overlay hides, Continue has it, and one held Enter closes the paid sheet
    game = make_game(before_goto=_with_wallet)
    _overlay_open(game)
    game.js("() => scfgTest.payOnChain()")
    game.frames(3)
    assert game.view()["state"] == "paid", game.view()
    assert game.step() == "done"
    assert game.js(OVERLAY_FOCUS) == "cp-close", "the overlay kept the focus through the rebuild"
    assert game.focused_key() is None
    game.hold("Enter")  # the overlay's Back to the game
    assert [o["shown"] for o in game.js(OVERLAYS)] == [False]
    assert game.focused_key() == "continue", "the done step's Continue has the focus"
    game.hold("Enter")
    game.frames(3)
    assert game.screen().count() == 0, "one held Enter closed the sheet"
    assert game.box()["result"]["status"] == "paid"
    assert game.js(OVERLAYS) == [], "the overlay went with the sheet"
