// Explicit CLI resource roots, the retail launch vocabulary, and an install mounted as a
// launch with those flags mounts it.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <base/resource_index/boot_policy.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/pff/pff.h>

#include "common/test_paths.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

void test_flags_parse_case_insensitively_with_values() {
    const LaunchFlags f = parse_launch_flags(
        {"game.exe", "/D", "/EXP", " jox01 ", "--Resource-Dir", "C:/Games/JO", "/game", "JoDemo",
         "--", "--loose-mission", "00TRg.bms", "--LOOSE-ROOT"});
    CHECK(f.loose_override);
    CHECK(f.expansion == "jox01");
    CHECK(f.game == "jodemo");
    CHECK(f.resource_dir == "C:/Games/JO");
    CHECK(f.resource_dir_given);
    CHECK(f.loose_mission == "00TRg.bms");
    CHECK(f.loose_root);
    // A trailing flag with no value token reads empty; absent flags read empty/false.
    const LaunchFlags g = parse_launch_flags({"game.exe", "/exp"});
    CHECK(!g.loose_override && g.expansion.empty() && g.game.empty() && !g.loose_root);
    CHECK(parse_launch_flags({}).resource_dir.empty());
    CHECK(!parse_launch_flags({}).resource_dir_given);
    // The flag with no value is present but empty: the shell's usage error,
    // distinct from no flag at all (the bundled assets/ boot).
    const LaunchFlags bare = parse_launch_flags({"game.exe", "--resource-dir"});
    CHECK(bare.resource_dir.empty() && bare.resource_dir_given);
    // /NOHUD: the whole token in any case; a prefix or suffix is no match
    // [orig: Game_ParseCommandLineAndInit `_stricmp(token, "/NOHUD")` @0x4A79FC].
    CHECK(parse_launch_flags({"game.exe", "/nohud"}).no_hud);
    CHECK(parse_launch_flags({"game.exe", "/NOHUD", "/d"}).no_hud);
    CHECK(!parse_launch_flags({"game.exe", "/NOHUDX"}).no_hud);
    CHECK(!parse_launch_flags({"game.exe"}).no_hud);
}

// `/mod` is `/exp`: one arm of the game's walk, the value token stepped over,
// the last copy winning, 32 bytes kept [orig: Game_ParseCommandLineAndInit
// @ 0x4a76ac / 0x4a76c1 / 0x4a76cf].
void test_mod_is_exp_and_the_name_keeps_32_bytes() {
    CHECK(parse_launch_flags({"game.exe", "/mod", "jox01"}).expansion == "jox01");
    CHECK(parse_launch_flags({"game.exe", "/MOD", "jox01"}).expansion == "jox01");
    // Both spellings copy over the one before.
    CHECK(parse_launch_flags({"game.exe", "/exp", "a", "/mod", "b"}).expansion == "b");
    CHECK(parse_launch_flags({"game.exe", "/mod", "a", "/exp", "b"}).expansion == "b");
    CHECK(parse_launch_flags({"game.exe", "/exp", "a", "/exp", "b"}).expansion == "b");
    // A last flag with no token after it copies nothing over the earlier name.
    CHECK(parse_launch_flags({"game.exe", "/exp", "a", "/mod"}).expansion == "a");
    // The token after the flag is its value, never a flag of its own.
    CHECK(parse_launch_flags({"game.exe", "/exp", "/mod", "b"}).expansion == "/mod");
    // strncpy(g_ExpansionName, token, 0x20): 32 bytes kept, the rest dropped.
    const std::string name32 = "abcdefghijklmnopqrstuvwxyz012345";
    CHECK(parse_launch_flags({"game.exe", "/mod", name32}).expansion == name32);
    CHECK(parse_launch_flags({"game.exe", "/exp", name32 + "6789ABCD"}).expansion == name32);
    CHECK(launch_expansion_name(" " + name32 + "XY ") == name32);
    CHECK(launch_expansion_name("jox01") == "jox01");
}

