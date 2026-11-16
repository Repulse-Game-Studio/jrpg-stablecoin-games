// No Python twin: the test runner's registry, selection and main().
#include "support/Test.h"

#include "Scfg/Err.h"

#include <algorithm>
#include <cstdio>

namespace scfg_test {

bool Ctx::check(bool ok, std::string_view msg) {
    if (!ok) fail(msg);
    return ok;
}

void Ctx::fail(std::string_view msg) { messages_.emplace_back(msg); }

void Registry::add(std::string group, std::string name, Body body) {
    tests_.push_back(Test{std::move(group), std::move(name), std::move(body)});
}

void Registry::on_start(std::function<void(Registry&)> fn) { on_start_.push_back(std::move(fn)); }

std::vector<std::string> Registry::groups() const {
    std::vector<std::string> out;
    for (const Test& t : tests_) out.push_back(t.group);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int Registry::run(const std::vector<std::string>& args, const Sink& out) {
    bool list = false;
    std::string group, test;
    bool bad = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--list") {
            list = true;
        } else if ((args[i] == "--group" || args[i] == "--test") && i + 1 < args.size()) {
            (args[i] == "--group" ? group : test) = args[i + 1];
            ++i;
        } else {
            bad = true;
        }
    }
    if (bad || (!group.empty() && !test.empty())) {
        out("usage: scfg_core_tests [--list] [--group <Group> | --test <Group.Name>]\n");
        return 2;
    }

    std::vector<std::function<void(Registry&)>> starts;
    starts.swap(on_start_);
    for (auto& fn : starts) fn(*this);

    // Run a sorted copy, so a test that registers more tests cannot invalidate the loop.
    std::vector<Test> selected;
    for (const Test& t : tests_) {
        if (!group.empty() && t.group != group) continue;
        if (!test.empty() && t.group + "." + t.name != test) continue;
        selected.push_back(t);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const Test& a, const Test& b) {
        return a.group != b.group ? a.group < b.group : a.name < b.name;
    });
    for (size_t i = 1; i < selected.size(); ++i) {
        const Test& a = selected[i - 1];
        if (a.group == selected[i].group && a.name == selected[i].name) {
            out("duplicate test " + a.group + "." + a.name + "\n");
            return 2;
        }
    }

    if (list) {
        for (const Test& t : selected) out(t.group + "." + t.name + "\n");
        return 0;
    }
    int passed = 0, failed = 0;
    for (const Test& t : selected) {
        Ctx ctx;
        t.body(ctx);
        // An unknown error kind (Err::make or Err::is_a, ruling R5) is a programming error: it fails the test that
        // made it. A test that expects entries reads them and calls clear_misuse() itself.
        std::vector<std::string>& misuse = scfg::errors::misuse();
        for (const std::string& m : misuse) ctx.fail("unknown error kind (scfg::errors::misuse): " + m);
        misuse.clear();
        if (ctx.failed()) {
            ++failed;
            out("FAIL " + t.group + "." + t.name + "\n");
            for (const std::string& m : ctx.messages()) out("    " + m + "\n");
        } else {
            ++passed;
            out("ok   " + t.group + "." + t.name + "\n");
        }
    }
    out(std::to_string(passed) + " passed, " + std::to_string(failed) + " failed\n");
    if (selected.empty()) {
        out("no test matched the selection\n");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

Registry& registry() {
    static Registry r;
    return r;
}

static std::string& self_path_storage() {
    static std::string path;
    return path;
}

const std::string& self_path() { return self_path_storage(); }

namespace detail {

std::string show(std::string_view s) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += ch;  // UTF-8 passes through
        }
    }
    return out + "\"";
}

std::string show(bool b) { return b ? "true" : "false"; }
std::string show(int64_t v) { return std::to_string(v); }
std::string show(uint64_t v) { return std::to_string(v); }

bool fail_at(Ctx& t, const char* file, int line, std::string_view what) {
    t.fail(std::string(file) + ":" + std::to_string(line) + ": " + std::string(what));
    return false;
}

}  // namespace detail
}  // namespace scfg_test

int main(int argc, char** argv) {
    scfg_test::self_path_storage() = argc > 0 ? argv[0] : "";
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    return scfg_test::registry().run(args, [](std::string_view s) {
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    });
}
