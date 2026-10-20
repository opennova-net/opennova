// Launch flags and path helpers -- see boot_policy.h.

#include <base/resource_index/boot_policy.h>

#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>

#include <cerrno>
#include <cstdlib>

namespace opennova {

namespace {

// ASCII-only by design (io/strutil.h): flag matching never follows the locale.
bool has_flag(const std::vector<std::string> &args, const char *flag) {
    for (const std::string &a : args)
        if (strutil::iequals(a, flag)) return true;
    return false;
}

// The token following `flag`, stripped, or "" when absent/empty; a flag given
// twice takes its last value, as the game's one walk over its command line
// copies each `/exp` value over the one before
// [orig: Game_ParseCommandLineAndInit @ 0x4a7310, "/exp" @ 0x4a76a6, the next
//  token @ 0x4a76b8..0x4a76c3 copied to g_ExpansionName @ 0x4a76cf].
std::string value_after(const std::vector<std::string> &args, const char *flag) {
    std::string value;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (strutil::iequals(args[i], flag)) value = strutil::trim(args[i + 1]);
    return value;
}

// The NovaWorld switches as the game's one walk over its tokens reads them, in the walk's order:
// at each token every switch is tested in turn, and a switch with a next token steps onto it, so
// the switches after it test that value token in place of the switch (`/USEJUNCTION
// /UDPNOVAWORLD 1.2.3.4` reads "/UDPNOVAWORLD" as the junction word, atol 0, and keeps no
// entry), and the walk goes on past it. Each value is the token as it stands (no trim); the last
// one stands, the /UDPNOVAWORLD entries keeping the first six to 31 characters, a seventh's token
// stepped over all the same. /GLSVSSRIMS and /GLSVSSAGRMS keep the atol of theirs (D-NET-707).
// The switches outside this block keep the shared walk (D-GAMECFG-7).
// [orig: Game_ParseCommandLineAndInit @0x4a7310 - "/UDPNOVAWORLD" @0x4A74F1 (the next token
//  @0x4A7503..0x4A750C, the count test @0x4A750E..0x4A7516, Napi_CopyString(.., 0x20)
//  @0x4A751C..0x4A7528, the position reloaded @0x4A7536), "/GLSVSSRIMS" @0x4A753A,
//  "/GLSVSSAGRMS" @0x4A756F, "/POSTIPADDRESS" @0x4A75A4..0x4A75CF, "/POSTPORTNUMBER"
//  @0x4A75D4..0x4A75FE, "/USEJUNCTION" @0x4A7603..0x4A762D, "/CLEARJUNCTION"
//  @0x4A7632..0x4A765C]
void parse_novaworld_switches(const std::vector<std::string> &args, LaunchFlags &f) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto takes = [&](const char *flag) {
            if (!strutil::iequals(args[i], flag) || i + 1 >= args.size()) return false;
            ++i;
            return true;
        };
        if (takes(kLaunchFlagUdpNovaWorld) && f.udp_novaworld.size() < kLaunchUdpNovaWorldMax)
            f.udp_novaworld.push_back(args[i].substr(0, kLaunchUdpNovaWorldChars));
        if (takes(kLaunchFlagGlsvssRims)) f.glsvss_rims = io::retail_atol(args[i].c_str());
        if (takes(kLaunchFlagGlsvssAgrms)) f.glsvss_agrms = io::retail_atol(args[i].c_str());
        if (takes(kLaunchFlagPostIpAddress)) f.post_ip_address = args[i];
        if (takes(kLaunchFlagPostPortNumber)) f.post_port_number = io::retail_atol(args[i].c_str());
        if (takes(kLaunchFlagUseJunction)) f.use_junction = io::retail_atol(args[i].c_str());
        if (takes(kLaunchFlagClearJunction)) f.clear_junction = io::retail_atol(args[i].c_str());
    }
}

