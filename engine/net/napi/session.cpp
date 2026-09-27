#include <net/napi/session.h>

#include <base/io/strutil.h>

#include <cstdlib>
#include <string>
#include <utility>

namespace opennova {

std::string novaworld_error_tag(NovaWorldError err) {
	switch (err) {
		case NovaWorldError::Ok: return "";
		case NovaWorldError::TimeoutPoll: return "NWEC02";
		case NovaWorldError::UserCancelled: return "NWEC03";
		case NovaWorldError::Banned: return "NWEC11";
		case NovaWorldError::Restricted: return "NWEC12";
		case NovaWorldError::Reject3000: return "NWEC04";
		case NovaWorldError::Reject3001: return "NWEC05";
		case NovaWorldError::Reject3002: return "NWEC06";
		case NovaWorldError::Reject3003: return "NWEC07";
		case NovaWorldError::Reject3004: return "NWEC08";
		case NovaWorldError::Reject3005: return "NWEC09";
		case NovaWorldError::Reject3006: return "NWEC10";
		case NovaWorldError::Reject1009: return "NWEC14"; // [D-NET-25] special-case reject
		case NovaWorldError::UnknownReject: return "NWEC13";
	}
	// Binary's default branch is NWEC13 for unknown positive reject codes.
	return "NWEC13";
}

NovaWorldError novaworld_error_from_code(int code) {
	switch (code) {
		case 0: return NovaWorldError::Ok;
		case -1: return NovaWorldError::TimeoutPoll;
		case -2: return NovaWorldError::UserCancelled;
		case 200: return NovaWorldError::Banned;
		case 203: return NovaWorldError::Restricted;
		case 3000: return NovaWorldError::Reject3000;
		case 3001: return NovaWorldError::Reject3001;
		case 3002: return NovaWorldError::Reject3002;
		case 3003: return NovaWorldError::Reject3003;
		case 3004: return NovaWorldError::Reject3004;
		case 3005: return NovaWorldError::Reject3005;
		case 3006: return NovaWorldError::Reject3006;
		case 1009: return NovaWorldError::Reject1009; // [D-NET-25] -> NWEC14
		default:
			// [D-NET-25] A reject with an unrecognized NONZERO code maps to NWEC13 (the dword_B60110
			// switch default @0x4d4f10), NOT NWEC02 — NWEC02 is the poll-timeout path (code -1). Was
			// wrongly returning TimeoutPoll (NWEC02) here.
			return NovaWorldError::UnknownReject;
	}
}

// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10, the `*(this + 304)` switch @0x4d5207]
std::string novaworld_host_error_tag(int msg_code) {
	switch (msg_code) {
		case 0x3E9: return "NWEC53";
		case 0x3EC: return "NWEC54";
		case 0x3ED: return "NWEC55";
		case 0x3F0: return "NWEC56";
		case 0x3F1: return "NWEC57";
		case 0x3F3: return "NWEC60";
		default: return "NWEC58";
	}
}

// [orig: UI_ProcessLANSessionStateMachine @0x558de0 — state 1 @0x558ec6.., state 3 @0x5590c5..]
std::string novaworld_gate_error_tag(int gate_result, bool response_phase) {
	switch (gate_result) {
		case -2: return "NWEC18";
		case -3: return "NWEC19";
		case -4: return "NWEC20";
		case -5: return "NWEC21";
		case -6: return "NWEC22";
		case -7: return "NWEC23";
		case -8: return "NWEC24";
		case -9:
			if (response_phase) return "NWEC25";
			break;
		default:
			break;
	}
	return response_phase ? "NWEC16" : "NWEC15";
}

namespace {

// The 52-row {code, key} table at dword_7CB960 / off_7CB964, read from .rdata.
struct ServerMsgCodeRow {
	int code;
	const char *key;
};
constexpr ServerMsgCodeRow kServerMsgCodeTable[] = {
	{1, "NWUSERVERMSGCODE_UNKNOWNERROR"},
	{2, "NWUSERVERMSGCODE_INTERNALERROR"},
	{3, "NWUSERVERMSGCODE_FEATURENOTIMPLEMENTEDYET"},
	{4, "NWUSERVERMSGCODE_OPERATIONTIMEDOUT"},
	{5, "NWUSERVERMSGCODE_FAILEDCONSISTENCYCHECK"},
	{6, "NWUSERVERMSGCODE_NOVAWORLDDOWNFORMAINTENANCE"},
	{7, "NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT"},
	{8, "NWUSERVERMSGCODE_MISUSEOFNWUPROTOCOL"},
	{100, "NWUSERVERMSGCODE_GENERALDATABASEERROR"},
	{101, "NWUSERVERMSGCODE_DATABASETIMEOUT"},
	{102, "NWUSERVERMSGCODE_DATABASEEXECERROR"},
	{103, "NWUSERVERMSGCODE_DATABASEGETTABLEERROR"},
	{104, "NWUSERVERMSGCODE_CANNOTEXTRACTIDENTITY"},
	{105, "NWUSERVERMSGCODE_DATABASECONSISTENCYERROR"},
	{106, "NWUSERVERMSGCODE_DATABASEDENIED"},
	{107, "NWUSERVERMSGCODE_DATABASENOTAVAILABLE"},
	{108, "NWUSERVERMSGCODE_DATABASETOOBUSY"},
	{200, "NWUSERVERMSGCODE_NEEDTOLOGIN"},
	{201, "NWUSERVERMSGCODE_ACCOUNTDOESNOTEXIST"},
	{202, "NWUSERVERMSGCODE_ACCOUNTINFOISBAD"},
	{203, "NWUSERVERMSGCODE_ACCOUNTALREADYINUSE"},
	{204, "NWUSERVERMSGCODE_ACCOUNTSUSPENDED"},
	{205, "NWUSERVERMSGCODE_ACCOUNTBANNED"},
	{206, "NWUSERVERMSGCODE_ACCOUNTRESTRICTED"},
	{207, "NWUSERVERMSGCODE_REQUIRESPREMIUMSERVICE"},
	{1000, "NWUSERVERMSGCODE_MISSINGLOBBYNAME"},
	{1001, "NWUSERVERMSGCODE_MISSINGSERVERNAME"},
	{1002, "NWUSERVERMSGCODE_MISSINGMAXPLAYERS"},
	{1003, "NWUSERVERMSGCODE_MISSINGAPPID"},
	{1004, "NWUSERVERMSGCODE_REJECTEDHOSTINGINFO"},
	{1005, "NWUSERVERMSGCODE_BADSERVERNAME"},
	{1006, "NWUSERVERMSGCODE_BADMAXPLAYERS"},
	{1007, "NWUSERVERMSGCODE_LOBBYDOESNOTEXIST"},
	{1008, "NWUSERVERMSGCODE_TOOMANYSERVERSRUNNING"},
	{1009, "NWUSERVERMSGCODE_SERVERNAMENOTACCEPTED"},
	{1010, "NWUSERVERMSGCODE_BADNUMPLAYERS"},
	{1011, "NWUSERVERMSGCODE_MSGNOTACCEPTED"},
	{2000, "NWUSERVERMSGCODE_NOVAWORLDLOBBYRESTRICTED"},
	{2001, "NWUSERVERMSGCODE_AFFILIATELOBBYRESTRICTED"},
	{2002, "NWUSERVERMSGCODE_BADHOSTVARVALUE"},
	{3000, "NWUSERVERMSGCODE_MISSINGGAMESERVERIPADDRESS"},
	{3001, "NWUSERVERMSGCODE_MISSINGGAMESERVERPORTNUMBER"},
	{3002, "NWUSERVERMSGCODE_MISSINGGAMESERVERAPPID"},
	{3003, "NWUSERVERMSGCODE_GAMESERVERDOESNOTEXIST"},
	{3004, "NWUSERVERMSGCODE_APPIDSDONOTMATCH"},
	{3005, "NWUSERVERMSGCODE_PFIDSDONOTMATCH"},
	{3006, "NWUSERVERMSGCODE_PFID2SDONOTMATCH"},
	{4000, "NWUSERVERMSGCODE_GAMESERVERHASSHUTDOWN"},
	{4001, "NWUSERVERMSGCODE_ACCOUNTBEINGUSEDSOMEWHEREELSE"},
	{6000, "NWUSERVERMSGCODE_SERVERNOTHOSTING"},
	{6001, "NWUSERVERMSGCODE_JOINTICKETNOTFOUND"},
	{6002, "NWUSERVERMSGCODE_JOINTICKETIPADDRESSDOESNOTMATCH"},
};
static_assert(sizeof(kServerMsgCodeTable) / sizeof(kServerMsgCodeTable[0]) == 0x34,
              "the witnessed 52-row table");

// The retail atol over a param value: strtol base 10 (leading whitespace and sign tolerant,
// trailing garbage ignored, non-numeric text reads as 0).
int atol_field(const NapiField &f) {
	return static_cast<int>(std::strtol(field_to_string(f).c_str(), nullptr, 10));
}

NapiField str_field(const char *name, const std::string &value) {
	NapiField f;
	f.name = name;
	f.data.assign(value.begin(), value.end());
	return f;
}

// Napi_CopyString(dst, src, N): at most N-1 characters, always NUL-terminated.
std::string copy_capped(const std::string &s, size_t n) {
	return s.size() < n ? s : s.substr(0, n - 1);
}

} // namespace

std::string novaworld_server_msg_code_key(int msg_code) {
	for (const ServerMsgCodeRow &row : kServerMsgCodeTable) {
		if (row.code == msg_code) return row.key;
	}
	return "NWUSERVERMSGCODE_UNKNOWNERROR";
}

ServerResultFields parse_server_result_fields(const NapiMessage &container) {
	ServerResultFields out;
	for (const NapiField &f : container.fields) {
		if (strutil::iequals(f.name, "Success")) out.success = atol_field(f);
		else if (strutil::iequals(f.name, "MsgCode")) out.msg_code = atol_field(f);
		else if (strutil::iequals(f.name, "MsgParam1")) out.msg_param1 = atol_field(f);
		else if (strutil::iequals(f.name, "MsgParam2")) out.msg_param2 = atol_field(f);
	}
	return out;
}

std::map<std::string, std::string> parse_host_commands(const NapiMessage &result) {
	std::map<std::string, std::string> out;
	for (const NapiMessage &child : result.children) {
		if (!strutil::iequals(child.name, "ServerVarList")) continue;
		std::string list_name;
		for (const NapiField &f : child.fields) {
			if (strutil::iequals(f.name, "VarList")) list_name = copy_capped(field_to_string(f), 64);
		}
		if (!strutil::iequals(list_name, "HostCommands")) continue;
		for (const NapiMessage &var : child.children) {
			if (!strutil::iequals(var.name, "ServerVar")) continue;
			std::string name, value;
			for (const NapiField &f : var.fields) {
				if (strutil::iequals(f.name, "VarName")) name = copy_capped(field_to_string(f), 64);
				else if (strutil::iequals(f.name, "VarValue")) value = copy_capped(field_to_string(f), 4096);
			}
			if (!name.empty()) out[name] = value;
		}
	}
	return out;
}

NapiMessage make_client_connected() {
	NapiMessage m;
	m.name = "ClientConnected";
	return m;
}

NapiMessage make_client_stop_hosting() {
	NapiMessage m;
	m.name = "ClientStopHosting";
	return m;
}

NapiMessage make_client_stop_playing() {
	NapiMessage m;
	m.name = "ClientStopPlaying";
	return m;
}

// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700] — the host-registration
// statement: CurrentlyHosting + VarCheck params, then four serialized var-lists
// (Cookie / HostSetup / Host / PlayerList). The earlier builder emitted three
// containers literally named HostSetup/Host/PlayerList with no CurrentlyHosting/
// VarCheck and no Cookie — a shape our own gate (extract_var_lists, which keys on
// "ClientVarList" + the VarList field) could not parse, the same bug the
// play-request builder already had corrected.
NapiMessage make_client_host_request(int currently_hosting,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &host_setup,
                                     const std::vector<ClientVar> &host,
                                     const std::vector<ClientVar> &player_list) {
	NapiMessage m;
	m.name = "ClientHostRequest";
	m.fields.push_back(str_field("CurrentlyHosting", std::to_string(currently_hosting)));
	m.fields.push_back(str_field("VarCheck", "1"));
	m.children.push_back(make_client_var_list("Cookie", cookie));
	m.children.push_back(make_client_var_list("HostSetup", host_setup));
	m.children.push_back(make_client_var_list("Host", host));
	m.children.push_back(make_client_var_list("PlayerList", player_list));
	return m;
}

// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860] — the heartbeat refresh:
// only the Host and PlayerList var-lists, no params, no Cookie/HostSetup.
NapiMessage make_client_host_update(const std::vector<ClientVar> &host,
                                    const std::vector<ClientVar> &player_list) {
	NapiMessage m;
	m.name = "ClientHostUpdate";
	m.children.push_back(make_client_var_list("Host", host));
	m.children.push_back(make_client_var_list("PlayerList", player_list));
	return m;
}

// [orig: NapiStatement_SerializeVarList @ 0x4d0660] — a "ClientVarList" parent
// carrying a "VarList" param (the list name), then one "ClientVar" child per
// entry with VarFNum / VarName / VarValue. Mirrors the verify-request var-list
// builder in client_session.cpp and is the shape lobby_session::extract_var_lists
// parses.
NapiMessage make_client_var_list(const std::string &list_name,
                                 const std::vector<ClientVar> &vars) {
	NapiMessage list;
	list.name = "ClientVarList";
	list.fields.push_back(str_field("VarList", list_name));
	for (const auto &v : vars) {
		// [D-NET-28] The statement param builder rejects a param whose name is not 1..63 chars or whose
		// data exceeds 4095 bytes (sets an error flag + returns null). [orig: NapiStatementParam_Create
		// @0x632b30: `strlen(name)-1 > 0x3E` / `dataSize >= 4096`]. Our param NAMES are the fixed
		// "VarFNum"/"VarName"/"VarValue" literals (always valid); the data limit applies to the values
		// (v.name carried as the VarName param's data, v.value as VarValue's). Skip a violating entry —
		// the faithful reject (never triggers with in-range config; defensive parity).
		if (v.name.size() > 4095 || v.value.size() > 4095) continue;
		NapiMessage entry;
		entry.name = "ClientVar";
		entry.fields.push_back(str_field("VarFNum", std::to_string(v.fnum)));
		entry.fields.push_back(str_field("VarName", v.name));
		entry.fields.push_back(str_field("VarValue", v.value));
		list.children.push_back(std::move(entry));
	}
	return list;
}

// [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920] — a top-level
// CurrentlyPlaying param (decimal of the flag) FIRST, then the Cookie var-list,
// then the PlaySetup var-list. The earlier builder emitted two containers
// literally named PlaySetup/Cookie with no CurrentlyPlaying — output our own
// server (extract_var_lists, which keys on "ClientVarList") could not parse.
NapiMessage make_client_play_request(int currently_playing,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &play_setup) {
	NapiMessage m;
	m.name = "ClientPlayRequest";
	m.fields.push_back(str_field("CurrentlyPlaying", std::to_string(currently_playing)));
	m.children.push_back(make_client_var_list("Cookie", cookie));
	m.children.push_back(make_client_var_list("PlaySetup", play_setup));
	return m;
}

// [orig: CNapiGameSession_ConnectOrHost @0x4d53d1..0x4d542b — ServerName @0x4d53d1,
//  IpAddress @0x4d53e7, PortNumber @0x4d53fd, AppId @0x4d5413, the int "Lan" @0x4d542b]
std::vector<ClientVar> make_play_setup_vars(const std::string &server_name,
                                            const std::string &ip_address,
                                            const std::string &port_number,
                                            const std::string &app_id, int lan) {
	return {
		{0, "ServerName", server_name},
		{0, "IpAddress", ip_address},
		{0, "PortNumber", port_number},
		{0, "AppId", app_id},
		{0, "Lan", std::to_string(lan)},
	};
}

// [orig: CNapiGameSession_SendPlayerAdded @0x4cfec0 — PlayerNumber, PlayerName, then the
//  four buffers PlayerIpAndPort / PlayerPCID / PlayerTeam / PlayerType]
NapiMessage make_client_host_player_added(int player_number, const std::string &player_name,
                                          const std::string &ip_and_port,
                                          const std::string &pcid, const std::string &team,
                                          const std::string &type) {
	NapiMessage m;
	m.name = "ClientHostPlayerAdded";
	m.fields.push_back(str_field("PlayerNumber", std::to_string(player_number)));
	m.fields.push_back(str_field("PlayerName", player_name));
	m.fields.push_back(str_field("PlayerIpAndPort", ip_and_port));
	m.fields.push_back(str_field("PlayerPCID", pcid));
	m.fields.push_back(str_field("PlayerTeam", team));
	m.fields.push_back(str_field("PlayerType", type));
	return m;
}

// [orig: CNapiGameSession_SendPlayerRemoved @0x4d01a0]
NapiMessage make_client_host_player_removed(int player_number) {
	NapiMessage m;
	m.name = "ClientHostPlayerRemoved";
	m.fields.push_back(str_field("PlayerNumber", std::to_string(player_number)));
	return m;
}

// [orig: CNapiGameSession_SendPlayEnterRequest @0x4d02a0 — the three "%ld" params from
//  playerNode[6]/[12]/[13], then the JoinTicket buffer (empty when the KV has none)]
NapiMessage make_client_player_enter_request(uint32_t connection_id, uint32_t ip_address,
                                             uint32_t port_number,
                                             const std::string &join_ticket) {
	NapiMessage m;
	m.name = "ClientPlayerEnterRequest";
	m.fields.push_back(str_field("ConnectionId", std::to_string(connection_id)));
	m.fields.push_back(str_field("IpAddress", std::to_string(ip_address)));
	m.fields.push_back(str_field("PortNumber", std::to_string(port_number)));
	m.fields.push_back(str_field("JoinTicket", join_ticket));
	return m;
}

// [orig: CNapiGameSession_SendGLSVSSRequest @0x4d3a40]
NapiMessage make_client_glsvss_request(const std::string &request,
                                       const std::vector<ClientVar> &cookie) {
	NapiMessage m;
	m.name = "ClientGLSVSSRequest";
	m.fields.push_back(str_field("GLSVSSRequest", request));
	m.children.push_back(make_client_var_list("Cookie", cookie));
	return m;
}

// ---- ServerCommand ----------------------------------------------------------

const char *server_command_verb_name(ServerCommandVerb verb) {
	switch (verb) {
		case ServerCommandVerb::None: return "";
		case ServerCommandVerb::PuntPlayer: return "PuntPlayer";
		case ServerCommandVerb::TextChatServer: return "TextChatServer";
		case ServerCommandVerb::TextChatPlayer: return "TextChatPlayer";
		case ServerCommandVerb::CmdEchoPlayer: return "CmdEchoPlayer";
		case ServerCommandVerb::KillPlayer: return "KillPlayer";
		case ServerCommandVerb::ChangeTeam: return "ChangeTeam";
		case ServerCommandVerb::SwapTeam: return "SwapTeam";
		case ServerCommandVerb::Cycle: return "Cycle";
		case ServerCommandVerb::EndMission: return "EndMission";
		case ServerCommandVerb::GameOver: return "GameOver";
		case ServerCommandVerb::Earthquake: return "Earthquake";
		case ServerCommandVerb::Lightning: return "Lightning";
		case ServerCommandVerb::TimeOfDay: return "TimeOfDay";
		case ServerCommandVerb::SetServerName: return "SetServerName";
		case ServerCommandVerb::SetServerMsg: return "SetServerMsg";
		case ServerCommandVerb::SetMPReset: return "SetMPReset";
		case ServerCommandVerb::ReloadPlayer: return "ReloadPlayer";
		case ServerCommandVerb::DisarmPlayer: return "DisarmPlayer";
	}
	return "";
}

// [orig: String_TokenizeQuotedToArray @0x616d60]
std::vector<std::string> tokenize_quoted(std::string_view text) {
	std::vector<std::string> tokens;
	bool in_token = false;
	bool in_quote = false;
	for (const char c : text) {
		// The retail isspace set (C locale), spelled out so the process locale never matters.
		const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
		if (!space || in_quote) {
			if (!in_token) {
				in_token = true;
				tokens.emplace_back();
			}
			if (c == '"') {
				in_quote = !in_quote;
			} else {
				tokens.back().push_back(c);
			}
		} else if (in_token) {
			in_token = false;
		}
	}
	return tokens;
}

namespace {

// [orig: String_MatchSuffix @0x617040] — case-insensitive suffix test over the whole token.
bool match_suffix(std::string_view token, std::string_view suffix) {
	return strutil::ends_with_icase(token, suffix);
}

ServerCommandTarget parse_target_suffix(std::string_view token) {
	if (match_suffix(token, "ByIndex")) return ServerCommandTarget::ByIndex;
	if (match_suffix(token, "ByIpAndPort")) return ServerCommandTarget::ByIpAndPort;
	if (match_suffix(token, "ByName")) return ServerCommandTarget::ByName;
	if (match_suffix(token, "ByPCID")) return ServerCommandTarget::ByPCID;
	return ServerCommandTarget::None;
}

struct VerbRow {
	ServerCommandVerb verb;
	const char *name;
	bool prefix_match; // String_StartsWithNoCase (player-targeted) vs Napi_StrCaseEqual
};
// In the witnessed dispatch order.
constexpr VerbRow kVerbs[] = {
	{ServerCommandVerb::PuntPlayer, "PuntPlayer", true},
	{ServerCommandVerb::TextChatServer, "TextChatServer", false},
	{ServerCommandVerb::TextChatPlayer, "TextChatPlayer", true},
	{ServerCommandVerb::CmdEchoPlayer, "CmdEchoPlayer", true},
	{ServerCommandVerb::KillPlayer, "KillPlayer", true},
	{ServerCommandVerb::ChangeTeam, "ChangeTeam", true},
	{ServerCommandVerb::SwapTeam, "SwapTeam", true},
	{ServerCommandVerb::Cycle, "Cycle", false},
	{ServerCommandVerb::EndMission, "EndMission", false},
	{ServerCommandVerb::GameOver, "GameOver", false},
	{ServerCommandVerb::Earthquake, "Earthquake", false},
	{ServerCommandVerb::Lightning, "Lightning", false},
	{ServerCommandVerb::TimeOfDay, "TimeOfDay", false},
	{ServerCommandVerb::SetServerName, "SetServerName", false},
	{ServerCommandVerb::SetServerMsg, "SetServerMsg", false},
	{ServerCommandVerb::SetMPReset, "SetMPReset", false},
	{ServerCommandVerb::ReloadPlayer, "ReloadPlayer", true},
	{ServerCommandVerb::DisarmPlayer, "DisarmPlayer", true},
};

} // namespace

bool parse_server_command(const NapiMessage &container, ServerCommand &out) {
	out = ServerCommand{};
	std::string cmd;
	bool have_cmd = false;
	for (const NapiField &f : container.fields) {
		if (strutil::iequals(f.name, "Cmd")) {
			cmd = copy_capped(field_to_string(f), 512);
			have_cmd = true;
		}
	}
	if (!have_cmd) return false;
	std::vector<std::string> tokens = tokenize_quoted(cmd);
	if (tokens.empty()) return false;
	out.command = tokens[0];
	out.args.assign(tokens.begin() + 1, tokens.end());
	for (const VerbRow &row : kVerbs) {
		const bool hit = row.prefix_match ? strutil::starts_with_icase(out.command, row.name)
		                                  : strutil::iequals(out.command, row.name);
		if (!hit) continue;
		out.verb = row.verb;
		if (row.prefix_match) out.target = parse_target_suffix(out.command);
		return true;
	}
	return false;
}

} // namespace opennova