void test_runtime_launch_flags_parse() {
    const LaunchFlags f = parse_launch_flags(
        {"game.exe", "--", "--Mission", " 00TRa.bms ", "--lan-host", "ASH_I5A.BMS",
         "--lan-join", "192.168.10.120:32770", "--lan-port", "32768", "--LAN-GAMETYPE",
         "0x10020", "--lan-mode", "3", "--lan-max-players", "16", "--callsign", "Host",
         "--spectator", "--spectator-password", " watch me ", "--integrity-profile",
         "retail", "--capture-pcap", "C:/cap/s.pcapng", "--mcp-port", "8975", "--Working-Dir",
         " C:/p/.opennova/run/1 "});
    CHECK(f.mission == "00TRa.bms");
    CHECK(f.lan_host == "ASH_I5A.BMS");
    CHECK(f.lan_join == "192.168.10.120:32770");
    CHECK(f.lan_port == 32768);
    CHECK(f.lan_gametype == 0x10020);
    CHECK(f.lan_mode == 3);
    CHECK(f.lan_max_players == 16);
    CHECK(f.spectator);
    CHECK(f.spectator_password == "watch me");
    CHECK(f.callsign == "Host");
    CHECK(f.integrity_profile == "retail");
    CHECK(f.capture_pcap == "C:/cap/s.pcapng");
    CHECK(f.mcp_port == 8975);
    // The directory a source run was started in (ADR 0046 S13 A8: the editor's run directory).
    CHECK(f.working_dir == "C:/p/.opennova/run/1");
    // Absent flags read their sentinels.
    const LaunchFlags none = parse_launch_flags({"game.exe"});
    CHECK(none.mission.empty() && none.lan_host.empty() && none.lan_join.empty());
    CHECK(none.working_dir.empty() && parse_launch_flags({"--working-dir"}).working_dir.empty());
    CHECK(none.lan_port == 0 && none.lan_gametype == -1 && none.lan_mode == 0);
    CHECK(none.lan_max_players == 0 && none.mcp_port == 0);
    CHECK(!none.spectator && none.spectator_password.empty());
    CHECK(none.callsign.empty() && none.integrity_profile.empty() && none.capture_pcap.empty());
    // Malformed or out-of-range integers read the sentinel, never a clamp.
    const LaunchFlags bad = parse_launch_flags(
        {"--lan-port", "fast", "--lan-gametype", "-3", "--lan-mode", "5", "--lan-max-players",
         "65", "--mcp-port", "70000"});
    CHECK(bad.lan_port == 0 && bad.lan_gametype == -1 && bad.lan_mode == 0);
    CHECK(bad.lan_max_players == 0 && bad.mcp_port == 0);
    CHECK(parse_launch_flags({"--lan-mode", "0"}).lan_mode == 0);
    CHECK(parse_launch_flags({"--lan-mode", "4"}).lan_mode == 4);
    CHECK(parse_launch_flags({"--lan-max-players", "1"}).lan_max_players == 1);
    CHECK(parse_launch_flags({"--lan-max-players", "64"}).lan_max_players == 64);
    CHECK(parse_launch_flags({"--lan-gametype", "0"}).lan_gametype == 0);
    CHECK(parse_launch_flags({"--mcp-port", "12abc"}).mcp_port == 0);
    // A trailing flag with no value token reads the sentinel too.
    CHECK(parse_launch_flags({"--mcp-port"}).mcp_port == 0);
    CHECK(parse_launch_flags({"--mission"}).mission.empty());
}

void test_lan_fallbacks_and_join_endpoint() {
    const LaunchFlags none = parse_launch_flags({});
    CHECK(launch_lan_port(none, 32768) == 32768);
    CHECK(launch_lan_mode(none, 1) == 1);
    CHECK(launch_lan_max_players(none, 4) == 4);
    const LaunchFlags set = parse_launch_flags(
        {"--lan-port", "32770", "--lan-mode", "2", "--lan-max-players", "8"});
    CHECK(launch_lan_port(set, 32768) == 32770);
    CHECK(launch_lan_mode(set, 1) == 2);
    CHECK(launch_lan_max_players(set, 4) == 8);

    const LanEndpoint absent = launch_lan_join_endpoint(none, 32768);
    CHECK(absent.ip.empty() && absent.port == 32768);
    const LanEndpoint bare = launch_lan_join_endpoint(parse_launch_flags({"--lan-join", "127.0.0.1"}), 32768);
    CHECK(bare.ip == "127.0.0.1" && bare.port == 32768);
    const LanEndpoint full = launch_lan_join_endpoint(
        parse_launch_flags({"--lan-join", "192.168.10.120:32770"}), 32768);
    CHECK(full.ip == "192.168.10.120" && full.port == 32770);
    const LanEndpoint junk = launch_lan_join_endpoint(parse_launch_flags({"--lan-join", "10.0.0.2:zz"}), 32768);
    CHECK(junk.ip == "10.0.0.2" && junk.port == 32768);
}

void test_flags_win_over_fallbacks_and_jo_is_the_default_game() {
    const LaunchFlags none = parse_launch_flags({"game.exe"});
    CHECK(launch_expansion(none, "revx02") == "revx02");
    CHECK(launch_game(none, " DFX ") == "dfx");
    CHECK(launch_game(none, "") == "jo");
    CHECK(none.resource_dir.empty());
    const LaunchFlags set = parse_launch_flags({"/exp", "jox01", "/game", "JODEMO", "--resource-dir", "E:/y"});
    CHECK(launch_expansion(set, "revx02") == "jox01");
    CHECK(launch_game(set, "dfx") == "jodemo");
    CHECK(set.resource_dir == "E:/y");
}