// The server log switches in the game's walk order: /INOUT, then /PROFILE, which steps onto its
// value token, then /CHEATLOG, /PUNT.TXT, /PUNTLOG and /NETLOG, tested against that token when
// /PROFILE took one, as every later test of the walk's iteration is.
// [orig: Game_ParseCommandLineAndInit @0x4a7310 - "/INOUT" @0x4a743d; "/PROFILE" @0x4a778f,
//  the step `add edi, ebx` @0x4a77a1 and the copy into g_ProfileLogPath @0x4a77a7..0x4a77b9,
//  g_RunningWithProfile @0x4a77bb; "/CHEATLOG" @0x4a77fc; "/PUNT.TXT" @0x4a782d; "/PUNTLOG"
//  @0x4a7863; "/NETLOG" @0x4a7a11]
void parse_log_switches(const std::vector<std::string> &args, LaunchFlags &f) {
    LogSwitches &s = f.log_switches;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (strutil::iequals(args[i], kLaunchFlagInout)) s.inout = true;
        if (strutil::iequals(args[i], kLaunchFlagProfile) && i + 1 < args.size()) {
            ++i;
            s.profile_path = args[i];
        }
        if (strutil::iequals(args[i], kLaunchFlagCheatLog)) s.cheat_log = true;
        if (strutil::iequals(args[i], kLaunchFlagPuntTxt)) s.punt_log = true;
        if (strutil::iequals(args[i], kLaunchFlagPuntLog)) s.punt_log = true;
        if (strutil::iequals(args[i], kLaunchFlagNetLog)) s.net_log = true;
    }
}

// `/mod` and `/exp` share one arm of the game's walk [orig:
// Game_ParseCommandLineAndInit @ 0x4a7310, the two `stricmp`s @ 0x4a76ac]: with
// a next token the walk steps onto it (@ 0x4a76c1), so that token is never read
// as a flag, and copies it into the 32-byte `g_ExpansionName @ 0xb4c584` with
// `strncpy(.., 0x20)` (@ 0x4a76cf), padding a shorter name with NULs. Every
// later `/mod` or `/exp` copies over the one before; a last one with no token
// after it copies nothing.
std::string expansion_after(const std::vector<std::string> &args) {
    std::string name;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!strutil::iequals(args[i], "/mod") && !strutil::iequals(args[i], kLaunchFlagExpansion))
            continue;
        if (i + 1 >= args.size()) continue;
        ++i;
        name = launch_expansion_name(args[i]);
    }
    return name;
}

// A whole decimal or 0x-prefixed integer, or `fallback` for an empty,
// non-numeric or trailing-garbage token.
long parse_long(const std::string &text, long fallback) {
    if (text.empty()) return fallback;
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0') return fallback;
    return value;
}

// The integer following `flag` when it lies in [lo, hi], else `fallback`.
int int_after(const std::vector<std::string> &args, const char *flag, long lo, long hi,
              int fallback) {
    const long value = parse_long(value_after(args, flag), fallback);
    if (value < lo || value > hi) return fallback;
    return static_cast<int>(value);
}

} // namespace

LaunchFlags parse_launch_flags(const std::vector<std::string> &args) {
    LaunchFlags f;
    f.loose_override = has_flag(args, kLaunchFlagLooseOverride);
    f.expansion = expansion_after(args);
    f.game = strutil::to_lower(value_after(args, kLaunchFlagGame));
    f.resource_dir = value_after(args, kLaunchFlagResourceDir);
    f.resource_dir_given = has_flag(args, kLaunchFlagResourceDir);
    f.loose_mission = value_after(args, kLaunchFlagLooseMission);
    f.loose_root = has_flag(args, kLaunchFlagLooseRoot);
    f.mission = value_after(args, kLaunchFlagMission);
    f.lan_host = value_after(args, kLaunchFlagLanHost);
    f.lan_join = value_after(args, kLaunchFlagLanJoin);
    f.lan_port = int_after(args, kLaunchFlagLanPort, 1, 65535, 0);
    f.lan_gametype = int_after(args, kLaunchFlagLanGametype, 0, 0x7fffffffL, -1);
    f.lan_mode = int_after(args, kLaunchFlagLanMode, 1, 4, 0);
    f.lan_max_players = int_after(args, kLaunchFlagLanMaxPlayers, 1, 64, 0);
    f.spectator = has_flag(args, kLaunchFlagSpectator);
    f.spectator_password = value_after(args, kLaunchFlagSpectatorPassword);
    f.callsign = value_after(args, kLaunchFlagCallsign);
    f.capture_pcap = value_after(args, kLaunchFlagCapturePcap);
    f.mcp_port = int_after(args, kLaunchFlagMcpPort, 1, 65535, 0);
    f.novaworld_host = value_after(args, kLaunchFlagNovaWorldHost);
    f.working_dir = value_after(args, kLaunchFlagWorkingDir);
    f.no_hud = has_flag(args, kLaunchFlagNoHud); // [orig: @0x4A79F6..0x4A7A09]
    f.no_reload = has_flag(args, kLaunchFlagNoReload); // [orig: @0x4A76D7..0x4A76E9]
    f.lan = has_flag(args, kLaunchFlagLan); // [orig: @0x4A74D9..0x4A74EB]
    parse_novaworld_switches(args, f);
    parse_log_switches(args, f);
    return f;
}

