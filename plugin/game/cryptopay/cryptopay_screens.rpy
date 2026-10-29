## cryptopay screens: the payment screen (one step at a time) and the restore screen.
##
## Both are a light sheet on the right of the screen, so the game stays visible beside it.
## Developers can restyle everything through the cryptopay_* styles below, pass a logo
## and an accent color to cryptopay.configure(), or replace a screen entirely by defining
## a screen with the same name in their own game.
## All logic lives in the controller objects (cryptopay_core.checkout / restore_flow);
## these screens only display their fields and call cryptopay._* helpers.

init -10 python in cryptopay:

    from cryptopay_core.amounts import format_units as _format_units
    from cryptopay_core.config import DEFAULT_ACCENT as _DEFAULT_ACCENT, text_on as _text_on

    METHOD_LABELS = {
        "browser": _("Browser wallet"),
        "qr": _("Phone wallet"),
        "manual": _("Manual transfer"),
    }
    METHOD_HINTS = {
        "browser": _("Use a wallet extension in your browser"),
        "qr": _("Scan a code with your phone wallet"),
        "manual": _("Send from an exchange or any wallet"),
    }
    FEE_LABELS = {"low": _("low fees"), "lowest": _("lowest fees"), "high": _("high fees")}

    ## Characters that can appear in a transaction hash (hex) or a Solana signature (base58).
    TX_CHARS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

    def _tx_word(ctl):
        return _("signature") if ctl.family == "solana" else _("transaction hash")

    ## Palette. Override these in an `init python` block to recolor the sheet.
    SHEET = "#fbfcfd"
    INK = "#17202e"
    INK_HOVER = "#2c3a50"
    MUTED = "#5b6676"
    FAINT = "#a3abb6"
    LINE = "#cfd5dd"
    RULE = "#e4e8ed"
    HOVER = "#eef1f4"
    DIM = "#0a0c1866"
    OK = "#17804f"
    WARN = "#9a5b00"
    ERROR = "#c0392b"

    # ---------------------------------------------------------------- sizing

    def _k():
        """Scale factor: the screens are designed for 1280x720 and grow with the game."""
        sw, sh = renpy.config.screen_width, renpy.config.screen_height
        return min(sh / 720.0, sw / 960.0)

    def _px(n):
        return int(round(n * _k()))

    def _sheet_width():
        sw, sh = renpy.config.screen_width, renpy.config.screen_height
        w = _px(520)
        return sw if (sw < sh or w * 1.6 > sw) else w

    def _inner_width():
        return _sheet_width() - 2 * _px(44)

    def _content_width():
        """Width inside the scrolling body (leaves room for the scrollbar and its gap)."""
        return _inner_width() - _px(6) - _px(14)

    def _cell(n, total=None, max_cols=4):
        """Width of one of n equal buttons in a row (wrapping after max_cols)."""
        cols = max(1, min(n, max_cols))
        total = _content_width() if total is None else total
        return (total - _px(8) * (cols - 1)) // cols

    def _half():
        return (_inner_width() - _px(10)) // 2

    _heights = python_dict()

    def _option_height(items, width):
        """Height of the tallest option in a row, so all buttons of a row line up.
        items: (label, sub) pairs; sub may be None."""
        key = (tuple(items), width)
        h = _heights.get(key)
        if h is None:
            inner = width - 2 * _px(6)
            tallest = 0
            for label, sub in items:
                th = _store.Text(label, style="cryptopay_option_text", substitute=False).size(width=inner)[1]
                if sub:
                    th += _store.Text(sub, style="cryptopay_option_sub", substitute=False).size(width=inner)[1]
                tallest = max(tallest, th)
            h = int(tallest) + 2 * _px(7)
            if len(_heights) > 32:
                _heights.clear()
            _heights[key] = h
        return h

    def _plain_amount(value):
        """1.00 -> "1", 2.50 -> "2.50"."""
        text = "%s" % value
        return text[:-3] if text.endswith(".00") else text

    def _short_address(address):
        return address[:6] + "..." + address[-4:]

    # ---------------------------------------------------------------- drawing

    _frames = python_dict()

    def _frame(kind, color):
        """A rounded rectangle (kind: "fill" or "line") in `color`, sized to the game."""
        key = (kind, color)
        d = _frames.get(key)
        if d is None:
            zoom = _k() / 2.0          # the images are drawn at 2x
            img = _store.Transform("cryptopay/ui/%s.png" % kind, zoom=zoom,
                                   matrixcolor=_store.TintMatrix(color))
            border = int(round(24 * zoom))
            d = _store.Frame(img, border, border)
            _frames[key] = d
        return d

    def accent():
        return _state.config.accent if _state.config is not None else _DEFAULT_ACCENT

    def on_accent():
        """Text color that stays readable on the accent color."""
        return _text_on(accent())

    def _accent_fill(st, at):
        # Resolved at draw time: configure() may run after these styles are defined.
        return _frame("fill", accent()), None

    def _accent_solid(st, at):
        return _store.Solid(accent()), None

    def _dot(color):
        return _store.Transform("cryptopay/ui/dot.png", zoom=_k() / 2.4,
                                matrixcolor=_store.TintMatrix(color), yalign=0.5)

    def _logo():
        """The developer's logo, fitted into the logo slot, or None."""
        if _state.config is None or _state.config.logo is None:
            return None
        return _store.Transform(_state.config.logo, fit="contain",
                                xysize=(_px(240), _px(64)), xalign=0.0)

    # ---------------------------------------------------------------- text

    def _status_text(ctl):
        """Status line in the footer. The waiting and done steps say it in their heading."""
        if ctl.step in ("amount", "waiting", "done") or ctl.state == "choose":
            return ""
        return _progress_text(ctl)

    def _waiting_title(ctl):
        return _progress_text(ctl)

    def _done_title(ctl):
        if ctl.state == "paid":
            return _("Payment confirmed. Thank you!")
        if ctl.state == "underpaid":
            return _("Payment too small")
        return _("The payment window has closed")

    def _method_hint(ctl, method):
        if method == "browser" and ctl.family == "solana":
            return _("Phantom, Solflare or another wallet in this browser") if WEB else _("Phantom, Solflare or another wallet extension")
        if method == "browser":
            return _("MetaMask, Rabby or another wallet in this browser") if WEB else _("MetaMask, Rabby or another wallet extension")
        return METHOD_HINTS[method]

    def _progress_text(ctl):
        chain = ctl.chain.name if ctl.chain else ""
        if ctl.state == "starting":
            return _("Preparing the payment...")
        if ctl.state == "waiting":
            if ctl.hint_tx:
                return _("Transaction received. Waiting for it to be included on %s...") % chain
            return _("Waiting for your payment on %s...") % chain
        if ctl.state == "confirming":
            return _("Confirming on %s... %d/%d") % (chain, ctl.confirmations, ctl.required)
        return ""

    def _status_color(ctl):
        return {"paid": OK, "underpaid": ERROR, "expired": WARN}.get(ctl.state, accent())

    def _amount_line(ctl):
        return "%s %s" % (ctl.pay_amount_text, ctl.spec.token)

    def _receipt_line(r):
        what = _item_title(r.get("item")) if r.get("kind") == "unlock" else _("Tip")
        return "%s: %s %s on %s" % (what, r.get("amount"), r.get("token"), _chain_name(r.get("chain")))

    def _candidate_line(c):
        return _("Payment of %s %s on %s. What was it for?") % (
            _format_units(c.transfer.value), c.token, _chain_name(c.chain))

    def _choice_label(choice):
        return _("Keep as a tip") if choice == "tip" else _item_title(choice)

    def _restored_line(r):
        return _receipt_line(r)


