import json
from types import SimpleNamespace

from cryptopay_core.web_page import WebPage


class FakeJs:
    def __init__(self, ints=(), strings=()):
        self.scripts = []
        self.ints = list(ints)
        self.strings = list(strings)

    def run_script(self, source):
        self.scripts.append(source)

    def run_script_int(self, source):
        self.scripts.append(source)
        return self.ints.pop(0)

    def run_script_string(self, source):
        self.scripts.append(source)
        return self.strings.pop(0)


def assets(name):
    return {"bridge.js": b"/*bridge*/", "evm.js": b"/*evm*/", "solana.js": b"/*sol*/",
            "app.js": b"/*app*/", "index.html": b"<main id=\"x\">\"</main>"}[name]


def test_install_loads_bridge_first_then_the_page_once():
    js = FakeJs()
    page = WebPage(js, assets)
    page.install()
    page.install()
    assert js.scripts[:4] == ["/*bridge*/", "/*evm*/", "/*sol*/", "/*app*/"]
    html = json.dumps('<main id="x">"</main>')
    assert js.scripts[4] == f"cryptopayBridge.setPage({html})"
    assert len(js.scripts) == 5


def test_values_cross_as_json_literals_never_as_code():
    js = FakeJs(ints=[3])
    hostile = 'http://x/");alert(1);("'
    assert WebPage(js, assets).start(hostile, '{"a":1}', 2.5) == 3
    url, body = json.dumps(hostile), json.dumps('{"a":1}')
    assert js.scripts == [f"cryptopayBridge.post({url}, {body}, 2500)"]


def test_poll_is_none_while_pending_then_the_envelope():
    js = FakeJs(strings=["", '{"status": 200, "body": "{}"}'])
    page = WebPage(js, assets)
    assert page.poll(4) is None
    assert page.poll(4) == {"status": 200, "body": "{}"}
    assert js.scripts == ["cryptopayBridge.result(4)"] * 2


def test_has_wallet_asks_for_the_chain_family():
    js = FakeJs(ints=[1, 0])
    page = WebPage(js, assets)
    sol = SimpleNamespace(family="solana", wallet_chain="solana:mainnet")
    evm = SimpleNamespace(family="evm", wallet_chain="")
    assert page.has_wallet(sol) is True and page.has_wallet(evm) is False
    assert js.scripts[0] == 'cryptopayBridge.hasWallet("solana", "solana:mainnet")'


def test_overlay_calls():
    js = FakeJs(strings=['[{"type": "tx"}]'])
    page = WebPage(js, assets)
    page.open("o1", {"title": "T"}, None)
    page.show("o1")
    page.set_status("o1", {"state": "waiting"})
    page.reply("o1", 2, 200, {"transaction": "AA"})
    assert page.take("o1") == [{"type": "tx"}]
    page.close("o1")
    assert js.scripts == [
        'cryptopayBridge.openCheckout("o1", {"title": "T"}, null)',
        'cryptopayBridge.show("o1")',
        'cryptopayBridge.setStatus("o1", {"state": "waiting"})',
        'cryptopayBridge.reply("o1", 2, 200, {"transaction": "AA"})',
        'cryptopayBridge.take("o1")',
        'cryptopayBridge.closeCheckout("o1")',
    ]


def test_clipboard_and_links():
    js = FakeJs(strings=["null", '"0xab"', '""'])
    page = WebPage(js, assets)
    page.copy("0xabc")
    page.request_paste()
    assert page.take_paste() is None          # not answered yet
    assert page.take_paste() == "0xab"
    assert page.take_paste() == ""            # the browser refused
    page.open_url("https://arbiscan.io/tx/1")
    assert js.scripts[0] == 'cryptopayBridge.copy("0xabc")'
    assert js.scripts[1] == "cryptopayBridge.requestPaste()"
    assert js.scripts[-1] == 'cryptopayBridge.openUrl("https://arbiscan.io/tx/1")'


def test_take_copy_failed_reads_the_one_shot_flag():
    js = FakeJs(ints=[1, 0])
    page = WebPage(js, assets)
    assert page.take_copy_failed() is True
    assert page.take_copy_failed() is False
    assert js.scripts == ["cryptopayBridge.takeCopyFailed()"] * 2
