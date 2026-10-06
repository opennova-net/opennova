// Launch flags and path helpers -- see boot_policy.h.

#include <base/resource_index/boot_policy.h>

#include <base/gameprofile/gameprofile.h>
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
    f.integrity_profile = value_after(args, kLaunchFlagIntegrityProfile);
    f.capture_pcap = value_after(args, kLaunchFlagCapturePcap);
    f.mcp_port = int_after(args, kLaunchFlagMcpPort, 1, 65535, 0);
    f.working_dir = value_after(args, kLaunchFlagWorkingDir);
    f.no_hud = has_flag(args, kLaunchFlagNoHud); // [orig: @0x4A79F6..0x4A7A09]
    return f;
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
    if (colon == std::string::npos) {
        endpoint.ip = strutil::trim(target);
        return endpoint;
    }
    endpoint.ip = strutil::trim(target.substr(0, colon));
    const long port = parse_long(strutil::trim(target.substr(colon + 1)), 0);
    if (port >= 1 && port <= 65535) endpoint.port = static_cast<int>(port);
    return endpoint;
}

std::string launch_expansion_name(const std::string &token) {
    const std::string name = strutil::trim(token);
    // [orig: Game_ParseCommandLineAndInit @ 0x4a76cf] strncpy(g_ExpansionName, token, 0x20)
    return name.size() > kExpansionNameBytes ? name.substr(0, kExpansionNameBytes) : name;
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