transform cryptopay_sheet_in:
    on show:
        xoffset 48 alpha 0.0
        easein 0.22 xoffset 0 alpha 1.0

## A step body coming in: from the right going forward (1), from the left going back (-1).
transform cryptopay_step_in(direction=1):
    xoffset cryptopay._px(40) * direction alpha 0.0
    easein 0.22 xoffset 0 alpha 1.0

transform cryptopay_dim_in:
    on show:
        alpha 0.0
        linear 0.22 alpha 1.0


style cryptopay_sheet is default:
    background Solid(cryptopay.SHEET)
    padding (cryptopay._px(44), cryptopay._px(32), cryptopay._px(44), cryptopay._px(28))
    xsize cryptopay._sheet_width()
    yfill True
    xalign 1.0

style cryptopay_side is side:
    spacing cryptopay._px(14)

style cryptopay_step is cryptopay_side:
    spacing cryptopay._px(12)

style cryptopay_viewport is viewport:
    xfill True

style cryptopay_vscrollbar is vscrollbar:
    xsize cryptopay._px(6)
    base_bar Null()
    thumb Solid(cryptopay.LINE)
    hover_thumb Solid(cryptopay.MUTED)
    unscrollable "hide"

style cryptopay_body is vbox:
    spacing cryptopay._px(10)

