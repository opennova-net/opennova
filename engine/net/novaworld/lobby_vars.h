#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <net/napi/session.h> // ClientVar / NapiMessage / make_client_host_request / make_client_host_update
#include <net/novaworld/lobby_update.h> // LobbyStatusBlob (the POST status heartbeat)

// P7 Part 2 (B2) — the NovaWorld lobby var-builders + identity set, moved out of the Godot bindings
// so they become pure pumps (ADR 0010 / .agents/README.md: lobby payloads belong in engine/net/novaworld,
// sockets + signals in Godot). The host registration var-lists (the GSB row), the NW-S5 client
// identity "Cookie" set (used for BOTH the UDP verify var-list and the HTTP login cookies), and the
// host-and-port split with its DNS-off resolve (the UDPNOVAWORLD entries, the ServerCommand's
// ByIpAndPort target). Godot-free. The bindings pass their GDScript-set config in.
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

// The host fields the binding holds (NovaWorldClient's hosting half, NwuHostRole) plus the live
// server state the Host list republishes; a field the hosting leg leaves alone keeps its default.
struct HostRegistration {
	// --- HostSetup [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50, the +460 list] ---
	// LobbyName (g_LobbyName): the gate response's LOBBYNAME, empty until a gate names one (the
	// host leg refuses an empty one: connect_or_host.h). [orig: CNapiGateManager_ProcessResponse
	//  @0x4ced20 (LOBBYNAME)]
	std::string lobby_name;
	std::string server_name = "OpenNova Host"; // ServerName
	std::string server_message;               // Msg (serverMessage_560)
	int max_players = 32;                     // MaxPlayers (HostSetup, the initial Host list)
	// The in-match published cap (dword_24D211C: the cfg max, plus the dedicated slot when the host
	// is not a peer). The refreshed Host list's MaxPlayers is it, minus that slot for a non-peer,
	// so a dedicated server never counts its own slot; 0 = max_players.
	// [orig: HostDialog_StartSession @0x5587d7..0x5587e5 (the +1); Lobby_UpdateServerInfo
	//  @0x4feb03..0x4feb0a (the -1 for a non-peer)]
	int published_cap = 0;
	bool password = false;                    // Password "1"/"0" (hostGamePassword_350[0])
	bool listen_host = true;                  // Dedicated "0" when is_mp_session_peer (the host plays)
	uint32_t app_id = 0;                      // AppId: make_session_app_id, minted per registration
	std::string access_code_list;             // AccessCodeList
	std::string expansion;                    // Exp (g_ExpansionName); PLoad is always empty
	int lan_only = 0;                         // LAN (hostLanOnly_340)
	// DFXCATEGORY1, a title's host lists' var after LAN (TitleIdentity::host_category_var, which
	// NwuHostRole sets): the game.cfg `mpdfxcategory` word printed "%ld", on HostSetup and on the
	// Host list alike. [orig: the twin of CNapiGameSession_BuildHostVarLists @0x4d0b50 at
	//  @0x4d0ce0 (dfx2.exe) - HostSetup @0x4d0e8a..0x4d0eb1, Host @0x4d0faf..0x4d0fd5]
	bool category_var = false;
	int32_t category = 0;
	// ReconnectCounter (session+0x500): the HostSetup list's last var. A fresh host request
	// zeroes it; each re-host after an NWU reconnect counts one up.
	// [orig: BuildHostVarLists @0x4d0b50 zeroes it @0x4d0b7b and appends it through
	//  CNapiGameSession_InitHeapsAndSerializeCounter @0x4ce300 (@0x4d0e08);
	//  HandleConnectVerifyResponse @0x4d5800 ++ @0x4d5968 before StartHostingSession(1)]
	int reconnect_counter = 0;
	// --- Host extras [orig: Lobby_UpdateServerInfo @0x4fe8c0] ---
	std::string host_key;                     // HostKey (byte_C867B0, the NWHost relay HOSTKEY)
	std::string game_type;                    // GameType (GameType_GetAbbreviation(g_GameType, 1))
	std::string mission_name;                 // MissionName (the mission "info"/"title", else the file)
	int region_index = 0;                     // Region: lod_level 0/1/2 -> STRNOVA07/08/09, else "?"
	// Players: the active slots, the host's own counted only when it is a peer (a listen host);
	// NwuHostRole sets it from its roster. [orig: Lobby_UpdateServerInfo @0x4feaa6..0x4feacb]
	int player_count = 1;
	// MI1..MI3: the host's net-quality send-window averages (g_NetQualityHostAvgBandwidth /
	// g_NetQualityHostAvgPing / g_NetQualityHostAvgLoss), the hosted match's
	// (inmatch::read_session_listing_columns, D-NET-NEW-L9-50).
	int mi1 = 0, mi2 = 0, mi3 = 0;
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
	uint32_t uptime_ms = 0;                   // Age: GetTickCount() - g_RoundStateInitTick
	int time_of_day = 0;                      // TimeOfDay (g_EnvTimeOfDayEnum 0..4)
	uint32_t pcid_key = 0;                    // PCIDKey (the SessionIdRing's current key)
	uint32_t game_server_baffle_key = 0;      // GameServerBaffleKey (g_GameServerBaffleKey)
	int bb_mode = 0;                          // BBMode (dword_24D21A4)
	// GCC (byte_C87044): the install's CC.BIN, as the session's create reads it into the process
	// tick; the embedder sets it, and the first mission's METPROTOCOL ID report prints it too.
	// [orig: Server_InitServerTicks @0x5018C0 - Game_ReadCCBinFile @0x5018E8]
	std::string gcc;
	// GV and Version (g_VersionText): NwuHostRole sets it from the title's version text.
	std::string version;
	// The gate reply's METEXT (g_IsDedicatedServer, a misnomer: its one writer is the gate parse):
	// set, the Host list carries the CountryName / Lang / TZB trio, whatever the dedicated mode
	// (D-NET-347). NwuHostRole sets it from the session's gate response; the trio's values are the
	// embedder's machine locale.
	// [orig: Lobby_UpdateServerInfo `cmp g_IsDedicatedServer, 0` @0x4ff2de..0x4ff2e5;
	//  CNapiGateManager_ProcessResponse @0x4cf2a1..0x4cf2c1 (METEXT, atol)]
	bool met_ext = false;
	std::string country_name;                 // CountryName (GetLocaleInfoA LOCALE_SENGCOUNTRY)
	std::string language;                     // Lang (LOCALE_SENGLANGUAGE)
	int tz_bias = 0;                          // TZB (TIME_ZONE_INFORMATION.Bias)
	bool pb_server = false;                   // PBServer "1"/"0" (game_settings.reserved_D4)
	// Ver1 / Ver2: the title's version pair (TitleIdentity::ver1 / ver2); NwuHostRole sets it from
	// its session's title. [orig: Lobby_UpdateServerInfo @0x4ff3ea / @0x4ff3ff]
	std::string ver1;
	std::string ver2;
	// game.cfg `sendplayerlist` (default 1) -> g_NWSendPlayerList: the POST status blob
	// carries the " p=<name>" suffix at all. [orig: the config apply @0x54e2c4 region
	//  (g_NWSendPlayerList = g_GameConfigState+0x4BC); Lobby_UpdateServerInfo @0x4ff560]
	bool send_player_list = true;
};

