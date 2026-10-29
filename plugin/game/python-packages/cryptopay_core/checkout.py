"""State behind the payment screen, independent of Ren'Py.

The Ren'Py screen only displays this object's fields and calls its methods from
buttons. All engine work goes through ``submit`` (the Worker), and results come
back through ``poll()`` (futures) and ``on_outcome()`` (watcher outcomes routed by
session id). Keeping this in plain Python makes the whole flow unit-testable.

States: choose -> starting -> waiting -> confirming -> paid
                                     \\-> underpaid | expired

The sheet shows one step at a time (``step``):
amount (tips only) -> network -> method -> browser | qr | manual -> waiting -> done.
Steps with a single option are skipped both ways. ``back()`` works until a transaction
is seen; a seen payment moves the sheet to waiting and a final state to done.
"""

from __future__ import annotations

from collections.abc import Callable
from decimal import Decimal

from .amounts import format_units, parse_amount
from .backends import backend_for
from .chains import Chain
from .config import Config, enabled_chains, text_on
from .models import PaymentResult
from .paylinks import eip681_transfer_uri
from .payments import MIN_TIP, TIP_PRESETS, PaymentSpec
from .rpc import RpcUnavailable
from .solana.paylinks import solana_pay_uri

FINAL_STATES = ("paid", "underpaid", "expired")
# Errors that judge the reported transaction (the hint) and drop it.
HINT_VERDICTS = ("tx_reverted", "tx_not_a_payment", "tx_not_landed")
# Order of the steps; the three method pages share a place.
STEP_ORDER = {"amount": 0, "network": 1, "method": 2, "browser": 3, "qr": 3, "manual": 3,
              "waiting": 4, "done": 5}

MESSAGES = {
    "network_unavailable": "Can't reach the network. Retrying...",
    "tx_reverted": "That transaction failed on-chain; no funds were sent.",
    "tx_not_a_payment": "That transaction isn't a payment for this purchase.",
    "tx_not_landed": "That transaction didn't reach the network. You can pay again.",
    "invalid_hash": "That doesn't look like a transaction hash or signature.",
    "invalid_amount": f"Enter an amount of at least {MIN_TIP} with up to 2 decimals.",
    "browser_unavailable": "Couldn't start the browser checkout. Use another method.",
    "start_failed": "Couldn't start the payment. Check your connection and try again.",
    "solana_unavailable": "Payments on Solana aren't available right now.",
    "internal_error": "Something went wrong while checking the payment. Retrying...",
    "paste_blocked": ("Your browser blocked pasting. Type it instead: "
                      "the game also finds the payment without it."),
}