int parse_log_switch(const std::vector<std::string> &args, std::size_t i, LogSwitches &out,
        std::string &error) {
    const std::string &a = args[i];
    if (strutil::iequals(a, kLaunchFlagProfile)) {
        if (i + 1 >= args.size()) {
            error = a + " needs a value";
            return -1;
        }
        out.profile_path = args[i + 1];
        return 2;
    }
    if (strutil::iequals(a, kLaunchFlagPuntTxt) || strutil::iequals(a, kLaunchFlagPuntLog)) {
        out.punt_log = true;
        return 1;
    }
    if (strutil::iequals(a, kLaunchFlagCheatLog)) {
        out.cheat_log = true;
        return 1;
    }
    if (strutil::iequals(a, kLaunchFlagNetLog)) {
        out.net_log = true;
        return 1;
    }
    if (strutil::iequals(a, kLaunchFlagInout)) {
        out.inout = true;
        return 1;
    }
    return 0;
}

int launch_lan_port(const LaunchFlags &flags, int fallback) {
    return flags.lan_port > 0 ? flags.lan_port : fallback;
}

int launch_lan_mode(const LaunchFlags &flags, int fallback) {
    return flags.lan_mode > 0 ? flags.lan_mode : fallback;
}

int launch_lan_max_players(const LaunchFlags &flags, int fallback) {
    return flags.lan_max_players > 0 ? flags.lan_max_players : fallback;
}

LanEndpoint launch_lan_join_endpoint(const LaunchFlags &flags, int default_port) {
    LanEndpoint endpoint;
    endpoint.port = default_port;
    const std::string &target = flags.lan_join;
    if (target.empty()) return endpoint;
    const std::size_t colon = target.find(':');
    endpoint.ip = strutil::trim(colon == std::string::npos ? target : target.substr(0, colon));
    if (strutil::iequals(endpoint.ip, "browse")) {
        endpoint.browse = true;
        endpoint.ip.clear();
    }
    if (colon == std::string::npos) return endpoint;
    const long port = parse_long(strutil::trim(target.substr(colon + 1)), 0);
    if (port >= 1 && port <= 65535) endpoint.port = static_cast<int>(port);
    return endpoint;
}

std::string launch_expansion_name(const std::string &token) {
    const std::string name = strutil::trim(token);
    // [orig: Game_ParseCommandLineAndInit @ 0x4a76cf] strncpy(g_ExpansionName, token, 0x20)
    return name.size() > kExpansionNameBytes ? name.substr(0, kExpansionNameBytes) : name;
}

bool launch_token_breaks_at(char c) {
    // [orig: Config_TokenizeLine @ 0x53cb60: ' ', '\t' and ',' split outside quotes
    //  @ 0x53cc44, '"' toggles quoting and is never kept @ 0x53cc51, ';' ends the line @ 0x53cc31]
    return c == ' ' || c == '\t' || c == ',' || c == '"' || c == ';';
}

std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback) {
    return flags.expansion.empty() ? fallback : flags.expansion;
}

std::string launch_game(const LaunchFlags &flags, const std::string &fallback) {
    if (!flags.game.empty()) return flags.game;
    const std::string fb = strutil::to_lower(strutil::trim(fallback));
    return fb.empty() ? std::string("jo") : fb;
}

std::string boot_path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

bool mount_install(Vfs &vfs, const std::string &root, const LaunchFlags &flags) {
    vfs.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(launch_game(flags, std::string()).c_str()));
    const VfsMountMode mode =
            flags.loose_override ? VfsMountMode::PackedWithLooseOverride : VfsMountMode::Packed;
    return vfs.mount_game(root, flags.expansion, mode, VfsArchiveDiscovery::RetailTable) &&
           vfs.has_mounted_archive();
}

} // namespace opennova
