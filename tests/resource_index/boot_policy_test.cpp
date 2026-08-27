// Launch flags and the boot resource directory ladder
// (base/resource_index/boot_policy.h): the retail flag vocabulary parsed
// case-insensitively, a flag winning over a persisted fallback, the JO game
// default, and the flag > persisted > bundled game > bundled assets ladder
// with its loose-mount blessing.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <base/resource_index/boot_policy.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct FakeFs {
    std::set<std::string> files;
    std::set<std::string> dirs;
    BootDirProbe probe() const {
        BootDirProbe p;
        p.file_exists = [this](const std::string &path) { return files.count(path) != 0; };
        p.dir_exists = [this](const std::string &path) { return dirs.count(path) != 0; };
        return p;
    }
};

void test_flags_parse_case_insensitively_with_values() {
    const LaunchFlags f = parse_launch_flags(
        {"game.exe", "/D", "/EXP", " jox01 ", "--Resource-Dir", "C:/Games/JO", "/game", "JoDemo",
         "--", "--loose-mission", "00TRg.bms", "--LOOSE-ROOT"});
    CHECK(f.loose_override);
    CHECK(f.expansion == "jox01");
    CHECK(f.game == "jodemo");
    CHECK(f.resource_dir == "C:/Games/JO");
    CHECK(f.loose_mission == "00TRg.bms");
    CHECK(f.loose_root);
    // A trailing flag with no value token reads empty; absent flags read empty/false.
    const LaunchFlags g = parse_launch_flags({"game.exe", "/exp"});
    CHECK(!g.loose_override && g.expansion.empty() && g.game.empty() && !g.loose_root);
    CHECK(parse_launch_flags({}).resource_dir.empty());
}

void test_flags_win_over_fallbacks_and_jo_is_the_default_game() {
    const LaunchFlags none = parse_launch_flags({"game.exe"});
    CHECK(launch_expansion(none, "revx02") == "revx02");
    CHECK(launch_game(none, " DFX ") == "dfx");
    CHECK(launch_game(none, "") == "jo");
    CHECK(launch_resource_dir(none, " D:/x ") == "D:/x");
    const LaunchFlags set = parse_launch_flags({"/exp", "jox01", "/game", "JODEMO", "--resource-dir", "E:/y"});
    CHECK(launch_expansion(set, "revx02") == "jox01");
    CHECK(launch_game(set, "dfx") == "jodemo");
    CHECK(launch_resource_dir(set, "D:/x") == "E:/y");
}

void test_path_join() {
    CHECK(boot_path_join("", "assets") == "assets");
    CHECK(boot_path_join("C:/exe", "assets") == "C:/exe/assets");
    CHECK(boot_path_join("C:/exe/", "assets") == "C:/exe/assets");
    CHECK(boot_path_join("C:\\exe\\", "assets") == "C:\\exe\\assets");
}

void test_boot_dir_ladder() {
    FakeFs fs;
    const LaunchFlags none = parse_launch_flags({});
    // Nothing anywhere: ask.
    CHECK(boot_resource_dir(none, "", "C:/exe", fs.probe()).empty());
    // The loose assets/ sibling is the last rung.
    fs.dirs.insert("C:/exe/assets");
    CHECK(bundled_assets_dir("C:/exe", fs.probe()) == "C:/exe/assets");
    CHECK(boot_resource_dir(none, "", "C:/exe", fs.probe()) == "C:/exe/assets");
    // A boot archive beside the exe outranks the assets/ sibling.
    fs.files.insert("C:/exe/localres.pff");
    CHECK(bundled_game_dir("C:/exe", fs.probe()) == "C:/exe");
    CHECK(boot_resource_dir(none, "", "C:/exe", fs.probe()) == "C:/exe");
    // Any boot-table archive qualifies (case as probed).
    FakeFs lang;
    lang.files.insert("C:/exe/language.pff");
    CHECK(bundled_game_dir("C:/exe", lang.probe()) == "C:/exe");
    FakeFs stray;
    stray.files.insert("C:/exe/mod.pff");
    CHECK(bundled_game_dir("C:/exe", stray.probe()).empty());
    // The persisted pick outranks the bundle; the flag outranks both.
    CHECK(boot_resource_dir(none, "C:/picked", "C:/exe", fs.probe()) == "C:/picked");
    const LaunchFlags flagged = parse_launch_flags({"--resource-dir", "C:/flag"});
    CHECK(boot_resource_dir(flagged, "C:/picked", "C:/exe", fs.probe()) == "C:/flag");
    // No exe dir known: only the flag / persisted rungs.
    CHECK(boot_resource_dir(none, "", "", fs.probe()).empty());
}

void test_loose_mount_blessing() {
    FakeFs fs;
    fs.dirs.insert("C:/exe/assets");
    const LaunchFlags none = parse_launch_flags({});
    CHECK(boot_loose_allowed(none, "C:/exe/assets", "C:/exe", fs.probe()));
    CHECK(!boot_loose_allowed(none, "C:/picked", "C:/exe", fs.probe()));
    CHECK(!boot_loose_allowed(none, "", "C:/exe", fs.probe()));
    const LaunchFlags root = parse_launch_flags({"--loose-root"});
    CHECK(boot_loose_allowed(root, "C:/picked", "C:/exe", fs.probe()));
    // Without an assets/ sibling nothing is blessed by default.
    FakeFs bare;
    CHECK(!boot_loose_allowed(none, "C:/exe/assets", "C:/exe", bare.probe()));
}

} // namespace

int main() {
    test_flags_parse_case_insensitively_with_values();
    test_flags_win_over_fallbacks_and_jo_is_the_default_game();
    test_path_join();
    test_boot_dir_ladder();
    test_loose_mount_blessing();
    if (failures == 0) std::printf("boot_policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
