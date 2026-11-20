// Copyright 2026 Repulse
// No Python twin: a value or the Python exception (scfg::Err) that took its place, and the macros that propagate it.
//
//   Result<Json> loads(std::string_view s);          // Python: def loads(s) -> Any, may raise
//   SCFG_TRY(Json doc, loads(text));                   // doc = loads(text), or return its Err to our caller
//   SCFG_TRY_VOID(check(doc));                         // check(doc), or return its Err
//   SCFG_CO_TRY(Json r, co_await rpc.call(...));       // the same inside a coroutine (co_return)
//
// Reading the alternative a Result does not hold (value() of an error, err() of a success) is a programming error:
// it prints what happened to stderr and aborts (detail::bad_result_access in Err.cpp), as std::get does without
// exceptions. Check ok() first.
#pragma once

#include "Scfg/Err.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace scfg {

template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Err>, "Result<Err> would be ambiguous");
    static_assert(!std::is_reference_v<T>, "Result holds values");

public:
    // From anything a T is made of (so `return "text";` works for a Result<std::string>), and from an Err. As
    // std::expected's, it is explicit when T's own constructor is (`return n;` is no Result<std::vector<T>>).
    template <class U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Err> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Result>)
    explicit(!std::is_convertible_v<U&&, T>) Result(U&& v) : v_(std::in_place_index<0>, std::forward<U>(v)) {}
    Result(Err e) : v_(std::in_place_index<1>, std::move(e)) {}

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { return *get_value("Result::value() of an error"); }
    const T& value() const& { return *get_value("Result::value() of an error"); }
    T value() && { return std::move(*get_value("Result::value() of an error")); }
    // Moves the value out (the Result keeps a moved-from T).
    T take() { return std::move(*get_value("Result::take() of an error")); }

    const Err& err() const& { return *get_err(); }
    Err err() && { return std::move(*get_err()); }

private:
    T* get_value(const char* what) {
        if (T* p = std::get_if<0>(&v_)) return p;
        detail::bad_result_access(what, std::get_if<1>(&v_));
    }
    const T* get_value(const char* what) const { return const_cast<Result*>(this)->get_value(what); }
    Err* get_err() {
        if (Err* p = std::get_if<1>(&v_)) return p;
        detail::bad_result_access("Result::err() of a success", nullptr);
    }
    const Err* get_err() const { return const_cast<Result*>(this)->get_err(); }

    std::variant<T, Err> v_;
};

// Success or an Err: a Python function that returns None but may raise.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Err e) : e_(std::move(e)) {}

    bool ok() const { return !e_.has_value(); }
    explicit operator bool() const { return ok(); }

    const Err& err() const& { return *get_err(); }
    Err err() && { return std::move(*get_err()); }

private:
    const Err* get_err() const {
        if (e_) return &*e_;
        detail::bad_result_access("Result::err() of a success", nullptr);
    }
    Err* get_err() { return const_cast<Err*>(static_cast<const Result*>(this)->get_err()); }

    std::optional<Err> e_;
};

using Status = Result<void>;

// The success Status (Python's `return None`).
inline Status ok() { return Status(); }

}  // namespace scfg

#define SCFG_DETAIL_CAT2(a, b) a##b
#define SCFG_DETAIL_CAT(a, b) SCFG_DETAIL_CAT2(a, b)

#define SCFG_DETAIL_TRY(tmp, decl, expr, ret) \
    auto tmp = (expr);                        \
    if (!tmp.ok()) ret std::move(tmp).err();  \
    decl = std::move(tmp).take()

#define SCFG_DETAIL_TRY_VOID(tmp, expr, ret)                     \
    do {                                                         \
        auto tmp = (expr);                                       \
        if (!tmp.ok()) ret std::move(tmp).err();                 \
    } while (0)

// `decl = <value of expr>`, or return expr's Err from the enclosing function (which returns a Result or Status).
// decl declares a variable (`auto x`, `Json doc`) or names an existing one. A statement, not an expression.
#define SCFG_TRY(decl, expr) SCFG_DETAIL_TRY(SCFG_DETAIL_CAT(scfg_try_, __COUNTER__), decl, expr, return)
// Evaluates expr (a Status, or a Result whose value is dropped) and returns its Err if it failed.
#define SCFG_TRY_VOID(expr) SCFG_DETAIL_TRY_VOID(SCFG_DETAIL_CAT(scfg_try_, __COUNTER__), expr, return)
// The same inside a coroutine (Co<T>): the Err leaves through co_return. Tested in CoTests.cpp.
#define SCFG_CO_TRY(decl, expr) SCFG_DETAIL_TRY(SCFG_DETAIL_CAT(scfg_try_, __COUNTER__), decl, expr, co_return)
#define SCFG_CO_TRY_VOID(expr) SCFG_DETAIL_TRY_VOID(SCFG_DETAIL_CAT(scfg_try_, __COUNTER__), expr, co_return)
