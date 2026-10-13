#pragma once

// The original-engine launch flags the runtime honors and the boot resource
// directory ladder they feed. These mirror Jointops.exe's command line, so a
// user launches the runtime the same way:
//
//   /d            dev mode: loose files next to the PFFs override the packed
//                 entries. Without it the runtime reads from PFFs exclusively.
//   /exp <name>   mount the expansion <name> (e.g. "jox01") over the base game.
//   /mod <name>   the same flag (retail reads the two spellings in one arm).
//   /game <code>  which game the data is from (e.g. "jo", "jodemo"); selects
//                 the SCR decode key. Defaults to "jo" when absent.
//   /LAN          every game.cfg read forces mpnovaworldhostlanonly on: a
//                 NovaWorld host advertises LAN only.
//   /POSTIPADDRESS <address>, /POSTPORTNUMBER <n>
//                 override the NovaWorld gate response's POST address and
//                 port (NwuLobbySession::CommandLine).
//   /UDPNOVAWORLD <address>
//                 a NovaWorld session server to probe in place of the gate's
//                 UDPNOVAWORLD entries (up to six).
//   /USEJUNCTION <n>, /CLEARJUNCTION <n>
//                 override the gate response's USEJUNCTION / CLEARJUNCTION.
//   --resource-dir <absolute path>
//                 the game-data directory for this process. Without the flag
//                 the shell boots its bundled assets/ (ADR 0048); the flag
//                 with no value is a usage error.
//   --loose-mission <name.bms>
//                 boot the exact top-level loose BMS from --resource-dir.
//   --loose-root  Loose-data runs: when the directory holds none of the packed
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
//   --spectator                 request the spectator role when joining.
//   --spectator-password <text> spectator password for that join request.
//   --callsign <name>           the local player's callsign (wire clamp applies).
//   --capture-pcap <path>       record this session's datagrams to a pcap.
//   --mcp-port <n>              host the runtime MCP endpoint on that loopback
//                               port (1..65535); absent = no endpoint.
//   --working-dir <path>        the directory the game was started in, where it
//                               keeps the files it writes beside itself as retail
//                               keeps its saves in its working directory
//                               [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0];
//                               absent = the process's own working directory. A
//                               source run passes it (the editor's Play: its run
//                               directory), since Godot's `--path` moves the
//                               process's working directory to the project; a
//                               packaged runtime needs none (ADR 0046 S13 A8).
//
// Flags match case-insensitively; a flag's value is the following token,
// stripped, and a flag given twice takes its last value, as the game's own
// command line does. Integers read decimal or 0x-prefixed; an absent, empty or
// malformed value reads the field's "unset" sentinel. The embedder hands over
// the whole token list (engine and user args alike) so packaged and source
// launches share one parser.

#include <cstddef>
#include <string>
#include <vector>

