## End-to-end test game: drives a real payment through the real Ren'Py screens
## against a local Anvil chain (EVM) or a local Solana validator.
## Launched by tools/run_renpy_e2e.py, never shipped.

label main_menu:
    return

init python:
    import json as _e2e_json
    import os as _e2e_os
    import time as _e2e_time
    from dataclasses import replace as _e2e_replace

    from cryptopay_core import chains as _e2e_chains
    from cryptopay_core.chains import Token as _E2EToken
    from cryptopay_core.coio import Sync as _E2ESync
    from cryptopay_core.rpc import RpcClient as _E2ERpc, UrllibTransport as _E2ETransport

    E2E = _e2e_os.environ
    E2E_FAMILY = E2E.get("CRYPTOPAY_E2E_FAMILY", "evm")

    if E2E_FAMILY == "solana":
        _e2e_chains.TESTNET["solana"] = _e2e_replace(
            _e2e_chains.TESTNET["solana"], rpc=(E2E["CRYPTOPAY_E2E_RPC"],),
            tokens=python_dict(USDC=_E2EToken(E2E["CRYPTOPAY_E2E_USDC"])))
        cryptopay.configure(solana_wallet=E2E["CRYPTOPAY_E2E_DEV"], testnet=True,
                            chains=["solana"], tokens=["USDC"])
    else:
        _e2e_chains.MAINNET["arbitrum"] = _e2e_replace(
            _e2e_chains.MAINNET["arbitrum"], rpc=(E2E["CRYPTOPAY_E2E_RPC"],), confirmations=2,
            tokens=python_dict(USDC=_E2EToken(E2E["CRYPTOPAY_E2E_USDC"])))
        cryptopay.configure(wallet=E2E["CRYPTOPAY_E2E_DEV"], chains=["arbitrum"], tokens=["USDC"])

    cryptopay.add_item("chapter2", code=1, price="1.00", token="USDC", title="Chapter 2")

    def e2e_rpc():
        return _E2ESync(_E2ERpc([E2E["CRYPTOPAY_E2E_RPC"]]), _E2ETransport())

    def _e2e_pay_solana(units, reference):
        """Ren'Py has no solders to sign with: hand the payment to the runner, which pays it
        from the funded player and writes the signature back."""
        path = E2E["CRYPTOPAY_E2E_PAYREQ"]
        with open(path + ".tmp", "w") as f:
            _e2e_json.dump(python_dict(units=units, reference=reference), f)
        _e2e_os.replace(path + ".tmp", path)  # the runner never sees a half-written request
        deadline = _e2e_time.time() + 30
        while not _e2e_os.path.exists(path + ".sig"):
            if _e2e_time.time() > deadline:
                raise RuntimeError("the runner did not pay the Solana request within 30 s")
            _e2e_time.sleep(0.2)
        with open(path + ".sig") as f:
            sig = f.read().strip()
        store.e2e_last_tx = sig
        return sig

    def e2e_pay_open_checkout(units=None):
        """Pay the session shown on the payment screen, from the funded payer account."""
        ctl = python_list(cryptopay._state.checkouts.values())[0]
        units = units or ctl.session["units"]
        if E2E_FAMILY == "solana":
            return _e2e_pay_solana(units, ctl.session["reference"])
        data = ("0xa9059cbb" + "0" * 24 + E2E["CRYPTOPAY_E2E_DEV"][2:].lower()
                + format(units, "064x"))
        rpc = e2e_rpc()
        tx = rpc.call("eth_sendTransaction", [python_dict(
            {"from": E2E["CRYPTOPAY_E2E_PAYER"], "to": E2E["CRYPTOPAY_E2E_USDC"], "data": data})])
        rpc.call("anvil_mine", ["0x3"])
        store.e2e_last_tx = tx
        return tx

    def e2e_report(**values):
        with open(E2E["CRYPTOPAY_E2E_OUT"], "w") as f:
            _e2e_json.dump(values, f)

default e2e_last_tx = None
default e2e_status = None
default e2e_restore_status = None

