## Source lints over the whole addon, res://addons/scfg/ (every .gd file, recursively: the core in
## core/ and the Godot glue around it).
##
## ERROR PROPAGATION (`test_err_propagation_lint`). GDScript has no exceptions:
## a fallible function returns its value or an ScfgErr, and an unchecked ScfgErr flows on as if
## it were a value. The lint works on lines and names, with cheap type resolution for receivers,
## under these exact rules.
##
##   Marking. A function is fallible when the contiguous `##` doc-comment block directly above
##   its `func` line (no blank line between) contains a line `## @fallible` (text may follow the
##   tag). Violations:
##     - a `## @fallible` line whose doc block is not directly followed by a `func` line;
##     - a fallible function declaring a return type other than `Variant` (it must be able to
##       return an ScfgErr).
##
##   Collect step. Every core file is indexed by class: a file's class is its `class_name` (the
##   path when it has none), an inner `class Name` is `<outer>.Name`; each class records its
##   functions (static or not, fallible or not, declared return type), its body-level `var` /
##   `const` members with their types, and its `extends`. A fallible function is keyed
##   `ClassName[.Inner].name` (collect_fallible lists those keys).
##
##   Resolution. A call `NAME(` is a fallible call only when it resolves to a fallible function:
##     - bare `name(` or `self.name(`: the class the call is written in (an inner class's own
##       functions, not its outer class's: GDScript does not resolve those), then its core
##       `extends` chain; a name the class does not define is a global or engine function;
##     - `ScfgX.name(`, `ScfgX.Inner.name(`, or an inner class by its short name in its own file:
##       exactly that class (and its bases); `JSON.parse(`, `OS.x(`, `String.num(` and any other
##       engine class or built-in type never are;
##     - `x.name(` where the receiver's type is known: a parameter, local, `for` variable or
##       member declared with a type (`x: T`, `var x: T`, `var x := <literal | T.new() | y as T |
##       str(..) | a call with a declared return type | a String method returning String>`),
##       `self.member`, `(expr as T)`, `T.new()`, a string literal, `str(..)`, a call of a core
##       function with a declared return type, or a String method returning a String
##       (`s.substr(1).to_int()`). A core type resolves in that class (a function it does not
##       define is an engine method); any other known type (String, int, Array[..], an engine
##       class) is never fallible;
##     - a receiver of unknown type (untyped, `Variant`, an element `a[i]`, a call without a
##       declared return type): matched by name against the fallible INSTANCE methods of every
##       core class. A static function is never matched this way.
##
##   Calls. Every `NAME(` in code (not in a comment or string literal) that resolves to a
##   fallible function, other than its own `func NAME(` line, is a fallible call. Lines are
##   first joined into statements while brackets are open. A fallible call is checked when one of these holds:
##     1. assignment: the statement is `[var] X [: Variant] = [await] CALLEE(...)`, CALLEE being
##        the call (with an optional dotted receiver such as `ScfgPy.` or `self.limbs.`), nothing
##        but an optional `; ...` follows its closing parenthesis, and `X is ScfgErr` or
##        `ScfgErr.is_err(X)` appears after an `if`/`elif` in that `; ...` tail or in one of the
##        next 2 non-blank, non-comment lines. `X` declared with a type other than `Variant` is
##        a violation even when checked (the assignment itself would crash on an ScfgErr);
##     2. direct return: the statement is `return [await] CALLEE(...)` with nothing after the
##        call; the enclosing function must then be fallible (see Propagation);
##     3. waiver: a line of the statement carries `# err-ok: <reason>` with a non-empty reason.
##        It covers every fallible call in the statement. Use it where a precondition is proven
##        (`p.sub(one)  # err-ok: p > 1`) and for a call on a receiver the resolution cannot type.
##        A waiver on a statement with no fallible call is itself a violation (a stale waiver).
##   Anything else (arithmetic on the result, passing it as an argument, storing it in a
##   dictionary or member, a check 3 lines later) is an unchecked call.
##
##   Propagation. A function that builds or returns an ScfgErr must be marked fallible. The
##   statement forms that make a function count as one (reported once per function):
##     - `ScfgErr.make(` anywhere in a statement: a returned variable (`var bad := ScfgErr.make(..)`
##       ... `return bad`), an argument, a ternary branch, a lambda inside the function;
##     - `return ScfgErr.<anything>` other than `ScfgErr.is_err(..)`;
##     - a direct return of a fallible call (rule 2);
##     - `return X` where `X is ScfgErr` (or `ScfgErr.is_err(X)`, not preceded by `not`) is tested
##       in the same statement (`if X is ScfgErr: return X`) or in the header of any enclosing
##       block of the function, at any distance (`if X is ScfgErr:` ... `return X`);
##     - a ternary `return ... if ... else ...` that tests anything against ScfgErr.
##
##   Known limits (what the lint cannot see): triple-quoted strings are not understood (addon code
##   does not use them); on a receiver of unknown type, an instance method name shared by a
##   fallible and an infallible function flags both (type the receiver, or waive); a static
##   function called through an instance, and a lambda's parameters, are not resolved; types are
##   not inferred through `=` without a type, through `Variant` returns, or across `.call()`; a
##   local's type is per function, not per block (two declarations with different types make it
##   unknown); an ScfgErr built any other way than `ScfgErr.make(` (`ScfgErr.new()`), or
##   handed over through a variable the function never tests (`return d["err"]`, `return x` where
##   x was assigned from a Callable's `.call()` or from an untyped member); a fallible function
##   reached by `.call()`, a Callable or a signal; a bool-valued ternary that mentions ScfgErr is
##   a false positive; the `X is ScfgErr` test is matched by name, so another variable of the same
##   name in an enclosing block also counts; `match` patterns are not tests; and a check that is
##   not in an `if`/`elif` line (`assert`, `and`/`or` chains in a `return`, `while`) is not seen.
##
## NO ASSERTS (`test_no_asserts_in_addon`): `assert(` never appears in addon code, because export
## builds strip it (a caller-triggered precondition must return an ScfgErr instead).
extends RefCounted

const T := preload("res://tests/t.gd")
const ADDON_DIR := "res://addons/scfg"

const _FUNC_RE := "^(\\s*)(?:static\\s+)?func\\s+([A-Za-z_]\\w*)\\s*\\((.*)$"
const _ASSIGN_RE := "^\\s*(?:var\\s+)?([A-Za-z_]\\w*)\\s*(?::\\s*([A-Za-z_][\\w.]*)?\\s*)?=(?!=)\\s*(?:await\\s+)?"
const _RETURN_RE := "^\\s*return\\s+(?:await\\s+)?"
## A member (class body level) or local declaration: name, explicit type, `=` or `:=`, value.
const _MEMBER_RE := "^\\s*(?:@\\w+(?:\\([^)]*\\))?\\s+)*(?:static\\s+)?(?:var|const)\\s+([A-Za-z_]\\w*)\\s*(?::(?!=)\\s*([A-Za-z_][\\w.]*(?:\\[[\\w.]+\\])?)?\\s*)?(?:(:?=)\\s*(.*?))?\\s*:?\\s*$"
const _LOCAL_RE := "^\\s*var\\s+([A-Za-z_]\\w*)\\s*(?::(?!=)\\s*([A-Za-z_][\\w.]*(?:\\[[\\w.]+\\])?)?\\s*)?(?:(:?=)\\s*(.*))?$"


func test_no_asserts_in_addon() -> bool:
	var sources := addon_sources()
	T.ok(sources.size() >= 3, "found the core sources (%d)" % sources.size())
	for path: String in sources:
		var lines: PackedStringArray = (sources[path] as String).split("\n")
		var found: Array[String] = []
		for n in lines.size():
			var code: String = _split_comment(lines[n])[0]
			if _regex("\\bassert\\s*\\(").search(code) != null:
				found.append("%s:%d: %s" % [path, n + 1, lines[n].strip_edges()])
		T.ok(found.is_empty(), "assert() in addon code (exports strip it): %s" % "; ".join(PackedStringArray(found)))
	return T.done()


func test_err_propagation_lint() -> bool:
	var sources := addon_sources()
	T.ok(sources.size() >= 3, "found the core sources (%d)" % sources.size())
	var fallible := collect_fallible(sources)
	T.ok(fallible.size() > 0, "the core marks some functions @fallible")
	for v: String in lint_err_propagation(sources):
		T.ok(false, v)
	return T.done()


const _PARSE := """## Parses.
## @fallible
static func parse(s: String) -> Variant:
	if s == "":
		return ScfgErr.make("ValueError", "empty")
	return 1


## An instance method.
## @fallible
func sub(o: Variant) -> Variant:
	return parse(str(o))
"""


