extends RefCounted

const T := preload("res://tests/t.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const ALL_CHAINS := ["arbitrum", "base", "arc", "polygon", "ethereum"]


func _cfg(extra: Dictionary = {}) -> Dictionary:
	var o := {"wallet": DEV}
	o.merge(extra, true)
	var c: Variant = ScfgConfig.build_config(o)
	if not c is Dictionary:
		T.ok(false, "build_config failed: %s" % str(c))
		return {}
	var item: Variant = ScfgConfig.add_item(c, "chapter2", {"code": 37, "price": "4.99", "token": "USDC",
		"title": "Chapter 2", "min_price": "3.99"})
	T.ok(item is Dictionary, "add_item")
	return c


func _code(v: Variant) -> Variant:
	return v.code if v is ScfgErr and v.is_a("ConfigError") else (v.kind if v is ScfgErr else "<ok>")


func test_constants() -> bool:
	T.eq(str(ScfgPayments.MIN_TIP), "0.50", "MIN_TIP")
	T.ok(ScfgPayments.MIN_TIP is ScfgDec, "MIN_TIP is a Decimal")
	T.eq(ScfgPayments.TIP_PRESETS.map(func(d: ScfgDec) -> String: return str(d)), ["1.00", "3.00", "5.00", "10.00"], "TIP_PRESETS")
	return T.done()


func test_unlock_defaults_from_item() -> bool:
	var spec: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2"})
	T.ok(spec is Dictionary, "prepare_payment")
	if spec is Dictionary:
		T.eq(spec.keys(), ["kind", "item", "price", "token", "code", "title", "chains"], "PaymentSpec fields in order")
		T.eq([spec["kind"], spec["item"], str(spec["price"]), spec["token"], spec["code"], spec["title"], spec["chains"]],
			["unlock", "chapter2", "4.99", "USDC", 37, "Chapter 2", ALL_CHAINS], "values")
	return T.done()


func test_unlock_sale_price_down_to_min_price() -> bool:
	var ok: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "amount": "3.99"})
	T.eq(str(ok["price"]) if ok is Dictionary else str(ok), "3.99", "at min_price")
	T.eq(_code(ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "amount": "3.98"})), "payment.below_min_price", "below")
	var msg: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "amount": "3.98"})
	T.eq((msg as ScfgErr).message, "amount 3.98 is below the item's min_price 3.99", "message")
	T.eq(str(ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "amount": 7})["price"]), "7.00", "an int amount")
	return T.done()


func test_unlock_in_other_token_limits_chains() -> bool:
	var spec: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "token": "USDT"})
	T.ok(spec is Dictionary and spec["token"] == "USDT" and not "base" in spec["chains"], "USDT: no Base")
	# falsy values fall back to the item's
	var falsy: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "token": "", "title": ""})
	T.ok(falsy is Dictionary and falsy["token"] == "USDC" and falsy["title"] == "Chapter 2", "'' falls back")
	var titled: Variant = ScfgPayments.prepare_payment(_cfg(), "unlock", {"item": "chapter2", "title": "A better title"})
	T.eq(titled["title"], "A better title", "title override")
	return T.done()


func test_tip_spec() -> bool:
	var spec: Variant = ScfgPayments.prepare_payment(_cfg(), "tip", {"amount": "3", "token": "USDC", "title": "Coffee"})
	T.ok(spec is Dictionary, "tip")
	if spec is Dictionary:
		T.eq([spec["kind"], spec["code"], str(spec["price"]), spec["title"], spec["item"]], ["tip", 0, "3.00", "Coffee", null], "values")
	var plain: Variant = ScfgPayments.prepare_payment(_cfg(), "tip", {"amount": "1", "token": "USDC", "title": 0, "item": ["z"]})
	T.ok(plain is Dictionary and plain["title"] == "Tip" and plain["item"] == ["z"],
		"a falsy title is 'Tip'; item is passed through whatever it is")
	T.eq(str(ScfgPayments.prepare_payment(_cfg(), "tip", {"amount": "0.5", "token": "USDC"})["price"]), "0.50", "the minimum tip")
	T.eq(str(ScfgPayments.prepare_payment(_cfg(), "tip", {"amount": "1e1", "token": "USDC"})["price"]), "10.00", "exponent")
	return T.done()


func test_rejections() -> bool:
	var c := _cfg()
	var rows := [
		["unlock", {"item": "nope"}, "item.unknown"], ["unlock", {}, "item.unknown"], ["unlock", {"item": 5}, "item.unknown"],
		["unlock", {"item": ["chapter2"]}, "TypeError"],
		["tip", {"token": "USDC"}, "tip.needs_amount_and_token"], ["tip", {"amount": "1"}, "tip.needs_amount_and_token"],
		["tip", {}, "tip.needs_amount_and_token"], ["tip", {"amount": 0}, "tip.needs_amount_and_token"],
		["tip", {"amount": "0.49", "token": "USDC"}, "tip.below_minimum"],
		["gift", {"amount": "1", "token": "USDC"}, "payment.bad_kind"], ["", {}, "payment.bad_kind"],
		[null, {}, "payment.bad_kind"], [["a"], {}, "payment.bad_kind"], ["UNLOCK", {}, "payment.bad_kind"],
		["unlock", {"item": "chapter2", "token": "DAI"}, "token.not_enabled"],
		["unlock", {"item": "chapter2", "token": ["USDC"]}, "token.not_enabled"],
		["tip", {"amount": "1", "token": ""}, "token.not_enabled"], ["tip", {"amount": "1", "token": 5}, "token.not_enabled"],
		["tip", {"amount": "1", "token": "DAI"}, "token.not_enabled"],
		["unlock", {"item": "chapter2", "amount": 1.5}, "TypeError"], ["tip", {"amount": 1.5, "token": "USDC"}, "TypeError"],
		["tip", {"amount": true, "token": "USDC"}, "TypeError"],
		["unlock", {"item": "chapter2", "amount": 0}, "ValueError"], ["tip", {"amount": "abc", "token": "USDC"}, "ValueError"],
		["tip", {"amount": "1", "token": "USDC", "foo": 1}, "TypeError"],
	]
	for row: Array in rows:
		T.eq(_code(ScfgPayments.prepare_payment(c, row[0], row[1])), row[2],
			"prepare_payment(%s, %s)" % [ScfgPy.py_repr(row[0]), ScfgPy.py_repr(row[1])])
	T.eq(_code(ScfgPayments.prepare_payment(_cfg({"tokens": ["USDC"]}), "tip", {"amount": "1", "token": "USDT"})), "token.not_enabled", "token not enabled")
	T.eq(_code(ScfgPayments.prepare_payment(_cfg({"chains": ["base"]}), "tip", {"amount": "1", "token": "USDT"})), "token.unavailable",
		"USDT is enabled but no enabled chain carries it")
	return T.done()
