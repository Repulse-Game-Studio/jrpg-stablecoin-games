## Stablecoin for Games (scfg): USDC/USDT tips and unlock purchases for Ren'Py, paid straight to
## the developer's wallet. No server, no accounts.
##
## Developer API (see docs/developers/):
##
##     init python:
##         scfg.add_chain(...)   # optional: your own EVM chain, before configure()
##         scfg.configure(wallet="0x...", testnet=True)
##         scfg.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")
##
##     $ result = scfg.pay("unlock", item="chapter2")
##     if scfg.owns("chapter2"):
##         jump chapter2
##
## Design notes for maintainers:
##   * All blockchain work runs in scfg_core (plain Python, unit-tested) on one
##     background Worker thread. This file only wires it into Ren'Py.
##   * Runtime objects live on the _state object created at init time. Store variables
##     created at init are never saved, so threads and sockets never end up in save files.
##   * Inside .rpy code, `object`, `dict` and `{}` are Ren'Py's rollback-tracked types.
##     Plugin state uses python_object/python_dict instead, so rolling back the story
##     never rewinds payments, and the worker thread never touches the rollback log.
##   * Named stores only see a minimal namespace, so displayables and actions are taken
##     from the main store (_store.Solid, _store.OpenURL, ...).
##   * Engine state is mirrored into persistent.scfg_* by a periodic callback on
##     the main thread, the only thread allowed to touch Ren'Py.