func test_err_propagation_lint_rules() -> bool:
	var good := _PARSE + """
## @fallible: says why
static func twice(s: String) -> Variant:
	var v = parse(s)
	if v is ScfgErr:
		return v
	return v * 2


## @fallible
static func passthrough(s: String):
	return parse(s)


## @fallible
func awaited(s: String) -> Variant:
	var v: Variant = await parse(s)
	if v is ScfgErr: return v
	return v


static func handled(s: String) -> int:
	var v: Variant = parse(s)

	# blank and comment-only lines do not count
	if v is ScfgErr:
		return 0
	return v


static func waived(s: String) -> int:
	return parse(s) + 1  # err-ok: s is never empty here


static func one_line(s: String) -> int:
	var v = parse(s); if ScfgErr.is_err(v): return 0
	return v


static func multi_line(s: String) -> int:
	var v = parse(
		s)
	if v is ScfgErr:
		return 0
	print("parse(s) in a string is not a call")  # nor is parse(s) in a comment
	return v


static func else_branch(s: String) -> int:
	var v = parse(s)
	if v is ScfgErr:
		return 0
	else:
		return v


static func negated(s: String) -> int:
	var v = parse(s)
	if not v is ScfgErr:
		return v
	return 0


static func not_an_error_type(s: String) -> bool:
	var v = parse(s)
	if v is ScfgErr: return false
	return ScfgErr.is_err(v)  # a bool, not the ScfgErr itself


static func other_name(s: String) -> int:
	var v = parse(s)
	if v is ScfgErr:
		return 0
	var w := 5
	if w > 3:
		return w
	return 1
"""
	var accepted := lint_err_propagation({"good.gd": good})
	T.eq(accepted.size(), 0, "the accepted forms: %s" % [accepted])
	var bad := {
		"arithmetic": ["static func f(s: String) -> int:\n\treturn parse(s) + 1\n", "unchecked call to parse"],
		"argument": ["static func f(s: String) -> int:\n\treturn abs(parse(s))\n", "unchecked call to parse"],
		"receiver": ["static func f(s: String) -> int:\n\treturn obj.sub(s) + 1\n", "unchecked call to sub"],
		"after_call": ["static func f(s: String) -> Variant:\n\tvar v = make().sub(s)\n\tif v is ScfgErr:\n"
			+ "\t\treturn 0\n\treturn v\n", "unchecked call to sub"],
		"stored": ["func f(s: String) -> void:\n\t_state[\"x\"] = parse(s)\n", "unchecked call to parse"],
		"too_late": ["static func f(s: String) -> int:\n\tvar v = parse(s)\n\tprint(1)\n\tprint(2)\n"
			+ "\tif v is ScfgErr:\n\t\treturn 0\n\treturn v\n", "unchecked call to parse"],
		"chained": ["static func f(s: String) -> int:\n\tvar v = parse(s).abs()\n\tif v is ScfgErr:\n"
			+ "\t\treturn 0\n\treturn v\n", "unchecked call to parse"],
		"empty_waiver": ["static func f(s: String) -> int:\n\treturn parse(s) + 1  # err-ok:\n",
			"unchecked call to parse"],
		"typed": ["static func f(s: String) -> int:\n\tvar v: int = parse(s)\n\tif v is ScfgErr:\n"
			+ "\t\treturn 0\n\treturn v\n", "typed int"],
		"unmarked_return": ["static func f(s: String) -> Variant:\n\treturn parse(s)\n", "not marked"],
		"unmarked_propagate": ["static func f(s: String) -> Variant:\n\tvar v = parse(s)\n"
			+ "\tif v is ScfgErr:\n\t\treturn v\n\treturn 1\n", "not marked"],
		"unmarked_make": ["static func f() -> Variant:\n\treturn ScfgErr.make(\"ValueError\", \"x\")\n",
			"not marked"],
		"return_type": ["## @fallible\nstatic func f() -> int:\n\treturn 1\n", "return type int"],
		"make_anywhere": ["static func f() -> Variant:\n\tvar bad := ScfgErr.make(\"ValueError\", \"x\")\n"
			+ "\tprint(1)\n\treturn bad\n", "not marked"],
		"make_in_argument": ["static func f(errors: Array) -> void:\n\terrors.append(ScfgErr.make(\"ValueError\", \"x\"))\n",
			"not marked"],
		"ternary_make": ["static func f(c: bool) -> Variant:\n\treturn ScfgErr.make(\"ValueError\", \"x\") if c else 1\n",
			"not marked"],
		"ternary_tested": ["static func f(v: Variant) -> Variant:\n\treturn v if v is ScfgErr else 1\n", "not marked"],
		"ternary_is_err": ["static func f(v: Variant) -> Variant:\n\treturn 1 if not ScfgErr.is_err(v) else v\n",
			"not marked"],
		"tested_earlier": ["static func f(s: String) -> Variant:\n\tvar v = parse(s)\n\tif v is ScfgErr:\n"
			+ "\t\tprint(\"bad\")\n\t\treturn v\n\treturn 1\n", "not marked"],
		"tested_nested": ["static func f(s: String, c: bool) -> Variant:\n\tvar v = parse(s)\n\tif v is ScfgErr:\n"
			+ "\t\tif c:\n\t\t\treturn v\n\treturn 1\n", "not marked"],
		"tested_is_err": ["static func f(s: String) -> Variant:\n\tvar v = parse(s)\n\tif ScfgErr.is_err(v):\n"
			+ "\t\tprint(\"bad\")\n\t\treturn v\n\treturn 1\n", "not marked"],
		"stray_tag": ["## @fallible\nvar x := 1\n", "stray @fallible"],
		"blank_before_func": ["## @fallible\n\nstatic func f() -> Variant:\n\treturn 1\n", "stray @fallible"],
		"stale_waiver": ["static func f(s: String) -> int:\n\treturn s.length()  # err-ok: nothing fallible here\n",
			"covers no fallible call"],
	}
	for name: String in bad:
		# the bad function shares the file with parse() and sub(): a bare call resolves in its file
		var src: String = _PARSE + "\n\n" + bad[name][0]
		var want: String = bad[name][1]
		var got := lint_err_propagation({name + ".gd": src})
		T.eq(got.size(), 1, "%s: one violation (got %s)" % [name, got])
		T.ok(got.size() == 1 and want in got[0] and got[0].begins_with(name + ".gd:"),
			"%s: the violation says '%s' (got %s)" % [name, want, got])
	return T.done()


## Core classes for the resolution rules: ScfgP has fallible statics, instance methods and an
## inner class; ScfgQ has infallible functions of the same names; ScfgPSub inherits from ScfgP.
const _CLASSES := {
	"p.gd": """class_name ScfgP
extends RefCounted

var coef: ScfgP


## @fallible
static func parse(s: String) -> Variant:
	return ScfgErr.make("ValueError", s)


## @fallible
func sub(o: Variant) -> Variant:
	return ScfgErr.make("ValueError", str(o))


## @fallible
func to_int() -> Variant:
	return ScfgErr.make("ValueError", "x")


static func make_p() -> ScfgP:
	return ScfgP.new()


class Inner:
	extends RefCounted

	## @fallible
	static func create(n: int) -> Variant:
		return ScfgErr.make("ValueError", str(n))
""",
	"q.gd": """class_name ScfgQ
extends RefCounted


static func parse(s: String) -> int:
	return s.length()


func to_int() -> int:
	return 1
""",
	"psub.gd": """class_name ScfgPSub
extends ScfgP
""",
}


