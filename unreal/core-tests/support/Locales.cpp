// No Python twin: reruns a test body under non-English C locales (see Locales.h).
#include "support/Locales.h"

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <locale>

namespace scfg_test::locales {

Options from_env() {
    Options o;
    const char* env = std::getenv("SCFG_REQUIRE_LOCALES");
    o.require_named = env != nullptr && std::strcmp(env, "1") == 0;
    return o;
}

namespace detail {

void restore_c() {
    std::setlocale(LC_ALL, "C");
    std::locale::global(std::locale::classic());
}

// Whether setlocale accepts name; with comma_only, also whether its decimal point is not ".". Leaves "C" set.
bool usable(const std::string& name, bool comma_only) {
    bool ok = std::setlocale(LC_ALL, name.c_str()) != nullptr;
    if (ok && comma_only) ok = std::strcmp(std::localeconv()->decimal_point, ".") != 0;
    std::setlocale(LC_ALL, "C");
    return ok;
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

}  // namespace detail

Report rerun(const Options& o, const std::function<void(std::string_view name)>& body) {
    Report r;
    std::vector<std::string> chosen, missing;
    for (const std::string& n : o.named) (detail::usable(n, false) ? chosen : missing).push_back(n);
    if (o.require_named && !missing.empty()) {
        r.error = "SCFG_REQUIRE_LOCALES=1 but setlocale refuses " + detail::join(missing) +
                  " (install them, e.g. locale-gen " + detail::join(missing) + ")";
    }
    if (chosen.empty()) {
        for (const std::string& n : o.fallbacks) {
            if (detail::usable(n, true)) {
                chosen.push_back(n);
                break;
            }
        }
    }
    if (chosen.empty()) {
        r.error += (r.error.empty() ? "" : "; ") + std::string("no usable test locale: setlocale refused ") +
                   detail::join(o.named) + ", and found no locale with a decimal point other than \".\" among " +
                   detail::join(o.fallbacks);
        return r;
    }
    for (const std::string& n : chosen) {
        if (std::setlocale(LC_ALL, n.c_str()) == nullptr) {
            r.error += (r.error.empty() ? "" : "; ") + ("setlocale refused " + n + " the second time");
            continue;
        }
        std::locale::global(std::locale(n.c_str()));
        body(n);
        detail::restore_c();
        r.ran.push_back(n);
    }
    return r;
}

std::string describe(const Report& r) { return r.ran.empty() ? "(none)" : detail::join(r.ran); }

}  // namespace scfg_test::locales
