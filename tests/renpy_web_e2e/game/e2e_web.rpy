## Web end-to-end test game: the plugin in Ren'Py's web build, paying on a local Anvil
## chain from headless Chrome. Built and driven by tools/run_renpy_web_e2e.py through the
## page's renpy_exec() hook; never shipped. The runner writes e2e_web_config.json.

## Without a save directory the web build keeps persistent in game/saves, which lives in
## memory and is gone after a reload; with one it goes under ~/.renpy, the IndexedDB mount.
define config.save_directory = "scfg-web-e2e"

label main_menu:
    return

init python:
    import json as _e2e_json
    import os as _e2e_os
    from dataclasses import replace as _e2e_replace
    from renpy.persistent import load as _e2e_load_persistent

    from scfg_core import chains as _e2e_chains
    from scfg_core.chains import Token as _E2EToken

    E2E = _e2e_json.loads(renpy.open_file("e2e_web_config.json").read().decode("utf-8"))
    _e2e_chains.MAINNET["arbitrum"] = _e2e_replace(
        _e2e_chains.MAINNET["arbitrum"], rpc=(E2E["rpc"],), confirmations=2,
        tokens=python_dict(USDC=_E2EToken(E2E["usdc"])))
    scfg.configure(wallet=E2E["dev"], chains=["arbitrum"], tokens=["USDC"])
    scfg.add_item("chapter2", code=1, price="1.00", token="USDC", title="Chapter 2")

    def e2e_checkout():
        ctls = python_list(scfg._state.checkouts.values())
        return ctls[-1] if ctls else None

    def e2e_pending_units():
        """The amount (token units) of the first pending session in persistent, or None."""
        pending = persistent.scfg_pending or python_dict()
        for sid in sorted(pending):
            return int(pending[sid]["units"])
        return None

    def e2e_state():
        """What the runner polls: JSON-safe plain values."""
        ctl = e2e_checkout()
        worker = scfg._state.worker
        return python_dict(
            web=scfg.WEB, supported=scfg.supported(),
            worker=type(worker).__name__ if worker else None,
            cid=ctl.cid if ctl else None, step=ctl.step if ctl else None,
            state=ctl.state if ctl else None, error=ctl.error if ctl else None,
            methods=python_list(ctl.methods()) if ctl else python_list(),
            units=ctl.session["units"] if ctl and ctl.session else None,
            hint=ctl.hint_tx if ctl else None,
            owned=scfg.owns("chapter2"), receipts=len(scfg.receipts()),
            tip_total=str(scfg.tip_total()),
            pending=len(persistent.scfg_pending or python_dict()),
            pending_units=e2e_pending_units())

    def e2e_disk():
        """The persistent file a reload would load: its pending sessions and its mtime in
        ms, which the runner compares with the copy in IndexedDB."""
        fn = _e2e_os.path.join(renpy.config.savedir, "persistent")
        saved = _e2e_load_persistent(fn)
        pending = (saved.scfg_pending or python_dict()) if saved is not None else python_dict()
        mtime = int(_e2e_os.path.getmtime(fn) * 1000) if saved is not None else None
        return python_dict(path=fn, pending=len(pending), mtime=mtime)

default e2e_ready = False

label start:
    $ e2e_ready = True
label e2e_idle:
    "Web e2e running."
    jump e2e_idle