func test_err_propagation_lint_resolution() -> bool:
	var keys := collect_fallible(_CLASSES)
	T.eq(keys.keys().filter(func(k: String) -> bool: return k.begins_with("ScfgP")),
		["ScfgP.parse", "ScfgP.sub", "ScfgP.to_int", "ScfgP.Inner.create"], "keyed by Class[.Inner].name")
	# calls that resolve to no fallible function: no violation
	var accepted := {
		"other_class": "static func f(s: String) -> int:\n\treturn ScfgQ.parse(s) + 1\n",
		"engine_static": "static func f(s: String) -> Variant:\n\treturn JSON.parse_string(s)\n",
		"engine_instance": "static func f(s: String) -> int:\n\treturn JSON.new().parse(s) + 1\n",
		"bare_other_file": "static func f(s: String) -> int:\n\treturn parse(s) + 1\n",
		"bare_own_infallible": "static func parse(s: String) -> int:\n\treturn 1\n\n\n"
			+ "static func f(s: String) -> int:\n\treturn parse(s) + 1\n",
		"typed_param": "static func f(s: String) -> int:\n\treturn s.to_int() + 1\n",
		"typed_local": "static func f(x: Variant) -> int:\n\tvar s: String = str(x)\n\treturn s.to_int() + 1\n",
		"inferred_literal": "static func f() -> int:\n\tvar s := \"12\"\n\treturn s.to_int() + 1\n",
		"string_literal": "static func f() -> int:\n\treturn \"12\".to_int() + 1\n",
		"string_method": "static func f(s: String) -> int:\n\treturn s.substr(1).strip_edges().to_int() + 1\n",
		"inferred_string_method": "static func f(s: String) -> int:\n\tvar t := s.trim_prefix(\"-\")\n"
			+ "\treturn t.to_int() + 1\n",
		"str_call": "static func f(x: Variant) -> int:\n\treturn str(x).to_int() + 1\n",
		"cast_other": "static func f(x: Variant) -> int:\n\treturn (x as String).to_int() + 1\n",
		"typed_other_core": "static func f(q: ScfgQ) -> int:\n\treturn q.to_int() + 1\n",
		"static_name_on_unknown": "static func f(x: Variant) -> int:\n\treturn x.parse(1) + 1\n",
		"for_typed": "static func f(xs: Array[String]) -> int:\n\tfor x: String in xs:\n\t\treturn x.to_int()\n\treturn 0\n",
	}
	for name: String in accepted:
		var src: Dictionary = _CLASSES.duplicate()
		src[name + ".gd"] = accepted[name]
		var got := lint_err_propagation(src)
		T.eq(got.size(), 0, "%s: no violation (got %s)" % [name, got])
	# calls that resolve to a fallible function: one unchecked-call violation each
	var flagged := {
		"qualified": ["static func f(s: String) -> int:\n\treturn ScfgP.parse(s) + 1\n", "parse"],
		"qualified_inner": ["static func f() -> int:\n\treturn ScfgP.Inner.create(1) + 1\n", "create"],
		"typed_core": ["static func f(p: ScfgP) -> int:\n\treturn p.sub(1) + 1\n", "sub"],
		"typed_core_local": ["static func f() -> int:\n\tvar p: ScfgP = ScfgP.make_p()\n\treturn p.sub(1) + 1\n", "sub"],
		"declared_return": ["static func f() -> int:\n\tvar p := ScfgP.make_p()\n\treturn p.sub(1) + 1\n", "sub"],
		"cast_core": ["static func f(x: Variant) -> int:\n\treturn (x as ScfgP).sub(1) + 1\n", "sub"],
		"new_core": ["static func f() -> int:\n\treturn ScfgP.new().sub(1) + 1\n", "sub"],
		"unknown_receiver": ["static func f(x: Variant) -> int:\n\treturn x.to_int() + 1\n", "to_int"],
		"untyped_param": ["static func f(x) -> int:\n\treturn x.sub(1) + 1\n", "sub"],
		"inherited": ["class_name ScfgPSub2\nextends ScfgP\n\n\nfunc f() -> int:\n\treturn sub(1) + 1\n", "sub"],
	}
	for name: String in flagged:
		var src: Dictionary = _CLASSES.duplicate()
		src[name + ".gd"] = flagged[name][0]
		var got := lint_err_propagation(src)
		T.ok(got.size() == 1 and ("unchecked call to %s()" % flagged[name][1]) in got[0] and got[0].begins_with(name + ".gd:"),
			"%s: one unchecked %s() (got %s)" % [name, flagged[name][1], got])
	# inside ScfgP's own file: bare calls, members, self and the inner class by its short name
	var own := {
		"bare": ["static func f(s: String) -> int:\n\treturn parse(s) + 1\n", "parse"],
		"self": ["func f() -> int:\n\treturn self.sub(1) + 1\n", "sub"],
		"member": ["func f() -> int:\n\treturn coef.sub(1) + 1\n", "sub"],
		"inner_short": ["static func f() -> int:\n\treturn Inner.create(1) + 1\n", "create"],
	}
	for name: String in own:
		var src: Dictionary = _CLASSES.duplicate()
		src["p.gd"] = (_CLASSES["p.gd"] as String).replace("\n\nclass Inner:", "\n\n" + own[name][0] + "\n\nclass Inner:")
		var got := lint_err_propagation(src)
		T.ok(got.size() == 1 and ("unchecked call to %s()" % own[name][1]) in got[0],
			"own %s: one unchecked %s() (got %s)" % [name, own[name][1], got])
	# a bare call inside the inner class resolves in the inner class, not in ScfgP
	var inner_src: Dictionary = _CLASSES.duplicate()
	inner_src["p.gd"] = (_CLASSES["p.gd"] as String) + "\n\n\tstatic func g() -> int:\n\t\treturn create(1) + parse(\"\")\n"
	var got_inner := lint_err_propagation(inner_src)
	T.ok(got_inner.size() == 1 and "unchecked call to create()" in got_inner[0],
		"inner: create() resolves, parse() is not Inner's (got %s)" % [got_inner])
	return T.done()


# --- the lint ---------------------------------------------------------------------------------


func test_no_unawaited_coroutines() -> bool:
	var sources := addon_sources()
	var idx := index_sources(sources, [])
	var cos := mark_coroutines(idx, sources)
	T.ok(cos.size() > 10, "the core has coroutines (%d)" % cos.size())
	for key: String in ["ScfgRpcClient.call_rpc", "ScfgRpcClient.get_logs", "ScfgWatcher.advance",
			"ScfgEngine.tick", "ScfgEngine.start_session", "ScfgBaseBackend.start", "ScfgCoio.Transport.post_json",
			"ScfgSolanaBackend.advance", "ScfgSolanaBackend.build_payment", "ScfgSolanaBackend._walk",
			"ScfgEngine.build_solana_payment", "ScfgEngine.LazyRpc.account_exists"]:
		T.ok(cos.has(key), "%s is a known coroutine" % key)
	for v: String in lint_unawaited(sources):
		T.ok(false, v)
	return T.done()


const _CO_CLASSES := {
	"co.gd": """class_name ScfgCo
extends RefCounted

var peer: ScfgCo


## By its body.
func fetch() -> Variant:
	await Engine.get_main_loop().process_frame
	return 1


## An interface method: no await in its body, but implementations suspend.
## @coroutine
func start(_o: Dictionary) -> Variant:
	return null


static func sfetch() -> Variant:
	var v: Variant = await ScfgCo.new().fetch()
	return v


func plain() -> int:
	return 2
""",
	"cosub.gd": """class_name ScfgCoSub
extends ScfgCo


## Overrides a coroutine: callers must still await it.
func start(_o: Dictionary) -> Variant:
	return 3
""",
}


func test_no_unawaited_coroutines_rules() -> bool:
	var keys := mark_coroutines(index_sources(_CO_CLASSES, []), _CO_CLASSES)
	T.eq(keys.keys(), ["ScfgCo.fetch", "ScfgCo.start", "ScfgCo.sfetch", "ScfgCoSub.start"],
		"by body, by @coroutine tag, and by overriding one")
	var good := """extends RefCounted

var co: ScfgCo


func a(x: Variant, c: ScfgCo) -> Variant:
	var v: Variant = await x.fetch()
	var w: Variant = await c.start({})
	var y: Variant = await ScfgCo.sfetch()
	var z: Variant = await (x as ScfgCo).fetch()
	var p := c.plain()
	var q: Variant = await co.peer.fetch()
	print("x.fetch() in a string is not a call")  # nor is x.fetch() in a comment
	return [v, w, y, z, p, q]


func b(x: Variant) -> Variant:
	return await x.start(
		{})


func fetch_not_core(o: RefCounted) -> Variant:
	return o.get_class()
"""
	var sources := _CO_CLASSES.duplicate()
	sources["good.gd"] = good
	var accepted := lint_unawaited(sources)
	T.eq(accepted.size(), 0, "the accepted forms: %s" % [accepted])
	var bad := {
		"untyped": ["func f(x: Variant) -> Variant:\n\treturn x.fetch()\n", "fetch() is a coroutine"],
		"typed": ["func f(c: ScfgCo) -> Variant:\n\tvar v: Variant = c.fetch()\n\treturn v\n", "fetch() is a coroutine"],
		"static": ["func f() -> Variant:\n\treturn ScfgCo.sfetch()\n", "sfetch() is a coroutine"],
		"tagged": ["func f(c: ScfgCo) -> Variant:\n\treturn c.start({})\n", "start() is a coroutine"],
		"override": ["func f(c: ScfgCoSub) -> Variant:\n\treturn c.start({})\n", "start() is a coroutine"],
		"argument": ["func f(x: Variant) -> Variant:\n\treturn await g(x.fetch())\n\n\nfunc g(v: Variant) -> Variant:\n"
			+ "\tawait Engine.get_main_loop().process_frame\n\treturn v\n", "fetch() is a coroutine"],
		"bare_in_own_class": ["func f() -> Variant:\n\tvar v: Variant = g()\n\treturn v\n\n\nfunc g() -> Variant:\n"
			+ "\tawait Engine.get_main_loop().process_frame\n\treturn 1\n", "g() is a coroutine"],
		"member": ["var co: ScfgCo\n\n\nfunc f() -> Variant:\n\treturn co.peer.fetch()\n", "fetch() is a coroutine"],
		"stray_tag": ["## @coroutine\nvar x := 1\n", "stray @coroutine"],
	}
	var overriding := _CO_CLASSES.duplicate()
	overriding["coplain.gd"] = "class_name ScfgCoPlain\nextends ScfgCoSub\n\n\nfunc plain() -> int:\n" \
		+ "\tawait Engine.get_main_loop().process_frame\n\treturn 4\n\n\nfunc start(_o: Dictionary) -> Variant:\n" \
		+ "\tawait Engine.get_main_loop().process_frame\n\treturn 5\n\n\nstatic func sfetch() -> Variant:\n\treturn 6\n"
	var over := lint_unawaited(overriding)
	T.eq(over.size(), 1, "a coroutine overriding a plain base method: one violation (got %s)" % [over])
	T.ok(over.size() == 1 and over[0].begins_with("coplain.gd:5:") and "plain() of ScfgCo (co.gd:" in over[0],
		"it names the override and the plain base method (got %s)" % [over])
	for name: String in bad:
		var src: Dictionary = _CO_CLASSES.duplicate()
		src[name + ".gd"] = "extends RefCounted\n\n\n" + bad[name][0]
		var want: String = bad[name][1]
		var got := lint_unawaited(src)
		T.eq(got.size(), 1, "%s: one violation (got %s)" % [name, got])
		T.ok(got.size() == 1 and want in got[0] and got[0].begins_with(name + ".gd:"),
			"%s: the violation says '%s' (got %s)" % [name, want, got])
	return T.done()