namespace opennova {

// The flag names parse_launch_flags reads (the list above); a launcher that writes one
// spells it with these.
inline constexpr const char *kLaunchFlagLooseOverride = "/d";
inline constexpr const char *kLaunchFlagExpansion = "/exp";
inline constexpr const char *kLaunchFlagGame = "/game";
inline constexpr const char *kLaunchFlagResourceDir = "--resource-dir";
inline constexpr const char *kLaunchFlagLooseMission = "--loose-mission";
inline constexpr const char *kLaunchFlagLooseRoot = "--loose-root";
inline constexpr const char *kLaunchFlagMission = "--mission";
inline constexpr const char *kLaunchFlagLanHost = "--lan-host";
inline constexpr const char *kLaunchFlagLanJoin = "--lan-join";
inline constexpr const char *kLaunchFlagLanPort = "--lan-port";
inline constexpr const char *kLaunchFlagLanGametype = "--lan-gametype";
inline constexpr const char *kLaunchFlagLanMode = "--lan-mode";
inline constexpr const char *kLaunchFlagLanMaxPlayers = "--lan-max-players";
inline constexpr const char *kLaunchFlagSpectator = "--spectator";
inline constexpr const char *kLaunchFlagSpectatorPassword = "--spectator-password";
inline constexpr const char *kLaunchFlagCallsign = "--callsign";
inline constexpr const char *kLaunchFlagCapturePcap = "--capture-pcap";
inline constexpr const char *kLaunchFlagMcpPort = "--mcp-port";
inline constexpr const char *kLaunchFlagWorkingDir = "--working-dir";
inline constexpr const char *kLaunchFlagNoHud = "/NOHUD";
inline constexpr const char *kLaunchFlagNoReload = "/noreload";
inline constexpr const char *kLaunchFlagLan = "/LAN";
inline constexpr const char *kLaunchFlagPostIpAddress = "/POSTIPADDRESS";
inline constexpr const char *kLaunchFlagPostPortNumber = "/POSTPORTNUMBER";
inline constexpr const char *kLaunchFlagUdpNovaWorld = "/UDPNOVAWORLD";
inline constexpr const char *kLaunchFlagUseJunction = "/USEJUNCTION";
inline constexpr const char *kLaunchFlagClearJunction = "/CLEARJUNCTION";
// The /UDPNOVAWORLD table's bounds: six entries of 32 bytes (31 characters and the NUL).
inline constexpr std::size_t kLaunchUdpNovaWorldMax = 6;
inline constexpr std::size_t kLaunchUdpNovaWorldChars = 31;

struct LaunchFlags {
    bool loose_override = false;   // /d
    std::string expansion;         // /exp or /mod <name>: the last one, its first 32 bytes
    std::string game;              // /game <code>, lowercased
    std::string resource_dir;      // --resource-dir <path>
    bool resource_dir_given = false; // --resource-dir present, even without a value
    std::string loose_mission;     // --loose-mission <name.bms>
    bool loose_root = false;       // --loose-root
    std::string mission;           // --mission <name.bms>
    std::string lan_host;          // --lan-host <mission.bms>
    std::string lan_join;          // --lan-join <ip[:port]>, verbatim
    int lan_port = 0;              // --lan-port; 0 = unset (1..65535 accepted)
    int lan_gametype = -1;         // --lan-gametype; -1 = auto (>= 0 accepted)
    int lan_mode = 0;              // --lan-mode; 0 = unset (1..4 accepted)
    int lan_max_players = 0;       // --lan-max-players; 0 = unset (1..64 accepted)
    bool spectator = false;        // --spectator
    std::string spectator_password;// --spectator-password <text>
    std::string callsign;          // --callsign <name>
    std::string capture_pcap;      // --capture-pcap <path>
    int mcp_port = 0;              // --mcp-port; 0 = no endpoint (1..65535 accepted)
    std::string working_dir;       // --working-dir <path>; "" = the process's own
    // /NOHUD — the whole token, any case: clears the HUD overlay master word
    // (runtime/hud/hud_frame.h hud_overlay_master) [orig:
    // Game_ParseCommandLineAndInit @0x4a7310 — `_stricmp(token, "/NOHUD")`
    // @0x4A79FC -> sub_58FF20(0) @0x4A7A09].
    bool no_hud = false;
    // /noreload — the whole token, any case: the session copy forces the
    // auto-reload global off whatever the profile's word holds
    // (profile::session_input) [orig: Game_ParseCommandLineAndInit
    // `_stricmp(token, "/noreload")` @0x4A76DD -> dword_B4C4F4 = 1 @0x4A76E9;
    // Game_ApplySessionSettingsToGlobals @0x551a24..0x551a48].
    bool no_reload = false;
    // /LAN: the whole token, any case: every game.cfg read forces
    // mpnovaworldhostlanonly to 1 (inmatch::GameConfigBlock::set_lan_switch)
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - `_stricmp(token, "/LAN")`
    // @0x4A74D9..0x4A74E9 -> g_LanSwitch = 1 @0x4A74EB; Game_LoadConfig
    // @0x5514B1..0x5514BA].
    bool lan = false;
    // /POSTIPADDRESS <address> and /POSTPORTNUMBER <n>, each the whole token in any case and
    // its next token, the last one standing: the address the game resolves (DNS on; one that
    // does not resolve overrides nothing) and the port's CRT atol (0 overrides nothing), which
    // the NovaWorld gate response takes over its own POSTIPADDRESS / POSTIPPORT
    // (NwuLobbySession::CommandLine). "" / 0 when absent.
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - "/POSTIPADDRESS" @0x4A75A4,
    //  Napi_ResolveAddress(token, 1) @0x4A75C7 into dword_B4C6B0 @0x4A75CF; "/POSTPORTNUMBER"
    //  @0x4A75D4, _atol @0x4A75F6 into dword_B4C6B4 @0x4A75FE]
    std::string post_ip_address;
    int post_port_number = 0;
    // /UDPNOVAWORLD <address>, the whole token in any case: its next token is stepped over and,
    // while fewer than kLaunchUdpNovaWorldMax are kept, kept to kLaunchUdpNovaWorldChars
    // characters, in order; the NovaWorld connect probes these in place of the gate's entries
    // and counts the NWU session in use for them (NwuLobbySession::CommandLine).
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - "/UDPNOVAWORLD" @0x4A74F1, the next token
    //  stepped onto @0x4A7503..0x4A750C, the count test dword_B4C71C < 6 @0x4A750E..0x4A7516,
    //  Napi_CopyString(0xB4BFB0 + count * 32, token, 0x20) @0x4A751C..0x4A7528, the count
    //  @0x4A7530]
    std::vector<std::string> udp_novaworld;
    // /USEJUNCTION <n> and /CLEARJUNCTION <n>: the CRT atol of the next token, the last one
    // standing, over the gate response's USEJUNCTION / CLEARJUNCTION; -1 (the words' initial
    // value) overrides nothing.
    // [orig: Game_ParseCommandLineAndInit @0x4a7310 - "/USEJUNCTION" @0x4A7603, _atol into
    //  dword_829F88 @0x4A762D; "/CLEARJUNCTION" @0x4A7632, _atol into dword_829F8C @0x4A765C;
    //  both -1 in .data @0x829F88]
    int use_junction = -1;
    int clear_junction = -1;
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

// The bytes of an expansion name the game keeps: `g_ExpansionName @ 0xb4c584`
// is 32 bytes and `/exp`/`/mod` fill it with `strncpy(.., 0x20)`.
inline constexpr std::size_t kExpansionNameBytes = 32;
// The longest name the game mounts: `/exp` copies its token into the 32-byte
// g_ExpansionName with strncpy, so a longer one is left unterminated, and the
// version.txt CRC written right after it then runs into every path made from
// the name, which no longer opens [orig: Game_ParseCommandLineAndInit
// @ 0x4a76ca; Expansion_LoadAssets @ 0x4a4885, the paths @ 0x4a48a6..0x4a49d4];
// the join carries it in 32 bytes too [orig: UI_JoinSelectedSession
// @ 0x569afa, @ 0x569dc4]. Nothing shorter fails (D-VFS-8's search-path spill
// is read as one string, vfs-pff-mount-re.md § Expansions item 2).
inline constexpr std::size_t kExpansionNameMax = kExpansionNameBytes - 1;
// Whether the command line's tokenizer ends an `/exp` token at `c`: a space,
// a tab or a comma splits it outside quotes, a '"' toggles quoting and is
// never kept, a ';' ends the line [orig: Config_TokenizeLine @ 0x53cb60
// over GetCommandLineA, @ 0x4a73b2; ' ', '\t' and ',' @ 0x53cc44, '"'
// @ 0x53cc51, ';' @ 0x53cc31]. A name holding one mounts only quoted.
bool launch_token_breaks_at(char c);
// A `/exp`/`/mod` token as the game holds it: stripped, then its first 32
// bytes [orig: Game_ParseCommandLineAndInit @ 0x4a76cf]. A name of 32 or more
// is left unterminated there and read on into the next global; the port stops
// at 32 (D-VFS-8).
std::string launch_expansion_name(const std::string &token);
// A launch flag always wins over a persisted (possibly stale) fallback.
std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback);
// Lowercased; the persisted fallback next, then "jo" — the absence of any
// choice is the JO default. An unknown code resolves to the JO default
// downstream (gameprofile_scr_policy_for_code).
std::string launch_game(const LaunchFlags &flags, const std::string &fallback);
// `dir/name`, with an empty dir yielding the bare name and a trailing
// separator not doubled.
std::string boot_path_join(const std::string &dir, const std::string &name);

class Vfs;

// The game install at `root` mounted into `vfs` as a launch with `flags` mounts it:
// the witnessed boot archive table with the /exp expansion layered over it
// [orig: PFF_OpenAllArchives @ 0x4a4310; Expansion_LoadAssets @ 0x4a4730], its
// payloads decoded with the /game code's key (launch_game), and its lookup the
// archives alone while one is mounted unless /d puts the loose files first
// [orig: FileSystem_OpenFile @ 0x75b1c0, the gate @ 0x75b1e5; /D
// Game_ParseCommandLineAndInit @ 0x4a7667 -> Game_InitSubsystems @ 0x4a6fa3 ->
// FileSystem_SetSearchLooseFirst @ 0x75a5a0]. False when the root does not mount
// (vfs.game_root() stays empty, vfs.last_error() says why) or the root mounts but opens
// none of the game's archives, which retail's boot refuses [orig: Game_InitSubsystems @
// 0x4a6f44] (vfs.last_error() then names an archive that failed to open, when one did).
bool mount_install(Vfs &vfs, const std::string &root, const LaunchFlags &flags);

} // namespace opennova