// A flag given twice takes its last value, as the game's command line walk copies each
// `/exp` value over the one before [orig: Game_ParseCommandLineAndInit @ 0x4a7310, "/exp"
// @ 0x4a76a6 -> g_ExpansionName @ 0x4a76cf].
void test_repeated_flags_take_the_last_value() {
    const LaunchFlags f = parse_launch_flags(
        {"/exp", "jox01", "/game", "jo", "--lan-port", "2000", "/EXP", "revx02", "/game", "JoDemo", "--lan-port", "3000"});
    CHECK(f.expansion == "revx02");
    CHECK(f.game == "jodemo");
    CHECK(f.lan_port == 3000);
}

void test_path_join() {
    CHECK(boot_path_join("", "assets") == "assets");
    CHECK(boot_path_join("C:/exe", "assets") == "C:/exe/assets");
    CHECK(boot_path_join("C:/exe/", "assets") == "C:/exe/assets");
    CHECK(boot_path_join("C:\\exe\\", "assets") == "C:\\exe\\assets");
}

// An install mounts as a launch with the flags mounts it: a stock launch reads the
// archive's file where a loose one of the name sits beside it, /d the loose one
// [orig: FileSystem_OpenFile @ 0x75b1c0, the gate @ 0x75b1e5; Game_InitSubsystems
// @ 0x4a6fa3]; a folder with none of the table's archives, or none at all, does not mount.
void test_mount_install() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(test_paths_temp_dir()) /
            ("opennova_boot_policy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct TempRoot {
        fs::path path;
        ~TempRoot() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } const cleanup{root};
    const fs::path install = root / "install", empty = root / "empty";
    std::error_code ec;
    fs::create_directories(install, ec);
    fs::create_directories(empty, ec);
    const uint8_t packed[] = {'p', 'a', 'c', 'k', 'e', 'd'};
    const pff::PffWriteEntry entries[] = {{"note.txt", packed, sizeof(packed), 0, 0, 0}};
    CHECK(pff::pff_write_archive((install / "resource.pff").string().c_str(), pff::PFF_FORMAT_PFF3, entries, 1) ==
          pff::PFF_WRITE_OK);
    std::ofstream((install / "note.txt").string(), std::ios::binary) << "loose";

    std::vector<uint8_t> bytes;
    Vfs stock;
    CHECK(mount_install(stock, install.string(), parse_launch_flags({"game.exe"})));
    CHECK(stock.read_file("note.txt", bytes) && std::string(bytes.begin(), bytes.end()) == "packed");
    Vfs dev;
    CHECK(mount_install(dev, install.string(), parse_launch_flags({"game.exe", "/d"})));
    CHECK(dev.read_file("note.txt", bytes) && std::string(bytes.begin(), bytes.end()) == "loose");
    Vfs none;
    CHECK(!mount_install(none, empty.string(), parse_launch_flags({})) && none.last_error().empty());
    CHECK(!mount_install(none, (root / "absent").string(), parse_launch_flags({})) && !none.last_error().empty());

    // The index the game's ResourceRoot mounts through (scan_install) takes the same mount,
    // and says which refusal it met: a root with no archive that opens (none at all, or a
    // corrupt sole one beside usable loose files, which names it) is NoArchive, the boot's
    // no-archives refusal; a root that does not mount is Unmounted.
    using InstallScan = ResourceIndex::InstallScan;
    ResourceIndex index;
    CHECK(index.scan_install(install.string(), parse_launch_flags({"game.exe"})) == InstallScan::Mounted);
    CHECK(index.has_mounted_archive() && index.read_file("note.txt", bytes) &&
          std::string(bytes.begin(), bytes.end()) == "packed");
    CHECK(index.scan_install(install.string(), parse_launch_flags({"game.exe", "/d"})) == InstallScan::Mounted);
    CHECK(index.read_file("note.txt", bytes) && std::string(bytes.begin(), bytes.end()) == "loose");
    CHECK(index.scan_install(empty.string(), parse_launch_flags({})) == InstallScan::NoArchive &&
          index.last_error().empty());
    const fs::path corrupt = root / "corrupt";
    fs::create_directories(corrupt, ec);
    std::ofstream((corrupt / "resource.pff").string(), std::ios::binary) << "not an archive";
    std::ofstream((corrupt / "note.txt").string(), std::ios::binary) << "loose";
    CHECK(index.scan_install(corrupt.string(), parse_launch_flags({"game.exe", "/d"})) == InstallScan::NoArchive &&
          index.last_error().find("resource.pff") != std::string::npos && !index.has_mounted_archive());
    CHECK(index.scan_install((root / "absent").string(), parse_launch_flags({})) == InstallScan::Unmounted &&
          !index.last_error().empty());
}

} // namespace

int main() {
    test_flags_parse_case_insensitively_with_values();
    test_mod_is_exp_and_the_name_keeps_32_bytes();
    test_runtime_launch_flags_parse();
    test_lan_fallbacks_and_join_endpoint();
    test_flags_win_over_fallbacks_and_jo_is_the_default_game();
    test_repeated_flags_take_the_last_value();
    test_path_join();
    test_mount_install();
    if (failures == 0) std::printf("boot_policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
