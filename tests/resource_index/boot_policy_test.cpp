// Explicit CLI resource roots, the retail launch vocabulary, and an install mounted as a
// launch with those flags mounts it.
#include <algorithm>
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
#include <formats/update/update_wizard.h>

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
    // distinct from no flag at all (the executable directory).
    const LaunchFlags bare = parse_launch_flags({"game.exe", "--resource-dir"});
    CHECK(bare.resource_dir.empty() && bare.resource_dir_given);
    // /NOHUD: the whole token in any case; a prefix or suffix is no match
    // [orig: Game_ParseCommandLineAndInit `_stricmp(token, "/NOHUD")` @0x4A79FC].
    CHECK(parse_launch_flags({"game.exe", "/nohud"}).no_hud);
    CHECK(parse_launch_flags({"game.exe", "/NOHUD", "/d"}).no_hud);
    CHECK(!parse_launch_flags({"game.exe", "/NOHUDX"}).no_hud);
    CHECK(!parse_launch_flags({"game.exe"}).no_hud);
    // /noreload, the same whole-token match [orig: _stricmp(token,
    // "/noreload") @0x4A76DD].
    CHECK(parse_launch_flags({"game.exe", "/NoReload"}).no_reload);
    CHECK(!parse_launch_flags({"game.exe", "/noreloads"}).no_reload);
    CHECK(!parse_launch_flags({"game.exe"}).no_reload);
    // /LAN: the whole token, any case [orig: Game_ParseCommandLineAndInit
    // `_stricmp(token, "/LAN")` @0x4A74D9..0x4A74EB].
    CHECK(parse_launch_flags({"game.exe", "/LAN"}).lan);
    CHECK(parse_launch_flags({"game.exe", "/w", "/lan"}).lan);
    CHECK(!parse_launch_flags({"game.exe", "/LANX"}).lan);
    CHECK(!parse_launch_flags({"game.exe", "LAN"}).lan);
    CHECK(!parse_launch_flags({"game.exe", "/LAN="}).lan);
    CHECK(!parse_launch_flags({"game.exe"}).lan);
    // /sstga: the whole token, any case [orig: Game_ParseCommandLineAndInit
    // `_stricmp(token, "/sstga")` @0x4A77C1..0x4A77D3].
    CHECK(parse_launch_flags({"game.exe", "/sstga"}).screenshot_tga);
    CHECK(parse_launch_flags({"game.exe", "/w", "/SSTGA"}).screenshot_tga);
    CHECK(!parse_launch_flags({"game.exe", "/sstgax"}).screenshot_tga);
    CHECK(!parse_launch_flags({"game.exe", "sstga"}).screenshot_tga);
    CHECK(!parse_launch_flags({"game.exe"}).screenshot_tga);

    // D-NET-540: /POSTIPADDRESS <address> and /POSTPORTNUMBER <n>, any case, the last one
    // standing; the port is the CRT atol of its token (0 when absent, so it overrides nothing)
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - @0x4A75A4..0x4A75CF,
    // @0x4A75D4..0x4A75FE].
    const LaunchFlags post = parse_launch_flags(
            {"game.exe", "/postipaddress", "10.0.0.1", "/POSTIPADDRESS", "nw.example",
             "/PostPortNumber", " 7598x"});
    CHECK(post.post_ip_address == "nw.example");
    CHECK(post.post_port_number == 7598);
    CHECK(parse_launch_flags({"game.exe", "/POSTPORTNUMBER", "-3"}).post_port_number == -3);
    CHECK(parse_launch_flags({"game.exe", "/POSTPORTNUMBER", "port"}).post_port_number == 0);
    CHECK(parse_launch_flags({"game.exe", "/POSTPORTNUMBER"}).post_port_number == 0);
    CHECK(parse_launch_flags({"game.exe"}).post_ip_address.empty());

    // D-NET-542: /UDPNOVAWORLD <address>, any case: its next token is stepped over (so never read
    // as a flag) and the first six kept, each to 31 characters; one with no next token keeps
    // nothing [orig: Game_ParseCommandLineAndInit @0x4a7310 - @0x4A74F1..0x4A7530].
    const LaunchFlags udp = parse_launch_flags(
            {"game.exe", "/udpnovaworld", "1.2.3.4:5", "/UDPNOVAWORLD", "/UDPNOVAWORLD",
             "/UDPNOVAWORLD", std::string(40, 'a'), "/UDPNOVAWORLD", "c", "/UDPNOVAWORLD", "d",
             "/UDPNOVAWORLD", "e", "/UDPNOVAWORLD", "f", "/UDPNOVAWORLD"});
    CHECK((udp.udp_novaworld ==
           std::vector<std::string>{"1.2.3.4:5", "/UDPNOVAWORLD", std::string(31, 'a'), "c", "d", "e"}));
    CHECK(parse_launch_flags({"game.exe", "/UDPNOVAWORLD"}).udp_novaworld.empty());
    // /USEJUNCTION and /CLEARJUNCTION: the atol of the next token, the last one standing, -1
    // when absent [orig: @0x4A7603..0x4A762D, @0x4A7632..0x4A765C; -1 in .data @0x829F88].
    const LaunchFlags junction = parse_launch_flags(
            {"game.exe", "/USEJUNCTION", "1", "/usejunction", "0", "/ClearJunction", "2x"});
    CHECK(junction.use_junction == 0 && junction.clear_junction == 2);
    CHECK(parse_launch_flags({"game.exe"}).use_junction == -1);
    CHECK(parse_launch_flags({"game.exe", "/CLEARJUNCTION"}).clear_junction == -1);
    // D-NET-707: /GLSVSSRIMS and /GLSVSSAGRMS, any case: the atol of the next token, the last one
    // standing, set whatever the value (0 included); absent or with no next token, unset
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - @0x4A753A..0x4A7569, @0x4A756F..0x4A759E].
    const LaunchFlags glsvss = parse_launch_flags(
            {"game.exe", "/GLSVSSRIMS", "5000", "/glsvssrims", "0", "/GlsvssAgrms", " 12x"});
    CHECK(glsvss.glsvss_rims == 0 && glsvss.glsvss_agrms == 12);
    CHECK(!parse_launch_flags({"game.exe"}).glsvss_rims.has_value());
    CHECK(!parse_launch_flags({"game.exe", "/GLSVSSAGRMS"}).glsvss_agrms.has_value());
    // The one walk: a switch steps onto its value token, which the switches after it in the
    // walk's order test in place of the switch, and the walk goes on past it. So a value token
    // that names an earlier switch is only a value, one that names a later switch is that
    // switch too, and a token is kept as it stands (no trim) [orig: Game_ParseCommandLineAndInit
    // @0x4a7310 - the order @0x4A74F1..0x4A7632, the value stepped onto @0x4A7503..0x4A750C,
    // @0x4A761E..0x4A7620].
    const LaunchFlags walked = parse_launch_flags({"game.exe", "/USEJUNCTION", "/UDPNOVAWORLD", "1.2.3.4"});
    CHECK(walked.use_junction == 0 && walked.udp_novaworld.empty());
    const LaunchFlags chained = parse_launch_flags({"game.exe", "/POSTPORTNUMBER", "/USEJUNCTION", "5"});
    CHECK(chained.post_port_number == 0 && chained.use_junction == 5);
    const LaunchFlags glsvss_walked = parse_launch_flags({"game.exe", "/GLSVSSRIMS", "/GLSVSSAGRMS", "7"});
    CHECK(glsvss_walked.glsvss_rims == 0 && glsvss_walked.glsvss_agrms == 7);
    const LaunchFlags spaced = parse_launch_flags({"game.exe", "/UDPNOVAWORLD", " 1.2.3.4 ", "/POSTIPADDRESS", " nw "});
    CHECK((spaced.udp_novaworld == std::vector<std::string>{" 1.2.3.4 "}) && spaced.post_ip_address == " nw ");
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
    // The longest name that mounts: 31 bytes and the terminator [orig: @ 0x4a76ca].
    CHECK(kExpansionNameMax == 31 && kExpansionNameMax + 1 == kExpansionNameBytes);
}