// One PlayerList slot: the five per-player vars keyed by the slot index (VarFNum).
// [orig: Server_PlayerAdd @0x51d421..0x51d4aa]
struct HostPlayerSlot {
	int slot = 0;
	// The add's own identity, not a var: the in-match connection the player was added for. A
	// leave and a re-join into the same slot is a new connection, so a roster sync sends both the
	// removal and the add, as retail's remove and add each send theirs (plan_roster_sync).
	uint32_t connection_id = 0;
	std::string player_name;   // PlayerName
	std::string ip_and_port;   // PlayerIpAndPort ("a.b.c.d:port")
	std::string pcid;          // PlayerPCID
	std::string team;          // PlayerTeam
	std::string type;          // PlayerType
	// The add's own state, not a var: the local player's slot (a listen host's own), whose +5
	// byte the session create's round init sets, so its add runs no lists' update; a joiner's add
	// clears its slot's. [orig: Server_InitNewRoundState @0x51C8E0 - g_LocalNetPlayer +5 = 1
	//  @0x51CA35 (from CNapiGameSession_BuildAndCreateSession @0x5695B3, ahead of the create's own
	//  add); Server_PlayerAdd @0x51CBC0 - the local path @0x51CC33..0x51CC60 writes no +5, a
	//  joiner's +5 = 0 @0x51CF02, the test @0x51D4BA]
	bool local = false;
};

// The Host list's gametext tokens, each looked up by (section, key): the NovaWorld STRNOVA07..12
// (no fallback: a missing key reads empty) and the TimeOfDay quintet (the witnessed literals when
// the key is missing). `lookup` returns false when the table lacks the key.
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — GameText_GetString("NovaWorld", "STRNOVA07".."12")
//  @0x4fea56.. / @0x4fec2c.. / @0x4fed8d / @0x4fee51.., GameText_GetStringWithFallback("TimeOfDay",
//  ...) @0x4ff09b..0x4ff138]
HostLobbyText make_host_lobby_text(
		const std::function<bool(const char *section, const char *key, std::string &out)> &lookup);