style cryptopay_footer is vbox:
    spacing cryptopay._px(12)

style cryptopay_vbox is vbox:
    spacing cryptopay._px(8)

style cryptopay_hbox is hbox:
    spacing cryptopay._px(8)
    box_wrap True
    box_wrap_spacing cryptopay._px(8)

style cryptopay_actions is hbox:
    spacing cryptopay._px(10)

style cryptopay_text is default:
    size cryptopay._px(18)
    color cryptopay.INK

style cryptopay_title is cryptopay_text:
    size cryptopay._px(26)
    bold True
    top_margin cryptopay._px(6)

style cryptopay_heading is cryptopay_text:
    size cryptopay._px(21)
    bold True
    top_margin cryptopay._px(8)

style cryptopay_amount is cryptopay_text:
    size cryptopay._px(40)
    bold True

style cryptopay_amount_token is cryptopay_text:
    size cryptopay._px(22)
    color cryptopay.MUTED
    yalign 0.8

style cryptopay_label is cryptopay_text:
    size cryptopay._px(15)
    color cryptopay.MUTED
    top_margin cryptopay._px(8)

style cryptopay_muted is cryptopay_text:
    size cryptopay._px(16)
    color cryptopay.MUTED

style cryptopay_warning is cryptopay_text:
    size cryptopay._px(16)
    color cryptopay.WARN

style cryptopay_dev is cryptopay_warning:
    size cryptopay._px(14)

style cryptopay_error is cryptopay_text:
    size cryptopay._px(16)
    color cryptopay.ERROR

style cryptopay_ok is cryptopay_text:
    color cryptopay.OK

style cryptopay_status is cryptopay_text:
    size cryptopay._px(16)

style cryptopay_mono is cryptopay_text:
    size cryptopay._px(15)
    language "anywhere"

## Segmented options (network, method, tip amount, restore choices).
style cryptopay_option is default:
    background cryptopay._frame("line", cryptopay.LINE)
    hover_background cryptopay._frame("fill", cryptopay.HOVER)
    selected_background DynamicDisplayable(cryptopay._accent_fill)
    selected_hover_background DynamicDisplayable(cryptopay._accent_fill)
    insensitive_background cryptopay._frame("line", cryptopay.RULE)
    selected_insensitive_background DynamicDisplayable(cryptopay._accent_fill)
    padding (cryptopay._px(6), cryptopay._px(7))

style cryptopay_option_text is cryptopay_text:
    size cryptopay._px(15)
    xalign 0.5
    text_align 0.5

style cryptopay_option_sub is cryptopay_option_text:
    size cryptopay._px(12)

## Buttons. textbutton uses cryptopay_button (outlined) through style_prefix.
style cryptopay_button is default:
    background cryptopay._frame("line", cryptopay.LINE)
    hover_background cryptopay._frame("fill", cryptopay.HOVER)
    insensitive_background cryptopay._frame("line", cryptopay.RULE)
    padding (cryptopay._px(16), cryptopay._px(11))

style cryptopay_button_text is cryptopay_text:
    size cryptopay._px(17)
    bold True
    xalign 0.5
    text_align 0.5
    insensitive_color cryptopay.FAINT

style cryptopay_primary_button is cryptopay_button:
    background cryptopay._frame("fill", cryptopay.INK)
    hover_background cryptopay._frame("fill", cryptopay.INK_HOVER)
    insensitive_background cryptopay._frame("fill", cryptopay.LINE)

style cryptopay_primary_button_text is cryptopay_button_text:
    color "#ffffff"
    insensitive_color "#ffffff"