// The game's one walk over its command line [orig: Game_ParseCommandLineAndInit @0x4a7310]: the
// line split by its config tokenizer (30 tokens with the program's, [orig: Config_TokenizeLine
// @0x53CB60]), each token tested against every switch in the walk's order, a value-taking switch
// stepping onto its value for the switches after it, an unknown token passed over (D-GAMECFG-7).
void test_the_retail_walk() {
    // /NOPUNT, the whole token in any case [orig: @0x4A7473 -> dword_B4C698 @0x4A747F].
    CHECK(parse_launch_flags({"/nopunt"}).nopunt);
    CHECK(!parse_launch_flags({"/NOPUNTX"}).nopunt && !parse_launch_flags({}).nopunt);
    // /HOST steps onto its value: `/HOST /LAN` names the host file /LAN and sets no /LAN, since
    // the /LAN test, ahead of /HOST in the walk, never sees that token.
    const LaunchFlags host = parse_launch_flags({"/HOST", "/LAN"});
    CHECK(host.host_file == std::optional<std::string>("/LAN") && !host.lan);
    // `/exp /HOST /LAN`: /exp takes /HOST as its value, then the /HOST test, after /exp's in the
    // walk, reads that same token and steps onto /LAN as the host file.
    const LaunchFlags exp_host = parse_launch_flags({"/exp", "/HOST", "/LAN"});
    CHECK(exp_host.expansion == "/HOST" && exp_host.host_file == std::optional<std::string>("/LAN") &&
          !exp_host.lan);
    // `/mod /UDPNOVAWORLD x`: the /UDPNOVAWORLD test, ahead of /mod's, has passed, /mod takes it
    // as its value, and the walk goes on to x, which is no switch: no entry kept.
    const LaunchFlags mod_udp = parse_launch_flags({"/mod", "/UDPNOVAWORLD", "x"});
    CHECK(mod_udp.expansion == "/UDPNOVAWORLD" && mod_udp.udp_novaworld.empty());
    // /maxplayers, /HOST and /PROFILE read their next token with no bound: past the last, "".
    const LaunchFlags tail = parse_launch_flags({"/maxplayers", "12", "/HOST"});
    CHECK(tail.max_players == 12 && tail.host_file == std::optional<std::string>(""));
    CHECK(!parse_launch_flags({}).host_file.has_value() && parse_launch_flags({}).max_players == 0);
    // The startup page's switches and /AA step onto their value: a switch spelled there is read
    // as the value, not the switch [orig: @0x4A7B71, @0x4A7BA5].
    CHECK(!parse_launch_flags({"/URL", "/LAN"}).lan && !parse_launch_flags({"/AA", "/NOPUNT"}).nopunt);
    CHECK(parse_launch_flags({"/URL=x", "/LAN"}).lan);
    // Tokens the game does not know pass, the switches after them still read.
    const LaunchFlags passed = parse_launch_flags({"/STATLOG", "stray", "/w", "/NOHUD"});
    CHECK(passed.no_hud && !passed.lan);

    // The split is the game's tokenizer over the process's own line where it is given: a comma
    // or a tab ends a token, a ';' or "//" ends the line, a quoted run is one token.
    CHECK(parse_launch_flags({"/w,/LAN"}, "game.exe /w,/LAN").lan);
    CHECK(!parse_launch_flags({"/w", ";", "/LAN"}, "game.exe /w ; /LAN").lan);
    CHECK(!parse_launch_flags({}, "game.exe //LAN").lan);
    CHECK(parse_launch_flags({}, "game.exe \"/LAN\"").lan);
    CHECK(retail_command_line_tokens("\"C:\\Program Files\\x\\game.exe\" /exp jox01") ==
          std::vector<std::string>({"C:\\Program Files\\x\\game.exe", "/exp", "jox01"}));
    // The program's own token is never walked.
    CHECK(!parse_launch_flags({}, "/LAN").lan);
    // At most 30 tokens with the program's [orig: the 30 cap @0x53CC8C..0x53CC93]: the 30th
    // runs to the end of the line, so a /LAN past it is that token's text.
    std::string long_line = "game.exe";
    for (int i = 0; i < 28; ++i) long_line += " x";
    CHECK(parse_launch_flags({}, long_line + " /LAN").lan);        // the 30th token itself
    CHECK(!parse_launch_flags({}, long_line + " y /LAN").lan);     // the 30th is "y /LAN"
    // An argument list with no line of its own is spelled as the OS holds one: an argument with
    // a space or a tab quoted, the rest as they are, so the tokenizer splits and cuts it as it
    // does the game's own line.
    CHECK(retail_command_line({"a b", "c,d", "", "e\tf"}) == "opennova \"a b\" c,d \"\" \"e\tf\"");
    CHECK(retail_command_line_tokens(retail_command_line({"a b", "c,d", "/LAN"})) ==
          std::vector<std::string>({"opennova", "a b", "c", "d", "/LAN"}));
    CHECK(parse_launch_flags({"/w,/LAN"}).lan);
    CHECK(!parse_launch_flags({"e;f", "/LAN"}).lan && !parse_launch_flags({"res://x.tscn", "/LAN"}).lan);
    CHECK(parse_launch_flags({"C:\\Program Files\\x", "/LAN"}).lan);

    // The walk's line holds none of OpenNova's own options, which retail's never held: a share's
    // path (its "//") or a ';' in one's value ends nothing, and they count toward no 30 tokens, so
    // the retail switches after them are read (lane G's Q-G2).
    const LaunchFlags unc = parse_launch_flags({"--resource-dir", "//server/share/jo", "/HOST", "host.txt", "/LAN"});
    CHECK(unc.resource_dir == "//server/share/jo" && unc.host_file == std::optional<std::string>("host.txt") &&
          unc.lan);
    const LaunchFlags given = parse_launch_flags({},
            "\"C:\\Games\\opennova.exe\" --resource-dir \"//server/my share\" --lan-rotation a.bms;b.bms "
            "/exp jox01 -- --no-update /LAN");
    CHECK(given.expansion == "jox01" && given.lan);
    CHECK(without_own_options("game.exe --resource-dir //s/x /exp jox01 --loose-root /LAN", nullptr, 0) ==
          "game.exe --resource-dir //s/x /exp jox01 --loose-root /LAN");
    const OwnOption options[] = {{"--resource-dir", true}, {"--loose-root", false}};
    CHECK(without_own_options("game.exe  --Resource-Dir \"//s/my x\" /exp jox01\t--loose-root /LAN", options, 2) ==
          "game.exe /exp jox01 /LAN");
    CHECK(without_own_options("--loose-root /LAN", options, 2) == "--loose-root /LAN"); // the program's token
    std::vector<std::string> many;
    for (int i = 0; i < 15; ++i) {
        many.push_back("--lan-port");
        many.push_back("1");
    }
    many.push_back("/LAN");
    CHECK(parse_launch_flags(many).lan); // 31 arguments ahead of /LAN, none of them the walk's
    // A token of no OpenNova option still ends the line where retail's would.
    CHECK(!parse_launch_flags({"//server/share", "/LAN"}).lan);
}

