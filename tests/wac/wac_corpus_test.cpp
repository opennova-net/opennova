// WAC corpus test: lex + parse + compile EVERY shipped .wac file with zero
// crashes. Directories come from argv, else the documented retail gates;
// without either the test reports Skipped (docs/asset-gated-tests.md).
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <formats/wac/bytecode.h>
#include <runtime/wac/compiler.h>
#include "common/retail_paths.h"

namespace fs = std::filesystem;
using namespace opennova::wac;

static std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool is_wac(const fs::path &p) {
    std::string ext = p.extension().string();
    for (char &c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    return ext == ".wac";
}

int main(int argc, char **argv) {
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) {
        // Machine corpus roots come from the documented gates
        // (docs/asset-gated-tests.md): the extracted asset tree and the
        // mission corpus both carry shipped .wac scripts.
        if (const std::string assets = retail::assets(); !assets.empty())
            dirs.push_back(assets);
        if (const std::string corpus = retail::mission_corpus();
            !corpus.empty() && std::find(dirs.begin(), dirs.end(), corpus) == dirs.end())
            dirs.push_back(corpus);
    }
    if (dirs.empty())
        return retail::skip("OPENNOVA_JO_ASSETS or OPENNOVA_MISSION_CORPUS (directories of retail .wac scripts)");

    int files = 0;
    int hard_errors = 0;
    int total_warnings = 0;
    std::vector<std::string> unknown_examples;

    for (const std::string &d : dirs) {
        std::error_code ec;
        if (!fs::exists(d, ec) || !fs::is_directory(d, ec)) {
            std::printf("skip (absent): %s\n", d.c_str());
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(d, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !is_wac(it->path())) continue;
            ++files;
            std::string src = read_file(it->path());
            CompileEnv env;
            Program prog = compile_source(src, env); // must not crash
            int errs = prog.error_count();
            hard_errors += errs;
            for (const Diagnostic &dg : prog.diagnostics) {
                if (!dg.error) {
                    ++total_warnings;
                    if (unknown_examples.size() < 8 &&
                        dg.message.rfind("unknown command", 0) == 0) {
                        unknown_examples.push_back(it->path().filename().string() + ": " + dg.message);
                    }
                }
            }
            // sanity: every file produces a terminated program.
            if (prog.code.empty() || prog.code.back() != kProgramTerminator) {
                std::printf("FAIL: %s produced no terminated program\n",
                            it->path().filename().string().c_str());
                ++hard_errors;
            }
        }
    }

    std::printf("corpus: %d files, %d hard errors, %d warnings\n", files, hard_errors, total_warnings);
    for (const std::string &u : unknown_examples) std::printf("  warn: %s\n", u.c_str());

    // Pass criteria: no crash (reaching here), no hard compile errors. Unknown-
    // command warnings are allowed (older games extend the keyword set).
    if (hard_errors != 0) {
        std::printf("CORPUS TEST FAILED\n");
        return 1;
    }
    std::printf("corpus test passed\n");
    return 0;
}
