extends RefCounted

const T := preload("res://tests/t.gd")


func test_payment_result_has_the_python_defaults() -> bool:
	var r: Variant = ScfgModels.make_payment_result({"status": "paid", "kind": "unlock", "item": "chapter2"})
	T.eq(r, {"status": "paid", "kind": "unlock", "item": "chapter2", "amount": null, "token": null, "chain": null,
		"tx_hash": null, "payer": null, "session_id": null, "error": null}, "every field, in dataclass order")
	T.eq((r as Dictionary).keys(), ScfgModels.PAYMENT_RESULT_FIELDS, "field order")
	T.is_err(ScfgModels.make_payment_result({"status": "paid"}), "TypeError", "kind is required")
	T.is_err(ScfgModels.make_payment_result({"status": "paid", "kind": "tip", "bogus": 1}), "TypeError",
		"an unexpected keyword argument")
	return T.done()


func test_restore_result_has_the_python_defaults() -> bool:
	var a: Variant = ScfgModels.make_restore_result({"status": "nothing_found"})
	T.eq(a, {"status": "nothing_found", "restored": [], "error": null}, "defaults")
	var b: Variant = ScfgModels.make_restore_result({"status": "nothing_found"})
	(a as Dictionary)["restored"].append(1)
	T.eq(b["restored"], [], "restored is a fresh list per result (default_factory)")
	T.is_err(ScfgModels.make_restore_result({}), "TypeError", "status is required")
	return T.done()


func test_item_and_transfer_take_every_field() -> bool:
	var item: Variant = ScfgModels.make_item({"item_id": "x", "code": 1, "price": ScfgDec.parse("1.00"), "token": "USDC",
		"title": "X", "min_price": ScfgDec.parse("1.00")})
	T.eq((item as Dictionary).keys(), ["item_id", "code", "price", "token", "title", "min_price"], "Item fields")
	T.is_err(ScfgModels.make_item({"item_id": "x"}), "TypeError", "Item needs every field")
	var t: Variant = ScfgTransfers.make_transfer({"token": "t", "sender": "s", "recipient": "r", "value": 5,
		"tx_hash": "h", "log_index": 1, "block_number": 2})
	T.eq((t as Dictionary).keys(), ["token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number"],
		"Transfer fields")
	T.is_err(ScfgTransfers.make_transfer({"token": "t"}), "TypeError", "Transfer needs every field")
	T.is_err(ScfgTransfers.make_transfer({"token": "t", "sender": "s", "recipient": "r", "value": 5, "tx_hash": "h",
		"log_index": 1, "block_number": 2, "block_hash": "b"}), "TypeError", "no block_hash on a Transfer")
	return T.done()