style cryptopay_accent_button is cryptopay_button:
    background DynamicDisplayable(cryptopay._accent_fill)
    hover_background DynamicDisplayable(cryptopay._accent_fill)

style cryptopay_accent_button_text is cryptopay_button_text

style cryptopay_small_button is cryptopay_button:
    padding (cryptopay._px(10), cryptopay._px(6))

style cryptopay_small_button_text is cryptopay_button_text:
    size cryptopay._px(15)

## Top row of the sheet: the back arrow on the left, the step dots on the right.
style cryptopay_nav is default:
    xfill True
    ysize cryptopay._px(36)

style cryptopay_back_button is default:
    xysize (cryptopay._px(36), cryptopay._px(36))
    yalign 0.5
    background cryptopay._frame("line", cryptopay.LINE)
    hover_background cryptopay._frame("fill", cryptopay.HOVER)

style cryptopay_back_button_text is cryptopay_text:
    font "DejaVuSans.ttf"   # has the arrow glyph whatever font the game uses
    size cryptopay._px(20)
    align (0.5, 0.5)

style cryptopay_dots is hbox:
    spacing cryptopay._px(6)
    align (1.0, 0.5)

## Confirmations on the waiting step.
style cryptopay_progress is bar:
    ysize cryptopay._px(8)
    left_bar DynamicDisplayable(cryptopay._accent_solid)
    right_bar Solid(cryptopay.RULE)
    thumb None

style cryptopay_qr_frame is default:
    background cryptopay._frame("line", cryptopay.LINE)
    padding (cryptopay._px(8), cryptopay._px(8))

style cryptopay_input is cryptopay_text:
    size cryptopay._px(17)

style cryptopay_inputbox is default:
    background cryptopay._frame("line", cryptopay.LINE)
    hover_background cryptopay._frame("line", cryptopay.MUTED)
    padding (cryptopay._px(12), cryptopay._px(9))


## One segmented option. `sub` is a smaller second line (e.g. "low fees").
screen cryptopay_option(label, action, selected=False, sub=None, width=None, height=None, sensitive=True):
    $ fg = cryptopay.on_accent() if selected else (cryptopay.INK if sensitive else cryptopay.FAINT)
    button:
        style "cryptopay_option"
        xsize width
        ysize height
        action action
        selected selected
        sensitive sensitive
        vbox:
            align (0.5, 0.5)
            spacing 0
            text label style "cryptopay_option_text" color fg substitute False
            if sub:
                text sub style "cryptopay_option_sub" color (fg if selected else cryptopay.MUTED) substitute False


## The logo slot. Shows nothing when no logo is configured.
screen cryptopay_logo():
    $ logo = cryptopay._logo()
    if logo is not None:
        add logo


## Status line at the bottom of the sheet.
screen cryptopay_status(message, color):
    hbox:
        spacing cryptopay._px(10)
        add cryptopay._dot(color)
        text message style "cryptopay_status" substitute False


