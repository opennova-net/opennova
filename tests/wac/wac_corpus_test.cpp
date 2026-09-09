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
#include <runtime/wac/wac_layered_load.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/particle/effect_scene.h>
#include <formats/particle/parser.h>
#include <base/resource_index/resource_index.h>
#include <runtime/mission/runtime_boot.h>
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
        // The machine corpus root comes from the documented gate
        // (docs/asset-gated-tests.md): the extracted asset tree carries the
        // shipped .wac scripts beside their missions.
        if (const std::string assets = retail::assets(); !assets.empty())
            dirs.push_back(assets);
    }
    if (dirs.empty())
        return retail::skip("OPENNOVA_JO_ASSETS (the extracted tree's retail .wac scripts)");

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
        opennova::world::AmmoTable ammo;
        opennova::def::DefAmmoFile parsed = {};
        const std::string ammo_path = (fs::path(d) / "ammo.def").string();
        if (opennova::def::def_parse_ammo(ammo_path.c_str(), &parsed) == 0) {
            ammo = opennova::world::build_ammo_table(parsed);
            opennova::def::def_free_ammo(&parsed);
        }
        opennova::ResourceIndex index;
        index.scan(d);
        const auto mounted = opennova::mission::boot_files_from_index(index);
        opennova::particle::EffectScene effects;
        opennova::particle::EffectSceneConfig effect_config;
        for (const auto &extension : {std::string(".ptl"), index.particle_extension()}) {
            for (const auto &name : mounted.list_files(extension)) {
                std::vector<uint8_t> bytes;
                opennova::particle::EffectCatalogDocument document;
                opennova::particle::ParseError error;
                document.source = name;
                if (mounted.read_file(name, bytes) && opennova::particle::load_particles_from_buffer(
                        reinterpret_cast<const char *>(bytes.data()), bytes.size(), document.file, error))
                    effect_config.documents.push_back(std::move(document));
            }
        }
        effects.open(effect_config);
        for (auto it = fs::recursive_directory_iterator(d, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !is_wac(it->path())) continue;
            ++files;
            std::string src = read_file(it->path());
            CompileEnv env;
            env.ammo = &ammo;
            env.effects = &effects;
            opennova::audio::SoundSetIndex sounds;
            load_script_sound_sets(mounted, it->path().stem().string(), sounds);
            env.sounds = &sounds;
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
