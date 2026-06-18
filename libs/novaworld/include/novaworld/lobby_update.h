#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NovaWorld lobby state: fields a hosting jodemo publishes to
// POSTIPADDRESS:POSTIPPORT from Lobby_UpdateServerInfo @ 0x4fe8c0.
// The wire form is a single-line text KV blob:
//
//   <LobbyName>  HostKey = <hostkey> <K> = <V> ... p=<playername>
//
// Values are sanitized by the retail lobby string helper. Hosting teardown is
// a separate ClientStopHosting statement, not a "Port = -1 DELETE" variant.
struct LobbyServerInfo {
	// Identity / addressing.
	std::string lobby_name;
	std::string host_key;
	std::string host_did; // obsolete witness; not emitted

	// Server identity.
	std::string server_name;
	std::string game_type;
	std::string mission_name;
	std::string region;
	std::string msg;
	int players = 0;
	int max_players = 0;
	int mi1 = 0;
	int mi2 = 0;
	int mi3 = 0;

	// Boolean-ish flags emitted as retail Y/N tokens.
	bool dedicated = false;
	bool locked = false;
	bool skins_allowed = true;
	bool password_protected = false;
	bool tracers_disabled = false;

	// Misc UI / metadata.
	std::string time_left;
	std::string country;
	std::string tod;
	std::string access_code_list; // obsolete witness; not emitted
	int app_id = 0;
	int pcid_key = 0;
	int game_server_baffle_key = 0;
	std::string stat = "N";
	std::string level_range = " ";
	int bb_mode = 0;
	std::string mod;
	std::string pix = "1";
	std::string gv;
	std::string version;
	std::string country_name;
	std::string lang;
	int timezone_bias = 0;
	bool pb_server = false;
	int allow_ping = 110;
	std::string ver1 = "3";
	std::string ver2 = "2345";
	std::string port = "-1";
	std::string exp;
	std::string expbits = "3";
	std::string joicon2 = "4000";
	std::string gcc;

	// Uptime formatted like "%ld %2.2ld:%2.2ld:%2.2ld"; emitted as Age.
	std::string uptime;

	std::vector<std::string> player_names;
	std::vector<std::pair<std::string, std::string>> extra_pairs;
};

std::string lobby_update_build(const LobbyServerInfo &info);

} // namespace opennova
