// WAC corpus test: lex + parse + compile EVERY shipped .wac file with zero
// crashes. Directories come from argv (or a built-in default list of the dev
// corpus). Missing directories are skipped, so CI without the copyrighted assets
// still passes.
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <formats/wac/bytecode.h>
#include <runtime/wac/compiler.h>

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
        // Machine corpus roots come from the documented env gates
        // (docs/asset-gated-tests.md), never tracked paths; absent vars
        // skip-as-pass like every asset-gated sweep.
        if (const char *jox = std::getenv("OPENNOVA_JO_ASSETS"))
            dirs.push_back(jox);
        if (const char *extra = std::getenv("OPENNOVA_WAC_CORPUS_DIRS")) {
            std::string list(extra);
            size_t start = 0;
            while (start <= list.size()) {
                const size_t sep = list.find(';', start);
                const std::string dir = list.substr(start,
                        sep == std::string::npos ? std::string::npos
                                                 : sep - start);
                if (!dir.empty()) dirs.push_back(dir);
                if (sep == std::string::npos) break;
                start = sep + 1;
            }
        }
    }

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
