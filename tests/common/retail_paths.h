// The machine-local retail data the asset-gated ctests read, behind ONE seam.
//
// Two documented roots (docs/asset-gated-tests.md), read here and nowhere
// else (scripts/lint/env_lint.py enforces it):
//
//   OPENNOVA_JO_DIR          a packed retail JO install (the .pff set)
//   OPENNOVA_JO_ASSETS       an extracted retail asset tree (ITEMS.DEF,
//                            weapon.def, models, .bad/.adm, score.ini, the
//                            shipped .bms missions loose at its root, and the
//                            reference fixture set under fixtures/)
//
// Everything else is a convention under those roots: ITEMS.DEF and the .bms
// corpus directly under the asset tree, weapon.sav beside the install's
// expansion.
//
// A test whose whole body needs retail data returns retail::skip(...) — exit
// code 77, which tests/CMakeLists.txt's opennova_add_gated_test maps to
// SKIP_RETURN_CODE so ctest reports it Skipped, never Passed. A test whose
// synthetic legs ran prints retail::skip_leg(...) for the missing retail leg
// and exits 0 like any other passing test.

#pragma once

#include "test_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace retail {

constexpr int kSkipExitCode = 77;

// Mixed binaries have two separately registered CTest entries. The ordinary
// entry selects synthetic cases; --retail selects the compatibility cases as
// well. Root resolution is disabled for the ordinary entry, even on a machine
// with an installed game. Fully gated binaries retain their normal behavior.
inline bool &selected() {
    static bool enabled = true;
    return enabled;
}

inline void configure_mixed(int &argc, char **argv) {
    selected() = false;
    for (int i = 1; i < argc;) {
        if (std::string(argv[i]) == "--retail") {
            selected() = true;
            for (int j = i; j < argc; ++j) argv[j] = argv[j + 1];
            --argc;
        } else {
            ++i;
        }
    }
    std::printf("Selected test cases: %s\n", selected() ? "synthetic + retail compatibility" : "synthetic");
}

inline std::string env_or_empty(const char *name) {
    const char *value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string();
}

inline std::string strip_trailing_separators(std::string path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    return path;
}

inline std::string join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    return strip_trailing_separators(dir) + "/" + name;
}

inline bool file_exists(const std::string &path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

inline bool dir_exists(const std::string &path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec);
}

// The packed retail install, or "".
inline std::string install() { return selected() ? strip_trailing_separators(env_or_empty("OPENNOVA_JO_DIR")) : std::string(); }
// The extracted retail asset tree, or "".
inline std::string assets() { return selected() ? strip_trailing_separators(env_or_empty("OPENNOVA_JO_ASSETS")) : std::string(); }
inline std::string lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// `<assets>/<name>` looked up case-insensitively (retail trees mix ITEMS.DEF
// and items.def); "" when the tree is unset or the file is absent.
inline std::string asset_file(const char *name) {
    const std::string root = assets();
    if (root.empty() || !dir_exists(root)) return std::string();
    const std::string wanted = lower_ascii(name);
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (lower_ascii(entry.path().filename().string()) == wanted)
            return entry.path().generic_string();
    }
    return std::string();
}

// `<assets>/fixtures/<rel>`: the retail-interop fixture set the reference tree
// mirrors under its `fixtures/` subtree (the retail files the parsers prove they
// read as shipped: menus, string tables, defs, rigs, ...; they never live in this
// repository). "" when the tree is unset or the file is absent.
inline std::string reference_fixture(const char *rel) {
    const std::string root = assets();
    if (root.empty() || !dir_exists(root)) return std::string();
    const std::string path = join(join(root, "fixtures"), rel);
    return file_exists(path) ? path : std::string();
}

// The expansion names shipped under <install>/expansion/, sorted; empty when
// the install is unset or carries none. Which expansions a machine has is
// machine-specific (JO:CA ships jox01, JOTAC ships revx02), so a test that
// needs one enumerates and skips the leg that is absent.
inline std::vector<std::string> expansions() {
    std::vector<std::string> names;
    const std::string root = install();
    if (root.empty()) return names;
    std::error_code ec;
    const std::string dir = join(root, "expansion");
    if (!dir_exists(dir)) return names;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// The retail player profile: <install>/expansion/<exp>/weapon.sav for the
// first expansion that carries one, else <install>/weapon.sav, else "".
inline std::string weapon_sav() {
    const std::string root = install();
    if (root.empty()) return std::string();
    for (const std::string &exp : expansions()) {
        const std::string candidate = join(join(join(root, "expansion"), exp), "weapon.sav");
        if (file_exists(candidate)) return candidate;
    }
    const std::string base = join(root, "weapon.sav");
    return file_exists(base) ? base : std::string();
}

// The whole test is gated: report what it needs and exit Skipped.
inline int skip(const char *needs) {
    std::printf("SKIP: needs %s\n", needs);
    std::fflush(stdout);
    return kSkipExitCode;
}

// One retail leg inside a test whose synthetic legs ran: note it and pass.
inline int skip_leg(const char *needs) {
    // A nonselected compatibility case is not a missing-data skip. Its
    // separately named CTest entry is responsible for exercising this case.
    if (!selected()) return 0;
    std::printf("SKIP-LEG: needs %s\n", needs);
    std::fflush(stdout);
    return 0;
}

} // namespace retail

// Bind `var` to `expr`; return the gated skip when it is empty.
#define RETAIL_REQUIRE_OR_SKIP(var, expr, needs)                              \
    const std::string var = (expr);                                           \
    if ((var).empty()) return ::retail::skip(needs)

// Return the gated skip unless `path` names an existing file.
#define RETAIL_REQUIRE_FILE_OR_SKIP(path, needs)                              \
    if (!::retail::file_exists(path)) return ::retail::skip(needs)
