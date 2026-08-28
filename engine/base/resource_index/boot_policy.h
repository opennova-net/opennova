#pragma once

// The original-engine launch flags the runtime honors and the boot resource
// directory ladder they feed. These mirror Jointops.exe's command line, so a
// user launches the runtime the same way:
//
//   /d            dev mode: loose files next to the PFFs override the packed
//                 entries. Without it the runtime reads from PFFs exclusively.
//   /exp <name>   mount the expansion <name> (e.g. "jox01") over the base game.
//   /game <code>  which game the data is from (e.g. "jo", "jodemo"); selects
//                 the SCR decode key. Defaults to "jo" when absent.
//   --resource-dir <absolute path>
//                 use this resource directory for this process without
//                 changing the runtime's persisted preference.
//   --loose-mission <name.bms>
//                 boot the exact top-level loose BMS from --resource-dir.
//   --loose-root  ONED/dev runs: when the directory holds none of the packed
//                 game archives, mount it as loose files instead of failing
//                 the boot. Ordinary standalone launches omit it, keeping
//                 retail's no-archives fatal error (ADR 0025).
//
// The runtime launch vocabulary (dev, probe and automation launches; the
// retail exe has no equivalents — these are OpenNova extensions, ADR 0041):
//
//   --mission <name.bms>        boot straight into that single-player mission
//                               through the menu's Start path (no navigation).
//   --lan-host <mission.bms>    boot as a co-op LAN listen host on that mission.
//   --lan-join <ip[:port]>      boot as a LAN joiner dialing that host.
//   --lan-port <n>              host bind port (1..65535; else the retail default).
//   --lan-gametype <n|0xN>      numeric g_GameType override; absent = auto
//                               (derived from the mission).
//   --lan-mode <1..4>           host LAN rate mode (the witnessed holdoffs).
//   --lan-max-players <1..64>   listen-host capacity.
//   --callsign <name>           the local player's callsign (wire clamp applies).
//   --integrity-profile <name>  the session's integrity profile.
//   --capture-pcap <path>       record this session's datagrams to a pcap.
//   --mcp-port <n>              host the runtime MCP endpoint on that loopback
//                               port (1..65535); absent = no endpoint.
//
// Flags match case-insensitively; a flag's value is the following token,
// stripped. Integers read decimal or 0x-prefixed; an absent, empty or
// malformed value reads the field's "unset" sentinel. The embedder hands over
// the whole token list (engine and user args alike) so packaged and source
// launches share one parser.

#include <functional>
#include <string>
#include <vector>

namespace opennova {

struct LaunchFlags {
    bool loose_override = false;   // /d
    std::string expansion;         // /exp <name>
    std::string game;              // /game <code>, lowercased
    std::string resource_dir;      // --resource-dir <path>
    std::string loose_mission;     // --loose-mission <name.bms>
    bool loose_root = false;       // --loose-root
    std::string mission;           // --mission <name.bms>
    std::string lan_host;          // --lan-host <mission.bms>
    std::string lan_join;          // --lan-join <ip[:port]>, verbatim
    int lan_port = 0;              // --lan-port; 0 = unset (1..65535 accepted)
    int lan_gametype = -1;         // --lan-gametype; -1 = auto (>= 0 accepted)
    int lan_mode = 0;              // --lan-mode; 0 = unset (1..4 accepted)
    int lan_max_players = 0;       // --lan-max-players; 0 = unset (1..64 accepted)
    std::string callsign;          // --callsign <name>
    std::string integrity_profile; // --integrity-profile <name>
    std::string capture_pcap;      // --capture-pcap <path>
    int mcp_port = 0;              // --mcp-port; 0 = no endpoint (1..65535 accepted)
};

LaunchFlags parse_launch_flags(const std::vector<std::string> &args);

// The LAN fallbacks: a set flag wins, an unset one yields the caller's default.
int launch_lan_port(const LaunchFlags &flags, int fallback);
int launch_lan_mode(const LaunchFlags &flags, int fallback);
int launch_lan_max_players(const LaunchFlags &flags, int fallback);

// A joiner's dial target from `--lan-join <ip[:port]>`: no flag reads an
// empty ip; a missing or malformed port reads `default_port`.
struct LanEndpoint {
    std::string ip;
    int port = 0;
};
LanEndpoint launch_lan_join_endpoint(const LaunchFlags &flags, int default_port);

// A launch flag always wins over a persisted (possibly stale) fallback.
std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback);
// Lowercased; the persisted fallback next, then "jo" — the absence of any
// choice is the JO default. An unknown code resolves to the JO default
// downstream (gameprofile_scr_policy_for_code).
std::string launch_game(const LaunchFlags &flags, const std::string &fallback);
// The process-local override; callers must not persist it.
std::string launch_resource_dir(const LaunchFlags &flags, const std::string &fallback);

// The embedder's absolute-path filesystem probes.
struct BootDirProbe {
    std::function<bool(const std::string &path)> file_exists;
    std::function<bool(const std::string &path)> dir_exists;
};

// `dir/name`, with an empty dir yielding the bare name and a trailing
// separator not doubled.
std::string boot_path_join(const std::string &dir, const std::string &name);

// `exe_dir` when it holds any boot-table archive (vfs.h kBootArchiveTable) —
// a shipped game dir — else "".
std::string bundled_game_dir(const std::string &exe_dir, const BootDirProbe &fs);

// The loose game sources bundled beside a shipped exe: `<exe_dir>/assets`
// when it exists, else "".
std::string bundled_assets_dir(const std::string &exe_dir, const BootDirProbe &fs);

// The boot resource dir, in priority order: the --resource-dir flag, the
// persisted pick, then the game bundled around a shipped exe — the exe's own
// directory when it carries a boot archive (the tagged release zip,
// retail-style), else the loose assets/ beside it (the dev zip, where the
// game plays the same tree ONED exposes). The bundled defaults are per-boot
// and never persisted; dev runs from the editor are unchanged (its binary's
// dir carries neither). "" means ask.
std::string boot_resource_dir(const LaunchFlags &flags, const std::string &persisted,
                              const std::string &exe_dir, const BootDirProbe &fs);

// Whether `dir` may fall back to a loose mount when it holds no packed
// archives: the explicit --loose-root flag (ADR 0025), or the bundled loose
// assets/ default itself — the dev zip ships sources only, and blessing
// exactly that directory keeps a picked or persisted loose dir on retail's
// no-archives fatal.
bool boot_loose_allowed(const LaunchFlags &flags, const std::string &dir,
                        const std::string &exe_dir, const BootDirProbe &fs);

} // namespace opennova
