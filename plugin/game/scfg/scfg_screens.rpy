## Stablecoin for Games screens: the payment screen (one step at a time) and the restore screen.
##
## Both are a light sheet on the right of the screen, so the game stays visible beside it.
## Developers can restyle everything through the scfg_* styles below, pass a logo
## and an accent color to scfg.configure(), or replace a screen entirely by defining
## a screen with the same name in their own game.
## All logic lives in the controller objects (scfg_core.checkout / restore_flow);
## these screens only display their fields and call scfg._* helpers.

init -10 python in scfg:

    from scfg_core.amounts import format_units as _format_units
    from scfg_core.config import DEFAULT_ACCENT as _DEFAULT_ACCENT, text_on as _text_on

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
                th = _store.Text(label, style="scfg_option_text", substitute=False).size(width=inner)[1]
                if sub:
                    th += _store.Text(sub, style="scfg_option_sub", substitute=False).size(width=inner)[1]
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
            img = _store.Transform("scfg/ui/%s.png" % kind, zoom=zoom,
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
        return _store.Transform("scfg/ui/dot.png", zoom=_k() / 2.4,
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


transform scfg_sheet_in:
    on show:
        xoffset 48 alpha 0.0
        easein 0.22 xoffset 0 alpha 1.0

## A step body coming in: from the right going forward (1), from the left going back (-1).
transform scfg_step_in(direction=1):
    xoffset scfg._px(40) * direction alpha 0.0
    easein 0.22 xoffset 0 alpha 1.0

transform scfg_dim_in:
    on show:
        alpha 0.0
        linear 0.22 alpha 1.0


style scfg_sheet is default:
    background Solid(scfg.SHEET)
    padding (scfg._px(44), scfg._px(32), scfg._px(44), scfg._px(28))
    xsize scfg._sheet_width()
    yfill True
    xalign 1.0

style scfg_side is side:
    spacing scfg._px(14)

style scfg_step is scfg_side:
    spacing scfg._px(12)

style scfg_viewport is viewport:
    xfill True

style scfg_vscrollbar is vscrollbar:
    xsize scfg._px(6)
    base_bar Null()
    thumb Solid(scfg.LINE)
    hover_thumb Solid(scfg.MUTED)
    unscrollable "hide"

style scfg_body is vbox:
    spacing scfg._px(10)

style scfg_footer is vbox:
    spacing scfg._px(12)

style scfg_vbox is vbox:
    spacing scfg._px(8)

style scfg_hbox is hbox:
    spacing scfg._px(8)
    box_wrap True
    box_wrap_spacing scfg._px(8)

style scfg_actions is hbox:
    spacing scfg._px(10)

style scfg_text is default:
    size scfg._px(18)
    color scfg.INK

style scfg_title is scfg_text:
    size scfg._px(26)
    bold True
    top_margin scfg._px(6)

style scfg_heading is scfg_text:
    size scfg._px(21)
    bold True
    top_margin scfg._px(8)

style scfg_amount is scfg_text:
    size scfg._px(40)
    bold True

style scfg_amount_token is scfg_text:
    size scfg._px(22)
    color scfg.MUTED
    yalign 0.8

style scfg_label is scfg_text:
    size scfg._px(15)
    color scfg.MUTED
    top_margin scfg._px(8)

style scfg_muted is scfg_text:
    size scfg._px(16)
    color scfg.MUTED

style scfg_warning is scfg_text:
    size scfg._px(16)
    color scfg.WARN

style scfg_dev is scfg_warning:
    size scfg._px(14)

style scfg_error is scfg_text:
    size scfg._px(16)
    color scfg.ERROR

style scfg_ok is scfg_text:
    color scfg.OK

style scfg_status is scfg_text:
    size scfg._px(16)

style scfg_mono is scfg_text:
    size scfg._px(15)
    language "anywhere"

## Segmented options (network, method, tip amount, restore choices).
style scfg_option is default:
    background scfg._frame("line", scfg.LINE)
    hover_background scfg._frame("fill", scfg.HOVER)
    selected_background DynamicDisplayable(scfg._accent_fill)
    selected_hover_background DynamicDisplayable(scfg._accent_fill)
    insensitive_background scfg._frame("line", scfg.RULE)
    selected_insensitive_background DynamicDisplayable(scfg._accent_fill)
    padding (scfg._px(6), scfg._px(7))

style scfg_option_text is scfg_text:
    size scfg._px(15)
    xalign 0.5
    text_align 0.5

style scfg_option_sub is scfg_option_text:
    size scfg._px(12)

## Buttons. textbutton uses scfg_button (outlined) through style_prefix.
style scfg_button is default:
    background scfg._frame("line", scfg.LINE)
    hover_background scfg._frame("fill", scfg.HOVER)
    insensitive_background scfg._frame("line", scfg.RULE)
    padding (scfg._px(16), scfg._px(11))

style scfg_button_text is scfg_text:
    size scfg._px(17)
    bold True
    xalign 0.5
    text_align 0.5
    insensitive_color scfg.FAINT

style scfg_primary_button is scfg_button:
    background scfg._frame("fill", scfg.INK)
    hover_background scfg._frame("fill", scfg.INK_HOVER)
    insensitive_background scfg._frame("fill", scfg.LINE)

style scfg_primary_button_text is scfg_button_text:
    color "#ffffff"
    insensitive_color "#ffffff"

style scfg_accent_button is scfg_button:
    background DynamicDisplayable(scfg._accent_fill)
    hover_background DynamicDisplayable(scfg._accent_fill)

style scfg_accent_button_text is scfg_button_text

style scfg_small_button is scfg_button:
    padding (scfg._px(10), scfg._px(6))

style scfg_small_button_text is scfg_button_text:
    size scfg._px(15)

## Top row of the sheet: the back arrow on the left, the step dots on the right.
style scfg_nav is default:
    xfill True
    ysize scfg._px(36)

style scfg_back_button is default:
    xysize (scfg._px(36), scfg._px(36))
    yalign 0.5
    background scfg._frame("line", scfg.LINE)
    hover_background scfg._frame("fill", scfg.HOVER)

style scfg_back_button_text is scfg_text:
    font "DejaVuSans.ttf"   # has the arrow glyph whatever font the game uses
    size scfg._px(20)
    align (0.5, 0.5)

style scfg_dots is hbox:
    spacing scfg._px(6)
    align (1.0, 0.5)

## Confirmations on the waiting step.
style scfg_progress is bar:
    ysize scfg._px(8)
    left_bar DynamicDisplayable(scfg._accent_solid)
    right_bar Solid(scfg.RULE)
    thumb None

style scfg_qr_frame is default:
    background scfg._frame("line", scfg.LINE)
    padding (scfg._px(8), scfg._px(8))

style scfg_input is scfg_text:
    size scfg._px(17)

style scfg_inputbox is default:
    background scfg._frame("line", scfg.LINE)
    hover_background scfg._frame("line", scfg.MUTED)
    padding (scfg._px(12), scfg._px(9))


## One segmented option. `sub` is a smaller second line (e.g. "low fees").
screen scfg_option(label, action, selected=False, sub=None, width=None, height=None, sensitive=True):
    $ fg = scfg.on_accent() if selected else (scfg.INK if sensitive else scfg.FAINT)
    button:
        style "scfg_option"
        xsize width
        ysize height
        action action
        selected selected
        sensitive sensitive
        vbox:
            align (0.5, 0.5)
            spacing 0
            text label style "scfg_option_text" color fg substitute False
            if sub:
                text sub style "scfg_option_sub" color (fg if selected else scfg.MUTED) substitute False


## The logo slot. Shows nothing when no logo is configured.
screen scfg_logo():
    $ logo = scfg._logo()
    if logo is not None:
        add logo


## Status line at the bottom of the sheet.
screen scfg_status(message, color):
    hbox:
        spacing scfg._px(10)
        add scfg._dot(color)
        text message style "scfg_status" substitute False


## The payment sheet. One step at a time slides in (see CheckoutController.step):
## amount (tips) -> network -> method -> browser | qr | manual -> waiting -> done.
## The header (back arrow, step dots, logo, title, amount) and the footer (status and
## Cancel / Continue playing) stay put. Each step body is its own screen, so a game can
## restyle one step by defining a screen with the same name.
screen scfg_payment(cid):
    modal True
    zorder 200

    $ ctl = scfg._lookup_checkout(cid)

    if ctl.can_go_back:
        key "game_menu" action Function(scfg._back, cid)
    else:
        key "game_menu" action NullAction()

    add Solid(scfg.DIM) at scfg_dim_in

    frame:
        style "scfg_sheet"
        style_prefix "scfg"
        at scfg_sheet_in

        side "t c b":
            use scfg_payment_header(ctl, cid)

            # One branch per step: each is a new displayable, so its slide starts afresh.
            if ctl.state == "closed":
                use scfg_step_view(1):
                    text _("This payment screen is no longer active. Any payment you made is still being checked in the background.")
            elif ctl.step == "amount":
                use scfg_step_view(ctl.nav_dir, _("Next"), Function(scfg._next_step, cid)):
                    use scfg_step_amount(ctl, cid)
            elif ctl.step == "network":
                use scfg_step_view(ctl.nav_dir):
                    use scfg_step_network(ctl, cid)
            elif ctl.step == "method":
                use scfg_step_view(ctl.nav_dir):
                    use scfg_step_method(ctl, cid)
            elif ctl.step == "browser":
                use scfg_step_view(ctl.nav_dir, (_("Open wallet") if scfg.WEB else _("Open checkout in browser")), Function(scfg._open_browser, cid)):
                    use scfg_step_browser(ctl, cid)
            elif ctl.step == "qr":
                use scfg_step_view(ctl.nav_dir, _("I've sent it"), Function(scfg._mark_sent, cid)):
                    use scfg_step_qr(ctl, cid)
            elif ctl.step == "manual":
                use scfg_step_view(ctl.nav_dir, _("I've sent it"), Function(scfg._mark_sent, cid)):
                    use scfg_step_manual(ctl, cid)
            elif ctl.step == "waiting":
                use scfg_step_view(ctl.nav_dir):
                    use scfg_step_waiting(ctl, cid)
            else:
                use scfg_step_view(ctl.nav_dir):
                    use scfg_step_done(ctl, cid)

            use scfg_payment_footer(ctl, cid)


## One step sliding in from the right (forward) or the left (back): its scrolling body,
## and below it the button that moves on (when the step has one), always in view.
screen scfg_step_view(direction, next_label=None, next_action=None):
    side "c b":
        style "scfg_step"
        at scfg_step_in(direction)

        viewport:
            mousewheel True
            draggable True
            scrollbars "vertical"

            vbox:
                style "scfg_body"
                xsize scfg._content_width()
                transclude

        if next_label:
            textbutton next_label:
                style "scfg_accent_button"
                text_color scfg.on_accent()
                xsize scfg._inner_width()
                action next_action
        else:
            null


screen scfg_payment_header(ctl, cid):
    vbox:
        style "scfg_body"
        xsize scfg._inner_width()

        fixed:
            style "scfg_nav"
            if ctl.can_go_back:
                textbutton "←":
                    style "scfg_back_button"
                    alt _("Back")
                    action Function(scfg._back, cid)
            if ctl.state != "closed" and ctl.step != "done":
                $ index, total = ctl.step_dots()
                if total > 1:
                    hbox:
                        style "scfg_dots"
                        for i in range(total):
                            add scfg._dot(scfg.accent() if i <= index else scfg.LINE)

        use scfg_logo

        if ctl.state != "closed":
            if config.developer:
                text ("DEV MODE: paying to " + scfg._short_address(ctl.recipient) + (" (testnet)" if ctl.config.testnet else "")) style "scfg_dev" substitute False
                for w in ctl.dev_warnings():
                    text w style "scfg_dev" substitute False

            text ctl.spec.title style "scfg_title" substitute False
            hbox:
                spacing scfg._px(8)
                text ctl.pay_amount_text style "scfg_amount" substitute False
                text ctl.spec.token style "scfg_amount_token" substitute False


screen scfg_payment_footer(ctl, cid):
    vbox:
        style "scfg_footer"
        if ctl.message():
            text ctl.message() style "scfg_error" substitute False
        add Solid(scfg.RULE, xsize=scfg._inner_width(), ysize=max(1, scfg._px(1.5)))
        $ status = scfg._status_text(ctl) if ctl.state != "closed" else ""
        if status:
            use scfg_status(status, scfg._status_color(ctl))
        hbox:
            style "scfg_actions"
            if ctl.state in ("closed", "paid", "underpaid", "expired"):
                textbutton _("Continue") style "scfg_primary_button" xsize scfg._inner_width() action Function(scfg._close_checkout, cid, "finish")
            elif ctl.session:
                textbutton _("Cancel") xsize scfg._half() action Function(scfg._close_checkout, cid, "cancel")
                textbutton _("Continue playing") style "scfg_primary_button" xsize scfg._half() action Function(scfg._close_checkout, cid, "keep_playing")
            else:
                textbutton _("Cancel") xsize scfg._inner_width() action Function(scfg._close_checkout, cid, "cancel")


screen scfg_step_amount(ctl, cid):
    text _("Choose an amount") style "scfg_label"
    $ presets = python_list(("%s %s" % (scfg._plain_amount(p), ctl.spec.token), p) for p in ctl.tip_presets())
    $ cell = scfg._cell(len(presets))
    $ row_h = scfg._option_height(python_list((label, None) for label, p in presets), cell)
    hbox:
        for label, preset in presets:
            use scfg_option(label, Function(scfg._set_tip, cid, str(preset)), selected=(ctl.amount == preset), width=cell, height=row_h)
    text _("Or enter your own") style "scfg_label"
    default amount_value = FieldInputValue(ctl, "custom_amount", default=False)
    hbox:
        button:
            style "scfg_inputbox"
            xsize scfg._content_width() - scfg._px(98)
            action amount_value.Toggle()
            input value amount_value length 8 allow "0123456789." style "scfg_input"
        textbutton _("Set") style "scfg_small_button" xsize scfg._px(90) yalign 0.5 action Function(scfg._apply_custom_tip, cid)


screen scfg_step_network(ctl, cid):
    text (_("Which network holds your %s?") % ctl.spec.token) style "scfg_label" substitute False
    $ options = python_list((key, name, scfg.FEE_LABELS.get(fee, fee)) for key, name, fee in ctl.chain_options())
    $ cell = scfg._cell(len(options), max_cols=2)
    $ row_h = scfg._option_height(python_list((name, fee) for key, name, fee in options), cell)
    hbox:
        for key, name, fee in options:
            use scfg_option(name, Function(scfg._choose_network, cid, key), selected=(ctl.chain_key == key), sub=fee, width=cell, height=row_h)


screen scfg_step_method(ctl, cid):
    text _("How do you want to pay?") style "scfg_label"
    $ methods = python_list((m, scfg.METHOD_LABELS[m], scfg._method_hint(ctl, m)) for m in ctl.methods())
    $ row_h = scfg._option_height(python_list((label, hint) for m, label, hint in methods), scfg._content_width()) + scfg._px(10)
    vbox:
        for m, label, hint in methods:
            use scfg_option(label, Function(scfg._choose_method, cid, m), selected=(ctl.method == m), sub=hint, width=scfg._content_width(), height=row_h)


screen scfg_step_browser(ctl, cid):
    text _("Browser wallet") style "scfg_label"
    if scfg.WEB:
        if ctl.family == "solana":
            text _("Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions.")
        else:
            text _("Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")
    elif ctl.family == "solana":
        text _("Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions.")
    else:
        text _("Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")
    text (_("Approve %s on %s in your wallet.") % (scfg._amount_line(ctl), ctl.chain.name)) style "scfg_muted" substitute False


screen scfg_step_qr(ctl, cid):
    text _("Phone wallet") style "scfg_label"
    hbox:
        spacing scfg._px(20)
        box_wrap False
        frame:
            style "scfg_qr_frame"
            add scfg.QR(ctl.payment_uri(), scfg._px(176))
        vbox:
            yalign 0.5
            spacing scfg._px(6)
            xmaximum scfg._content_width() - scfg._px(176 + 16 + 20)
            text _("1. Open your wallet app")
            text _("2. Scan the code")
            text (_("3. Approve %s on %s") % (scfg._amount_line(ctl), ctl.chain.name)) substitute False
    if renpy.mobile and ctl.config.methods["qr"]["wallet_button"]:
        textbutton _("Open wallet app") xsize scfg._content_width() action Function(scfg._open_wallet_app, cid)
    if ctl.family == "solana":
        text _("If your wallet changes the amount, the game still finds the payment.") style "scfg_muted"
    else:
        text _("If your wallet changes the amount, use Manual transfer and paste the transaction hash.") style "scfg_muted"


screen scfg_step_manual(ctl, cid):
    text _("Manual transfer") style "scfg_label"
    if ctl.family == "solana":
        text _("Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds.") style "scfg_warning"
    else:
        text (_("Send exactly this amount on %s only. Sending on another network may lose your funds.") % ctl.chain.name) style "scfg_warning" substitute False
    for label, value in ((_("Amount"), scfg._amount_line(ctl)), (_("To address"), ctl.recipient), ((_("Token mint") if ctl.family == "solana" else _("Token contract")), ctl.token_address)):
        text label style "scfg_label"
        hbox:
            text value style "scfg_mono" min_width scfg._content_width() - scfg._px(98) xmaximum scfg._content_width() - scfg._px(98) yalign 0.5 substitute False
            textbutton _("Copy") style "scfg_small_button" xsize scfg._px(90) yalign 0.5 action scfg.Copy(value.split(" ")[0])
    if ctl.config.methods["manual"]["allow_tx_paste"]:
        null height scfg._px(4)
        text (_("Paid from an exchange or another wallet? Paste the %s:") % scfg._tx_word(ctl)) style "scfg_muted" substitute False
        default tx_value = FieldInputValue(ctl, "tx_input", default=False)
        hbox:
            button:
                style "scfg_inputbox"
                xsize scfg._content_width() - scfg._px(196 if scfg.WEB else 98)
                action tx_value.Toggle()
                input value tx_value length 90 copypaste True allow scfg.TX_CHARS style "scfg_input"
            if scfg.WEB:
                textbutton _("Paste") style "scfg_small_button" xsize scfg._px(90) yalign 0.5 action Function(scfg._paste_hash, cid)
            textbutton _("Check") style "scfg_small_button" xsize scfg._px(90) yalign 0.5 action Function(scfg._submit_hash, cid)


screen scfg_step_waiting(ctl, cid):
    text scfg._waiting_title(ctl) style "scfg_heading" substitute False
    if ctl.state == "confirming":
        bar value StaticValue(ctl.confirmations, max(1, ctl.required)) style "scfg_progress"
    if ctl.hint_tx:
        text (_("Your %s:") % scfg._tx_word(ctl)) style "scfg_label" substitute False
        text ctl.hint_tx style "scfg_mono" substitute False
        if ctl.explorer_url():
            textbutton _("View on explorer") style "scfg_small_button" action Function(scfg._open_explorer, ctl.explorer_url())
    elif ctl.method == "browser":
        if scfg.WEB:
            text _("Finish the payment in the wallet window.") style "scfg_muted"
            textbutton _("Reopen wallet window") style "scfg_small_button" action Function(scfg._open_browser, cid)
        else:
            text _("Finish the payment on the checkout page in your browser.") style "scfg_muted"
            textbutton _("Reopen checkout page") style "scfg_small_button" action Function(scfg._open_browser, cid)
    null height scfg._px(4)
    text _("You can keep playing: the game keeps checking in the background and finishes the purchase by itself.") style "scfg_muted"


screen scfg_step_done(ctl, cid):
    text scfg._done_title(ctl) style "scfg_heading" color scfg._status_color(ctl) substitute False
    if ctl.state == "paid" and ctl.result and ctl.result.tx_hash:
        text (_("Keep this %s to restore your purchase on another device:") % scfg._tx_word(ctl)) style "scfg_muted" substitute False
        text ctl.result.tx_hash style "scfg_mono" substitute False
        hbox:
            textbutton (_("Copy signature") if ctl.family == "solana" else _("Copy hash")) style "scfg_small_button" action scfg.Copy(ctl.result.tx_hash)
            textbutton _("View on explorer") style "scfg_small_button" action Function(scfg._open_explorer, ctl.explorer_url())
    elif ctl.state == "underpaid":
        text _("The amount received was less than the price. Please contact the developer.") style "scfg_muted"
    elif ctl.state == "expired":
        text _("If you already paid, the game will keep checking for 24 hours and unlock it automatically.") style "scfg_muted"


screen scfg_restore(rid):
    modal True
    zorder 200

    $ ctl = scfg._lookup_restore(rid)

    key "game_menu" action NullAction()

    add Solid(scfg.DIM) at scfg_dim_in

    frame:
        style "scfg_sheet"
        style_prefix "scfg"
        at scfg_sheet_in

        side "c b":
          viewport:
            mousewheel True
            draggable True
            scrollbars "vertical"

            vbox:
              style "scfg_body"
              xsize scfg._content_width()

              use scfg_logo

              text _("Restore purchases") style "scfg_title"

              if ctl.state == "closed":
                  text _("This screen is no longer active.")

              elif ctl.state in ("input", "checking"):
                  text _("Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history.") style "scfg_muted"
                  default hash_value = FieldInputValue(ctl, "tx_input", default=True)
                  hbox:
                      button:
                          style "scfg_inputbox"
                          xsize scfg._content_width() - (scfg._px(98) if scfg.WEB else 0)
                          action hash_value.Toggle()
                          input value hash_value length 90 copypaste True allow scfg.TX_CHARS style "scfg_input"
                      if scfg.WEB:
                          textbutton _("Paste") style "scfg_small_button" xsize scfg._px(90) yalign 0.5 action Function(scfg._paste_hash, rid) sensitive ctl.state == "input"
                  textbutton _("Check"):
                      style "scfg_accent_button"
                      text_color scfg.on_accent()
                      xsize scfg._content_width()
                      action Function(scfg._restore_check, rid)
                      sensitive ctl.state == "input"
                  if ctl.state == "checking":
                      use scfg_status(_("Checking the blockchain..."), scfg.accent())

              elif ctl.state == "choose":
                  for i in ctl.pending_choices():
                      text scfg._candidate_line(ctl.candidates[i]) substitute False
                      $ choices = ctl.candidates[i].choices
                      $ cell = scfg._cell(len(choices), max_cols=2)
                      $ row_h = scfg._option_height(python_list((scfg._choice_label(c), None) for c in choices), cell)
                      hbox:
                          for choice in choices:
                              use scfg_option(scfg._choice_label(choice), Function(scfg._restore_choose, rid, i, choice), selected=(ctl.choices.get(i) == choice), width=cell, height=row_h)
                  textbutton _("Restore") style "scfg_primary_button" xsize scfg._content_width() action Function(scfg._restore_confirm, rid) sensitive ctl.can_confirm

              elif ctl.state == "saving":
                  use scfg_status(_("Saving..."), scfg.accent())

              elif ctl.state == "done":
                  if ctl.added:
                      text _("Restored:") style "scfg_ok"
                      for r in ctl.added:
                          text scfg._restored_line(r) substitute False
                  else:
                      text _("Nothing new to restore. These purchases are already on this device.")

              if ctl.message():
                  text ctl.message() style "scfg_error" substitute False

              $ owned = scfg.receipts()
              if owned:
                  text _("Purchases on this device") style "scfg_label"
                  for r in owned:
                      hbox:
                          text scfg._receipt_line(r) min_width scfg._content_width() - scfg._px(118) xmaximum scfg._content_width() - scfg._px(118) yalign 0.5 substitute False
                          textbutton _("Copy hash") style "scfg_small_button" xsize scfg._px(110) yalign 0.5 action scfg.Copy(r["tx_hash"])

          vbox:
            style "scfg_footer"
            add Solid(scfg.RULE, xsize=scfg._inner_width(), ysize=max(1, scfg._px(1.5)))
            textbutton _("Close") xsize scfg._inner_width() action Function(scfg._close_restore, rid)
