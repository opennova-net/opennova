#include <napi/session.h>

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
	NapiField ch;
	ch.name = "CurrentlyHosting";
	const std::string ch_str = std::to_string(currently_hosting);
	ch.data.assign(ch_str.begin(), ch_str.end());
	m.fields.push_back(std::move(ch));
	NapiField vc;
	vc.name = "VarCheck";
	vc.data.assign(1, '1');
	m.fields.push_back(std::move(vc));
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
	NapiField var_list;
	var_list.name = "VarList";
	var_list.data.assign(list_name.begin(), list_name.end());
	list.fields.push_back(std::move(var_list));
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
		auto add = [&entry](const char *name, const std::string &value) {
			NapiField f;
			f.name = name;
			f.data.assign(value.begin(), value.end());
			entry.fields.push_back(std::move(f));
		};
		add("VarFNum", std::to_string(v.fnum));
		add("VarName", v.name);
		add("VarValue", v.value);
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
	NapiField cp;
	cp.name = "CurrentlyPlaying";
	const std::string cp_str = std::to_string(currently_playing);
	cp.data.assign(cp_str.begin(), cp_str.end());
	m.fields.push_back(std::move(cp));
	m.children.push_back(make_client_var_list("Cookie", cookie));
	m.children.push_back(make_client_var_list("PlaySetup", play_setup));
	return m;
}

} // namespace opennova
