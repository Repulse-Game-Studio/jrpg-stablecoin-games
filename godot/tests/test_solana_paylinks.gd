extends RefCounted

const T := preload("res://tests/t.gd")

const WALLET := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const MINT := "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
const REF := "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny"


func _uri(units: Variant, label: Variant, message: Variant) -> Variant:
	return ScfgSolanaPaylinks.solana_pay_uri({"recipient": WALLET, "units": units, "mint": MINT, "reference": REF,
		"label": label, "message": message})


# tests/unit/test_solana_paylinks.py::test_transfer_request_uri
func test_transfer_request_uri() -> bool:
	T.eq(_uri(4993742, "Ruby Dreams", "Chapter 2: The Fall & more"),
		"solana:%s?amount=4.993742&spl-token=%s&reference=%s" % [WALLET, MINT, REF]
		+ "&label=Ruby%20Dreams&message=Chapter%202%3A%20The%20Fall%20%26%20more", "the Solana Pay link")
	return T.done()


# tests/unit/test_solana_paylinks.py::test_whole_amount_keeps_six_decimals_and_empty_label_is_omitted
func test_whole_amount_keeps_six_decimals_and_empty_label_is_omitted() -> bool:
	var uri: String = _uri(5000000, "", "Tip")
	T.ok("amount=5.000000&" in uri and not "label=" in uri, uri)
	return T.done()


## urllib.parse.quote(v, safe="") over UTF-8 (expected strings from CPython).
func test_quote_matches_python() -> bool:
	var table := [
		["a/b+c=d%e#f", "a%2Fb%2Bc%3Dd%25e%23f"], ["unreserved !*'()", "unreserved%20%21%2A%27%28%29"],
		["tab\there", "tab%09here"], ["Café", "Caf%C3%A9"], ["\U01f468‍\U01f469", "%F0%9F%91%A8%E2%80%8D%F0%9F%91%A9"],
		["~._-", "~._-"], ["\u007f", "%7F"], ["AZaz09", "AZaz09"],
	]
	for row: Array in table:
		T.eq(ScfgSolanaPaylinks.quote(row[0]), row[1], "quote(%s)" % ScfgPy.py_repr(row[0]))
	T.is_err(_uri(1, 5, "x"), "TypeError", "quote() of a non-str label")
	T.eq(_uri(-5, "a/b c~é😀", ""), "solana:%s?amount=-0.000005&spl-token=%s&reference=%s&label=a%%2Fb%%20c~%%C3%%A9%%F0%%9F%%98%%80" % [
		WALLET, MINT, REF], "negative units and an emoji; an empty message is omitted")
	T.is_err(ScfgSolanaPaylinks.solana_pay_uri({"recipient": WALLET}), "TypeError", "missing keyword arguments")
	return T.done()
