// No Python twin: the in-house test runner (no third-party framework, no exceptions).
//
//   SCFG_TEST(Group, Name) { SCFG_CHECK_EQ(t, a, b); }   registers Group.Name before main runs
//   SCFG_TEST_SOURCE(Id) { scfg_test::add(g, n, body); }  runs at the start of the run, before selection, so it
//                                                         can register one test per vector at run time
//   scfg_core_tests [--list | --group G | --test G.N]     prints "<n> passed, <m> failed"; exits non-zero on a
//                                                         failure or when the selection ran no test
#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace scfg_test {

class Ctx {
public:
    // Records msg as a failure when ok is false; returns ok so a test can stop early.
    bool check(bool ok, std::string_view msg);
    void fail(std::string_view msg);
    bool failed() const { return !messages_.empty(); }
    const std::vector<std::string>& messages() const { return messages_; }

private:
    std::vector<std::string> messages_;
};

using Body = std::function<void(Ctx&)>;
using Sink = std::function<void(std::string_view)>;

class Registry {
public:
    void add(std::string group, std::string name, Body body);
    // fn runs once, at the start of the next run(), and may add() tests.
    void on_start(std::function<void(Registry&)> fn);
    // The distinct groups registered so far (sorted).
    std::vector<std::string> groups() const;
    // args are the command-line arguments without argv[0]. Returns the process exit code.
    int run(const std::vector<std::string>& args, const Sink& out);

private:
    struct Test {
        std::string group, name;
        Body body;
    };
    std::vector<Test> tests_;
    std::vector<std::function<void(Registry&)>> on_start_;
};

// The registry main() runs.
Registry& registry();
inline void add(std::string group, std::string name, Body body) {
    registry().add(std::move(group), std::move(name), std::move(body));
}
// argv[0] of the running binary (for tests that run it as a child process).
const std::string& self_path();

namespace detail {

struct Registrar {
    Registrar(const char* group, const char* name, void (*fn)(Ctx&)) { add(group, name, fn); }
};
struct SourceRegistrar {
    explicit SourceRegistrar(void (*fn)()) {
        registry().on_start([fn](Registry&) { fn(); });
    }
};

std::string show(std::string_view s);
std::string show(bool b);
std::string show(int64_t v);
std::string show(uint64_t v);

template <class T>
concept Text = std::is_convertible_v<const T&, std::string_view>;
template <class T>
concept Flag = std::same_as<std::remove_cvref_t<T>, bool>;
template <class T>
concept Int = std::is_integral_v<std::remove_cvref_t<T>> && !Flag<T> &&
              !std::same_as<std::remove_cvref_t<T>, char> && !std::same_as<std::remove_cvref_t<T>, char8_t> &&
              !std::same_as<std::remove_cvref_t<T>, char16_t> && !std::same_as<std::remove_cvref_t<T>, char32_t> &&
              !std::same_as<std::remove_cvref_t<T>, wchar_t>;

template <class T>
std::string show_value(const T& v) {
    if constexpr (Text<T>) {
        return show(std::string_view(v));
    } else if constexpr (Flag<T>) {
        return show(static_cast<bool>(v));
    } else if constexpr (std::is_signed_v<T>) {
        return show(static_cast<int64_t>(v));
    } else {
        return show(static_cast<uint64_t>(v));
    }
}

template <class A, class B>
bool equal(const A& a, const B& b) {
    if constexpr (Text<A> && Text<B>) {
        return std::string_view(a) == std::string_view(b);
    } else if constexpr (Flag<A> && Flag<B>) {
        return static_cast<bool>(a) == static_cast<bool>(b);
    } else if constexpr (Int<A> && Int<B>) {
        return std::cmp_equal(a, b);
    } else {
        static_assert(Text<A> && Text<B>, "SCFG_CHECK_EQ compares two strings, two integers or two bools");
        return false;
    }
}

// Records "file:line: what" as a failure; returns false.
bool fail_at(Ctx& t, const char* file, int line, std::string_view what);

template <class A, class B>
bool check_eq(Ctx& t, const A& a, const B& b, const char* text, const char* file, int line) {
    if (equal(a, b)) return true;
    return fail_at(t, file, line,
                   std::string("SCFG_CHECK_EQ(") + text + ")\n    left:  " + show_value(a) + "\n    right: " + show_value(b));
}

}  // namespace detail
}  // namespace scfg_test

#define SCFG_TEST(Group, Name)                                                                      \
    static void scfg_test_##Group##_##Name(::scfg_test::Ctx& t);                                    \
    static const ::scfg_test::detail::Registrar scfg_test_reg_##Group##_##Name(#Group, #Name,       \
                                                                               &scfg_test_##Group##_##Name); \
    static void scfg_test_##Group##_##Name([[maybe_unused]] ::scfg_test::Ctx& t)

#define SCFG_TEST_SOURCE(Id)                                                                       \
    static void scfg_test_source_##Id();                                                           \
    static const ::scfg_test::detail::SourceRegistrar scfg_test_src_##Id(&scfg_test_source_##Id); \
    static void scfg_test_source_##Id()

#define SCFG_CHECK(t, cond) \
    (static_cast<bool>(cond) || ::scfg_test::detail::fail_at((t), __FILE__, __LINE__, "SCFG_CHECK(" #cond ")"))

#define SCFG_CHECK_EQ(t, a, b) ::scfg_test::detail::check_eq((t), (a), (b), #a ", " #b, __FILE__, __LINE__)