## The payment sheet. One step at a time slides in (see CheckoutController.step):
## amount (tips) -> network -> method -> browser | qr | manual -> waiting -> done.
## The header (back arrow, step dots, logo, title, amount) and the footer (status and
## Cancel / Continue playing) stay put. Each step body is its own screen, so a game can
## restyle one step by defining a screen with the same name.
screen cryptopay_payment(cid):
    modal True
    zorder 200

    $ ctl = cryptopay._lookup_checkout(cid)

    if ctl.can_go_back:
        key "game_menu" action Function(cryptopay._back, cid)
    else:
        key "game_menu" action NullAction()

    add Solid(cryptopay.DIM) at cryptopay_dim_in

    frame:
        style "cryptopay_sheet"
        style_prefix "cryptopay"
        at cryptopay_sheet_in

        side "t c b":
            use cryptopay_payment_header(ctl, cid)

            # One branch per step: each is a new displayable, so its slide starts afresh.
            if ctl.state == "closed":
                use cryptopay_step_view(1):
                    text _("This payment screen is no longer active. Any payment you made is still being checked in the background.")
            elif ctl.step == "amount":
                use cryptopay_step_view(ctl.nav_dir, _("Next"), Function(cryptopay._next_step, cid)):
                    use cryptopay_step_amount(ctl, cid)
            elif ctl.step == "network":
                use cryptopay_step_view(ctl.nav_dir):
                    use cryptopay_step_network(ctl, cid)
            elif ctl.step == "method":
                use cryptopay_step_view(ctl.nav_dir):
                    use cryptopay_step_method(ctl, cid)
            elif ctl.step == "browser":
                use cryptopay_step_view(ctl.nav_dir, (_("Open wallet") if cryptopay.WEB else _("Open checkout in browser")), Function(cryptopay._open_browser, cid)):
                    use cryptopay_step_browser(ctl, cid)
            elif ctl.step == "qr":
                use cryptopay_step_view(ctl.nav_dir, _("I've sent it"), Function(cryptopay._mark_sent, cid)):
                    use cryptopay_step_qr(ctl, cid)
            elif ctl.step == "manual":
                use cryptopay_step_view(ctl.nav_dir, _("I've sent it"), Function(cryptopay._mark_sent, cid)):
                    use cryptopay_step_manual(ctl, cid)
            elif ctl.step == "waiting":
                use cryptopay_step_view(ctl.nav_dir):
                    use cryptopay_step_waiting(ctl, cid)
            else:
                use cryptopay_step_view(ctl.nav_dir):
                    use cryptopay_step_done(ctl, cid)

            use cryptopay_payment_footer(ctl, cid)


## One step sliding in from the right (forward) or the left (back): its scrolling body,
## and below it the button that moves on (when the step has one), always in view.
screen cryptopay_step_view(direction, next_label=None, next_action=None):
    side "c b":
        style "cryptopay_step"
        at cryptopay_step_in(direction)

        viewport:
            mousewheel True
            draggable True
            scrollbars "vertical"

            vbox:
                style "cryptopay_body"
                xsize cryptopay._content_width()
                transclude

        if next_label:
            textbutton next_label:
                style "cryptopay_accent_button"
                text_color cryptopay.on_accent()
                xsize cryptopay._inner_width()
                action next_action
        else:
            null


screen cryptopay_payment_header(ctl, cid):
    vbox:
        style "cryptopay_body"
        xsize cryptopay._inner_width()

        fixed:
            style "cryptopay_nav"
            if ctl.can_go_back:
                textbutton "←":
                    style "cryptopay_back_button"
                    alt _("Back")
                    action Function(cryptopay._back, cid)
            if ctl.state != "closed" and ctl.step != "done":
                $ index, total = ctl.step_dots()
                if total > 1:
                    hbox:
                        style "cryptopay_dots"
                        for i in range(total):
                            add cryptopay._dot(cryptopay.accent() if i <= index else cryptopay.LINE)

        use cryptopay_logo

        if ctl.state != "closed":
            if config.developer:
                text ("DEV MODE: paying to " + cryptopay._short_address(ctl.recipient) + (" (testnet)" if ctl.config.testnet else "")) style "cryptopay_dev" substitute False
                for w in ctl.dev_warnings():
                    text w style "cryptopay_dev" substitute False

            text ctl.spec.title style "cryptopay_title" substitute False
            hbox:
                spacing cryptopay._px(8)
                text ctl.pay_amount_text style "cryptopay_amount" substitute False
                text ctl.spec.token style "cryptopay_amount_token" substitute False


screen cryptopay_payment_footer(ctl, cid):
    vbox:
        style "cryptopay_footer"
        if ctl.message():
            text ctl.message() style "cryptopay_error" substitute False
        add Solid(cryptopay.RULE, xsize=cryptopay._inner_width(), ysize=max(1, cryptopay._px(1.5)))
        $ status = cryptopay._status_text(ctl) if ctl.state != "closed" else ""
        if status:
            use cryptopay_status(status, cryptopay._status_color(ctl))
        hbox:
            style "cryptopay_actions"
            if ctl.state in ("closed", "paid", "underpaid", "expired"):
                textbutton _("Continue") style "cryptopay_primary_button" xsize cryptopay._inner_width() action Function(cryptopay._close_checkout, cid, "finish")
            elif ctl.session:
                textbutton _("Cancel") xsize cryptopay._half() action Function(cryptopay._close_checkout, cid, "cancel")
                textbutton _("Continue playing") style "cryptopay_primary_button" xsize cryptopay._half() action Function(cryptopay._close_checkout, cid, "keep_playing")
            else:
                textbutton _("Cancel") xsize cryptopay._inner_width() action Function(cryptopay._close_checkout, cid, "cancel")