// What ends an /exp token on the command line [orig: Config_TokenizeLine @ 0x53cb60]: a
// space, tab or comma, a quote, a semicolon; nothing else a name holds.
void test_launch_token_breaks() {
    for (const char c : {' ', '\t', ',', '"', ';'}) CHECK(launch_token_breaks_at(c));
    for (const char c : {'a', 'Z', '0', '.', '-', '_', '/', '\\', ':', '\'', '\x01', '\xe9'})
        CHECK(!launch_token_breaks_at(c));
}

void test_runtime_launch_flags_parse() {
    const LaunchFlags f = parse_launch_flags(
        {"game.exe", "--", "--Mission", " 00TRa.bms ", "--lan-host", "ASH_I5A.BMS",
         "--lan-join", "192.168.10.120:32770", "--lan-port", "32768", "--LAN-GAMETYPE",
         "0x10020", "--lan-mode", "3", "--lan-max-players", "16", "--callsign", "Host",
         "--spectator", "--spectator-password", " watch me ", "--capture-pcap",
         "C:/cap/s.pcapng", "--mcp-port", "8975", "--Working-Dir",
         " C:/p/.opennova/run/1 ", "--NovaWorld-Host", " nw.staging.opennova.net "});
    CHECK(f.mission == "00TRa.bms");
    CHECK(f.lan_host == "ASH_I5A.BMS");
    CHECK(f.lan_rotation.empty());
    // The rotation after --lan-host's mission, in order, the hook's comma form.
    const LaunchFlags rotation =
        parse_launch_flags({"--lan-host", "TDH_I3A.BMS", "--lan-rotation", "TDH_G11A.bms,,ash_i5a.bms"});
    CHECK((rotation.lan_rotation == std::vector<std::string>{"TDH_G11A.bms", "ash_i5a.bms"}));
    CHECK(f.lan_join == "192.168.10.120:32770");
    CHECK(f.lan_port == 32768);
    CHECK(f.lan_gametype == 0x10020);
    CHECK(f.lan_mode == 3);
    CHECK(f.lan_max_players == 16);
    CHECK(f.spectator);
    CHECK(f.spectator_password == "watch me");
    CHECK(f.callsign == "Host");
    CHECK(f.capture_pcap == "C:/cap/s.pcapng");
    CHECK(f.mcp_port == 8975);
    // The directory a source run was started in (ADR 0046 S13 A8: the editor's run directory).
    CHECK(f.working_dir == "C:/p/.opennova/run/1");
    // The OpenNova target's gate, a name or an address, stripped (ADR 0010 d2).
    CHECK(f.novaworld_host == "nw.staging.opennova.net");
    // Absent flags read their sentinels.
    const LaunchFlags none = parse_launch_flags({"game.exe"});
    CHECK(none.mission.empty() && none.lan_host.empty() && none.lan_join.empty());
    CHECK(none.working_dir.empty() && parse_launch_flags({"--working-dir"}).working_dir.empty());
    CHECK(none.lan_port == 0 && none.lan_gametype == -1 && none.lan_mode == 0);
    CHECK(none.lan_max_players == 0 && none.mcp_port == 0);
    CHECK(!none.spectator && none.spectator_password.empty());
    CHECK(none.callsign.empty() && none.capture_pcap.empty() && none.novaworld_host.empty());
    CHECK(parse_launch_flags({"--novaworld-host"}).novaworld_host.empty());
    // The NovaWorld server a mod install's relaunch joins (ADR 0074): a row id, 0 when absent
    // or not one; an own option, which the retail switch walk never sees.
    CHECK(none.novaworld_join == 0);
    CHECK(parse_launch_flags({"--", "--novaworld-join", "4127"}).novaworld_join == 4127);
    CHECK(parse_launch_flags({"--novaworld-join", "0"}).novaworld_join == 0);
    CHECK(parse_launch_flags({"--novaworld-join", "-5"}).novaworld_join == 0);
    CHECK(parse_launch_flags({"--novaworld-join", "x"}).novaworld_join == 0);
    CHECK(parse_launch_flags({"game.exe", "--novaworld-join", "/LAN"}).novaworld_join == 0 &&
          !parse_launch_flags({"game.exe", "--novaworld-join", "/LAN"}).lan);
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
    CHECK(!absent.browse && !bare.browse && !full.browse && !junk.browse);

    // `browse` is no address: the join searches and takes the row on the port.
    const LanEndpoint browse = launch_lan_join_endpoint(
        parse_launch_flags({"--lan-join", "browse:32786"}), 32768);
    CHECK(browse.browse && browse.ip.empty() && browse.port == 32786);
    const LanEndpoint any_row = launch_lan_join_endpoint(parse_launch_flags({"--lan-join", " Browse "}), 0);
    CHECK(any_row.browse && any_row.ip.empty() && any_row.port == 0);
    const LanEndpoint bad_port = launch_lan_join_endpoint(
        parse_launch_flags({"--lan-join", "BROWSE:70000"}), 0);
    CHECK(bad_port.browse && bad_port.ip.empty() && bad_port.port == 0);
}