class CheckoutController:
    def __init__(self, *, config: Config, spec: PaymentSpec, submit: Callable,
                 mobile: bool = False, server_factory: Callable | None = None,
                 unready: Callable[[], frozenset] | None = None, game_name: str = "Game",
                 browser_wallet: Callable[[Chain], bool] | None = None):
        self.config = config
        self.spec = spec
        self.submit = submit
        self.mobile = mobile
        self.server_factory = server_factory
        self.unready = unready or frozenset  # frozenset() is an empty set: nothing blocked
        self.browser_wallet = browser_wallet
        self.game_name = game_name or "Game"
        self.amount: Decimal = spec.price
        self.custom_amount = ""
        self.tx_input = ""
        self.chain_key: str | None = None
        self.session: dict | None = None
        self.method: str | None = None
        self.state = "choose"
        self.error: str | None = None
        self.confirmations = 0
        self.required = 0
        self.hint_tx: str | None = None
        self.dropped_tx: str | None = None  # the hint that never landed (tx_not_landed)
        self.result: PaymentResult | None = None
        self.browser_failed = False
        self._future = None
        self._server = None
        self.step = "amount" if spec.kind == "tip" else "network"
        self.nav_dir = 1  # 1: the step came in going forward, -1: going back
        self._picked_network = False  # move on from the network step once the session starts
        if self.step == "network":
            self._enter_network()

    # --- read-only views ---------------------------------------------------------

    @property
    def session_id(self) -> str | None:
        return self.session["id"] if self.session else None

    @property
    def chain(self) -> Chain | None:
        return enabled_chains(self.config).get(self.chain_key) if self.chain_key else None

    @property
    def family(self) -> str | None:
        return self.chain.family if self.chain else None

    def chain_options(self) -> list[tuple[str, str, str]]:
        chains = enabled_chains(self.config)
        blocked = self.unready()
        return [(k, chains[k].name, chains[k].fee_hint) for k in self.spec.chains
                if k in chains and (k, self.spec.token) not in blocked]

    def tip_presets(self) -> tuple[Decimal, ...]:
        return TIP_PRESETS

    def methods(self) -> list[str]:
        m = self.config.methods
        out = []
        if m["browser"]["enabled"] and not self.browser_failed and self._browser_possible():
            out.append("browser")
        qr_chains = m["qr"]["chains"]
        if m["qr"]["enabled"] and (qr_chains is None or self.chain_key in qr_chains):
            out.append("qr")
        if m["manual"]["enabled"]:
            out.append("manual")
        return out

    def _browser_possible(self) -> bool:
        """Desktop: a computer's browser can always open the page. Web build: only if a
        wallet that can pay on the chosen network is in the page (phones included: a
        wallet app's own browser injects one)."""
        if self.browser_wallet is not None:
            return self.chain is not None and self.browser_wallet(self.chain)
        return not self.mobile

    @property
    def pay_amount_text(self) -> str:
        return format_units(self.session["units"]) if self.session else f"{self.amount}"

    @property
    def token_address(self) -> str | None:
        chain = self.chain
        return chain.tokens[self.spec.token].address if chain else None

    @property
    def recipient(self) -> str:
        if self.chain is not None:
            return self.config.wallet_for(self.chain)
        return self.config.wallet or self.config.solana_wallet

    def payment_uri(self) -> str | None:
        if not self.session:
            return None
        if self.family == "solana":
            return solana_pay_uri(recipient=self.recipient, units=self.session["units"],
                                  mint=self.token_address, reference=self.session["reference"],
                                  label=self.game_name, message=self.spec.title)
        return eip681_transfer_uri(token_address=self.token_address, chain_id=self.chain.chain_id,
                                   recipient=self.recipient, units=self.session["units"])

    def explorer_url(self) -> str | None:
        tx = self.result.tx_hash if self.result and self.result.tx_hash else self.hint_tx
        return self.chain.explorer_url(tx) if tx and self.chain else None

    def message(self) -> str | None:
        if self.error:
            return MESSAGES.get(self.error)
        if self.state == "choose" and self.spec.chains and not self.chain_options():
            return MESSAGES["solana_unavailable"]
        return None

    def dev_warnings(self) -> list[str]:
        """Shown in DEV MODE only: setup problems the developer must fix."""
        registry = self.config.registry
        return [f"Your Solana wallet can't receive {token} yet: receive any amount once."
                for key, token in sorted(self.unready())
                if key in registry and registry[key].family == "solana"]

    def status_info(self) -> dict:
        """Status for the localhost page (read from the server thread).

        ``dropped_tx`` names the transaction a ``tx_not_landed`` is about, so the page
        offers Pay again only if it is the one the page sent."""
        return {"state": self.state, "confirmations": self.confirmations,
                "required": self.required, "tx_hash": self.hint_tx, "error": self.error,
                "dropped_tx": self.dropped_tx}

    @property
    def tx_seen(self) -> bool:
        """A transaction for this payment exists, so going back could start a second one."""
        return bool(self.hint_tx) or self.state == "confirming"

    def step_path(self) -> list[str]:
        """The steps this payment walks through, without the single-option ones."""
        path = ["amount"] if self.spec.kind == "tip" else []
        if len(self.chain_options()) != 1:
            path.append("network")
        if len(self.methods()) != 1:
            path.append("method")
        return path + [self.method or "browser", "waiting"]

    def step_dots(self) -> tuple[int, int]:
        """(index of the current step, number of steps) for the progress dots."""
        path = self.step_path()
        here = STEP_ORDER[self.step]
        return sum(1 for s in path if STEP_ORDER[s] < here), len(path)

    @property
    def can_go_back(self) -> bool:
        if self.step == "done" or self.tx_seen:
            return False
        return self._previous_step() is not None

    # --- actions -----------------------------------------------------------------

    def select_chain(self, key: str) -> None:
        if key not in self.spec.chains:
            raise ValueError(f"chain {key!r} not available for this payment")
        self._discard_session()
        self.chain_key = key
        self.state = "starting"
        self.error = None
        spec, amount = self.spec, self.amount
        self._future = self.submit(lambda e: e.start_session_co(
            spec.kind, key, item=spec.item, amount=amount, token=spec.token, title=spec.title))
        available = self.methods()
        if self.method not in available:
            self.method = available[0] if available else None

    def choose_network(self, key: str) -> None:
        """Network step: start a session on `key`, or keep the one already running."""
        self.step, self.nav_dir = "network", 1
        if key == self.chain_key and self.session and self.state == "waiting":
            self._after_network()
            return
        self._picked_network = True  # poll() moves on once the session has started
        if key != self.chain_key or self.state != "starting":
            self.select_chain(key)

    def choose_method(self, name: str) -> None:
        if name in self.methods():
            self.method = name
            self.error = None
            self._go(name, 1)

    def next_step(self) -> bool:
        """Leave the amount step (tips), applying a typed amount first."""
        if self.step != "amount":
            return False
        if self.custom_amount.strip() and not self.apply_custom_amount():
            return False
        self._enter_network()
        return True

    def back(self) -> None:
        if self.can_go_back:
            self.error = None
            self._go(self._previous_step(), -1)

    def mark_sent(self) -> None:
        """The player says they sent the payment from the QR or manual page."""
        if self.step in ("qr", "manual"):
            self._go("waiting", 1)

    def open_browser(self) -> str | None:
        """Browser page: start the localhost page and move to waiting; returns its URL."""
        url = self.start_browser()
        if url:
            self._go("waiting", 1)
        elif self.browser_failed and self.step == "browser":
            methods = self.methods()
            self._go("method" if len(methods) != 1 else methods[0], -1)
        return url

    def set_tip_amount(self, value) -> bool:
        if self.spec.kind != "tip":
            return False
        try:
            amount = parse_amount(value)
        except (TypeError, ValueError):
            self.error = "invalid_amount"
            return False
        if amount < MIN_TIP:
            self.error = "invalid_amount"
            return False
        self.error = None
        if amount == self.amount and self.chain_key:
            return True  # same amount: keep the session already started
        self.amount = amount
        if self.chain_key:
            self.select_chain(self.chain_key)
        return True

    def apply_custom_amount(self) -> bool:
        return self.set_tip_amount(self.custom_amount.strip())

    def submit_hash(self, text: str | None = None) -> bool:
        if not self.session or self.chain is None:
            return False
        try:
            tx = backend_for(self.chain).normalize_tx(self.tx_input if text is None else text)
        except ValueError:
            self.error = "invalid_hash"
            return False
        self._report_hint(tx)
        self.tx_input = ""
        return True

    def _report_hint(self, tx_hash: str, sender: str | None = None) -> None:
        sid = self.session_id
        self.hint_tx = tx_hash
        self.error = None
        self.dropped_tx = None
        self.submit(lambda e: e.report_hint(sid, tx_hash))
        if self.step != "done":
            self._go("waiting", 1)

    def start_browser(self) -> str | None:
        """Start the localhost page for this session; returns its URL or None."""
        if not self.session or self.server_factory is None:
            return None
        if self._server is None:
            chain = self.chain
            info = {
                "title": self.spec.title, "kind": self.spec.kind, "token": self.spec.token,
                "family": chain.family, "decimals": 6,
                "amount_units": str(self.session["units"]), "amount_text": self.pay_amount_text,
                "recipient": self.recipient, "chain_name": chain.name,
                "native_symbol": chain.native_symbol,
                "explorer_tx": chain.explorer_tx, "testnet": self.config.testnet,
                "logo": self.config.logo is not None, "accent": self.config.accent,
                "on_accent": text_on(self.config.accent),
            }
            if chain.family == "solana":
                info.update({"wallet_chain": chain.wallet_chain, "mint": self.token_address,
                             "reference": self.session["reference"]})
            else:
                info.update({"chain_id": chain.chain_id, "chain_id_hex": hex(chain.chain_id),
                             "rpc_url": chain.rpc[-1], "token_address": self.token_address})
            kwargs = dict(session_info=info, on_tx=self._report_hint, get_status=self.status_info)
            if chain.family == "solana":
                kwargs["build_tx"] = self._submit_build
            try:
                self._server = self.server_factory(**kwargs)
                self._server.start()
            except Exception:
                self._server = None
                self.browser_failed = True
                self.error = "browser_unavailable"
                if self.method == "browser":
                    self.method = self.methods()[0] if self.methods() else None
                return None
        else:
            reopen = getattr(self._server, "reopen", None)
            if reopen is not None:
                reopen()
        return self._server.url

    def _submit_build(self, account: str):
        """Ask the Worker for the unsigned Solana transfer; returns its Future. The desktop
        page's server waits on it, the web overlay polls it."""
        sid = self.session_id
        return self.submit(lambda e: e.build_solana_payment_co(sid, account))

    def cancel(self) -> PaymentResult:
        sid = self.session_id
        if sid and self.state not in FINAL_STATES:
            self.submit(lambda e: e.set_ui_status(sid, "cancelled"))
        self.close()
        self.result = self.result or self._result("cancelled")
        return self.result

    def keep_playing(self) -> PaymentResult:
        sid = self.session_id
        if sid:
            self.submit(lambda e: e.set_ui_status(sid, "pending"))
        return self._result("pending")

    def finish(self) -> PaymentResult:
        """Close after a final state (paid/underpaid/expired)."""
        self.close()
        return self.result or self._result("cancelled")

    def close(self) -> None:
        self._abandon_start()
        if self._server is not None:
            self._server.stop()
            self._server = None

    # --- updates -------------------------------------------------------------

    def poll(self) -> None:
        """Pick up the result of start_session (call often from the UI)."""
        pump = getattr(self._server, "pump", None)
        if pump is not None:
            pump()
        fut = self._future
        if fut is None or not fut.done():
            return
        self._future = None
        exc = fut.exception()
        if exc is not None:
            self.state = "choose"
            self.chain_key = None
            network = isinstance(exc, RpcUnavailable)
            self.error = "network_unavailable" if network else "start_failed"
            if self.step not in ("amount", "network"):
                self._go("network", -1)
            return
        self.session = fut.result()
        self.state = "waiting"
        self.required = self.chain.confirmations if self.chain else 0
        if self.step == "network" and self._picked_network:
            self._after_network()

    def on_outcome(self, outcome) -> None:
        if not self.session or outcome.session_id != self.session_id:
            return
        if outcome.error in HINT_VERDICTS:
            self._hint_verdict(outcome)
        elif outcome.error:
            self.error = outcome.error
        status = outcome.status
        if status == "confirming":
            self.state = "confirming"
            self.confirmations = outcome.confirmations
            self.required = outcome.required
            if outcome.error is None:
                self.error = None
        elif status == "reorged":
            self.state = "waiting"
            self.confirmations = 0
        elif status in ("paid", "underpaid"):
            self.state = status
            self.result = outcome.result
            self.error = None
        elif status == "expired" and self.state not in ("paid", "underpaid"):
            self.state = "expired"
            self.result = outcome.result
        if self.state in FINAL_STATES:
            if self.step != "done":
                self._go("done", 1)
        elif self.tx_seen and self.step != "waiting":
            self._go("waiting", 1)

    # --- internals -----------------------------------------------------------

    def _go(self, step: str, direction: int) -> None:
        self.step, self.nav_dir = step, direction
        self._picked_network = False

    def _previous_step(self) -> str | None:
        here = STEP_ORDER[self.step]
        earlier = [s for s in self.step_path() if STEP_ORDER[s] < here]
        return earlier[-1] if earlier else None

    def _enter_network(self) -> None:
        """Go forward to the network step, or past it when there is only one network."""
        options = self.chain_options()
        if len(options) == 1:
            self.choose_network(options[0][0])
        else:
            self._go("network", 1)

    def _after_network(self) -> None:
        """The session is running: go to the method step, or straight to the only method."""
        methods = self.methods()
        if self.method not in methods:
            self.method = methods[0] if methods else None
        self._go(self.method if len(methods) == 1 else "method", 1)

    def _hint_verdict(self, outcome) -> None:
        about = outcome.hint_tx
        if about and self.hint_tx and about != self.hint_tx:
            return  # judged an older hint; the newer one reported since is still pending
        self.error = outcome.error
        self.dropped_tx = (about or self.hint_tx) if outcome.error == "tx_not_landed" else None
        self.hint_tx = None

    def _abandon_start(self) -> None:
        """Stop waiting for a session start the player moved away from, and discard that
        session once it exists: it would be watched for 24 h and reported expired."""
        fut, self._future = self._future, None
        if fut is None:
            return
        submit = self.submit

        def discard(f) -> None:
            if f.cancelled() or f.exception() is not None:
                return
            sid = f.result()["id"]
            submit(lambda e: e.discard_session(sid))

        fut.add_done_callback(discard)

    def _discard_session(self) -> None:
        if self.session and self.state in ("waiting", "starting"):
            sid = self.session_id
            self.submit(lambda e: e.discard_session(sid))
        self.close()
        self.session = None
        self.hint_tx = None
        self.dropped_tx = None
        self.confirmations = 0

    def _result(self, status: str) -> PaymentResult:
        return PaymentResult(status=status, kind=self.spec.kind, item=self.spec.item,
                             token=self.spec.token, chain=self.chain_key,
                             session_id=self.session_id)