// The "HostSetup" var-list, in retail's insertion order: LobbyName, ServerName, Msg,
// MaxPlayers, Password, Dedicated, AppId, AccessCodeList, PLoad, Exp, LAN, ReconnectCounter.
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

// ClientHostRequest: CurrentlyHosting (0 on a fresh host, 1 on the re-host path), VarCheck=1,
// Cookie (the login cookie jar + locale, i.e. the verify Cookie set with NWUID filled),
// HostSetup, the initial Host list, and an EMPTY PlayerList (Server_PlayerAdd has not run).
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700; the fresh-host 0 from
//  CNapiGameSession_ConnectOrHost @0x4d5113 -> StartHostingSession @0x4d4540 @0x4d456d]
NapiMessage make_host_request(const HostRegistration &cfg, const std::vector<ClientVar> &cookie,
                              int currently_hosting);

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

// Inputs for the NW-S5 identity "Cookie" set. Environment collection belongs to the embedder
// (the locale, make_retail_machine_tokens over the machine's inputs or fallback_machine_tokens);
// this portable builder only keeps the witnessed names/order and the values it is given.
struct LobbyIdentityParams {
	std::string tz_bias = "0";
	std::string country = "United States";
	std::string language = "English";
	std::string my_installed_exp_bits = "0";
	std::string nwpssk;
	std::string nwusid;
	std::string nwhwi; // make_nwhwi over the embedder's HardwareIdentity
};

// The device facts the NWHWI cookie prints, as the embedder reads them: on Windows the reads retail
// makes (lobby_identity.h: read_display_adapter, read_total_physical_memory,
// read_primary_screen_size), elsewhere the platform's equivalents; the display mode is game.cfg's
// (gamecfg::display_size over the same desktop).
struct HardwareIdentity {
	// Display adapter 0's description; with no display device retail's %s prints the CRT's
	// "(null)". [orig: CGfxDevice_GetAdapterDescription @0x67C590 - 0 with no Direct3D object
	//  @0x67C596, IDirect3D9 slot 5 (GetAdapterIdentifier) called @0x67C5B8, "%s" over the
	//  identifier's Description sprintf'd @0x67C5D0]
	bool has_adapter = false;
	std::string adapter_description;
	// The rendering adapter's display memory in MB: the leading number of DxDiag's
	// szDisplayMemoryEnglish, truncated; 0 when the query finds none.
	int64_t display_memory_mb = 0;
	// The machine's physical memory in bytes.
	uint64_t total_phys_bytes = 0;
	// game.cfg's display mode, and the primary desktop's size at the renderer's start.
	int32_t display_width = 0;
	int32_t display_height = 0;
	int32_t desktop_width = 0;
	int32_t desktop_height = 0;
};

// NWHWI's second field: the query's number shifted into bytes in 32 bits, which the device keeps
// only when it is not negative (else 0), shifted back logically; so a size whose bit 11 is set
// prints 0 and every other prints its low 11 bits.
// [orig: CGfxDevice_QueryVideoDriverVersion @0x67C820 - wcstod called @0x67CA45, _ftol2_sse
//  called @0x67CA57, `shl eax, 14h` @0x67CA5C; CGfxDevice_CreateDevice @0x67E5D0 - the query
//  called @0x67EBEC, a negative result `jl` @0x67EBFB to the 0 store @0x67EC25, else stored
//  @0x67EBFD; CNapiGameSession_OnNovaWorldConnected @0x4D1570 - the read @0x4D176A, `shr eax,
//  14h` @0x4D1771]
uint32_t nwhwi_display_memory_mb(int64_t display_memory_mb);

// NWHWI's third field (g_VideoMemoryMB, which holds physical memory): GlobalMemoryStatus's
// dwTotalPhys, which Windows caps at 0x7FFFFFFF for an image without IMAGE_FILE_LARGE_ADDRESS_AWARE
// (Jointops.exe's FileHeader.Characteristics are 0x103), plus 0xC0000, shifted right by 20, all in
// 32 bits: 2048 on every machine with 2 GiB or more.
// [orig: System_GetCPUAndOSInfo @0x53CD30 - GlobalMemoryStatus called @0x53CF0E, dwTotalPhys read
//  @0x53CF17 into info+0x64 @0x53CF25; Game_ParseCommandLineAndInit @0x4A7310 - `add ecx,
//  0C0000h` @0x4A7DF2, `shr ecx, 14h` @0x4A7E05, stored to g_VideoMemoryMB @0x4A7E15]
uint32_t nwhwi_physical_memory_mb(uint64_t total_phys_bytes);

