#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <net/napi/session.h> // ClientVar / NapiMessage / make_client_host_request / make_client_host_update
#include <net/novaworld/lobby_update.h> // LobbyStatusBlob (the POST status heartbeat)

// P7 Part 2 (B2) — the NovaWorld lobby var-builders + identity set, moved out of the Godot bindings
// so they become pure pumps (ADR 0010 / .agents/README.md: lobby payloads belong in engine/net/novaworld,
// sockets + signals in Godot). The host registration var-lists (the GSB row), the NW-S5 client
// identity "Cookie" set (used for BOTH the UDP verify var-list and the HTTP login cookies), and the
// UDPNOVAWORLD "host:port" split. Godot-free. The bindings pass their GDScript-set config in.
namespace opennova {

// The GameText tokens the Host list carries as VALUES (the retail builder resolves them through
// GameText_GetString("NovaWorld", ...) / GameText_GetStringWithFallback("TimeOfDay", ...)); the
// binding resolves them against the registered gametext table. The TimeOfDay fallbacks are the
// witnessed literals, the STRNOVA tokens have none (an unregistered table emits them empty).
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — STRNOVA07/08/09 @0x4fea56.., STRNOVA10 @0x4fed8d,
//  STRNOVA11/12 @0x4fec2c.., the TimeOfDay quintet @0x4ff09b..0x4ff138]
struct HostLobbyText {
	std::string yes;                          // "NovaWorld"/"STRNOVA11"
	std::string no;                           // "NovaWorld"/"STRNOVA12"
	std::string no_time_limit;                // "NovaWorld"/"STRNOVA10"
	std::array<std::string, 3> region{};      // "NovaWorld"/"STRNOVA07".."STRNOVA09"
	// Index = g_EnvTimeOfDayEnum: 0 UNKNOWN, 1 DAWN, 2 DAY, 3 DUSK, 4 NIGHT.
	std::array<std::string, 5> time_of_day{"Unknown", "Dawn", "Day", "Dusk", "Night"};
};

// The GDScript-set host fields the binding holds (the NovaWorldHost members) plus the live
// server state the Host list republishes. Defaults match the binding's property defaults.
struct HostRegistration {
	// --- HostSetup [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50, the +460 list] ---
	std::string lobby_name = "jop_2_consumer"; // LobbyName (g_LobbyName)
	std::string server_name = "OpenNova Host"; // ServerName
	std::string server_message;               // Msg (serverMessage_560)
	int max_players = 32;                     // MaxPlayers
	bool password = false;                    // Password "1"/"0" (hostGamePassword_350[0])
	bool listen_host = true;                  // Dedicated "0" when is_mp_session_peer (the host plays)
	uint32_t app_id = 0;                      // AppId: make_session_app_id, minted per registration
	std::string access_code_list;             // AccessCodeList
	std::string expansion;                    // Exp (g_ExpansionName); PLoad is always empty
	int lan_only = 0;                         // LAN (hostLanOnly_340)
	// --- Host extras [orig: Lobby_UpdateServerInfo @0x4fe8c0] ---
	std::string host_key;                     // HostKey (byte_C867B0, the NWHost relay HOSTKEY)
	std::string game_type;                    // GameType (GameType_GetAbbreviation(g_GameType, 1))
	std::string mission_name;                 // MissionName (the mission "info"/"title", else the file)
	int region_index = 0;                     // Region: lod_level 0/1/2 -> STRNOVA07/08/09, else "?"
	int player_count = 1;                     // Players (active slots; the host itself is one)
	int mi1 = 0, mi2 = 0, mi3 = 0;            // MI1..MI3 (dword_82BEEC..F4)
	bool locked = false;                      // Locked (g_ServerJoinLocked)
	bool skins = false;                       // Skins (dword_24D218C)
	// TimeLeft: the LIVE round clock, re-read at every refresh, / 3720 (whole
	// minutes); < 0 -> STRNOVA10. The embedder feeds world::Match's clock.
	// [orig: Lobby_UpdateServerInfo `mov ecx, g_RoundTimeRemaining` @0x4FED57]
	int round_time_remaining_ticks = -1;
	bool tracers = true;                      // Tracers: (g_RulesFlags & 1) == 0
	std::string country = "XX";               // Country (str1; "XX" while locked; "XX"/empty -> " ")
	// AllowPing 'y'/'n': game.cfg `ping`, default 1, copied to g_NWAllowPing.
	// OpenNova has no game.cfg surface, so the stock default holds.
	// [orig: Config_SetDefaults @0x54D324 (ping = 1); Game_ApplySessionSettingsToGlobals
	//  @0x551D4F -> g_NWAllowPing; Lobby_UpdateServerInfo read @0x4FEF72]
	bool allow_ping = true;
	uint32_t uptime_ms = 0;                   // Age: GetTickCount() - dword_C8FC74
	int time_of_day = 0;                      // TimeOfDay (g_EnvTimeOfDayEnum 0..4)
	uint32_t pcid_key = 0;                    // PCIDKey (the SessionIdRing's current key)
	uint32_t game_server_baffle_key = 0;      // GameServerBaffleKey (dword_C8FC70)
	int bb_mode = 0;                          // BBMode (dword_24D21A4)
	std::string gcc;                          // GCC (byte_C87044)
	std::string version;                      // GV and Version (byte_B4C0B0)
	bool dedicated_server = false;            // g_IsDedicatedServer: the CountryName/Lang/TZB block
	std::string country_name;                 // CountryName (GetLocaleInfoA LOCALE_SENGCOUNTRY)
	std::string language;                     // Lang (LOCALE_SENGLANGUAGE)
	int tz_bias = 0;                          // TZB (TIME_ZONE_INFORMATION.Bias)
	bool pb_server = false;                   // PBServer "1"/"0" (game_settings.reserved_D4)
	// game.cfg `sendplayerlist` (default 1) -> g_NWSendPlayerList: the POST status blob
	// carries the " p=<name>" suffix at all. [orig: the config apply @0x54e2c4 region
	//  (g_NWSendPlayerList = g_GameConfigState+0x4BC); Lobby_UpdateServerInfo @0x4ff560]
	bool send_player_list = true;
};

// One PlayerList slot: the five per-player vars keyed by the slot index (VarFNum).
// [orig: Server_PlayerAdd @0x51d421..0x51d4aa]
struct HostPlayerSlot {
	int slot = 0;
	std::string player_name;   // PlayerName
	std::string ip_and_port;   // PlayerIpAndPort ("a.b.c.d:port")
	std::string pcid;          // PlayerPCID
	std::string team;          // PlayerTeam
	std::string type;          // PlayerType
};

// The "HostSetup" var-list, in retail's insertion order: LobbyName, ServerName, Msg,
// MaxPlayers, Password, Dedicated, AppId, AccessCodeList, PLoad, Exp, LAN.
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50 — the +460 SetOrCreate run; the second
//  AccessCodeList store @0x4d0d9d re-sets the same entry]
std::vector<ClientVar> make_host_setup_var_list(const HostRegistration &cfg);

// The "Host" var-list. The initial list (`full == false`, what the ClientHostRequest carries
// before the session is in a mission) is BuildHostVarLists' eight: LobbyName, ServerName, Msg,
// MaxPlayers, AppId, PLoad, Exp, LAN. Once in session, Lobby_UpdateServerInfo SetOrCreates its
// 36 columns into that list — existing names update in place, new ones append in its order
// (HostKey, GameType, MissionName, Region, Players, MI1, MI2, MI3, Dedicated, Locked, Skins,
// TimeLeft, Password, Tracers, Mod, Country, Port, AllowPing, Age, TimeOfDay, PCIDKey,
// GameServerBaffleKey, Stat, LevelRange, BBMode, GCC, GV, Version, [CountryName, Lang, TZB],
// Ver1, Ver2, PBServer); "AppID" folds onto "AppId" (the lookup is case-insensitive).
// [orig: BuildHostVarLists @0x4d0b50 (the +532 run); Lobby_UpdateServerInfo @0x4fe8c0;
//  CNapiVarList_SetOrCreate @0x6318c0 -> NapiLinkedList_FindByTypeAndName @0x6304f0
//  (Napi_StrCaseEqual)]
std::vector<ClientVar> make_host_var_list(const HostRegistration &cfg, const HostLobbyText &text,
                                          bool full);

// The "PlayerList" var-list: five VarFNum-keyed vars per slot.
std::vector<ClientVar> make_player_list(const std::vector<HostPlayerSlot> &players);

// The plaintext status heartbeat the host posts to the gate's
// POSTIPADDRESS:POSTIPPORT on the same 1860-tick refresh as the
// ClientHostUpdate: the whole full Host list in list order (so LobbyName and
// HostKey repeat as ordinary entries after the preamble) plus every
// PlayerList PlayerName. The text form is lobby_update_build (lobby_update.h).
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — the UDP leg @0x4ff448..0x4ff62c walks
//  dword_B6022C (the Host list) then dword_B60274 (the PlayerList) for PlayerName]
LobbyStatusBlob make_host_status_blob(const HostRegistration &cfg, const HostLobbyText &text,
                                      const std::vector<HostPlayerSlot> &players);

// The vars whose values changed (or are new) between two snapshots, in `next`'s order: the
// dirty set a periodic ClientHostUpdate carries. Retail marks an entry dirty only when
// SetValue sees a different string, and the whole update is skipped when nothing is dirty.
// [orig: CNapiVarEntry_SetValue @0x630590 (String_ExactMatch gate); NapiStatement_SerializeVarList
//  @0x4d0660 (`includeAll || node+24`); CPlayerManager_RebuildLists @0x4d4590]
std::vector<ClientVar> dirty_client_vars(const std::vector<ClientVar> &previous,
                                         const std::vector<ClientVar> &next);

// ClientHostRequest: CurrentlyHosting (0 on a fresh host, 1 on the re-host path), VarCheck=1,
// Cookie (the login cookie jar + locale, i.e. the verify Cookie set with NWUID filled),
// HostSetup, the initial Host list, and an EMPTY PlayerList (Server_PlayerAdd has not run).
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700; the fresh-host 0 from
//  CNapiGameSession_ConnectOrHost @0x4d5113 -> StartHostingSession @0x4d4540 @0x4d456d]
NapiMessage make_host_request(const HostRegistration &cfg, const std::vector<ClientVar> &cookie,
                              int currently_hosting);

// Deterministic [A-Z] fallback for callers without platform machine inputs.
// The Godot binding supplies the retail transform below on Windows.
std::string az_fingerprint(uint32_t seed, int len);

// Inputs consumed by retail's two stable-machine token encoders. Platform
// discovery (GetVolumeInformation/GetAdaptersInfo in the original) stays in
// the binding; these byte transforms are portable and testable.
// [orig: CDKey_GenerateHardwareFingerprint @ 0x4a4a00 /
// CDKey_GenerateHardwareFingerprint_0 @ 0x4a4d00]
struct RetailMachineInputs {
	uint32_t volume_serial = 0;
	uint32_t maximum_component_length = 0;
	uint32_t filesystem_flags = 0;
	std::string volume_name;
	std::string filesystem_name;
	std::array<uint8_t, 6> ethernet_address{};
	bool has_ethernet_address = false;
};

struct LobbyMachineTokens {
	std::string nwpssk;
	std::string nwusid;
};

LobbyMachineTokens make_retail_machine_tokens(const RetailMachineInputs &in);

// Inputs for the NW-S5 identity "Cookie" set. Environment collection belongs to the binding;
// this portable builder only preserves the witnessed names/order and values it is given. The
// client_index/client_key fallback exists for non-Godot callers, but bindings should supply the
// stable machine-derived NWPSSK/NWUSID explicitly.
struct LobbyIdentityParams {
	uint32_t client_index = 0;
	uint32_t client_key = 0;
	std::string tz_bias = "0";
	std::string country = "United States";
	std::string language = "English";
	std::string my_installed_exp_bits = "0";
	std::string nwpssk;
	std::string nwusid;
	std::string nwhwi = "OpenNova$0$2048$1920x1080$1920x1080";
};

// The wire-load-bearing NWPSSK/NWUSID token lengths: retail's stable-machine
// token encoders always emit exactly these many [A-Z] characters, and any
// fallback generator must match or the verify Cookie is malformed.
// [orig: CDKey_GenerateHardwareFingerprint @ 0x4a4a00 /
// CDKey_GenerateHardwareFingerprint_0 @ 0x4a4d00 output widths]
inline constexpr int kNwpsskLen = 23;
inline constexpr int kNwusidLen = 16;

// The NW-S5 10-var identity "Cookie" set (capture frame 10166), built once and reused for BOTH the
// UDP verify var-list (ClientSession::Config::cookie_vars) and the HTTP login cookies. NWUID
// is left empty here (the consumer substitutes the SessionInit nwuid at use). The XOR masks + the
// lengths kNwpsskLen/kNwusidLen (23/16) on NWPSSK/NWUSID are load-bearing.
// [orig: NovaWorldClient::begin_session identity build]
std::vector<std::pair<std::string, std::string>> make_lobby_identity_vars(const LobbyIdentityParams &p);

// Split a "host:port" string (the gate's UDPNOVAWORLD). Returns false on a missing colon or a
// non-numeric port; the port is the faithful truncating static_cast<uint16_t>(stoi(...)).
// [orig: the inline UDPNOVAWORLD split in NovaWorldClient/Host::poll_gate]
bool parse_host_port(const std::string &in, std::string &host, uint16_t &port);

} // namespace opennova
