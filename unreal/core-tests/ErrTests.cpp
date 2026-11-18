// Copyright 2026 Repulse
// No Python twin: tests of scfg::Err (Python's exception classes as values), Result and SCFG_TRY.
// SCFG_CO_TRY and SCFG_CO_TRY_VOID are tested in CoTests.cpp, with the coroutine type they return through.
#include "support/Test.h"

#include "Scfg/Err.h"
#include "Scfg/Result.h"

#include <memory>
#include <type_traits>
#include <vector>

using scfg::Err;
using scfg::Result;
using scfg::Status;

SCFG_TEST(ErrTests, ConfigErrorIsAValueError) {
    Err e = Err::config_error("chains.empty", "no chain configured");
    SCFG_CHECK_EQ(t, e.kind, "ConfigError");
    SCFG_CHECK_EQ(t, e.code, "chains.empty");
    SCFG_CHECK_EQ(t, e.message, "no chain configured");
    SCFG_CHECK(t, e.is_a("ConfigError"));
    SCFG_CHECK(t, e.is_a("ScfgError"));
    SCFG_CHECK(t, e.is_a("ValueError"));
    SCFG_CHECK(t, e.is_a("Exception"));
    SCFG_CHECK(t, e.is_a("BaseException"));
    SCFG_CHECK(t, !e.is_a("TypeError"));
    SCFG_CHECK(t, !e.is_a("RestoreError"));
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

SCFG_TEST(ErrTests, RestoreErrorIsNotAValueError) {
    Err e = Err::restore_error("tx_not_found", "tx_not_found");
    SCFG_CHECK_EQ(t, e.kind, "RestoreError");
    SCFG_CHECK_EQ(t, e.code, "tx_not_found");
    SCFG_CHECK(t, e.is_a("Exception"));
    SCFG_CHECK(t, !e.is_a("ValueError"));
    SCFG_CHECK(t, !e.is_a("ScfgError"));
}

SCFG_TEST(ErrTests, CancelledErrorIsNotAnException) {
    Err e = Err::make("CancelledError", "");
    SCFG_CHECK(t, e.is_a("CancelledError"));
    SCFG_CHECK(t, e.is_a("BaseException"));
    SCFG_CHECK(t, !e.is_a("Exception"));
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

SCFG_TEST(ErrTests, BuiltinFactoriesAndTheirBases) {
    SCFG_CHECK(t, Err::value_error("v").is_a("ValueError"));
    SCFG_CHECK(t, Err::type_error("t").is_a("TypeError"));
    Err k = Err::key_error("'chain'");
    SCFG_CHECK_EQ(t, k.kind, "KeyError");
    SCFG_CHECK_EQ(t, k.message, "'chain'");
    SCFG_CHECK(t, k.is_a("LookupError"));
    SCFG_CHECK(t, Err::index_error("i").is_a("LookupError"));
    SCFG_CHECK(t, !Err::index_error("i").is_a("KeyError"));
    SCFG_CHECK(t, Err::overflow_error("o").is_a("ArithmeticError"));
    SCFG_CHECK(t, !Err::overflow_error("o").is_a("ValueError"));
    SCFG_CHECK(t, Err::make("ZeroDivisionError", "z").is_a("ArithmeticError"));
    SCFG_CHECK(t, Err::make("Overflow", "d").is_a("Inexact"));
    SCFG_CHECK(t, Err::make("Overflow", "d").is_a("Rounded"));
    SCFG_CHECK(t, Err::make("InvalidOperation", "d").is_a("ArithmeticError"));
    SCFG_CHECK(t, !Err::make("InvalidOperation", "d").is_a("ValueError"));
    SCFG_CHECK(t, Err::make("DataTooLongError", "q").is_a("ValueError"));
    SCFG_CHECK(t, Err::make("RuntimeError", "r").is_a("Exception"));
    SCFG_CHECK(t, Err::make("RpcUnavailable", "u").is_a("Exception"));
    SCFG_CHECK(t, !Err::make("RpcUnavailable", "u").is_a("RpcError"));

    Err r = Err::rpc_error(-32000, "header not found");
    SCFG_CHECK_EQ(t, r.kind, "RpcError");
    SCFG_CHECK(t, r.rpc_code.has_value() && *r.rpc_code == -32000);
    SCFG_CHECK_EQ(t, r.message, "header not found");
    SCFG_CHECK(t, !r.is_a("TransportError"));

    Err tr = Err::transport_error("HTTP 503", 503);
    SCFG_CHECK_EQ(t, tr.kind, "TransportError");
    SCFG_CHECK(t, tr.status.has_value() && *tr.status == 503);
    SCFG_CHECK(t, !Err::transport_error("timed out", std::nullopt).status.has_value());
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

SCFG_TEST(ErrTests, StrNamesKindCodeAndStatus) {
    SCFG_CHECK_EQ(t, Err::value_error("bad").str(), "ValueError: bad");
    SCFG_CHECK_EQ(t, Err::config_error("chains.empty", "no chain").str(), "ConfigError(chains.empty): no chain");
    SCFG_CHECK_EQ(t, Err::rpc_error(-32000, "nope").str(), "RpcError(-32000): nope");
    SCFG_CHECK_EQ(t, Err::transport_error("down", 503).str(), "TransportError(status=503): down");
}

// Ruling R5: an unknown kind is a programming error. It is recorded (never silently an "Exception"), and an Err of
// an unknown kind, or an is_a() question about one, is false.
SCFG_TEST(ErrTests, AnUnknownKindIsRecordedNeverAnException) {
    scfg::errors::clear_misuse();
    Err e = Err::make("ValueEror", "typo");
    SCFG_CHECK_EQ(t, scfg::errors::misuse().size(), 1);
    SCFG_CHECK(t, !e.is_a("Exception"));
    SCFG_CHECK(t, !e.is_a("BaseException"));
    SCFG_CHECK(t, !e.is_a("ValueError"));
    SCFG_CHECK(t, !Err::value_error("v").is_a("ValueEror"));
    SCFG_CHECK(t, scfg::errors::misuse().size() >= 2);
    if (!scfg::errors::misuse().empty()) {
        SCFG_CHECK(t, scfg::errors::misuse()[0].find("ValueEror") != std::string::npos);
    }
    scfg::errors::clear_misuse();
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

static Result<int> parse_digit(char c) {
    if (c < '0' || c > '9') return Err::value_error(std::string("not a digit: ") + c);
    return c - '0';
}

static Result<int> two_digits(char a, char b, int& reached) {
    SCFG_TRY(int hi, parse_digit(a));
    reached = 1;
    int lo = 0;
    SCFG_TRY(lo, parse_digit(b));  // assigns an existing variable
    reached = 2;
    return hi * 10 + lo;
}

static Status check_digit(char c, int& reached) {
    SCFG_TRY_VOID(parse_digit(c));  // a Result<T>'s value may be dropped
    reached = 1;
    SCFG_TRY_VOID(scfg::ok());
    reached = 2;
    return scfg::ok();
}

SCFG_TEST(ErrTests, TryPropagatesAndAssigns) {
    int reached = 0;
    Result<int> r = two_digits('4', '2', reached);
    SCFG_CHECK(t, r.ok());
    SCFG_CHECK(t, static_cast<bool>(r));
    if (r) SCFG_CHECK_EQ(t, r.value(), 42);
    SCFG_CHECK_EQ(t, reached, 2);

    reached = 0;
    r = two_digits('x', '2', reached);
    SCFG_CHECK(t, !r.ok());
    SCFG_CHECK_EQ(t, reached, 0);
    if (!r) {
        SCFG_CHECK_EQ(t, r.err().kind, "ValueError");
        SCFG_CHECK_EQ(t, r.err().message, "not a digit: x");
    }

    reached = 0;
    r = two_digits('4', 'y', reached);
    SCFG_CHECK(t, !r.ok());
    SCFG_CHECK_EQ(t, reached, 1);

    reached = 0;
    Status s = check_digit('7', reached);
    SCFG_CHECK(t, s.ok());
    SCFG_CHECK_EQ(t, reached, 2);
    reached = 0;
    s = check_digit('?', reached);
    SCFG_CHECK(t, !s.ok());
    SCFG_CHECK_EQ(t, reached, 0);
    if (!s) SCFG_CHECK_EQ(t, s.err().kind, "ValueError");
}

// Result converts implicitly only where T does (as std::expected): `return n;` is no Result<std::vector<uint8_t>>.
static_assert(std::is_convertible_v<const char*, Result<std::string>>);
static_assert(std::is_convertible_v<int, Result<int64_t>>);
static_assert(!std::is_convertible_v<size_t, Result<std::vector<uint8_t>>>);
static_assert(std::is_constructible_v<Result<std::vector<uint8_t>>, size_t>);

static Result<std::unique_ptr<int>> boxed(int v) { return std::make_unique<int>(v); }

static Result<int> unbox(int v) {
    SCFG_TRY(std::unique_ptr<int> p, boxed(v));  // a move-only value
    return *p + 1;
}

SCFG_TEST(ErrTests, ResultHoldsMoveOnlyValuesAndConverts) {
    Result<int> r = unbox(4);
    SCFG_CHECK(t, r.ok() && r.value() == 5);

    Result<std::string> s = "text";  // converting construction, as a return statement does
    SCFG_CHECK(t, s.ok());
    if (s) {
        std::string taken = s.take();
        SCFG_CHECK_EQ(t, taken, "text");
    }
    Result<std::string> e = Err::type_error("no");
    SCFG_CHECK(t, !e.ok());
    if (!e) {
        Err moved = std::move(e).err();
        SCFG_CHECK_EQ(t, moved.kind, "TypeError");
    }
    Result<std::string> v = std::string("rvalue");
    SCFG_CHECK_EQ(t, std::move(v).value(), "rvalue");
}