screen cryptopay_step_amount(ctl, cid):
    text _("Choose an amount") style "cryptopay_label"
    $ presets = python_list(("%s %s" % (cryptopay._plain_amount(p), ctl.spec.token), p) for p in ctl.tip_presets())
    $ cell = cryptopay._cell(len(presets))
    $ row_h = cryptopay._option_height(python_list((label, None) for label, p in presets), cell)
    hbox:
        for label, preset in presets:
            use cryptopay_option(label, Function(cryptopay._set_tip, cid, str(preset)), selected=(ctl.amount == preset), width=cell, height=row_h)
    text _("Or enter your own") style "cryptopay_label"
    default amount_value = FieldInputValue(ctl, "custom_amount", default=False)
    hbox:
        button:
            style "cryptopay_inputbox"
            xsize cryptopay._content_width() - cryptopay._px(98)
            action amount_value.Toggle()
            input value amount_value length 8 allow "0123456789." style "cryptopay_input"
        textbutton _("Set") style "cryptopay_small_button" xsize cryptopay._px(90) yalign 0.5 action Function(cryptopay._apply_custom_tip, cid)


screen cryptopay_step_network(ctl, cid):
    text (_("Which network holds your %s?") % ctl.spec.token) style "cryptopay_label" substitute False
    $ options = python_list((key, name, cryptopay.FEE_LABELS.get(fee, fee)) for key, name, fee in ctl.chain_options())
    $ cell = cryptopay._cell(len(options), max_cols=2)
    $ row_h = cryptopay._option_height(python_list((name, fee) for key, name, fee in options), cell)
    hbox:
        for key, name, fee in options:
            use cryptopay_option(name, Function(cryptopay._choose_network, cid, key), selected=(ctl.chain_key == key), sub=fee, width=cell, height=row_h)


screen cryptopay_step_method(ctl, cid):
    text _("How do you want to pay?") style "cryptopay_label"
    $ methods = python_list((m, cryptopay.METHOD_LABELS[m], cryptopay._method_hint(ctl, m)) for m in ctl.methods())
    $ row_h = cryptopay._option_height(python_list((label, hint) for m, label, hint in methods), cryptopay._content_width()) + cryptopay._px(10)
    vbox:
        for m, label, hint in methods:
            use cryptopay_option(label, Function(cryptopay._choose_method, cid, m), selected=(ctl.method == m), sub=hint, width=cryptopay._content_width(), height=row_h)


screen cryptopay_step_browser(ctl, cid):
    text _("Browser wallet") style "cryptopay_label"
    if cryptopay.WEB:
        if ctl.family == "solana":
            text _("Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions.")
        else:
            text _("Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")
    elif ctl.family == "solana":
        text _("Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions.")
    else:
        text _("Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")
    text (_("Approve %s on %s in your wallet.") % (cryptopay._amount_line(ctl), ctl.chain.name)) style "cryptopay_muted" substitute False


screen cryptopay_step_qr(ctl, cid):
    text _("Phone wallet") style "cryptopay_label"
    hbox:
        spacing cryptopay._px(20)
        box_wrap False
        frame:
            style "cryptopay_qr_frame"
            add cryptopay.QR(ctl.payment_uri(), cryptopay._px(176))
        vbox:
            yalign 0.5
            spacing cryptopay._px(6)
            xmaximum cryptopay._content_width() - cryptopay._px(176 + 16 + 20)
            text _("1. Open your wallet app")
            text _("2. Scan the code")
            text (_("3. Approve %s on %s") % (cryptopay._amount_line(ctl), ctl.chain.name)) substitute False
    if renpy.mobile and ctl.config.methods["qr"]["wallet_button"]:
        textbutton _("Open wallet app") xsize cryptopay._content_width() action Function(cryptopay._open_wallet_app, cid)
    if ctl.family == "solana":
        text _("If your wallet changes the amount, the game still finds the payment.") style "cryptopay_muted"
    else:
        text _("If your wallet changes the amount, use Manual transfer and paste the transaction hash.") style "cryptopay_muted"