## A lambda that awaits makes only the lambda a coroutine, not the function around it.
func test_a_lambda_with_await_does_not_make_its_function_a_coroutine() -> bool:
	var src := {"lam.gd": """class_name ScfgLam
extends RefCounted


func one_line(submit: Callable) -> Variant:
	return submit.call(func(e: Variant) -> Variant: return await e.go())


func block(submit: Callable) -> Variant:
	var job: Variant = submit.call(func(e: Variant) -> Variant:
		var v: Variant = await e.go()
		return v)
	return job


func after_block(submit: Callable) -> Variant:
	var cb := func() -> void:
		await Engine.get_main_loop().process_frame
	submit.call(cb)
	await Engine.get_main_loop().process_frame
	return 1


func awaits_the_call_that_takes_it(f: Callable) -> Variant:
	return await f.call(func() -> int: return 1)


func plain() -> int:
	return 2
"""}
	var keys := mark_coroutines(index_sources(src, []), src)
	T.eq(keys.keys(), ["ScfgLam.after_block", "ScfgLam.awaits_the_call_that_takes_it"],
		"only the functions with an await of their own")
	return T.done()


## Every engine method a controller dispatches by name (`func(e: Variant) -> Variant: return await
## e.NAME(...)`, which the parser cannot check on a Variant) that ScfgEngine does not define.
static func missing_engine_methods(sources: Dictionary) -> Array[String]:
	var have := {}
	for m: Dictionary in (ScfgEngine as GDScript).get_script_method_list():
		have[m["name"]] = true
	var missing: Array[String] = []
	for path: String in sources:
		for line in (sources[path] as String).split("\n"):
			if not "func(e: Variant)" in line:
				continue
			for m: RegExMatch in _regex("\\bawait\\s+e\\.(\\w+)\\(").search_all(_split_comment(line)[0]):
				if not have.has(m.get_string(1)):
					missing.append("%s: e.%s()" % [path, m.get_string(1)])
	return missing


func test_engine_methods_dispatched_by_name_exist() -> bool:
	var sources := addon_sources()
	var seen := 0
	for path: String in sources:
		for line in (sources[path] as String).split("\n"):
			seen += _regex("func\\(e: Variant\\)[^\\n]*\\bawait\\s+e\\.\\w+\\(").search_all(line).size()
	T.ok(seen >= 7, "the controllers dispatch engine commands by name (%d)" % seen)
	T.eq(missing_engine_methods(sources), [] as Array[String], "every dispatched name is an ScfgEngine method")
	var bad := {"x.gd": "var f := func(e: Variant) -> Variant: return await e.start_sessoin(1)\n"
		+ "var g := func(e: Variant) -> Variant: return await e.tick()\n"}
	T.eq(missing_engine_methods(bad), ["x.gd: e.start_sessoin()"], "a misspelt name is caught")
	return T.done()


## COROUTINE DISCIPLINE (`test_no_unawaited_coroutines`). A coroutine called
## without `await` hands its caller a function state instead of a value (or, on an untyped
## receiver, aborts the caller with "Trying to call an async function without await"), and the
## step goes on with garbage. Godot's parser already refuses an unawaited call it can resolve
## statically (a bare call, or a typed receiver); this lint covers what it cannot see (untyped
## receivers, through which the core reaches transports, RPC clients and backends), under these
## exact rules.
##
##   Coroutines. A core function is a coroutine when (a) its body (from its `func` line to its
##   last line, lambda bodies excluded: a lambda that awaits is a coroutine, its function is not) has
##   the keyword `await` in code (not in a comment or string), or
##   (b) its doc block has a `## @coroutine` line (an interface method whose body does not suspend
##   but whose implementations do: ScfgCoio.Transport.post_json, the ScfgBaseBackend methods), or
##   (c) it overrides a coroutine of a core base class (same name). A `## @coroutine` tag whose doc
##   block is not directly above a func is a violation (a stray tag).
##
##   Calls. Every `NAME(` in addon code that resolves to a coroutine (the Resolution rules of the
##   error lint: bare and `self.` in the class and its bases, `ScfgX.` exactly, typed receivers in
##   their class; an untyped receiver matches the coroutine INSTANCE methods by name) must have the
##   keyword `await` directly before its callee (`await NAME(`, `await x.y.NAME(`,
##   `await (x as T).NAME(`). Anything else is a violation: no await, an await on an enclosing
##   call only (`await f(g())` with g a coroutine), or the result used in an expression.
##
##   Overrides. A coroutine instance method that overrides a plain (non-coroutine) method of a
##   core base class is a violation: callers that reach it through the base (ScfgBaseBackend's plain
##   make_rpc and normalize_tx, say) do not await it.
##
##   Known limits: a coroutine reached through `.call()`, a Callable or a signal is not seen; an
##   instance method name shared by a coroutine and a plain function flags the plain one on an
##   untyped receiver (type the receiver); a lambda that awaits is a coroutine itself, which this
##   lint does not follow (its enclosing function is not one, and a one-line lambda's text after
##   its parameter list belongs to it, even past the closing `)` of the call it is passed to). A
##   receiver typed as something other than a core class (an engine class such as `RefCounted` or
##   `Object`, a built-in value, a typed Array) is skipped entirely, even when it holds a core
##   object at runtime: type such a receiver as its core class, or leave it untyped.


## {"Class[.Inner].name": "<path>:<line>"} of every coroutine of `idx` (rules (a), (b), (c)); each
## function entry also gets "coroutine": bool, and idx gains "co_names" and "co_instance".
static func mark_coroutines(idx: Dictionary, sources: Dictionary) -> Dictionary:
	var out := {}
	idx["co_names"] = {}
	idx["co_instance"] = {}
	for path: String in idx["files"]:
		var lines: PackedStringArray = (sources[path] as String).split("\n")
		for f: Dictionary in idx["files"][path]["funcs"]:
			f["coroutine"] = _own_await(lines, f) or f.get("coroutine_tag", false)
	# (c) overrides, to a fixed point (a chain of bases)
	var changed := true
	while changed:
		changed = false
		for key: String in idx["classes"]:
			var cls: Dictionary = idx["classes"][key]
			var base := _class_path(idx, cls["path"], cls["base"])
			if base == "":
				continue
			for name: String in cls["funcs"]:
				var f: Dictionary = cls["funcs"][name]
				var inherited: Variant = _find_func(idx, base, name)
				if not f["coroutine"] and inherited != null and inherited["coroutine"]:
					f["coroutine"] = true
					changed = true
	for key: String in idx["classes"]:
		for name: String in idx["classes"][key]["funcs"]:
			var f: Dictionary = idx["classes"][key]["funcs"][name]
			if f["coroutine"]:
				out["%s.%s" % [key, name]] = f["where"]
				idx["co_names"][name] = true
				if not f["static"] and not idx["co_instance"].has(name):
					idx["co_instance"][name] = f["where"]
	return out


## Whether the body of `f` has an `await` of its own: one inside a lambda (`func(...)` with no
## name) makes only that lambda a coroutine, so lambda code is skipped. A lambda's body is the rest
## of its line after the parameter list's `)` (`func(e): return await e.f()`), or, when that rest
## is only `:`, the following lines indented deeper than the line that opens it.
static func _own_await(lines: PackedStringArray, f: Dictionary) -> bool:
	var n: int = f["line"]
	var end: int = f["end"]
	while n <= end:
		var code: String = _split_comment(lines[n])[0]
		var lam := _regex("\\bfunc\\s*\\(").search(code)
		var own := code if lam == null else code.substr(0, lam.get_start())
		if _regex("\\bawait\\b").search(own) != null:
			return true
		n += 1
		if lam == null:
			continue
		var depth := 1
		var i := lam.get_end()
		while i < code.length() and depth > 0:
			depth += 1 if code[i] == "(" else (-1 if code[i] == ")" else 0)
			i += 1
		if _regex("^\\s*(->\\s*[\\w.\\[\\]]+\\s*)?:\\s*$").search(code.substr(i)) != null:
			var indent := _indent(code)
			while n <= end and (_split_comment(lines[n])[0].strip_edges() == "" or _indent(_split_comment(lines[n])[0]) > indent):
				n += 1
	return false