// The NWHWI cookie value: "%s$%ld$%ld$%ldx%ld$%ldx%ld" over adapter 0's description, the display
// memory field, the physical memory field, game.cfg's display width x height and the renderer's
// desktop width x height.
// [orig: CNapiGameSession_OnNovaWorldConnected @0x4D1570 - g_RendererDesktopHeight / Width read
//  @0x4D174B / @0x4D1751, g_VideoMemoryMB @0x4D1756, g_GameConfigState +0x60 / +0x64 @0x4D175D /
//  @0x4D1764, CGfxDevice_GetAdapterDescription(0) called @0x4D177C, sprintf called @0x4D178F,
//  CUIBrowser_AddFormField "NWHWI" called @0x4D17AA]
std::string make_nwhwi(const HardwareIdentity &hw);

// The wire-load-bearing NWPSSK/NWUSID token lengths: retail's stable-machine
// token encoders always emit exactly these many letters (NWPSSK's position-banded
// A..T, NWUSID's pairs A..W), the fallback's included (fallback_machine_tokens).
// [orig: CDKey_GenerateHardwareFingerprint @ 0x4a4a00 /
// CDKey_GenerateHardwareFingerprint_0 @ 0x4a4d00 output widths]
inline constexpr int kNwpsskLen = 23;
inline constexpr int kNwusidLen = 16;

// The NW-S5 10-var identity "Cookie" set (capture frame 10166), built once and reused for BOTH the
// UDP verify var-list (ClientSession::Config::cookie_vars) and the HTTP login cookies. NWUID
// is left empty here (the consumer substitutes the SessionInit nwuid at use). NWPSSK / NWUSID are
// the embedder's tokens as given.
// [orig: NovaWorldClient::begin_session identity build]
std::vector<std::pair<std::string, std::string>> make_lobby_identity_vars(const LobbyIdentityParams &p);

// Split an address string (a gate UDPNOVAWORLD entry, a ServerCommand's ByIpAndPort token) as
// retail's host-and-port parse does: the
// text to its first NUL is the string, and one longer than 511 bytes fails (the 512-byte local
// copy); the host is the text before its first ':' (all of it with no colon), and the port the
// napi literal reader's value of the text after that colon (`$7D00`, `0x7D00`, `076400` and
// `32000d` all read 32000), or 0 with no colon or no literal there. The host is the caller's to
// resolve (Napi_ResolveAddress).
// [orig: Network_ParseHostAndPort @0x62E200 - the length gate @0x62E260, the copy @0x62E288,
//  the first ':' @0x62E297, the literal @0x62E2B1 (0 @0x62E2BD), the host to
//  Napi_ResolveAddress @0x62DF90, called @0x62E2D2]
bool parse_host_and_port(const std::string &in, std::string &host, uint32_t &port);

// inet_addr, a platform primitive, as the retail game's platform answers it: the Windows
// ws2_32 call, written out from its measured answers (2026-10-13, Windows 11) so every host
// reads alike. One to four parts split by '.', each a C integer that starts with a digit
// (0x / 0X hex with at least one hex digit, a leading 0 octal without 8 or 9, else decimal),
// read into a 32-bit accumulator that fails only on a step that makes it smaller (so
// "6966210500" wraps to 159.55.239.196 and "4294967296" fails); every part but the last at
// most 255 and the last filling the bytes left (a, a.b with a 24-bit b, a.b.c with a 16-bit c,
// a.b.c.d); the text may end at an ASCII whitespace character, and a lone " " is 0. The
// address is packed as the platform stores it (the first octet in the lowest-addressed byte,
// PeerAddr's ip), and 0xFFFFFFFF (INADDR_NONE) is the answer for a text that reads no address,
// as it is for "255.255.255.255" itself.
uint32_t platform_inet_addr(const std::string &text);

// Napi_ResolveAddress with DNS off: "255.255.255.255" (any case) is itself, else inet_addr's
// address, else 0, so a text inet_addr does not read and an all-ones address in any other
// spelling both resolve to 0.
// [orig: Napi_ResolveAddress @0x62DF90 - Napi_StrCaseEqual @0x62DF9B..0x62DFA7, inet_addr
//  @0x62DFAD, its -1 test @0x62DFB6, 0 with DNS off @0x62DFD8]
uint32_t resolve_address_without_dns(const std::string &host);

// inet_ntoa's text of an address word packed as the platform stores it (the first octet in the
// lowest-addressed byte): "a.b.c.d". [orig: Network_FormatIPAddressToString @0x62DE80 and
// Napi_FormatAddress @0x62DED0, both through inet_ntoa (@0x62DE98, @0x62DF05)]
std::string in_addr_text(uint32_t address);

} // namespace opennova
