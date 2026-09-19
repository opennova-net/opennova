// Launch flags and path helpers -- see boot_policy.h.

#include <base/resource_index/boot_policy.h>

#include <base/io/strutil.h>

#include <cerrno>
#include <cstdlib>

namespace opennova {

namespace {

// ASCII-only by design (io/strutil.h): flag matching never follows the locale.
std::string lower_ascii(const std::string &s) { return strutil::to_lower(s); }
std::string strip(const std::string &s) { return strutil::trim(s); }

bool has_flag(const std::vector<std::string> &args, const char *flag) {
    const std::string wanted = lower_ascii(flag);
    for (const std::string &a : args)
        if (lower_ascii(a) == wanted) return true;
    return false;
}

// The token following `flag`, stripped, or "" when absent/empty.
std::string value_after(const std::vector<std::string> &args, const char *flag) {
    const std::string wanted = lower_ascii(flag);
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (lower_ascii(args[i]) == wanted) return strip(args[i + 1]);
    return std::string();
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
    f.loose_override = has_flag(args, "/d");
    f.expansion = value_after(args, "/exp");
    f.game = lower_ascii(value_after(args, "/game"));
    f.resource_dir = value_after(args, "--resource-dir");
    f.loose_mission = value_after(args, "--loose-mission");
    f.loose_root = has_flag(args, "--loose-root");
    f.mission = value_after(args, "--mission");
    f.lan_host = value_after(args, "--lan-host");
    f.lan_join = value_after(args, "--lan-join");
    f.lan_port = int_after(args, "--lan-port", 1, 65535, 0);
    f.lan_gametype = int_after(args, "--lan-gametype", 0, 0x7fffffffL, -1);
    f.lan_mode = int_after(args, "--lan-mode", 1, 4, 0);
    f.lan_max_players = int_after(args, "--lan-max-players", 1, 64, 0);
    f.spectator = has_flag(args, "--spectator");
    f.spectator_password = value_after(args, "--spectator-password");
    f.callsign = value_after(args, "--callsign");
    f.integrity_profile = value_after(args, "--integrity-profile");
    f.capture_pcap = value_after(args, "--capture-pcap");
    f.mcp_port = int_after(args, "--mcp-port", 1, 65535, 0);
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
        endpoint.ip = strip(target);
        return endpoint;
    }
    endpoint.ip = strip(target.substr(0, colon));
    const long port = parse_long(strip(target.substr(colon + 1)), 0);
    if (port >= 1 && port <= 65535) endpoint.port = static_cast<int>(port);
    return endpoint;
}

std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback) {
    return flags.expansion.empty() ? fallback : flags.expansion;
}

std::string launch_game(const LaunchFlags &flags, const std::string &fallback) {
    if (!flags.game.empty()) return flags.game;
    const std::string fb = lower_ascii(strip(fallback));
    return fb.empty() ? std::string("jo") : fb;
}

std::string boot_path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

} // namespace opennova