## The function `name` that class `key` (or a core base) defines, or null.
static func _find_func(idx: Dictionary, key: String, name: String) -> Variant:
	var seen := {}
	var k := key
	while k != "" and idx["classes"].has(k) and not seen.has(k):
		seen[k] = true
		var cls: Dictionary = idx["classes"][k]
		if cls["funcs"].has(name):
			return cls["funcs"][name]
		k = _class_path(idx, cls["path"], cls["base"])
	return null


## "<path>:<line>: <message>" for every coroutine instance method that overrides a plain method of
## a core base class: a caller holding the base type calls it without `await` (the parser even
## refuses an `await` there on a typed receiver), so the override must not suspend.
static func _coroutine_overrides(idx: Dictionary) -> Array[String]:
	var errors: Array[String] = []
	for key: String in idx["classes"]:
		var cls: Dictionary = idx["classes"][key]
		var base := _class_path(idx, cls["path"], cls["base"])
		if base == "":
			continue
		for name: String in cls["funcs"]:
			var f: Dictionary = cls["funcs"][name]
			if f["static"] or not f["coroutine"]:
				continue
			var inherited: Variant = _find_func(idx, base, name)
			if inherited != null and not inherited["coroutine"]:
				errors.append("%s:%d: %s() is a coroutine overriding the plain %s() of %s (%s): its callers do not await it" % [
					cls["path"], f["line"] + 1, name, name, inherited["cls"], inherited["where"]])
	return errors


## "<path>:<line>" of the coroutine a call resolves to, or "".
static func _co_target(ctx: Dictionary, code: String, name_start: int, name: String) -> String:
	var idx: Dictionary = ctx["idx"]
	var recv := _receiver_text(code, name_start)
	var key := ""
	if recv == "" or recv == "self":
		key = ctx["cls"]
	else:
		var rt := _receiver_type(ctx, recv)
		match rt[0]:
			"class", "inst":
				key = rt[1]
			"other":
				return ""
			_:
				return idx["co_instance"].get(name, "")
	var f: Variant = _find_func(idx, key, name)
	return f["where"] if f != null and f["coroutine"] else ""


## "<path>:<line>: <message>" for every violation of the COROUTINE DISCIPLINE rules.
static func lint_unawaited(sources: Dictionary) -> Array[String]:
	var index_errors: Array[String] = []
	var idx := index_sources(sources, index_errors)
	var errors: Array[String] = []
	for e in index_errors:
		if "@coroutine" in e:
			errors.append(e)  # the error lint reports the other index errors
	mark_coroutines(idx, sources)
	errors.append_array(_coroutine_overrides(idx))
	var paths := sources.keys()
	paths.sort()
	for path: String in paths:
		var lines: PackedStringArray = (sources[path] as String).split("\n")
		var codes: Array[String] = []
		for line in lines:
			codes.append(_split_comment(line)[0])
		var file: Dictionary = idx["files"][path]
		var funcs: Array[Dictionary] = file["funcs"]
		var stmts := _statements(codes)
		var locals_cache := {}
		for s: Dictionary in stmts:
			var code: String = s["code"]
			var owner: Variant = _owner(funcs, s["first"])
			var ctx := {"idx": idx, "path": path, "cls": _class_at(file, s["first"])}
			var owner_line: int = owner["line"] if owner != null else -1
			if not locals_cache.has(owner_line):
				locals_cache[owner_line] = _locals(ctx, owner, stmts)
			ctx["locals"] = locals_cache[owner_line]
			for m: RegExMatch in _regex("\\b([A-Za-z_]\\w*)\\s*\\(").search_all(code):
				var name := m.get_string(1)
				if not idx["co_names"].has(name):
					continue
				var name_start := m.get_start(1)
				if _regex("\\bfunc\\s+$").search(code.substr(0, name_start)) != null:
					continue  # its own declaration
				var target := _co_target(ctx, code, name_start, name)
				if target == "":
					continue
				var recv := _receiver_text(code, name_start)
				var callee_start := name_start - (recv.length() + 1 if recv != "" else 0)
				if _regex("\\bawait\\s*$").search(code.substr(0, callee_start)) != null:
					continue
				errors.append("%s:%d: %s() is a coroutine (%s) called without await: %s" % [
					path, s["first"] + 1, name, target, lines[s["first"]].strip_edges()])
	return errors


## {res path: text} of every .gd file under ADDON_DIR, recursively.
static func addon_sources() -> Dictionary:
	var out := {}
	_collect(ADDON_DIR, out)
	return out


static func _collect(dir: String, out: Dictionary) -> void:
	for f: String in DirAccess.get_files_at(dir):
		if f.ends_with(".gd"):
			out[dir.path_join(f)] = FileAccess.get_file_as_string(dir.path_join(f))
	for d: String in DirAccess.get_directories_at(dir):
		_collect(dir.path_join(d), out)


## {"Class[.Inner].name": "<path>:<line>"} of every function marked `## @fallible`.
static func collect_fallible(sources: Dictionary) -> Dictionary:
	var out := {}
	var idx := index_sources(sources, [])
	for key: String in idx["classes"]:
		var cls: Dictionary = idx["classes"][key]
		for name: String in cls["funcs"]:
			var f: Dictionary = cls["funcs"][name]
			if f["fallible"]:
				out["%s.%s" % [key, name]] = f["where"]
	return out


## The collect step: every class of every source with its functions, members and base.
##   {"classes": {key: {"path", "base" (key, engine name or ""), "funcs": {name: f},
##                      "members": {name: type}}},
##    "files": {path: {"key", "classes": [class ranges], "funcs": [f], "inner": {short: key}}},
##    "names": {name: true} of every fallible function (the fast path),
##    "instance": {name: "<path>:<line>"} of every fallible instance (non-static) method}
## A file's class key is its `class_name` (the path when it has none); an inner class is
## `<outer key>.<Name>`. Each f is {name, line, indent, end, fallible, static, cls, ret, where}.
static func index_sources(sources: Dictionary, errors: Array) -> Dictionary:
	var idx := {"classes": {}, "files": {}, "names": {}, "instance": {}}
	var paths := sources.keys()
	paths.sort()
	for path: String in paths:
		var lines: PackedStringArray = (sources[path] as String).split("\n")
		var classes := _classes(path, lines)
		var funcs := _functions(path, sources[path], errors)
		var inner := {}
		for c: Dictionary in classes:
			idx["classes"][c["key"]] = {"path": path, "base": c["base"], "funcs": {}, "members": {}}
			if c["line"] >= 0:
				inner[c["short"]] = c["key"]
		var file := {"key": classes[0]["key"], "classes": classes, "funcs": funcs, "inner": inner}
		idx["files"][path] = file
		for f in funcs:
			f["cls"] = _class_at(file, f["line"])
			f["where"] = "%s:%d" % [path, f["line"] + 1]
			idx["classes"][f["cls"]]["funcs"][f["name"]] = f
			if f["fallible"]:
				idx["names"][f["name"]] = true
				if not f["static"] and not idx["instance"].has(f["name"]):
					idx["instance"][f["name"]] = f["where"]
	# members last: their inferred types may name any class
	for path: String in paths:
		var file: Dictionary = idx["files"][path]
		var codes: Array[String] = []
		for line in (sources[path] as String).split("\n"):
			codes.append(_split_comment(line)[0])
		for c: Dictionary in file["classes"]:
			for n in range(c["line"] + 1, c["end"] + 1):
				var code := codes[n]
				if code.strip_edges() == "" or _indent(code) != c["body_indent"] or _class_at(file, n) != c["key"]:
					continue
				var m := _regex(_MEMBER_RE).search(code)
				if m == null:
					continue
				var ctx := {"idx": idx, "path": path, "cls": c["key"], "locals": {}}
				var typ := m.get_string(2)
				if typ == "" and m.get_string(3) == ":=":
					typ = _expr_type(ctx, m.get_string(4))
				idx["classes"][c["key"]]["members"][m.get_string(1)] = typ
	return idx


## The classes of one file, the file's own first: {key, short, line (-1 for the file), indent,
## body_indent, end, base}.
static func _classes(path: String, lines: PackedStringArray) -> Array[Dictionary]:
	var codes: Array[String] = []
	for line in lines:
		codes.append(_split_comment(line)[0])
	var key := path
	var base := ""
	for code in codes:
		var cn := _regex("^class_name\\s+([A-Za-z_]\\w*)").search(code)
		if cn != null:
			key = cn.get_string(1)
		var ex := _regex("^extends\\s+([A-Za-z_][\\w.]*)").search(code)
		if ex != null:
			base = ex.get_string(1)
	var out: Array[Dictionary] = [{"key": key, "short": "", "line": -1, "indent": -1, "body_indent": 0,
		"end": codes.size() - 1, "base": base}]
	for n in codes.size():
		var m := _regex("^(\\s*)class\\s+([A-Za-z_]\\w*)\\s*(?:extends\\s+([A-Za-z_][\\w.]*))?\\s*:").search(codes[n])
		if m == null:
			continue
		var indent := m.get_string(1).length()
		var c := {"short": m.get_string(2), "line": n, "indent": indent, "body_indent": indent + 1,
			"end": codes.size() - 1, "base": m.get_string(3)}
		var first_body := true
		for k in range(n + 1, codes.size()):
			if codes[k].strip_edges() == "":
				continue
			var ind := _indent(codes[k])
			if ind <= indent:
				c["end"] = k - 1
				break
			if first_body:
				first_body = false
				c["body_indent"] = ind
				var ex := _regex("^\\s*extends\\s+([A-Za-z_][\\w.]*)").search(codes[k])
				if ex != null and c["base"] == "":
					c["base"] = ex.get_string(1)
		# the innermost enclosing class is the last one found so far that contains this line
		var parent: Dictionary = out[0]
		for o: Dictionary in out:
			if o["line"] < n and n <= o["end"] and o["indent"] < indent and o["line"] >= parent["line"]:
				parent = o
		c["key"] = "%s.%s" % [parent["key"], c["short"]]
		out.append(c)
	return out