screen cryptopay_step_manual(ctl, cid):
    text _("Manual transfer") style "cryptopay_label"
    if ctl.family == "solana":
        text _("Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds.") style "cryptopay_warning"
    else:
        text (_("Send exactly this amount on %s only. Sending on another network may lose your funds.") % ctl.chain.name) style "cryptopay_warning" substitute False
    for label, value in ((_("Amount"), cryptopay._amount_line(ctl)), (_("To address"), ctl.recipient), ((_("Token mint") if ctl.family == "solana" else _("Token contract")), ctl.token_address)):
        text label style "cryptopay_label"
        hbox:
            text value style "cryptopay_mono" min_width cryptopay._content_width() - cryptopay._px(98) xmaximum cryptopay._content_width() - cryptopay._px(98) yalign 0.5 substitute False
            textbutton _("Copy") style "cryptopay_small_button" xsize cryptopay._px(90) yalign 0.5 action cryptopay.Copy(value.split(" ")[0])
    if ctl.config.methods["manual"]["allow_tx_paste"]:
        null height cryptopay._px(4)
        text (_("Paid from an exchange or another wallet? Paste the %s:") % cryptopay._tx_word(ctl)) style "cryptopay_muted" substitute False
        default tx_value = FieldInputValue(ctl, "tx_input", default=False)
        hbox:
            button:
                style "cryptopay_inputbox"
                xsize cryptopay._content_width() - cryptopay._px(196 if cryptopay.WEB else 98)
                action tx_value.Toggle()
                input value tx_value length 90 copypaste True allow cryptopay.TX_CHARS style "cryptopay_input"
            if cryptopay.WEB:
                textbutton _("Paste") style "cryptopay_small_button" xsize cryptopay._px(90) yalign 0.5 action Function(cryptopay._paste_hash, cid)
            textbutton _("Check") style "cryptopay_small_button" xsize cryptopay._px(90) yalign 0.5 action Function(cryptopay._submit_hash, cid)


screen cryptopay_step_waiting(ctl, cid):
    text cryptopay._waiting_title(ctl) style "cryptopay_heading" substitute False
    if ctl.state == "confirming":
        bar value StaticValue(ctl.confirmations, max(1, ctl.required)) style "cryptopay_progress"
    if ctl.hint_tx:
        text (_("Your %s:") % cryptopay._tx_word(ctl)) style "cryptopay_label" substitute False
        text ctl.hint_tx style "cryptopay_mono" substitute False
        if ctl.explorer_url():
            textbutton _("View on explorer") style "cryptopay_small_button" action Function(cryptopay._open_explorer, ctl.explorer_url())
    elif ctl.method == "browser":
        if cryptopay.WEB:
            text _("Finish the payment in the wallet window.") style "cryptopay_muted"
            textbutton _("Reopen wallet window") style "cryptopay_small_button" action Function(cryptopay._open_browser, cid)
        else:
            text _("Finish the payment on the checkout page in your browser.") style "cryptopay_muted"
            textbutton _("Reopen checkout page") style "cryptopay_small_button" action Function(cryptopay._open_browser, cid)
    null height cryptopay._px(4)
    text _("You can keep playing: the game keeps checking in the background and finishes the purchase by itself.") style "cryptopay_muted"


screen cryptopay_step_done(ctl, cid):
    text cryptopay._done_title(ctl) style "cryptopay_heading" color cryptopay._status_color(ctl) substitute False
    if ctl.state == "paid" and ctl.result and ctl.result.tx_hash:
        text (_("Keep this %s to restore your purchase on another device:") % cryptopay._tx_word(ctl)) style "cryptopay_muted" substitute False
        text ctl.result.tx_hash style "cryptopay_mono" substitute False
        hbox:
            textbutton (_("Copy signature") if ctl.family == "solana" else _("Copy hash")) style "cryptopay_small_button" action cryptopay.Copy(ctl.result.tx_hash)
            textbutton _("View on explorer") style "cryptopay_small_button" action Function(cryptopay._open_explorer, ctl.explorer_url())
    elif ctl.state == "underpaid":
        text _("The amount received was less than the price. Please contact the developer.") style "cryptopay_muted"
    elif ctl.state == "expired":
        text _("If you already paid, the game will keep checking for 24 hours and unlock it automatically.") style "cryptopay_muted"


