#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NovaWorld lobby state — the set of fields a hosting jodemo sends to
// POSTIPADDRESS:POSTIPPORT every 1860 server ticks (Lobby_UpdateServerInfo
// @ 0x4d2e10). The wire format is a text KV blob, NOT a type-737 TLV
// packet — the "type 737 / type 738" claim in the existing docs is
// refuted by IDA (see notes/net_verification_log.md).
//
// The binary builds a VarList by repeated Network_SendData_Thunk calls
// (which is a thunk for CNapiVarList_SetOrCreate), then iterates the list
// plus some "Host" setters (the for-loop at +dword_9896A4) to produce a
// single-line text blob:
//
//   <LobbyName> HostKey = <hostkey> <K> = <V> <K> = <V> ... p=<playername>
//
// That blob is sent 1 time on update, 4 times on delete (" Port = -1
// DELETE" suffix), each as a single UDP datagram via CGameSession_SendPacket
// (which wraps CNapiNPManager_SendPacket and ultimately the LSB-scatter
// CRC envelope). Destination is (POSTIPADDRESS, POSTIPPORT) from the
// gate response.

struct LobbyServerInfo {
	// Identity / addressing
	std::string lobby_name;      // LobbyName (prefix of the blob, no key= form)
	std::string host_key;        // HOSTKEY= from the registration URL
	std::string host_did;        // HostDID

	// Server identity
	std::string server_name;     // ServerName
	std::string game_type;       // GameType display name
	std::string mission_name;    // MissionName
	std::string region;          // "North America" / "Europe" / "Asia" / "?" per STRNOVA07..09
	int players = 0;             // active-player count
	int max_players = 0;         // from CGameSession_GetMaxPlayers
	int mi1 = 0;                 // mission index 1
	int mi2 = 0;                 // mission index 2
	int mi3 = 0;                 // mission index 3

	// Boolean-ish flags (stored as "Yes"/"No" via STRNOVA11/12)
	bool dedicated = false;      // `/S` flag
	bool locked = false;
	bool skins_allowed = true;
	bool password_protected = false;
	bool tracers_disabled = false;

	// Misc UI / metadata
	std::string time_left;       // "%i" minutes or STRNOVA10 ("unlimited")
	std::string country;         // 2-letter code or "XX"
	std::string tod;             // "Dawn"/"Day"/"Dusk"/"Night"/"Unknown"
	std::string access_code_list;
	int app_id = 0;              // the CGameSession_GetMaxPlayers value used as AppID in the blob
	int pcid_key = 0;
	int game_server_baffle_key = 0;
	std::string stat = "+";      // default when no level range
	std::string level_range;     // or empty -> omitted
	int bb_mode = 0;
	std::string gv;              // version string (from byte_9816E0)
	std::string version;         // same as gv in the binary
	std::string country_name;    // Windows locale LCTYPE_COUNTRY
	std::string lang;            // Windows locale LCTYPE_COUNTRY (?)
	int timezone_bias = 0;       // TIME_ZONE_INFORMATION.Bias
	bool pb_server = false;      // PunkBuster enabled
	int allow_ping = 110;        // 121 if ping enabled, 110 if disabled
	std::string ver1 = "1";
	std::string ver2 = "2780";
	std::string port = "-1";     // literally "-1" in the binary

	// Uptime formatted like "%ld %2.2ld:%2.2ld:%2.2ld" (days h:m:s)
	std::string uptime;

	// Arbitrary extra KV pairs (mirrors the Host's own registry iterator
	// in the decomp loop `for (i = dword_9896A4; ... )`).
	std::vector<std::pair<std::string, std::string>> extra_pairs;
};

// Serialize the KV blob exactly as Lobby_UpdateServerInfo does.
// Setting `is_delete = true` emits the trimmed " Port = -1 DELETE" form
// used when the host is tearing down.
std::string lobby_update_build(const LobbyServerInfo &info, bool is_delete = false);

} // namespace opennova
