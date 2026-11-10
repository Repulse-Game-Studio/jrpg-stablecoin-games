## Validation of a pay() request, independent of chain and network. Mirrors
## scfg_core/payments.py.
##
## Used synchronously by scfg.pay() (so programmer errors surface immediately in the developer's
## script) and again by Engine.start_session.
##
## A PaymentSpec is a Dictionary with Python's fields in order: kind ("unlock" | "tip"), item
## (String or whatever the caller passed for a tip, null by default), price (ScfgDec), token, code
## (int, 0 for tips), title, chains (Array of the enabled chain keys that carry the token, in
## display order).
class_name ScfgPayments
extends RefCounted

static var MIN_TIP: ScfgDec = ScfgDec.parse("0.50")  # err-ok: a valid literal
static var TIP_PRESETS: Array = [
	ScfgDec.parse("1.00"),  # err-ok: valid literals
	ScfgDec.parse("3.00"),  # err-ok: valid literals
	ScfgDec.parse("5.00"),  # err-ok: valid literals
	ScfgDec.parse("10.00"),  # err-ok: valid literals
]

const PAYMENT_SPEC_FIELDS: Array[String] = ["kind", "item", "price", "token", "code", "title", "chains"]


## Validate a payment request and describe it. `opts` holds the keyword arguments item, amount,
## token and title (all optional; null is Python's None). Returns a PaymentSpec Dictionary.
## @fallible
static func prepare_payment(config: Dictionary, kind: Variant, opts: Dictionary = {}) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in ["item", "amount", "token", "title"]):
			return ScfgErr.make("TypeError", "prepare_payment() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	var item: Variant = opts.get("item")
	var amount: Variant = opts.get("amount")
	var token: Variant = opts.get("token")
	var title: Variant = opts.get("title")
	var price: ScfgDec
	var code: int
	if kind is String and kind == "unlock":
		if item is Array or item is Dictionary:
			return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(item))
		if not ((item is String or item is StringName) and (config["items"] as Dictionary).has(str(item))):
			return ScfgConfig.config_error("item.unknown", "unknown item %s; declare it with scfg.add_item()" % ScfgPy.py_repr(item))
		var it: Dictionary = config["items"][str(item)]
		if amount == null:
			price = it["price"]
		else:
			var parsed: Variant = ScfgAmounts.parse_amount(amount)
			if parsed is ScfgErr:
				return parsed
			price = parsed
		if price.cmp(it["min_price"]) < 0:
			return ScfgConfig.config_error("payment.below_min_price", "amount %s is below the item's min_price %s" % [
				str(price), str(it["min_price"])])
		token = token if ScfgPy.py_truthy(token) else it["token"]
		code = it["code"]
		title = title if ScfgPy.py_truthy(title) else it["title"]
	elif kind is String and kind == "tip":
		if amount == null or token == null:
			return ScfgConfig.config_error("tip.needs_amount_and_token", "tips need an amount and a token")
		var parsed_tip: Variant = ScfgAmounts.parse_amount(amount)
		if parsed_tip is ScfgErr:
			return parsed_tip
		price = parsed_tip
		if price.cmp(MIN_TIP) < 0:
			return ScfgConfig.config_error("tip.below_minimum", "tips must be at least %s" % str(MIN_TIP))
		code = 0
		title = title if ScfgPy.py_truthy(title) else "Tip"
	else:
		return ScfgConfig.config_error("payment.bad_kind", "kind must be 'unlock' or 'tip', not %s" % ScfgPy.py_repr(kind))
	if not ScfgConfig.in_strs(config["tokens"], token):
		return ScfgConfig.config_error("token.not_enabled", "token %s is not enabled" % ScfgPy.py_repr(token))
	var chains: Variant = ScfgConfig.chains_for_token(config, token)
	if chains is ScfgErr:
		return chains
	if (chains as Array).is_empty():
		return ScfgConfig.config_error("token.unavailable", "%s is not available on any enabled chain" % str(token))
	return {"kind": kind, "item": item, "price": price, "token": token, "code": code, "title": title, "chains": chains}