init -10 python in scfg:

    import json as _json
    import time as _time
    import os as _os
    import base64 as _base64
    import traceback as _traceback

    import store as _store

    from scfg_core import __version__ as version
    from scfg_core.config import build_config as _build_config, add_item as _add_item
    from scfg_core.config import build_chain as _build_chain
    from scfg_core.config import logo_content_type as _logo_content_type
    from scfg_core.errors import ConfigError, ScfgError
    from scfg_core.models import PaymentResult, RestoreResult
    from scfg_core.payments import prepare_payment as _prepare_payment
    from scfg_core.receipts import compute_ownership as _compute_ownership
    from scfg_core.engine import Engine as _Engine
    from scfg_core.worker import Worker as _Worker
    from scfg_core.pump_worker import PumpWorker as _PumpWorker
    from scfg_core.web_page import WebPage as _WebPage
    from scfg_core.overlay import OverlayCheckout as _OverlayCheckout
    from scfg_core import worker as _worker_module
    from scfg_core.rpc import UrllibTransport as _UrllibTransport
    from scfg_core.checkout import CheckoutController as _CheckoutController
    from scfg_core.restore_flow import RestoreController as _RestoreController
    from scfg_core.paylinks import qr_rects as _qr_rects

    ## Developer-settable options.
    on_result = None
    notify = True

    class _State(python_object):
        """Every runtime object of the plugin. Never saved, never rolled back."""

        def __init__(self):
            self.config = None
            self.custom_chains = python_list()  # add_chain(), consumed by configure()
            self.worker = None
            self.checkouts = python_dict()     # cid -> controller with its screen open
            self.background = python_dict()    # cid -> "continue playing" controller (browser page alive)
            self.restores = python_dict()
            self.callbacks = python_dict()     # session id -> developer callback
            self.restore_callbacks = python_dict()
            self.next_id = 0
            self.last_snapshot = None
            self.ui_signature = None
            self.last_save = 0.0
            self.qr_cache = python_dict()
            self.warned_unready = frozenset()
            self.web_page = None
            self.paste_target = None  # cid or rid waiting for the browser's clipboard
            self.copy_watch_until = 0.0  # web: until when to ask the page whether a Copy failed

    _state = _State()

    ## True in Ren'Py's web (HTML5) build: the game runs in a browser page, without
    ## threads or sockets. Payments go through the page (scfg/web/bridge.js).
    WEB = bool(renpy.emscripten)

    def _plain(obj):
        """Deep copy into plain Python containers (records are JSON-safe by design)."""
        return _json.loads(_json.dumps(obj or python_dict()))

    if _store.persistent.scfg_receipts is None:
        _store.persistent.scfg_receipts = python_dict()
    if _store.persistent.scfg_pending is None:
        _store.persistent.scfg_pending = python_dict()

    # ------------------------------------------------------------------ configuration

    def add_chain(key, name, chain_id, rpc, tokens, confirmations, explorer_tx,
                  native_symbol="ETH", fee_hint="low", testnet=False, max_log_range=1000):
        """Add an EVM chain that isn't built in (e.g. Avalanche, Optimism, your own L2).
        Call before configure(); the key can then be used in configure(chains=...,
        rpc_urls=..., confirmations=...). tokens: {"USDC": "0x..."}, 6-decimal tokens only.
        testnet=True adds it for configure(testnet=True) games instead of mainnet ones."""
        if _state.config is not None:
            raise ConfigError("call scfg.add_chain() before scfg.configure()")
        chain = _build_chain(key, name=name, chain_id=chain_id, rpc=rpc, tokens=tokens,
                             confirmations=confirmations, explorer_tx=explorer_tx,
                             native_symbol=native_symbol, fee_hint=fee_hint,
                             max_log_range=max_log_range, testnet=testnet)
        _state.custom_chains.append(chain)

    def configure(wallet=None, testnet=False, chains=None, tokens=None, methods=None,
                  rpc_urls=None, confirmations=None, previous_wallets=None,
                  logo=None, accent=None, solana_wallet=None):
        """Set the developer's payout wallet and options. Call once, in an init block.

        solana_wallet: your Solana wallet address (base58) to accept USDC/USDT on Solana;
        wallet= may then be left out.
        logo: an image in the game folder ("images/logo.png"), shown on the payment
        screens and the browser checkout page. accent: "#rrggbb" for selected options,
        e.g. accent=gui.accent_color."""
        config = _build_config(
            wallet, testnet=testnet, chains=chains, tokens=tokens, methods=methods,
            rpc_urls=rpc_urls, confirmations=confirmations, previous_wallets=previous_wallets,
            logo=logo, accent=accent, custom_chains=_state.custom_chains,
            solana_wallet=solana_wallet)
        if config.logo is not None and not renpy.loadable(config.logo):
            raise ConfigError("logo file not found in the game folder: %r" % (config.logo,))
        _state.config = config

    def add_item(item_id, code, price, token, title, min_price=None):
        """Declare a paid unlock. `code` (1-99) must never change after release."""
        return _add_item(_require_config(), item_id, code=code, price=price, token=token,
                         title=title, min_price=min_price)

    def _require_config():
        if _state.config is None:
            raise ConfigError("call scfg.configure(...) in an init python block first")
        return _state.config

    # ------------------------------------------------------------------ platform support

    def supported():
        """True where payments can run: every platform Ren'Py builds for, the web (HTML5)
        build included. Kept so games written for older plugin versions still work."""
        return True

    # ------------------------------------------------------------------ worker lifecycle

    def _unready():
        return _state.worker.snapshot.unready if _state.worker is not None else frozenset()

    def _is_mobile():
        return bool(renpy.mobile or renpy.variant("mobile"))

    def _interval():
        return 3.0 if (_state.checkouts or _state.restores) else 15.0

    def _warn(message):
        """Setup advice for the developer: printed in developer mode only, never in a
        distributed game (config.developer is False there)."""
        if renpy.config.developer:
            print("scfg: " + message)

    def _report_error(heading, exc=None):
        """An error for the developer. On desktop it goes to the terminal, which players never
        see. Ren'Py's web build shows printed text over the game, so there it is printed in
        developer mode only."""
        if WEB and not renpy.config.developer:
            return
        print("scfg: " + heading)
        if exc is None:
            _traceback.print_exc()
        else:
            _traceback.print_exception(type(exc), exc, exc.__traceback__)

    def _log_error(exc):
        _report_error("worker error:", exc)

    def _ensure_started():
        if _state.worker is not None:
            return _state.worker
        config = _require_config()
        # After a script reload (Shift+R) the previous worker thread still exists.
        previous = getattr(_worker_module, "current", None)
        if previous is not None:
            previous.stop(0.5)
        engine = _Engine(config, transport=None if WEB else _UrllibTransport(),
                         pending=_plain(_store.persistent.scfg_pending),
                         receipts=_plain(_store.persistent.scfg_receipts),
                         on_error=_log_error)
        if WEB:
            if not renpy.config.save_directory:
                _warn("WARNING: config.save_directory is not set. In the web build the "
                      "player's receipts are then lost when the tab closes. Set "
                      "config.save_directory to a unique name in options.rpy.")
            # No threads in the browser: the frame pump (_periodic) drives the engine.
            worker = _PumpWorker(engine, _web_page(), interval=_interval, on_error=_log_error)
        else:
            worker = _Worker(engine, interval=_interval, on_error=_log_error)
        worker.start()
        _worker_module.current = worker
        _state.worker = worker
        return worker

    def _shutdown():
        for c in python_list(_state.checkouts.values()) + python_list(_state.background.values()):
            c.close()
        if _state.worker is not None:
            _state.worker.stop(1.0)

    # ------------------------------------------------------------------ main-thread pump

    def _periodic():
        """Runs on the main thread ~20x per second during interactions."""
        if _state.config is None:
            return
        try:
            worker = _ensure_started()
            if WEB:
                worker.pump()
                changed = _take_paste()
                if _time.time() < _state.copy_watch_until and _web_page().take_copy_failed():
                    _state.copy_watch_until = 0.0
                    renpy.notify(_("Couldn't copy: your browser blocked the clipboard."))
            else:
                changed = False
            controllers = python_list(_state.checkouts.values()) + python_list(_state.background.values())
            for outcome in worker.drain_outcomes():
                changed = True
                for c in controllers:
                    c.on_outcome(outcome)
                if outcome.result is not None:
                    _deliver(outcome.result)
            for c in controllers:
                before = c.state
                c.poll()
                _register_callback(c)
                changed = changed or c.state != before
            timeout = _state.config.methods["browser"]["timeout_min"] * 60
            now = _time.time()
            for cid, c in python_list(_state.background.items()):
                expired = c.session is None or now - c.session["created_at"] > timeout
                if expired or c.state in ("paid", "underpaid", "expired", "choose"):
                    c.close()
                    del _state.background[cid]
            for r in python_list(_state.restores.values()):
                before = r.state
                r.poll()
                if r.state != before:
                    changed = True
                    if r.state == "done":
                        _deliver_restore(r)
            snap = worker.snapshot
            if snap.unready and snap.unready != _state.warned_unready:
                _state.warned_unready = snap.unready
                for key, token in sorted(snap.unready):
                    _warn("your wallet can't receive %s on %s yet (no token account). "
                          "Receive any amount of %s there once; until then players aren't offered it."
                          % (token, key, token))
            if snap is not _state.last_snapshot:
                _state.last_snapshot = snap
                _sync_persistent(snap)
                signature = _ui_signature(snap)
                if signature != _state.ui_signature:
                    _state.ui_signature = signature
                    changed = True
            if changed:
                # Only refresh screens when something visible changed: a refresh landing
                # exactly on a player's click can swallow that click.
                renpy.restart_interaction()
        except Exception:
            _report_error("error in the payment pump:")

    def _clean_paste(text):
        """What a pasted clipboard becomes in a hash field, like typing it there: the
        first word, only hash characters, at most 90 of them."""
        words = (text or "").split()
        if not words:
            return ""
        return "".join(ch for ch in words[0] if ch in TX_CHARS)[:90]

    def _take_paste():
        """Web build: the browser's answer to a Paste button, into the tx field of the
        payment screen (cid) or the restore screen (rid)."""
        target = _state.paste_target
        if target is None:
            return False
        text = _web_page().take_paste()
        if text is None:
            return False
        _state.paste_target = None
        text = _clean_paste(text)
        ctl = _state.checkouts.get(target)
        if ctl is None:
            ctl = _state.restores.get(target)
            if ctl is not None and ctl.state != "input":
                return True       # the player moved on (Check was pressed): drop the paste
        if ctl is not None:
            if text:
                ctl.tx_input = text
                ctl.error = None
            else:
                ctl.error = "paste_blocked"
        return True

    def _paste_hash(target):
        _state.paste_target = target
        _web_page().request_paste()

    def _ui_signature(snap):
        sessions = sorted((sid, r.get("state"), r.get("ui_status"), r.get("hint_tx"))
                          for sid, r in snap.pending.items())
        return (snap.owned, snap.tip_total, tuple(sessions), len(snap.receipts), snap.unready)

    PENDING_SAVE_INTERVAL = 10.0

    def _pending_key(pending):
        """What must reach disk at once: which sessions exist, their state and tx hint."""
        key = python_dict()
        for sid, r in (pending or python_dict()).items():
            # python_dict: inside .rpy code `dict` is Ren'Py's revertable subclass.
            key[sid] = (r.get("state"), r.get("hint_tx")) if isinstance(r, python_dict) else r
        return key

    def _sync_persistent(snap):
        """Mirror engine state into persistent. New receipts, new or removed pending
        sessions and a session's state or tx hint changing are written to disk at once
        (in the web build a tab can be closed any moment); other pending changes, such as
        scan progress (cursors), at most every PENDING_SAVE_INTERVAL seconds."""
        p = _store.persistent
        receipts_changed = p.scfg_receipts != snap.receipts
        pending_changed = p.scfg_pending != snap.pending
        if not (receipts_changed or pending_changed):
            return
        sessions_changed = _pending_key(p.scfg_pending) != _pending_key(snap.pending)
        p.scfg_receipts = _plain(snap.receipts)
        p.scfg_pending = _plain(snap.pending)
        now = _time.time()
        if (receipts_changed or sessions_changed
                or now - _state.last_save >= PENDING_SAVE_INTERVAL):
            _state.last_save = now
            renpy.save_persistent()

    def _call(fn, *args):
        if fn is None:
            return
        try:
            fn(*args)
        except Exception:
            _report_error("exception in developer callback")

    def _deliver(result):
        cb = _state.callbacks.get(result.session_id) or on_result
        if result.status in ("paid", "underpaid"):
            _state.callbacks.pop(result.session_id, None)
        if result.status == "paid" and notify:
            item = _state.config.items.get(result.item) if result.item else None
            if item is not None:
                # notify() interpolates [..] and {..}: escape the developer's title.
                title = item.title.replace("[", "[[").replace("{", "{{")
                renpy.notify(_("Payment confirmed: %s. Thank you!") % title)
            else:
                renpy.notify(_("Payment confirmed. Thank you for your support!"))
        _call(cb, result)

    def _deliver_restore(controller):
        cb = _state.restore_callbacks.pop(controller.rid, None)
        result = controller.result()
        if result.restored and notify:
            renpy.notify(_("Purchases restored."))
        _call(cb, result)

    # ------------------------------------------------------------------ ownership

    def _snapshot_ownership():
        if _state.worker is not None:
            snap = _state.worker.snapshot
            return snap.owned, snap.tip_total
        owned, tips = _compute_ownership(_plain(_store.persistent.scfg_receipts),
                                         _require_config().items)
        return frozenset(owned), tips

    def owns(item_id):
        """True if a verified receipt unlocks `item_id`. Instant and offline."""
        if item_id not in _require_config().items:
            raise ConfigError("unknown item %r; declare it with scfg.add_item()" % (item_id,))
        return item_id in _snapshot_ownership()[0]

    def has_tipped():
        return _snapshot_ownership()[1] > 0

    def tip_total():
        return _snapshot_ownership()[1]

    def receipts():
        """Stored receipts as plain dicts (chain, tx_hash, kind, item, amount, token, ...)."""
        source = _state.worker.snapshot.receipts if _state.worker else _store.persistent.scfg_receipts
        return python_list(_plain(source).values())

    def pending():
        """Payments still being watched for."""
        source = _state.worker.snapshot.pending if _state.worker else _store.persistent.scfg_pending
        return python_list(_plain(source).values())

    def clear_local_data():
        """Testing helper: forget every receipt and pending payment on THIS device.

        Use it to see your paywall again after a test purchase. Real purchases are not
        lost: the payments are still on the blockchain and Restore brings them back.
        """
        def wipe(engine):
            engine.receipts.clear()
            engine.pending.clear()
        if _state.worker is not None:
            _state.worker.submit(wipe)
        _store.persistent.scfg_receipts = python_dict()
        _store.persistent.scfg_pending = python_dict()
        _state.last_snapshot = None
        renpy.save_persistent()
        renpy.restart_interaction()

    def export_receipts(path=None):
        """Support helper: write receipts as readable JSON. Returns the file path."""
        path = path or _os.path.join(renpy.config.basedir, "scfg-receipts.json")
        with open(path, "w") as f:
            _json.dump(receipts(), f, indent=2, sort_keys=True)
        return path

    # ------------------------------------------------------------------ payment flow

    class _ScreenCheckout(_CheckoutController):
        """CheckoutController + an id. Pickles as a lookup, so a save taken while the
        payment screen is up never tries to save threads or sockets."""

        def __reduce__(self):
            return (_lookup_checkout, (self.cid,))

    class _Closed(python_object):
        """Stands in for a controller that no longer exists (e.g. after loading a save)."""
        state = "closed"
        error = None
        blocking = False

        def __init__(self, cid=None):
            self.cid = cid

        def __reduce__(self):
            return (_lookup_checkout, (self.cid,))

        def message(self):
            return None

        def dev_warnings(self):
            return python_list()

        def pending_choices(self):
            return python_list()

        def __getattr__(self, name):
            return None

    def _lookup_checkout(cid):
        return _state.checkouts.get(cid) or _Closed(cid)

    def _lookup_restore(rid):
        return _state.restores.get(rid) or _Closed(rid)

    def _read_game_file(path):
        f = renpy.open_file(path)
        try:
            return f.read()
        finally:
            f.close()

    def _web_page():
        """The page's JavaScript side (web build only), loaded on first use."""
        if _state.web_page is None:
            import emscripten
            page = _WebPage(emscripten, lambda name: _read_game_file("scfg/web/" + name))
            page.install()
            _state.web_page = page
        return _state.web_page

    def _logo_data_url():
        logo = _state.config.logo
        if logo is None:
            return None
        return "data:%s;base64,%s" % (_logo_content_type(logo),
                                      _base64.b64encode(_read_game_file(logo)).decode("ascii"))

    def _server_factory(session_info, on_tx, get_status, build_tx=None):
        if WEB:
            return _OverlayCheckout(ui=_web_page(), key=_new_id("o"), session_info=session_info,
                                    on_tx=on_tx, get_status=get_status,
                                    logo_url=_logo_data_url(), build_tx=build_tx)
        from scfg_core.local_server import CheckoutServer

        def load_asset(name):
            return _read_game_file("scfg/web/" + name)
        load_logo = None
        logo = _state.config.logo
        if logo is not None:
            # Read here, on the main thread; the server thread only hands out the bytes.
            logo_file = (_read_game_file(logo), _logo_content_type(logo))
            load_logo = lambda: logo_file
        return CheckoutServer(load_asset=load_asset, session_info=session_info,
                              on_tx=on_tx, get_status=get_status, load_logo=load_logo,
                              build_tx=build_tx)

    def _new_id(prefix):
        _state.next_id += 1
        return "%s%d" % (prefix, _state.next_id)

    def _open_checkout(kind, item, amount, token, callback, blocking):
        spec = _prepare_payment(_require_config(), kind, item=item, amount=amount, token=token)
        if kind == "unlock" and item in _snapshot_ownership()[0]:
            return PaymentResult(status="already_owned", kind=kind, item=item, token=spec.token)
        worker = _ensure_started()
        cid = _new_id("c")
        ctl = _ScreenCheckout(config=_state.config, spec=spec, submit=worker.submit,
                              mobile=_is_mobile(), server_factory=_server_factory,
                              unready=_unready, game_name=renpy.config.name or "Game",
                              browser_wallet=_web_page().has_wallet if WEB else None)
        ctl.cid = cid
        ctl.blocking = blocking
        ctl.callback = callback
        _state.checkouts[cid] = ctl
        if blocking:
            return renpy.call_screen("scfg_payment", cid=cid)
        renpy.show_screen("scfg_payment", cid=cid)
        renpy.restart_interaction()
        return None

    def pay(kind, item=None, amount=None, token=None, callback=None):
        """Show the payment screen and return a PaymentResult when it closes.

        kind="unlock": item is required; amount/token default to the item's.
        kind="tip": amount (suggested; the player may change it) and token are required.
        Amounts are strings like "4.99" or Decimals, never floats.
        Call from script (``$ result = scfg.pay(...)``); from screens use ScfgPay().
        """
        return _open_checkout(kind, item, amount, token, callback, blocking=True)

    def _register_callback(ctl):
        if ctl.session_id and ctl.callback is not None:
            _state.callbacks[ctl.session_id] = ctl.callback

    def _close_checkout(cid, how):
        """Close the payment screen. how: cancel | keep_playing | finish."""
        ctl = _state.checkouts.pop(cid, None)
        if ctl is None:
            renpy.hide_screen("scfg_payment")
            return None
        _register_callback(ctl)
        if how == "cancel":
            result = ctl.cancel()
            _call(ctl.callback or on_result, result)
        elif how == "keep_playing" and ctl.session_id:
            result = ctl.keep_playing()
            _state.background[cid] = ctl   # keeps the browser page answering until final
        else:
            result = ctl.finish()
        if ctl.blocking:
            return result
        renpy.hide_screen("scfg_payment")
        return None

    # ------------------------------------------------------------------ screen helpers

    def _choose_network(cid, key):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.choose_network(key)

    def _set_tip(cid, value):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.set_tip_amount(value)

    def _apply_custom_tip(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.apply_custom_amount()

    def _choose_method(cid, name):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.choose_method(name)

    def _next_step(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.next_step()

    def _back(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.back()

    def _mark_sent(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.mark_sent()

    def _submit_hash(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None:
            ctl.submit_hash()

    def _open_url(url):
        """Ren'Py's OpenURL does nothing in the web build; open it from the page there."""
        if WEB:
            _web_page().open_url(url)
        else:
            _store.OpenURL(url)()

    def _open_browser(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is None:
            return None
        url = ctl.open_browser()
        if url and not WEB:           # on the web the overlay is already showing
            _open_url(url)
        return None

    def _open_wallet_app(cid):
        ctl = _state.checkouts.get(cid)
        if ctl is not None and ctl.payment_uri():
            _open_url(ctl.payment_uri())
        return None

    def _open_explorer(url):
        if url:
            _open_url(url)
        return None

    class Copy(_store.Action):
        """Copy text to the clipboard. Ren'Py's CopyToClipboard does nothing in the web
        build; there the browser's clipboard is used."""

        def __init__(self, text):
            self.text = text

        def __call__(self):
            if WEB:
                _state.copy_watch_until = _time.time() + 5.0
                _web_page().copy(self.text)
            else:
                _store.CopyToClipboard(self.text)()
            return None

    def QR(text, size=280, color="#000000", background="#ffffff"):
        """A QR code displayable, cached by text and size."""
        key = (text, size, color, background)
        d = _state.qr_cache.get(key)
        if d is None:
            modules, runs = _qr_rects(text)
            scale = float(size) / modules
            children = python_list([_store.Solid(background, xysize=(size, size))])
            for x, y, w in runs:
                x0 = int(round(x * scale))
                y0 = int(round(y * scale))
                x1 = int(round((x + w) * scale))
                y1 = int(round((y + 1) * scale))
                children.append(_store.Transform(_store.Solid(color), xpos=x0, ypos=y0,
                                                 xsize=max(1, x1 - x0), ysize=max(1, y1 - y0)))
            d = _store.Fixed(*children, xysize=(size, size))
            if len(_state.qr_cache) > 16:
                _state.qr_cache.clear()
            _state.qr_cache[key] = d
        return d

    # ------------------------------------------------------------------ restore flow

    def _open_restore(callback, blocking):
        worker = _ensure_started()
        rid = _new_id("r")
        ctl = _RestoreController(submit=worker.submit)
        ctl.rid = rid
        ctl.blocking = blocking
        _state.restores[rid] = ctl
        if callback is not None:
            _state.restore_callbacks[rid] = callback
        if blocking:
            return renpy.call_screen("scfg_restore", rid=rid)
        renpy.show_screen("scfg_restore", rid=rid)
        renpy.restart_interaction()
        return None

    def restore(callback=None):
        """Show the restore screen (paste a transaction hash). Returns a RestoreResult."""
        return _open_restore(callback, blocking=True)

    def _close_restore(rid):
        ctl = _state.restores.pop(rid, None)
        _state.restore_callbacks.pop(rid, None)
        result = ctl.result() if ctl is not None else RestoreResult(status="cancelled")
        if ctl is not None and ctl.blocking:
            return result
        renpy.hide_screen("scfg_restore")
        return None

    def _restore_check(rid):
        ctl = _state.restores.get(rid)
        if ctl is not None:
            ctl.check()

    def _restore_choose(rid, index, choice):
        ctl = _state.restores.get(rid)
        if ctl is not None:
            ctl.choose(index, choice)

    def _restore_confirm(rid):
        ctl = _state.restores.get(rid)
        if ctl is not None:
            ctl.confirm()

    def _item_title(item_id):
        item = _require_config().items.get(item_id) if item_id else None
        return item.title if item else item_id

    def _explorer_url(chain_key, tx_hash):
        chain = _require_config().registry.get(chain_key)
        return chain.explorer_url(tx_hash) if chain else None

    def _chain_name(chain_key):
        chain = _require_config().registry.get(chain_key)
        return chain.name if chain else chain_key

    renpy.config.periodic_callbacks.append(_periodic)
    renpy.config.quit_callbacks.append(_shutdown)


init -10 python:

    class ScfgPay(Action, DictEquality):
        """Screen action: open the payment screen without blocking the script.
        The outcome arrives through `callback` / scfg.on_result."""

        def __init__(self, kind, item=None, amount=None, token=None, callback=None):
            self.kind = kind
            self.item = item
            self.amount = amount
            self.token = token
            self.callback = callback

        def __call__(self):
            result = scfg._open_checkout(self.kind, self.item, self.amount, self.token,
                                              self.callback, blocking=False)
            if result is not None and self.callback is not None:
                scfg._call(self.callback, result)   # e.g. already_owned
            return None

    class CryptoRestore(Action, DictEquality):
        """Screen action: open the restore screen without blocking the script."""

        def __init__(self, callback=None):
            self.callback = callback

        def __call__(self):
            result = scfg._open_restore(self.callback, blocking=False)
            if result is not None and self.callback is not None:
                scfg._call(self.callback, result)   # e.g. unsupported platform
            return None
