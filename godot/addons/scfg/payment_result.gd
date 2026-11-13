## What `await Scfg.pay(...)`, a pay() callback and the `payment_resolved` signal give the game:
## the core's PaymentResult (ScfgModels) as a typed object.
##
## `status` is "paid", "already_owned", "cancelled", "expired", "underpaid", "failed" or
## "pending" (the player chose to keep playing while the payment confirms; the final result then
## comes later through the callback or `payment_resolved`). `kind` is "unlock" or "tip".
## `amount` is the amount actually paid as a decimal String ("4.99"), or null when nothing was
## matched on chain. `error` is null, a ConfigError code for a misused pay() (such as
## "item.unknown"), or the core's reason for a failed payment.
## Every other field is a String or null.
class_name ScfgPaymentResult
extends RefCounted

var status: String = ""
var kind: Variant = null
var item: Variant = null
var amount: Variant = null
var token: Variant = null
var chain: Variant = null
var tx_hash: Variant = null
var payer: Variant = null
var session_id: Variant = null
var error: Variant = null


## The result for a core PaymentResult Dictionary (ScfgModels.make_payment_result). An ScfgDec
## amount becomes its String; missing fields stay null.
static func from_core(d: Dictionary) -> ScfgPaymentResult:
	var r := ScfgPaymentResult.new()
	r.status = str(d.get("status", ""))
	r.kind = d.get("kind")
	r.item = d.get("item")
	r.amount = null if d.get("amount") == null else str(d["amount"])
	r.token = d.get("token")
	r.chain = d.get("chain")
	r.tx_hash = d.get("tx_hash")
	r.payer = d.get("payer")
	r.session_id = d.get("session_id")
	r.error = d.get("error")
	return r


## The fields as a Dictionary, in the core's order.
func to_dict() -> Dictionary:
	return {"status": status, "kind": kind, "item": item, "amount": amount, "token": token, "chain": chain,
		"tx_hash": tx_hash, "payer": payer, "session_id": session_id, "error": error}


func _to_string() -> String:
	return "ScfgPaymentResult(%s)" % JSON.stringify(to_dict())