## The key of the innermost class whose body holds line `n`.
static func _class_at(file: Dictionary, n: int) -> String:
	var best: Dictionary = file["classes"][0]
	for c: Dictionary in file["classes"]:
		if c["line"] < n and n <= c["end"] and c["line"] > best["line"]:
			best = c
	return best["key"]


## "<path>:<line>" of the fallible function `name` that class `key` (or a core base) defines, or
## "" when the class has no such function or it is not fallible.
static func _lookup(idx: Dictionary, key: String, name: String) -> String:
	var seen := {}
	var k := key
	while k != "" and idx["classes"].has(k) and not seen.has(k):
		seen[k] = true
		var cls: Dictionary = idx["classes"][k]
		if cls["funcs"].has(name):
			var f: Dictionary = cls["funcs"][name]
			return f["where"] if f["fallible"] else ""
		k = _class_path(idx, cls["path"], cls["base"])
	return ""


## The core class a dotted name refers to from file `path` (a class_name, an inner class's short
## name in its own file, then `.Inner` steps), or "".
static func _class_path(idx: Dictionary, path: String, chain: String) -> String:
	if chain == "":
		return ""
	var segs := chain.split(".")
	var key := ""
	if idx["classes"].has(segs[0]) and not (segs[0] as String).ends_with(".gd"):
		key = segs[0]
	elif idx["files"].has(path) and idx["files"][path]["inner"].has(segs[0]):
		key = idx["files"][path]["inner"][segs[0]]
	else:
		return ""
	for i in range(1, segs.size()):
		var sub := "%s.%s" % [key, segs[i]]
		if not idx["classes"].has(sub):
			return ""
		key = sub
	return key


const _BUILTIN_TYPES := ["String", "StringName", "int", "float", "bool", "Array", "Dictionary", "Callable",
	"Signal", "NodePath", "RID", "Object", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
	"Color", "Rect2", "Rect2i", "Transform2D", "Transform3D", "Basis", "Quaternion", "Plane", "AABB",
	"Projection", "PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
	"PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedColorArray",
	"PackedVector4Array"]
## String methods that return a String (so `s.substr(1).to_int()` is String.to_int).
const _STRING_TO_STRING := ["substr", "left", "right", "strip_edges", "strip_escapes", "lstrip", "rstrip",
	"trim_prefix", "trim_suffix", "to_lower", "to_upper", "capitalize", "replace", "replacen", "repeat", "insert",
	"get_slice", "get_slicec", "to_snake_case", "to_camel_case", "to_pascal_case", "xml_escape", "xml_unescape",
	"uri_encode", "uri_decode", "c_escape", "c_unescape", "json_escape", "lpad", "rpad", "pad_decimals",
	"pad_zeros", "get_basename", "get_extension", "get_file", "get_base_dir", "path_join", "simplify_path",
	"md5_text", "sha1_text", "sha256_text", "indent", "dedent", "reverse", "format", "join", "num", "chr",
	"num_int64", "num_uint64", "num_scientific"]
## Global functions that return a String.
const _STRING_FUNCS := ["str", "var_to_str", "type_string", "error_string"]


## A Godot class or a built-in type: never a core function.
static func _is_engine(name: String) -> bool:
	return name in _BUILTIN_TYPES or ClassDB.class_exists(name)


## A type name as the lint resolves it: a core class key, "!<name>" for any other known type
## (built-in, engine, a typed Array), or "" when unknown (`Variant`, no annotation).
static func _type_kind(ctx: Dictionary, typ: String) -> String:
	if typ == "" or typ == "Variant":
		return ""
	var key := _class_path(ctx["idx"], ctx["path"], typ)
	if key != "":
		return key
	return "!" + typ.get_slice("[", 0)


## [kind, key-or-name] for the receiver text of a call: ["class", key] for a core class itself
## (a static call), ["inst", key] for an instance of a core class, ["other", name] for an engine
## class, a built-in value or an instance of a non-core type, ["unknown", ""] otherwise.
static func _receiver_type(ctx: Dictionary, recv: String) -> Array:
	var idx: Dictionary = ctx["idx"]
	if recv == "self":
		return ["inst", ctx["cls"]]
	if recv == "super":
		var base := _class_path(idx, ctx["path"], idx["classes"][ctx["cls"]]["base"])
		return ["inst", base] if base != "" else ["other", ""]
	if _regex("^[A-Za-z_]\\w*(?:\\.[A-Za-z_]\\w*)*$").search(recv) != null:
		var segs := recv.split(".")
		var typ := ""
		var start := 1
		var locals: Dictionary = ctx.get("locals", {})
		if locals.has(segs[0]):
			typ = _type_kind(ctx, locals[segs[0]])
		elif _member_type(ctx, ctx["cls"], segs[0]) != null:
			typ = _type_kind(ctx, _member_type(ctx, ctx["cls"], segs[0]))
		else:
			# the longest prefix that is a core class; what follows are its members
			start = segs.size()
			typ = ""
			while start > 0:
				typ = _class_path(idx, ctx["path"], ".".join(segs.slice(0, start)))
				if typ != "":
					break
				start -= 1
			if typ == "":
				return ["other", segs[0]] if _is_engine(segs[0]) else ["unknown", ""]
			if start == segs.size():
				return ["class", typ]
		for i in range(start, segs.size()):
			if typ == "" or typ.begins_with("!"):
				return ["unknown", ""]
			var mt: Variant = _member_type(ctx, typ, segs[i])
			if mt == null:
				return ["unknown", ""]
			typ = _type_kind(ctx, mt)
		if typ == "":
			return ["unknown", ""]
		return ["other", typ.substr(1)] if typ.begins_with("!") else ["inst", typ]
	var t := _type_kind(ctx, _expr_type(ctx, recv))
	if t == "":
		return ["unknown", ""]
	return ["other", t.substr(1)] if t.begins_with("!") else ["inst", t]


## The declared type of member `name` of class `key` or a core base (null when it has none).
static func _member_type(ctx: Dictionary, key: String, name: String) -> Variant:
	var idx: Dictionary = ctx["idx"]
	var seen := {}
	var k := key
	while k != "" and idx["classes"].has(k) and not seen.has(k):
		seen[k] = true
		var cls: Dictionary = idx["classes"][k]
		if cls["members"].has(name):
			return cls["members"][name]
		k = _class_path(idx, cls["path"], cls["base"])
	return null


## The declared return type of function `name` of class `key` (or a base), or "".
static func _return_type(idx: Dictionary, key: String, name: String) -> String:
	var seen := {}
	var k := key
	while k != "" and idx["classes"].has(k) and not seen.has(k):
		seen[k] = true
		var cls: Dictionary = idx["classes"][k]
		if cls["funcs"].has(name):
			return cls["funcs"][name]["ret"]
		k = _class_path(idx, cls["path"], cls["base"])
	return ""


## The type of an expression, where it is cheap to know: a literal, `T.new()`, `x as T`, a
## typed local, parameter or member, `str(..)`, a call of a core function with a declared
## return type, or a String method returning a String. "" when unknown.
static func _expr_type(ctx: Dictionary, expr: String, depth: int = 0) -> String:
	var e := expr.strip_edges()
	if e == "" or depth > 8:
		return ""
	if (e.begins_with("\"") and e.ends_with("\"") or e.begins_with("'") and e.ends_with("'")) and e.length() >= 2:
		return "String"
	if _regex("^-?\\d+$").search(e) != null:
		return "int"
	if _regex("^-?\\d+\\.\\d*$").search(e) != null:
		return "float"
	var cast := _regex("^(.*\\S)\\s+as\\s+([A-Za-z_][\\w.]*)$").search(e)
	if cast != null and _depth(cast.get_string(1)) == 0:
		return cast.get_string(2)
	if (e.begins_with("[") or e.begins_with("{")) and _close_paren(e, 0) == e.length() - 1:
		return "Array" if e.begins_with("[") else "Dictionary"
	if e.ends_with(")"):
		var open := _open_paren(e, e.length() - 1)
		if open < 0:
			return ""
		var head := e.substr(0, open).strip_edges(false, true)
		if head == "":
			return _expr_type(ctx, e.substr(1, e.length() - 2), depth + 1)
		var cm := _regex("([A-Za-z_]\\w*)$").search(head)
		if cm == null:
			return ""
		var callee := cm.get_string(1)
		var recv := head.substr(0, cm.get_start(1))
		if recv == "":
			if callee in _STRING_FUNCS:
				return "String"
			return _return_type(ctx["idx"], ctx["cls"], callee)
		if not recv.ends_with("."):
			return ""
		recv = recv.substr(0, recv.length() - 1)
		if callee == "new":
			var key := _class_path(ctx["idx"], ctx["path"], recv)
			return key if key != "" else (recv if _is_engine(recv) else "")
		var rt := _receiver_type(ctx, recv)
		match rt[0]:
			"class", "inst":
				return _return_type(ctx["idx"], rt[1], callee)
			"other":
				if (rt[1] == "String" or rt[1] == "StringName") and callee in _STRING_TO_STRING:
					return "String"
		return ""
	if _regex("^[A-Za-z_]\\w*(?:\\.[A-Za-z_]\\w*)*$").search(e) != null:
		var rt := _receiver_type(ctx, e)
		if rt[0] == "inst":
			return rt[1]
		if rt[0] == "other":
			return rt[1]
	return ""


