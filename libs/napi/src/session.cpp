#include <napi/session.h>

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
		default:
			// Binary treats "unknown 3xxx" by falling through to NWEC13 via
			// dword_989588 default branch. Preserve that signal.
			return NovaWorldError::TimeoutPoll;
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

NapiMessage make_client_host_request(NapiMessage host_setup,
                                     NapiMessage host,
                                     NapiMessage player_list) {
	NapiMessage m;
	m.name = "ClientHostRequest";
	host_setup.name = "HostSetup";
	host.name = "Host";
	player_list.name = "PlayerList";
	m.children.push_back(std::move(host_setup));
	m.children.push_back(std::move(host));
	m.children.push_back(std::move(player_list));
	return m;
}

NapiMessage make_client_host_update(NapiMessage host, NapiMessage player_list) {
	NapiMessage m;
	m.name = "ClientHostUpdate";
	host.name = "Host";
	player_list.name = "PlayerList";
	m.children.push_back(std::move(host));
	m.children.push_back(std::move(player_list));
	return m;
}

NapiMessage make_client_play_request(NapiMessage play_setup, NapiMessage cookie) {
	NapiMessage m;
	m.name = "ClientPlayRequest";
	play_setup.name = "PlaySetup";
	cookie.name = "Cookie";
	m.children.push_back(std::move(play_setup));
	m.children.push_back(std::move(cookie));
	return m;
}

} // namespace opennova