void test_flags_win_over_fallbacks_and_acidgambit_is_the_default_game() {
    const LaunchFlags none = parse_launch_flags({"game.exe"});
    CHECK(launch_expansion(none, "revx02") == "revx02");
    CHECK(launch_game(none, " DFX ") == "dfx");
    CHECK(launch_game(none, "") == "acidgambit");
    CHECK(none.resource_dir.empty());
    // No /game: the folder's product, a folder with no wizard OpenNova's, a foreign one none.
    const FolderProduct fresh;
    const FolderProduct jo{"jo", "jo", "JOPTR.CMD", "", false};
    const FolderProduct dfx2{"dfx2", "dfx2", "dfx2.cmd", "", false};
    const FolderProduct bhd{"", "", "DFBHD.CMD", "", true};
    const LaunchProduct own = launch_product(none, fresh);
    CHECK(own.product == "acidgambit" && own.supported && own.game == "acidgambit" && own.command == "ACIDGAMBIT.CMD" &&
          own.name == "OP: Acid Gambit");
    const LaunchProduct retail = launch_product(none, dfx2);
    CHECK(retail.product == "dfx2" && retail.supported && retail.game == "dfx2" && retail.command == "dfx2.cmd" &&
          retail.name == "Delta Force Xtreme 2");
    CHECK(launch_product(none, jo).game == "jo");
    // A foreign folder runs nothing, though it still resolves a title (the default) and names the
    // command file its wizard names.
    const LaunchProduct foreign = launch_product(none, bhd);
    CHECK(foreign.product.empty() && !foreign.supported && foreign.game == "acidgambit" && foreign.command == "DFBHD.CMD" &&
          foreign.name.empty());
    // /game wins over the folder, a foreign one too.
    const LaunchProduct chosen = launch_product(parse_launch_flags({"/game", "Jo"}), dfx2);
    CHECK(chosen.product == "jo" && chosen.game == "jo" && chosen.command == "JOPTR.CMD" && chosen.supported);
    CHECK(launch_product(parse_launch_flags({"/game", "jo"}), bhd).supported);
    CHECK(launch_product(parse_launch_flags({"/game", "jo"}), bhd).command == "JOPTR.CMD");
    CHECK(launch_product(parse_launch_flags({"/game", "ACIDGAMBIT"}), dfx2).game == "acidgambit");
    CHECK(launch_product(parse_launch_flags({"/game", "DFX2"}), fresh).game == "dfx2");
    CHECK(!launch_product(parse_launch_flags({"/game", "missing"}), fresh).supported);
    const LaunchProduct tool = launch_product(parse_launch_flags({"/game", "dfx"}), fresh);
    CHECK(!tool.supported && tool.game == "dfx" && tool.command.empty() && tool.name.empty());
    CHECK(launch_game(parse_launch_flags({"/game", "dfx"}), "jo") == "dfx");
    CHECK(launch_game(parse_launch_flags({"/game", "ACIDGAMBIT"}), "dfx2") == "acidgambit");
    CHECK(!launch_product(parse_launch_flags({"/game"}), jo).supported);
    // A player's game (ADR 0074) plays in its own folder alone, as the profile its wizard's GAME
    // names, by its own name and command file; /game names it by its login.
    const FolderProduct player{"mygame", "dfx2", "MYGAME.CMD", "My Game", false};
    const LaunchProduct theirs = launch_product(none, player);
    CHECK(theirs.product == "mygame" && theirs.supported && theirs.game == "dfx2" && theirs.command == "MYGAME.CMD" &&
          theirs.name == "My Game");
    CHECK(launch_product(parse_launch_flags({"/game", "MyGame"}), player).game == "dfx2");
    CHECK(launch_product(parse_launch_flags({"/game", "mygame"}), player).supported);
    CHECK(!launch_product(parse_launch_flags({"/game", "mygame"}), jo).supported);
    CHECK(!launch_product(parse_launch_flags({"/game", "mygame"}), fresh).supported);
    // /game jo there is JO's, as anywhere.
    const LaunchProduct jo_there = launch_product(parse_launch_flags({"/game", "jo"}), player);
    CHECK(jo_there.supported && jo_there.game == "jo" && jo_there.command == "JOPTR.CMD" &&
          jo_there.name == "Joint Operations");
    CHECK(parse_launch_flags({"--NO-UPDATE"}).no_update && !none.no_update);
    CHECK(parse_launch_flags({"--", "--after-update"}).after_update && !none.after_update);
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

// The product a wizard names (wizard_product): a stock wizard's CMD names its title, JO:CA's, the
// Steam DFX2's and OpenNova content's (each title's command_file); any other (BHD's, an
// expansion's) none.
void test_wizard_product() {
    const auto named = [](const char *cmd, const char *login = "", const char *game = "") {
        update::UpdateWizard w;
        w.cmd = cmd;
        w.product_login = login;
        w.game = game;
        return wizard_product(w);
    };
    const WizardProduct jo = named("JOPTR.CMD", "JOTRqq");
    CHECK(jo.product == "jo" && jo.game == "jo" && !jo.native);
    const WizardProduct dfx2 = named(" dfx2.cmd ");
    CHECK(dfx2.product == "dfx2" && dfx2.game == "dfx2" && !dfx2.native);
    const WizardProduct own = named("ACIDGAMBIT.CMD");
    CHECK(own.product == "acidgambit" && own.game == "acidgambit" && own.native);
    CHECK(named("DFBHD.CMD").product.empty() && named("JOEPTR.CMD").product.empty() &&
            named("").product.empty());
    // A player's game (ADR 0074): any other CMD with GAME jo or dfx2 and a login, named by its
    // login lower-cased, native. GAME on a stock CMD changes nothing; another profile, no login
    // or no CMD names none.
    const WizardProduct player = named("MYGAME.CMD", " MyGame ", "DFX2");
    CHECK(player.product == "mygame" && player.game == "dfx2" && player.native);
    CHECK(named("JOPTR.CMD", "JOTRqq", "dfx2").game == "jo");
    CHECK(named("MYGAME.CMD", "MYGAME", "bhd").product.empty());
    CHECK(named("MYGAME.CMD", "", "jo").product.empty());
    // GAME names any title a stock wizard names, OP: Acid Gambit's among them; an unwitnessed one
    // (no command file) none.
    const WizardProduct ours = named("MYGAME.CMD", "MyGame", "AcidGambit");
    CHECK(ours.product == "mygame" && ours.game == "acidgambit" && ours.native);
    CHECK(named("MYGAME.CMD", "MYGAME", "jodemo").product.empty());
    CHECK(named("", "MYGAME", "jo").product.empty());
}

// The product a folder holds, from its UPDATE.WIZ (found case-insensitively): JO:CA's and
// the Steam DFX2's wizards name theirs, OpenNova content's its own, a player's game's its
// login with its GAME (ADR 0074); a folder with no wizard none; a wizard naming any other game
// (BHD's DFBHD.CMD) a foreign folder. ADR 0067 d2.
void test_folder_product() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(test_paths_temp_dir()) /
            ("opennova_folder_product_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct TempRoot {
        fs::path path;
        ~TempRoot() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } const cleanup{root};
    const auto write = [&](const fs::path &file, const char *text) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file.string(), std::ios::binary) << text;
    };
    const auto folder = [&](const char *name, const char *wizard_name, const char *text) {
        const fs::path dir = root / name;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (wizard_name) write(dir / wizard_name, text);
        return dir.string();
    };
    const FolderProduct jo = folder_product(folder("jo", "UPDATE.WIZ",
            "[HEADER]\r\nCMD=JOPTR.CMD\r\nURL=ftp.novaworld.com\r\nPRODUCT_LOGIN=JOTRqq\r\n"));
    CHECK(jo.product == "jo" && jo.game == "jo" && jo.command == "JOPTR.CMD" && jo.name.empty() && !jo.foreign);
    const FolderProduct dfx2 =
            folder_product(folder("dfx2", "update.wiz", "[HEADER]\r\nCMD=DFX2.CMD\r\nPRODUCT_LOGIN=DFX2\r\n"));
    CHECK(dfx2.product == "dfx2" && dfx2.game == "dfx2" && !dfx2.foreign);
    const FolderProduct own = folder_product(folder("acidgambit", "UPDATE.WIZ", "[HEADER]\r\nCMD=ACIDGAMBIT.CMD\r\n"));
    CHECK(own.product == "acidgambit" && own.game == "acidgambit" && own.name.empty() && !own.foreign);
    const FolderProduct bhd = folder_product(folder("bhd", "Update.Wiz", "[HEADER]\r\nCMD=DFBHD.CMD\r\n"));
    CHECK(bhd.foreign && bhd.product.empty() && bhd.command == "DFBHD.CMD");
    CHECK(folder_product(folder("unread", "UPDATE.WIZ", "")).foreign);
    const FolderProduct fresh = folder_product(folder("fresh", nullptr, ""));
    CHECK(fresh.product.empty() && !fresh.foreign && fresh.command.empty());
    const FolderProduct absent = folder_product((root / "absent").string());
    CHECK(absent.product.empty() && !absent.foreign);
    CHECK(folder_product("").product.empty() && !folder_product("").foreign);
    // A player's game: a fresh download's wizard names it by its login, its installed command
    // file by its release's PRODUCT. GAME is case-blind; another profile is no player game's.
    const std::string player = folder("player", "UPDATE.WIZ",
            "[HEADER]\r\nCMD=MYGAME.CMD\r\nURL=update.example.net\r\nPRODUCT_LOGIN=MyGame\r\nGAME=DFX2\r\n");
    const FolderProduct downloaded = folder_product(player);
    CHECK(downloaded.product == "mygame" && downloaded.game == "dfx2" && downloaded.command == "MYGAME.CMD" &&
          downloaded.name == "MyGame" && !downloaded.foreign);
    write(fs::path(player) / "MYGAME.CMD", "[HEADER]\r\nPRODUCT=My Game\r\nVERSION=1\r\n\r\n[COMMAND]\r\n");
    CHECK(folder_product(player).name == "My Game");
    CHECK(folder_product(folder("bhd_game", "UPDATE.WIZ",
            "[HEADER]\r\nCMD=MYGAME.CMD\r\nPRODUCT_LOGIN=MYGAME\r\nGAME=bhd\r\n")).foreign);
    CHECK(folder_product(folder("no_login", "UPDATE.WIZ", "[HEADER]\r\nCMD=MYGAME.CMD\r\nGAME=jo\r\n")).foreign);
}

