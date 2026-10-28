// A game's mods on NovaWorld and their folders (base/resource_index/mod_listing.h, ADR 0074): the
// service's GET /api/mods reply read into installable mods, and a mod's expansion folder written
// and removed as the runtime's NovaWorld menu installs and removes it.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <base/resource_index/mod_listing.h>

#include "common/test_paths.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// The reply's mods in order; an entry that cannot install (no code a folder takes, no updater
// login, no command file) is left out, and a reply that is no {"mods": [...]} object is refused.
void test_the_reply_reads_its_installable_mods() {
    std::vector<ModListing> mods;
    std::string error;
    CHECK(read_mod_listings(R"({"mods": [
        {"key": "MYMOD", "code": "mymod", "name": "My Mod", "owner": "biggy", "version": "1.2",
         "updater_login": "MYMOD", "command_file": "MYMOD.CMD"},
        {"key": "NOCMD", "code": "nocmd", "updater_login": "NOCMD"},
        {"key": "UP", "code": "../up", "updater_login": "UP", "command_file": "UP.CMD"},
        {"key": "LONG", "code": "abcdefghijklmnopqrstuvwxyz012345", "updater_login": "L", "command_file": "L.CMD"},
        {"key": "BARE", "code": " bare ", "updater_login": "BARE", "command_file": "BARE.CMD", "version": 3},
        "not an object"
    ]})", mods, error));
    CHECK(mods.size() == 2);
    CHECK(mods.size() == 2 && mods[0].key == "MYMOD" && mods[0].code == "mymod" && mods[0].name == "My Mod" &&
          mods[0].owner == "biggy" && mods[0].version == "1.2" && mods[0].updater_login == "MYMOD" &&
          mods[0].command_file == "MYMOD.CMD");
    CHECK(mods.size() == 2 && mods[1].code == "bare" && mods[1].name.empty() && mods[1].version.empty());
    CHECK(read_mod_listings(R"({"mods": []})", mods, error) && mods.empty());
    CHECK(!read_mod_listings(R"({"servers": []})", mods, error) && !error.empty());
    CHECK(!read_mod_listings("<html>", mods, error) && !error.empty());
    CHECK(!read_mod_listings("", mods, error));
}

// A mod's wizard names its product and the update host the runtime's OPENNOVA.WIZ names (found
// without case), none without one; a code that is no folder name the game mounts, or a value no
// wizard carries, is refused, and removal takes the folder with everything in it.
void test_a_mods_folder_is_written_and_removed() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(test_paths_temp_dir()) /
            ("opennova_mod_listing_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct TempRoot {
        fs::path path;
        ~TempRoot() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } const cleanup{root};
    const fs::path game = root / "game";
    const fs::path runtime = root / "runtime";
    std::error_code ec;
    fs::create_directories(game, ec);
    fs::create_directories(runtime, ec);
    const auto read = [](const fs::path &file) {
        std::ifstream in(file.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    ModListing mod;
    mod.code = "mymod";
    mod.updater_login = "MYMOD";
    mod.command_file = "MYMOD.CMD";
    std::string error;
    CHECK(write_mod_wizard(game.string(), runtime.string(), mod, error));
    CHECK(read(game / "expansion" / "mymod" / "UPDATE.WIZ") == "[HEADER]\r\nCMD=MYMOD.CMD\r\nPRODUCT_LOGIN=MYMOD\r\n");
    std::ofstream((runtime / "opennova.wiz").string(), std::ios::binary)
        << "[HEADER]\r\nCMD=ONRUNTIME.CMD\r\nURL=update.staging.opennova.net\r\nPRODUCT_LOGIN=ONRUNTIME\r\n";
    CHECK(write_mod_wizard(game.string(), runtime.string(), mod, error));
    CHECK(read(game / "expansion" / "mymod" / "UPDATE.WIZ") ==
          "[HEADER]\r\nCMD=MYMOD.CMD\r\nURL=update.staging.opennova.net\r\nPRODUCT_LOGIN=MYMOD\r\n");
    for (const char *code : {"", ".", "..", "a/b", "a\\b", "c:", "abcdefghijklmnopqrstuvwxyz012345"}) {
        ModListing bad = mod;
        bad.code = code;
        error.clear();
        CHECK(!write_mod_wizard(game.string(), runtime.string(), bad, error) && !error.empty());
        CHECK(!remove_mod_folder(game.string(), code, error));
    }
    CHECK(expansion_code_valid("abcdefghijklmnopqrstuvwxyz01234"));
    ModListing unwritable = mod;
    unwritable.code = "other";
    unwritable.command_file = "A;B.CMD";
    CHECK(!write_mod_wizard(game.string(), runtime.string(), unwritable, error));
    unwritable.command_file = "OTHER.CMD";
    unwritable.updater_login.clear();
    CHECK(!write_mod_wizard(game.string(), runtime.string(), unwritable, error));
    CHECK(!fs::exists(game / "expansion" / "other"));
    std::ofstream((game / "expansion" / "mymod" / "mymod.pff").string(), std::ios::binary) << "archive";
    CHECK(remove_mod_folder(game.string(), "mymod", error));
    CHECK(!fs::exists(game / "expansion" / "mymod") && fs::exists(game / "expansion"));
    CHECK(remove_mod_folder(game.string(), "mymod", error)); // already gone
}

} // namespace

int main() {
    test_the_reply_reads_its_installable_mods();
    test_a_mods_folder_is_written_and_removed();
    if (failures == 0) std::printf("mod_listing_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