## Index of the `(` (or `[`, `{`) matching the closer at `close`, or -1.
static func _open_paren(code: String, close: int) -> int:
	var d := 0
	for i in range(close, -1, -1):
		var ch := code[i]
		if ch == ")" or ch == "]" or ch == "}":
			d += 1
		elif ch == "(" or ch == "[" or ch == "{":
			d -= 1
			if d == 0:
				return i
	return -1


## The receiver text of the call whose name starts at `name_start` ("" for a bare call): the
## postfix expression before the `.` (identifiers, calls, indexing, a string literal).
static func _receiver_text(code: String, name_start: int) -> String:
	if name_start == 0 or code[name_start - 1] != ".":
		return ""
	var end := name_start - 1
	var p := end
	while p > 0:
		var ch := code[p - 1]
		if ch == ")" or ch == "]":
			var o := _open_paren(code, p - 1)
			if o < 0:
				break
			p = o
			continue
		if _is_word(ch):
			while p > 0 and _is_word(code[p - 1]):
				p -= 1
			if p > 0 and code[p - 1] == ".":
				p -= 1
				continue
			break
		if ch == "\"" or ch == "'":
			var q := code.rfind(ch, p - 2)
			if q < 0:
				break
			p = q
			if p > 0 and code[p - 1] == "&":
				p -= 1
			break
		break
	return code.substr(p, end - p)


## "<path>:<line>" of the fallible function a call resolves to, or "" (see the header's
## Resolution rules).
static func _call_target(ctx: Dictionary, code: String, name_start: int, name: String) -> String:
	var idx: Dictionary = ctx["idx"]
	var recv := _receiver_text(code, name_start)
	if recv == "" or recv == "self":
		return _lookup(idx, ctx["cls"], name)
	var rt := _receiver_type(ctx, recv)
	match rt[0]:
		"class", "inst":
			return _lookup(idx, rt[1], name)
		"other":
			return ""
	return idx["instance"].get(name, "")


## {name: type} of the parameters and locals of function `f` ("" for an untyped one; a name
## declared with two different types is unknown).
static func _locals(ctx: Dictionary, f: Variant, stmts: Array[Dictionary]) -> Dictionary:
	var out := {}
	if f == null:
		return out
	var add := func(name: String, typ: String) -> void:
		if out.has(name) and out[name] != typ:
			out[name] = ""
		else:
			out[name] = typ
	var lctx := ctx.duplicate()
	lctx["locals"] = out
	for s: Dictionary in stmts:
		if s["first"] < f["line"] or s["first"] > f["end"]:
			continue
		var code: String = s["code"]
		if s["first"] == f["line"]:
			var open := code.find("(", code.find("func"))
			var close := _close_paren(code, open)
			if open < 0 or close < 0:
				continue
			for p in _split_top(code.substr(open + 1, close - open - 1)):
				var pm := _regex("^\\s*([A-Za-z_]\\w*)\\s*(?::(?!=)\\s*([A-Za-z_][\\w.]*(?:\\[[\\w.]+\\])?)?\\s*)?(?:(:?=)\\s*(.*))?$").search(p)
				if pm == null:
					continue
				var typ := pm.get_string(2)
				if typ == "" and pm.get_string(3) == ":=":
					typ = _expr_type(lctx, pm.get_string(4))
				add.call(pm.get_string(1), typ)
			continue
		var vm := _regex(_LOCAL_RE).search(code)
		if vm != null:
			var typ := vm.get_string(2)
			if typ == "" and vm.get_string(3) == ":=":
				typ = _expr_type(lctx, vm.get_string(4))
			add.call(vm.get_string(1), typ)
		var fm := _regex("^\\s*for\\s+([A-Za-z_]\\w*)\\s*(?::\\s*([A-Za-z_][\\w.]*(?:\\[[\\w.]+\\])?))?\\s+in\\b").search(code)
		if fm != null:
			add.call(fm.get_string(1), fm.get_string(2))
	return out


## `a, b(c, d), e` split at the commas outside brackets.
static func _split_top(s: String) -> PackedStringArray:
	var out := PackedStringArray()
	var d := 0
	var cur := ""
	for ch in s:
		if ch == "(" or ch == "[" or ch == "{":
			d += 1
		elif ch == ")" or ch == "]" or ch == "}":
			d -= 1
		if ch == "," and d == 0:
			out.append(cur)
			cur = ""
		else:
			cur += ch
	if cur.strip_edges() != "":
		out.append(cur)
	return out


## "<path>:<line>: <message>" for every violation of the rules in this file's header.
static func lint_err_propagation(sources: Dictionary) -> Array[String]:
	var errors: Array[String] = []
	var idx := index_sources(sources, errors)
	var paths := sources.keys()
	paths.sort()
	for path: String in paths:
		var lines: PackedStringArray = (sources[path] as String).split("\n")
		var codes: Array[String] = []
		var comments: Array[String] = []
		for line in lines:
			var parts := _split_comment(line)
			codes.append(parts[0])
			comments.append(parts[1])
		var file: Dictionary = idx["files"][path]
		var funcs: Array[Dictionary] = file["funcs"]
		var stmts := _statements(codes)
		var flagged := {}  # functions already reported as returning an ScfgErr unmarked
		var locals_cache := {}
		for si in stmts.size():
			var s: Dictionary = stmts[si]
			var code: String = s["code"]
			var owner: Variant = _owner(funcs, s["first"])
			var where := "%s:%d" % [path, s["first"] + 1]
			var ctx := {"idx": idx, "path": path, "cls": _class_at(file, s["first"])}
			var owner_line: int = owner["line"] if owner != null else -1
			if not locals_cache.has(owner_line):
				locals_cache[owner_line] = _locals(ctx, owner, stmts)
			ctx["locals"] = locals_cache[owner_line]
			var waived := false
			for n in range(s["first"], s["last"] + 1):
				if _regex("#\\s*err-ok:\\s*\\S").search(comments[n]) != null:
					waived = true
			var calls := 0  # fallible calls in this statement
			var returns_err := _returns_err(stmts, si, codes, owner)
			for m: RegExMatch in _regex("\\b([A-Za-z_]\\w*)\\s*\\(").search_all(code):
				var name := m.get_string(1)
				if not idx["names"].has(name):
					continue
				var before := code.substr(0, _callee_start(code, m.get_start(1)))
				if _regex("\\bfunc\\s+$").search(before) != null:
					continue  # its own declaration
				var target := _call_target(ctx, code, m.get_start(1), name)
				if target == "":
					continue
				calls += 1
				var close := _close_paren(code, m.get_end(0) - 1)
				var tail := code.substr(close + 1).strip_edges() if close >= 0 else "?"
				var ends := tail == "" or tail.begins_with(";")
				var assign := _regex(_ASSIGN_RE + "$").search(before)
				var direct_return := _regex(_RETURN_RE + "$").search(before) != null and tail == ""
				if direct_return:
					returns_err = true
					continue
				if waived:
					continue
				if assign != null and ends:
					var x := assign.get_string(1)
					var typ := assign.get_string(2)
					if typ != "" and typ != "Variant":
						errors.append("%s: `%s` is typed %s and cannot hold the ScfgErr of %s()" % [
							where, x, typ, name])
						continue
					var check := _regex("\\b(?:if|elif)\\b.*(?:\\b%s\\s+is\\s+ScfgErr\\b|ScfgErr\\.is_err\\(\\s*%s\\s*\\))" % [x, x])
					var found := check.search(tail) != null
					var seen := 0
					var n: int = s["last"] + 1
					while not found and seen < 2 and n < codes.size():
						if codes[n].strip_edges() != "":
							seen += 1
							found = check.search(codes[n]) != null
						n += 1
					if found:
						continue
				errors.append("%s: unchecked call to %s() (fallible, %s): %s" % [
					where, name, target, lines[s["first"]].strip_edges()])
			if waived and calls == 0:
				errors.append("%s: the `# err-ok:` waiver covers no fallible call (remove it): %s" % [
					where, lines[s["first"]].strip_edges()])
			if returns_err and owner != null and not owner["fallible"]:
				if not flagged.has(owner["line"]):
					flagged[owner["line"]] = true
					errors.append("%s: %s() returns an ScfgErr but is not marked ## @fallible" % [where, owner["name"]])
			elif returns_err and owner == null:
				errors.append("%s: returns an ScfgErr outside a function" % where)
	return errors