// The release a launch's product has installed: its command file's [HEADER] VERSION in the launch
// folder, the file the updater's version mark reads; none without the file, without a VERSION, or
// with one no Version takes.
void test_launch_product_version() {
	namespace fs = std::filesystem;
	const fs::path root = fs::path(test_paths_temp_dir()) /
			("opennova_product_version_" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct TempRoot {
		fs::path path;
		~TempRoot() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	} const cleanup{ root };
	const auto folder = [&](const char *name, const char *command, const char *text) {
		const fs::path dir = root / name;
		std::error_code ec;
		fs::create_directories(dir, ec);
		if (command)
			std::ofstream((dir / command).string(), std::ios::binary) << text;
		return dir.string();
	};
	const LaunchFlags none = parse_launch_flags({});
	// OP: Acid Gambit's folder, which holds no wizard: ACIDGAMBIT.CMD.
	const std::string own = folder("acidgambit", "ACIDGAMBIT.CMD",
			"[HEADER]\r\nEXE=opennova.exe\r\nPRODUCT=OP: Acid "
			"Gambit\r\nVERSION=0.1.4\r\n\r\n[COMMAND]\r\n");
	const std::optional<Version> acid =
			launch_product_version(own, launch_product(none, folder_product(own)));
	CHECK(acid && acid->text() == "0.1.4");
	// JO's: JOPTR.CMD, its wizard's.
	const std::string jo =
			folder("jo", "JOPTR.CMD", "[HEADER]\r\nVERSION=1.0.1\r\n\r\n[COMMAND]\r\n");
	std::ofstream((fs::path(jo) / "UPDATE.WIZ").string(), std::ios::binary)
			<< "[HEADER]\r\nCMD=JOPTR.CMD\r\n";
	const LaunchProduct jo_product = launch_product(none, folder_product(jo));
	CHECK(jo_product.command == "JOPTR.CMD");
	const std::optional<Version> jo_version = launch_product_version(jo, jo_product);
	CHECK(jo_version && jo_version->text() == "1.0.1" && jo_version->packed() == 0x01000100u);
	// A /game launched in another game's folder reads that product's file, which is not there.
	CHECK(!launch_product_version(
			own, launch_product(parse_launch_flags({ "/game", "jo" }), folder_product(own))));
	// No command file, one with no VERSION, one whose VERSION no Version takes, one with no
	// [HEADER].
	const std::string fresh = folder("fresh", nullptr, "");
	CHECK(!launch_product_version(fresh, launch_product(none, folder_product(fresh))));
	const LaunchProduct acid_product = launch_product(none, FolderProduct{});
	CHECK(acid_product.command == "ACIDGAMBIT.CMD");
	CHECK(!launch_product_version(
			folder("unversioned", "ACIDGAMBIT.CMD", "[HEADER]\r\nPRODUCT=X\r\n\r\n[COMMAND]\r\n"),
			acid_product));
	CHECK(!launch_product_version(
			folder("bad", "ACIDGAMBIT.CMD", "[HEADER]\r\nVERSION=V1.0\r\n\r\n[COMMAND]\r\n"),
			acid_product));
	CHECK(!launch_product_version(
			folder("headless", "ACIDGAMBIT.CMD", "VERSION=1.0\r\n"), acid_product));
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

// The updater's command lines (updater_check_arguments / updater_install_arguments): the product
// roots, the relaunch's arguments, a first-run install's --after-update and a mod install's
// --novaworld-join behind one `--`, an earlier launch's markers never carried on.
void test_updater_arguments() {
    using V = std::vector<std::string>;
    const UpdaterProduct product{"acidgambit", "C:/Runtime", "D:/Game Data"};
    CHECK(updater_check_arguments(product) == V({"--check", "/game", "acidgambit", "--runtime-dir",
            "C:/Runtime", "--resource-dir", "D:/Game Data"}));
    const V run{"--", "/game", "acidgambit", "/exp", "onx"};
    CHECK(updater_install_arguments(product, 1234, run, false, 0) ==
          V({"/RUN", "--parent-pid", "1234", "/game", "acidgambit", "--runtime-dir", "C:/Runtime",
                  "--resource-dir", "D:/Game Data", "--run-arg", "--", "--run-arg", "/game",
                  "--run-arg", "acidgambit", "--run-arg", "/exp", "--run-arg", "onx"}));
    const auto count = [](const V &v, const char *what) {
        return std::count(v.begin(), v.end(), std::string(what));
    };
    const V marked = updater_install_arguments(product, 1234, run, true, 0);
    CHECK(V(marked.end() - 2, marked.end()) == V({"--run-arg", "--after-update"}));
    const V stale{"--verbose", "--", "/exp", "onx", "--after-update"};
    CHECK(count(updater_install_arguments(product, 1234, stale, false, 0), "--after-update") == 0);
    const V again = updater_install_arguments(product, 1234, stale, true, 0);
    CHECK(count(again, "--after-update") == 1 && count(again, "--") == 1);
    const V bare = updater_install_arguments(product, 1234, {}, true, 0);
    CHECK(V(bare.begin() + 9, bare.end()) == V({"--run-arg", "--", "--run-arg", "--after-update"}));
    const V joined = updater_install_arguments(product, 1234, run, false, 42);
    CHECK(V(joined.end() - 4, joined.end()) == V({"--run-arg", "--novaworld-join", "--run-arg", "42"}));
    CHECK(count(joined, "--") == 1);
    const V old_join{"--", "/exp", "onx", "--novaworld-join", "7"};
    const V later = updater_install_arguments(product, 1234, old_join, false, 0);
    CHECK(count(later, "--novaworld-join") == 0 && count(later, "7") == 0);
    const V both = updater_install_arguments(product, 1234, old_join, true, 9);
    CHECK(V(both.end() - 6, both.end()) ==
          V({"--run-arg", "--after-update", "--run-arg", "--novaworld-join", "--run-arg", "9"}));
    CHECK(count(both, "--novaworld-join") == 1);
}

} // namespace

int main() {
    test_flags_parse_case_insensitively_with_values();
    test_mod_is_exp_and_the_name_keeps_32_bytes();
    test_the_retail_walk();
    test_launch_token_breaks();
    test_runtime_launch_flags_parse();
    test_lan_fallbacks_and_join_endpoint();
    test_flags_win_over_fallbacks_and_acidgambit_is_the_default_game();
    test_repeated_flags_take_the_last_value();
    test_path_join();
    test_wizard_product();
    test_folder_product();
    test_launch_product_version();
    test_mount_install();
    test_updater_arguments();
    if (failures == 0) std::printf("boot_policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
