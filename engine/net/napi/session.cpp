#include <net/napi/session.h>

#include <base/io/cp1252.h> // cp1252_isspace
#include <base/io/crt_ftol.h> // retail_atol
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

// The retail atol over a param value (io::retail_atol: the locale's leading white space,
// 0xA0 included, a sign, trailing garbage ignored, non-numeric text reading 0, saturating at
// 32 bits; D-NET-384). [orig: _atol @0x76AB0A from CNapiGameSession_HandleVerifyResponse
//  @0x4D1E00, HandleHostVerifyResponse @0x4D59D0 and HandlePuntNotification @0x4D20B0]
int atol_field(const NapiField &f) {
	return io::retail_atol(field_to_string(f).c_str());
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

const char *server_command_target_name(ServerCommandTarget target) {
	switch (target) {
		case ServerCommandTarget::None: return "";
		case ServerCommandTarget::ByIndex: return "ByIndex";
		case ServerCommandTarget::ByIpAndPort: return "ByIpAndPort";
		case ServerCommandTarget::ByName: return "ByName";
		case ServerCommandTarget::ByPCID: return "ByPCID";
	}
	return "";
}

// [orig: String_TokenizeQuotedToArray @0x616d60 — the byte zero-extended (`movzx` @0x616da2)
//  into the locale-aware isspace @0x616da6, the in-quote test @0x616db2, the token start
//  @0x616dc7, the quote toggle @0x616ddc, the backslash copied @0x616def]
std::vector<std::string> tokenize_quoted(std::string_view text) {
	std::vector<std::string> tokens;
	bool in_token = false;
	bool in_quote = false;
	for (const char c : text) {
		// The CRT isspace under the game's ".ACP" LC_CTYPE, pinned to cp1252 (0xA0 splits too) so
		// the process locale never matters (docs/net/novaworld-net-re.md D-NET-381, D-NET-382).
		const bool space = cp1252_isspace(static_cast<uint8_t>(c));
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
	uint8_t min_args;  // tokens the body needs after the verb; fewer is the no-op tail
};
// In the witnessed dispatch order. min_args is the verb's token-count gate (`cmp edi, N; jle`
// to the no-op tail @0x4d3365, edi = the token count, so N tokens after the verb are needed),
// or 0 where every arg is optional or none is read.
constexpr VerbRow kVerbs[] = {
	{ServerCommandVerb::PuntPlayer, "PuntPlayer", true, 1},          // @0x4d23cc
	{ServerCommandVerb::TextChatServer, "TextChatServer", false, 1}, // @0x4d2584
	{ServerCommandVerb::TextChatPlayer, "TextChatPlayer", true, 2},  // @0x4d264a
	{ServerCommandVerb::CmdEchoPlayer, "CmdEchoPlayer", true, 2},    // @0x4d2791
	{ServerCommandVerb::KillPlayer, "KillPlayer", true, 1},          // @0x4d28d8
	{ServerCommandVerb::ChangeTeam, "ChangeTeam", true, 1},          // loc_4D31EA @0x4d31f6
	{ServerCommandVerb::SwapTeam, "SwapTeam", true, 1},              // loc_4D31EA @0x4d31f6
	{ServerCommandVerb::Cycle, "Cycle", false, 0},                   // winner optional @0x4d30f8
	{ServerCommandVerb::EndMission, "EndMission", false, 0},         // winner optional @0x4d30f8
	{ServerCommandVerb::GameOver, "GameOver", false, 0},             // winner optional @0x4d30f8
	{ServerCommandVerb::Earthquake, "Earthquake", false, 0},         // seconds optional @0x4d2ac2
	{ServerCommandVerb::Lightning, "Lightning", false, 0},           // reads no arg @0x4d2b5d
	{ServerCommandVerb::TimeOfDay, "TimeOfDay", false, 0},           // HHMM optional @0x4d2be3
	{ServerCommandVerb::SetServerName, "SetServerName", false, 1},   // @0x4d2cd4
	{ServerCommandVerb::SetServerMsg, "SetServerMsg", false, 1},     // @0x4d2d69
	{ServerCommandVerb::SetMPReset, "SetMPReset", false, 1},         // @0x4d2e12
	{ServerCommandVerb::ReloadPlayer, "ReloadPlayer", true, 1},      // @0x4d2e71
	{ServerCommandVerb::DisarmPlayer, "DisarmPlayer", true, 1},      // @0x4d2fbb
};

const VerbRow *find_verb_row(ServerCommandVerb verb) {
	for (const VerbRow &row : kVerbs) {
		if (row.verb == verb) return &row;
	}
	return nullptr;
}

} // namespace

bool parse_server_command(const NapiMessage &container, ServerCommand &out) {
	out = ServerCommand{};
	std::string cmd;
	bool have_cmd = false;
	for (const NapiField &f : container.fields) {
		if (strutil::iequals(f.name, "Cmd")) {
			cmd = copy_capped(field_to_string(f), SERVER_COMMAND_CMD_CAP);
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

// ---- The service side ---------------------------------------------------------

bool server_command_verb_takes_target(ServerCommandVerb verb) {
	const VerbRow *row = find_verb_row(verb);
	return row != nullptr && row->prefix_match;
}

bool server_command_verb_from_name(std::string_view name, ServerCommandVerb &out) {
	for (const VerbRow &row : kVerbs) {
		if (!strutil::iequals(name, row.name)) continue;
		out = row.verb;
		return true;
	}
	return false;
}

bool server_command_target_from_name(std::string_view name, ServerCommandTarget &out) {
	if (name.empty() || strutil::iequals(name, "None")) {
		out = ServerCommandTarget::None;
		return true;
	}
	for (const ServerCommandTarget target :
	     {ServerCommandTarget::ByIndex, ServerCommandTarget::ByIpAndPort, ServerCommandTarget::ByName,
	      ServerCommandTarget::ByPCID}) {
		if (!strutil::iequals(name, server_command_target_name(target))) continue;
		out = target;
		return true;
	}
	return false;
}

// [orig: String_TokenizeQuotedToArray @0x616d60] — inverted: a quoted run is one token.
std::string server_command_text(ServerCommandVerb verb, ServerCommandTarget target,
                                const std::vector<std::string> &args, const char **refusal) {
	auto refuse = [refusal](const char *why) {
		if (refusal != nullptr) *refusal = why;
		return std::string();
	};
	const VerbRow *row = find_verb_row(verb);
	if (row == nullptr) return refuse("no verb");
	// A pairing the reader drops: no suffix on a player-targeted verb (@0x4d24e3), or one on a
	// whole-token verb (Napi_StrCaseEqual, e.g. Cycle @0x4d2a46).
	if ((target != ServerCommandTarget::None) != row->prefix_match) {
		return refuse(row->prefix_match
				? "the verb needs a target suffix (ByIndex, ByIpAndPort, ByName or ByPCID)"
				: "the verb takes no target suffix");
	}
	// Fewer args than the verb's token-count gate: the reader drops the line (kVerbs' min_args).
	if (args.size() < row->min_args) return refuse("fewer args than the verb's token-count gate");
	// The tokenizer's isspace runs in the host's ANSI code page, not the C locale: WinMain's
	// System_InitTimerAndLocale sets LC_ALL to ".ACP" and only LC_NUMERIC back to "C", so on a
	// cp1252 host 0xA0 splits a token too, as tokenize_quoted does. Quote an arg holding one of
	// those seven spaces, and any other byte >= 0x80 besides, since a host on a double-byte code
	// page classes its high bytes otherwise (D-NET-382); a quoted run's bytes are copied as they
	// are, so a quote never changes the token. [orig: the tokenizer's isspace call @0x616da6 ->
	// the locale-aware CRT isspace @0x76b964; System_InitTimerAndLocale @0x762a00 —
	// setlocale(LC_ALL, ".ACP") @0x762a6e, setlocale(LC_NUMERIC, "C") @0x762a7a]
	auto needs_quotes = [](const std::string &arg) {
		if (arg.empty()) return true;
		for (const char c : arg) {
			const uint8_t byte = static_cast<uint8_t>(c);
			if (cp1252_isspace(byte) || byte >= 0x80) return true;
		}
		return false;
	};
	std::string text = server_command_verb_name(verb);
	text += server_command_target_name(target);
	for (const std::string &arg : args) {
		if (arg.find_first_of(std::string_view("\"\0", 2)) != std::string::npos)
			return refuse("an arg holds a double quote or a NUL, which the reader cannot carry");
		text.push_back(' ');
		if (needs_quotes(arg)) {
			text.push_back('"');
			text += arg;
			text.push_back('"');
		} else {
			text += arg;
		}
	}
	if (text.size() >= SERVER_COMMAND_CMD_CAP)
		return refuse("the line exceeds the reader's 511-character Cmd buffer");
	return text;
}

// [orig: CNapiGameSession_HandleServerCommand — the "Cmd" read @0x4d2333, the 0x200 copy
//  @0x4d2345..0x4d2356]
NapiMessage make_server_command(const std::string &cmd) {
	NapiMessage m;
	m.name = "ServerCommand";
	m.fields.push_back(str_field("Cmd", cmd));
	return m;
}

// [orig: CNapiGameSession_HandleServerMessage @0x4d1c50 — MsgCode @0x4d1c9e, MsgParam1 @0x4d1cbd,
//  MsgParam2 @0x4d1cde]
NapiMessage make_server_stop_hosting(int msg_code, int msg_param1, int msg_param2) {
	NapiMessage m;
	m.name = "ServerStopHosting";
	m.fields.push_back(str_field("MsgCode", std::to_string(msg_code)));
	m.fields.push_back(str_field("MsgParam1", std::to_string(msg_param1)));
	m.fields.push_back(str_field("MsgParam2", std::to_string(msg_param2)));
	return m;
}

} // namespace opennova