label start:
    $ result = cryptopay.pay("unlock", item="chapter2")
    $ e2e_status = result.status
    $ restore_result = cryptopay.restore()
    $ e2e_restore_status = restore_result.status
    $ renpy.quit()


init python:
    def e2e_restore_ctl():
        return python_list(cryptopay._state.restores.values())[0]



init python:
    def e2e_markers():
        """Which test milestones have been reached (shown as clickable markers)."""
        st = cryptopay._state
        ctl = st.checkouts.get("c1")
        out = python_list()
        if ctl is not None and ctl.session:
            out.append("e2e-session")
        if ctl is not None and ctl.state == "paid":
            out.append("e2e-paid")
        if st.restores:
            out.append("e2e-restore-open")
            if python_list(st.restores.values())[0].state == "done":
                out.append("e2e-restore-done")
        return out

    def e2e_method():
        ctl = cryptopay._state.checkouts.get("c1")
        return ctl.method if ctl is not None else None

    def e2e_step():
        ctl = cryptopay._state.checkouts.get("c1")
        return ctl.step if ctl is not None else None

    def e2e_has(marker):
        return marker in e2e_markers()

    config.overlay_screens.append("e2e_markers")

    # The test drives Ren'Py's virtual pointer. On a real display (x11) the desktop pointer
    # still sends enter/leave events for the window, and while Ren'Py believes the mouse is
    # outside it moves every mouse event, the test's synthetic clicks included, to (-1, -1):
    # clicks then hit nothing until the real pointer comes back. Ignore the real pointer so
    # only the test's pointer counts; clicks still go through Ren'Py's focus and buttons.
    import renpy.display.core as _e2e_core
    _e2e_pointer = python_dict(left=0)

    def _e2e_set_mouse_focused(interface, value):
        if not value:
            _e2e_pointer["left"] += 1

    _e2e_core.Interface.mouse_focused = property(lambda interface: True,
                                                 _e2e_set_mouse_focused)

screen e2e_markers():
    # Above the plugin's modal screens (zorder 200), so the test runner can find them.
    zorder 300
    vbox:
        align (1.0, 0.0)
        for m in e2e_markers():
            textbutton m action NullAction() text_size 10


init python:
    # Diagnostics: once a second, dump the plugin's state for tools/run_renpy_e2e.py.
    _e2e_debug = python_dict(last=0.0)

    def _e2e_debug_dump():
        now = _e2e_time.time()
        if now - _e2e_debug["last"] < 1.0 or "CRYPTOPAY_E2E_DEBUG" not in E2E:
            return
        _e2e_debug["last"] = now
        st = cryptopay._state
        import renpy.test.testexecution as _tx
        import renpy.test.testmouse as _tm
        lines = ["node=%s state=%r mouse=%r real_pointer_left=%d" % (
            getattr(_tx, "node_loc", None), getattr(_tx, "state", None), _tm.mouse_pos,
            _e2e_pointer["left"])]
        snap = st.worker.snapshot if st.worker else None
        lines += ["owned=%r snap_receipts=%r persistent_receipts=%r" % (
            sorted(snap.owned) if snap else None, len(snap.receipts) if snap else None,
            len(persistent.cryptopay_receipts or {}))]
        lines += ["running=%r status=%r restores=%d screens=%r" % (
            st.worker.running if st.worker else None, store.e2e_status, len(st.restores),
            [n for n in ("cryptopay_payment", "cryptopay_restore") if renpy.get_screen(n)])]
        for cid, c in st.checkouts.items():
            fut = c._future
            lines.append("%s state=%s step=%s error=%s method=%s session=%s fut_done=%s fut_exc=%r" % (
                cid, c.state, c.step, c.error, c.method, bool(c.session),
                fut.done() if fut else None, fut.exception() if fut and fut.done() else None))
        with open(E2E["CRYPTOPAY_E2E_DEBUG"], "a") as f:
            f.write(" | ".join(lines) + "\n")

    config.periodic_callbacks.append(_e2e_debug_dump)