screen cryptopay_restore(rid):
    modal True
    zorder 200

    $ ctl = cryptopay._lookup_restore(rid)

    key "game_menu" action NullAction()

    add Solid(cryptopay.DIM) at cryptopay_dim_in

    frame:
        style "cryptopay_sheet"
        style_prefix "cryptopay"
        at cryptopay_sheet_in

        side "c b":
          viewport:
            mousewheel True
            draggable True
            scrollbars "vertical"

            vbox:
              style "cryptopay_body"
              xsize cryptopay._content_width()

              use cryptopay_logo

              text _("Restore purchases") style "cryptopay_title"

              if ctl.state == "closed":
                  text _("This screen is no longer active.")

              elif ctl.state in ("input", "checking"):
                  text _("Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history.") style "cryptopay_muted"
                  default hash_value = FieldInputValue(ctl, "tx_input", default=True)
                  hbox:
                      button:
                          style "cryptopay_inputbox"
                          xsize cryptopay._content_width() - (cryptopay._px(98) if cryptopay.WEB else 0)
                          action hash_value.Toggle()
                          input value hash_value length 90 copypaste True allow cryptopay.TX_CHARS style "cryptopay_input"
                      if cryptopay.WEB:
                          textbutton _("Paste") style "cryptopay_small_button" xsize cryptopay._px(90) yalign 0.5 action Function(cryptopay._paste_hash, rid) sensitive ctl.state == "input"
                  textbutton _("Check"):
                      style "cryptopay_accent_button"
                      text_color cryptopay.on_accent()
                      xsize cryptopay._content_width()
                      action Function(cryptopay._restore_check, rid)
                      sensitive ctl.state == "input"
                  if ctl.state == "checking":
                      use cryptopay_status(_("Checking the blockchain..."), cryptopay.accent())

              elif ctl.state == "choose":
                  for i in ctl.pending_choices():
                      text cryptopay._candidate_line(ctl.candidates[i]) substitute False
                      $ choices = ctl.candidates[i].choices
                      $ cell = cryptopay._cell(len(choices), max_cols=2)
                      $ row_h = cryptopay._option_height(python_list((cryptopay._choice_label(c), None) for c in choices), cell)
                      hbox:
                          for choice in choices:
                              use cryptopay_option(cryptopay._choice_label(choice), Function(cryptopay._restore_choose, rid, i, choice), selected=(ctl.choices.get(i) == choice), width=cell, height=row_h)
                  textbutton _("Restore") style "cryptopay_primary_button" xsize cryptopay._content_width() action Function(cryptopay._restore_confirm, rid) sensitive ctl.can_confirm

              elif ctl.state == "saving":
                  use cryptopay_status(_("Saving..."), cryptopay.accent())

              elif ctl.state == "done":
                  if ctl.added:
                      text _("Restored:") style "cryptopay_ok"
                      for r in ctl.added:
                          text cryptopay._restored_line(r) substitute False
                  else:
                      text _("Nothing new to restore. These purchases are already on this device.")

              if ctl.message():
                  text ctl.message() style "cryptopay_error" substitute False

              $ owned = cryptopay.receipts()
              if owned:
                  text _("Purchases on this device") style "cryptopay_label"
                  for r in owned:
                      hbox:
                          text cryptopay._receipt_line(r) min_width cryptopay._content_width() - cryptopay._px(118) xmaximum cryptopay._content_width() - cryptopay._px(118) yalign 0.5 substitute False
                          textbutton _("Copy hash") style "cryptopay_small_button" xsize cryptopay._px(110) yalign 0.5 action cryptopay.Copy(r["tx_hash"])

          vbox:
            style "cryptopay_footer"
            add Solid(cryptopay.RULE, xsize=cryptopay._inner_width(), ysize=max(1, cryptopay._px(1.5)))
            textbutton _("Close") xsize cryptopay._inner_width() action Function(cryptopay._close_restore, rid)