## Whether statement `si` makes its function able to return an ScfgErr, by form (a direct return
## of a fallible call is recognised in the call loop instead):
##   - it contains `ScfgErr.make(` anywhere (a variable that is returned later, an argument, a
##     ternary branch: the function builds an error, so its callers must check it);
##   - `return ScfgErr.<anything>` other than `ScfgErr.is_err(...)`;
##   - `return X` where `X is ScfgErr` (or `ScfgErr.is_err(X)`) is tested positively in the same
##     statement, or in the header of an enclosing block (`if X is ScfgErr:` then, deeper,
##     `return X`), at any distance;
##   - a ternary `return` that tests anything against ScfgErr (`return v if v is ScfgErr else 1`).
static func _returns_err(stmts: Array[Dictionary], si: int, codes: Array[String], owner: Variant) -> bool:
	var code: String = stmts[si]["code"]
	if _regex("\\bScfgErr\\s*\\.\\s*make\\s*\\(").search(code) != null:
		return true
	if _regex("^\\s*return\\s+ScfgErr\\.(?!is_err\\b)").search(code) != null:
		return true
	if _regex("^\\s*return\\b").search(code) != null and _regex("\\bif\\b.*\\belse\\b").search(code) != null and (
			_regex("\\bis\\s+(?:not\\s+)?ScfgErr\\b").search(code) != null
			or _regex("ScfgErr\\.is_err\\(").search(code) != null):
		return true
	var ret_var := _regex("\\breturn\\s+([A-Za-z_]\\w*)\\s*$").search(code)
	if ret_var == null:
		return false
	var x := ret_var.get_string(1)
	if _tests_err(code, x):  # `if X is ScfgErr: return X`
		return true
	# the headers of the blocks around the return, innermost first, up to the function's `func`
	var indent := _indent(codes[stmts[si]["first"]])
	for j in range(si - 1, -1, -1):
		var first: int = stmts[j]["first"]
		if owner != null and first < owner["line"]:
			break
		var ind := _indent(codes[first])
		if ind < indent:
			indent = ind
			if _tests_err(stmts[j]["code"], x):
				return true
	return false


## `X is ScfgErr` or `ScfgErr.is_err(X)` in `code`, not negated by a `not` before it.
static func _tests_err(code: String, x: String) -> bool:
	var test := _regex("(not\\s+)?(?:\\b%s\\s+is\\s+ScfgErr\\b|ScfgErr\\.is_err\\(\\s*%s\\s*\\))" % [x, x])
	for m: RegExMatch in test.search_all(code):
		if m.get_string(1) == "":
			return true
	return false


static func _indent(code: String) -> int:
	return code.length() - code.lstrip(" \t").length()


## The functions of one file: {name, line, indent, end, fallible}. Reports stray tags and
## fallible functions whose return type cannot hold an ScfgErr into `errors`.
static func _functions(path: String, text: String, errors: Array) -> Array[Dictionary]:
	var lines := text.split("\n")
	var out: Array[Dictionary] = []
	var func_re := _regex(_FUNC_RE)
	var tagged := -1  # line of a @fallible tag in the doc block being read, or -1
	var co_tagged := -1  # line of a @coroutine tag in the doc block being read, or -1
	for n in lines.size():
		var line := lines[n]
		var stripped := line.strip_edges()
		if stripped.begins_with("##"):
			if _regex("^##\\s*@fallible\\b").search(stripped) != null:
				tagged = n
			if _regex("^##\\s*@coroutine\\b").search(stripped) != null:
				co_tagged = n
			continue
		var m := func_re.search(line)
		if m != null:
			var rt := _regex("\\)\\s*->\\s*([A-Za-z_][\\w.\\[\\]]*)\\s*:").search(_signature(lines, n))
			var f := {"name": m.get_string(2), "line": n, "indent": m.get_string(1).length(),
				"end": lines.size() - 1, "fallible": tagged >= 0, "coroutine_tag": co_tagged >= 0,
				"static": _regex("^\\s*static\\s+func\\b").search(line) != null,
				"ret": rt.get_string(1) if rt != null else ""}
			if tagged >= 0:
				if rt != null and rt.get_string(1) != "Variant":
					errors.append("%s:%d: %s() is @fallible but its return type %s cannot hold an ScfgErr" % [
						path, n + 1, f["name"], rt.get_string(1)])
			out.append(f)
		else:
			if tagged >= 0:
				errors.append("%s:%d: stray @fallible (its doc block is not directly above a func)" % [path, tagged + 1])
			if co_tagged >= 0:
				errors.append("%s:%d: stray @coroutine (its doc block is not directly above a func)" % [
					path, co_tagged + 1])
		tagged = -1
		co_tagged = -1
	# a function ends before the next code line indented no deeper than its `func`
	for i in out.size():
		var f := out[i]
		for n in range(f["line"] + 1, lines.size()):
			var code: String = _split_comment(lines[n])[0]
			if code.strip_edges() == "":
				continue
			var indent := code.length() - code.lstrip(" \t").length()
			if indent <= f["indent"]:
				f["end"] = n - 1
				break
	return out


## The code of the `func` statement starting at line `n` (joined while brackets are open).
static func _signature(lines: PackedStringArray, n: int) -> String:
	var code: String = _split_comment(lines[n])[0]
	var depth := _depth(code)
	var k := n
	while depth > 0 and k + 1 < lines.size():
		k += 1
		var more: String = _split_comment(lines[k])[0]
		code += " " + more.strip_edges()
		depth += _depth(more)
	return code


## Start of the callee `a.b.name(` whose name starts at `name_start`: the name plus a receiver
## made only of dotted identifiers. After `).` or `].` the receiver is an expression, and the
## callee is the name alone (so the call can never be the whole right-hand side).
static func _callee_start(code: String, name_start: int) -> int:
	var start := name_start
	while start >= 2 and code[start - 1] == ".":
		var k := start - 1
		while k > 0 and _is_word(code[k - 1]):
			k -= 1
		if k == start - 1:
			break
		start = k
	return start


static func _is_word(ch: String) -> bool:
	return ch == "_" or (ch >= "0" and ch <= "9") or (ch >= "a" and ch <= "z") or (ch >= "A" and ch <= "Z")


static func _owner(funcs: Array[Dictionary], line: int) -> Variant:
	var best: Variant = null
	for f in funcs:
		if f["line"] <= line and line <= f["end"]:
			if best == null or f["line"] > best["line"]:
				best = f
	return best


## Statements: physical lines joined while (), [] or {} are open or a line ends with `\`.
## Each is {code, first, last} (code joined with spaces; line numbers 0-based).
static func _statements(codes: Array[String]) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	var n := 0
	while n < codes.size():
		var first := n
		var code := codes[n]
		var depth := _depth(code)
		while (depth > 0 or code.strip_edges(false, true).ends_with("\\")) and n + 1 < codes.size():
			n += 1
			code = code.strip_edges(false, true).trim_suffix("\\") + " " + codes[n].strip_edges()
			depth += _depth(codes[n])
		if code.strip_edges() != "":
			out.append({"code": code, "first": first, "last": n})
		n += 1
	return out


static func _depth(code: String) -> int:
	var d := 0
	for ch in code:
		if ch == "(" or ch == "[" or ch == "{":
			d += 1
		elif ch == ")" or ch == "]" or ch == "}":
			d -= 1
	return d


## Index of the `)` matching the `(` at `open`, or -1.
static func _close_paren(code: String, open: int) -> int:
	var d := 0
	for i in range(open, code.length()):
		var ch := code[i]
		if ch == "(" or ch == "[" or ch == "{":
			d += 1
		elif ch == ")" or ch == "]" or ch == "}":
			d -= 1
			if d == 0:
				return i
	return -1


## [code, comment]: string literal contents blanked to spaces (so `"f("` is not a call), and the
## comment from the first `#` outside a string (`""` when there is none).
static func _split_comment(line: String) -> Array:
	var code := ""
	var quote := ""
	var i := 0
	while i < line.length():
		var ch := line[i]
		if quote != "":
			if ch == "\\":
				code += "  "
				i += 2
				continue
			if ch == quote:
				quote = ""
				code += ch
			else:
				code += " "
		elif ch == "#":
			return [code, line.substr(i)]
		else:
			if ch == "\"" or ch == "'":
				quote = ch
			code += ch
		i += 1
	return [code, ""]


static var _regex_cache := {}


static func _regex(pattern: String) -> RegEx:
	if not _regex_cache.has(pattern):
		var r := RegEx.new()
		if r.compile(pattern) != OK:
			push_error("test_lints: bad pattern %s" % pattern)
		_regex_cache[pattern] = r
	return _regex_cache[pattern]
