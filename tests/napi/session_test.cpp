#include <napi/session.h>
#include <napi/tlv.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Read a field's value (as a string) from a container by name.
std::string field_str(const opennova::NapiMessage &m, const char *name) {
	for (const auto &f : m.fields) {
		if (f.name == name) return std::string(f.data.begin(), f.data.end());
	}
	return {};
}

// Error → NWEC string mapping matches jodemo's CNapiGameSession_ConnectOrHost
// error-dispatch table.
bool check_error_nwec_mapping() {
	using opennova::NovaWorldError;
	using opennova::novaworld_error_tag;
	if (!expect(novaworld_error_tag(NovaWorldError::Banned) == "NWEC11", "200 -> NWEC11")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Restricted) == "NWEC12", "203 -> NWEC12")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3000) == "NWEC04", "3000 -> NWEC04")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3001) == "NWEC05", "3001 -> NWEC05")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3002) == "NWEC06", "3002 -> NWEC06")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3003) == "NWEC07", "3003 -> NWEC07")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3004) == "NWEC08", "3004 -> NWEC08")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3005) == "NWEC09", "3005 -> NWEC09")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3006) == "NWEC10", "3006 -> NWEC10")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::TimeoutPoll) == "NWEC02", "timeout -> NWEC02")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::UserCancelled) == "NWEC03", "user-cancel -> NWEC03")) return false;
	return true;
}

// from_code handles both known codes and the "default fall-through".
bool check_error_from_code() {
	using opennova::novaworld_error_from_code;
	using opennova::NovaWorldError;
	if (!expect(novaworld_error_from_code(200) == NovaWorldError::Banned, "200")) return false;
	if (!expect(novaworld_error_from_code(203) == NovaWorldError::Restricted, "203")) return false;
	if (!expect(novaworld_error_from_code(3006) == NovaWorldError::Reject3006, "3006")) return false;
	// [D-NET-25] 1009 -> NWEC14; an unknown NONZERO reject -> UnknownReject -> NWEC13 (NOT the NWEC02
	// poll-timeout path, which is code -1).
	if (!expect(novaworld_error_from_code(1009) == NovaWorldError::Reject1009, "1009 -> Reject1009")) return false;
	if (!expect(opennova::novaworld_error_tag(NovaWorldError::Reject1009) == "NWEC14", "1009 -> NWEC14")) return false;
	if (!expect(novaworld_error_from_code(9999) == NovaWorldError::UnknownReject, "unknown reject -> UnknownReject")) return false;
	if (!expect(opennova::novaworld_error_tag(NovaWorldError::UnknownReject) == "NWEC13", "unknown reject -> NWEC13")) return false;
	if (!expect(novaworld_error_from_code(-1) == NovaWorldError::TimeoutPoll, "poll timeout -> NWEC02")) return false;
	return true;
}

// Timing constants match the witnessed immediates.
bool check_timing_constants() {
	using namespace opennova;
	if (!expect(SESSION_CONNECT_TIMEOUT_MS == 60000u, "connect/host poll timeout 60s (0xEA60)")) return false;
	if (!expect(SESSION_PERIODIC_UPDATE_TIMEOUT_MS == 20000u, "periodic-update timeout 20s (0x4E20)")) return false;
	if (!expect(SESSION_MESSAGE_CHUNK_BYTES == 1300u, "message chunk size 1300 bytes")) return false;
	if (!expect(SESSION_TIMEOUT_RANDOM_MIN_MS == 1000u, "transport random min")) return false;
	if (!expect(SESSION_TIMEOUT_RANDOM_MAX_MS == 9999u, "transport random max")) return false;
	return true;
}

// State-code values match the witnessed dword_989574 transitions.
bool check_state_values() {
	using opennova::SessionState;
	if (!expect(static_cast<int>(SessionState::HostStarting) == 5, "HostStarting == 5")) return false;
	if (!expect(static_cast<int>(SessionState::HostEstablished) == 6, "HostEstablished == 6")) return false;
	if (!expect(static_cast<int>(SessionState::Connecting) == 7, "Connecting == 7")) return false;
	if (!expect(static_cast<int>(SessionState::Connected) == 8, "Connected == 8")) return false;
	return true;
}

// Handshake message builders produce containers with the exact names used
// by the jodemo senders.
bool check_handshake_names() {
	using opennova::make_client_connected;
	using opennova::make_client_host_request;
	using opennova::make_client_host_update;
	using opennova::make_client_play_request;
	using opennova::make_client_stop_hosting;
	using opennova::make_client_stop_playing;
	using opennova::NapiMessage;
	if (!expect(make_client_connected().name == "ClientConnected", "ClientConnected name")) return false;
	if (!expect(make_client_stop_hosting().name == "ClientStopHosting", "ClientStopHosting name")) return false;
	if (!expect(make_client_stop_playing().name == "ClientStopPlaying", "ClientStopPlaying name")) return false;

	// ClientHostRequest = CurrentlyHosting + VarCheck fields, then the Cookie,
	// HostSetup, Host, PlayerList var-lists (each a ClientVarList carrying a
	// VarList field + ClientVar children). [orig: CNapiGameSession_SendHostRequest
	// @ 0x4d3700 / NapiStatement_SerializeVarList @ 0x4d0660]
	using opennova::ClientVar;
	auto hr = make_client_host_request(
		1,
		{{0, "NWUID", "u"}},                                   // Cookie
		{{0, "AppId", "0"}, {0, "MaxPlayers", "65"}},          // HostSetup
		{{0, "ServerName", "OpenNova Host"}, {0, "Players", "1"}}, // Host
		{{0, "Slot0", "Taylor"}});                             // PlayerList
	if (!expect(hr.name == "ClientHostRequest", "ClientHostRequest name")) return false;
	if (!expect(field_str(hr, "CurrentlyHosting") == "1", "CurrentlyHosting == \"1\"")) return false;
	if (!expect(field_str(hr, "VarCheck") == "1", "VarCheck == \"1\"")) return false;
	if (!expect(hr.children.size() == 4, "ClientHostRequest has 4 var-lists")) return false;
	const char *hr_lists[] = {"Cookie", "HostSetup", "Host", "PlayerList"};
	for (int i = 0; i < 4; ++i) {
		if (!expect(hr.children[i].name == "ClientVarList", "host-request child is ClientVarList")) return false;
		if (!expect(field_str(hr.children[i], "VarList") == hr_lists[i], "host-request VarList name")) return false;
	}
	// Host var-list carries its ClientVar entries (VarName/VarValue).
	if (!expect(hr.children[2].children.size() == 2, "Host has 2 ClientVar")) return false;
	if (!expect(field_str(hr.children[2].children[0], "VarName") == "ServerName" &&
			field_str(hr.children[2].children[0], "VarValue") == "OpenNova Host",
			"Host first ClientVar ServerName")) return false;

	auto hu = make_client_host_update(
		{{0, "Players", "2"}},                                 // Host
		{{0, "Slot0", "Taylor"}, {0, "Slot1", "Joiner"}});     // PlayerList
	if (!expect(hu.name == "ClientHostUpdate", "ClientHostUpdate name")) return false;
	if (!expect(hu.children.size() == 2, "2 var-lists")) return false;
	if (!expect(hu.children[0].name == "ClientVarList" &&
			field_str(hu.children[0], "VarList") == "Host", "update child 0 ClientVarList(Host)")) return false;
	if (!expect(hu.children[1].name == "ClientVarList" &&
			field_str(hu.children[1], "VarList") == "PlayerList", "update child 1 ClientVarList(PlayerList)")) return false;

	// ClientPlayRequest = a top-level CurrentlyPlaying field, then the Cookie and
	// PlaySetup var-lists (each a ClientVarList carrying a VarList field + ClientVar
	// children), Cookie FIRST. [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920 /
	// NapiStatement_SerializeVarList @ 0x4d0660]
	using opennova::ClientVar;
	auto pr = make_client_play_request(
		7,
		{{0, "NWUID", "abc"}, {0, "NWHWI", "gpu"}},  // Cookie vars
		{{0, "Mission", "ASH_G11A"}});               // PlaySetup vars
	if (!expect(pr.name == "ClientPlayRequest", "ClientPlayRequest name")) return false;
	if (!expect(pr.fields.size() == 1 && pr.fields[0].name == "CurrentlyPlaying",
			"top-level CurrentlyPlaying field")) return false;
	if (!expect(field_str(pr, "CurrentlyPlaying") == "7", "CurrentlyPlaying == \"7\"")) return false;
	if (!expect(pr.children.size() == 2, "2 var-lists")) return false;
	if (!expect(pr.children[0].name == "ClientVarList" &&
			field_str(pr.children[0], "VarList") == "Cookie",
			"child 0 ClientVarList(Cookie)")) return false;
	if (!expect(pr.children[1].name == "ClientVarList" &&
			field_str(pr.children[1], "VarList") == "PlaySetup",
			"child 1 ClientVarList(PlaySetup)")) return false;
	// Cookie var-list carries two ClientVar children (VarFNum/VarName/VarValue).
	if (!expect(pr.children[0].children.size() == 2, "Cookie has 2 ClientVar")) return false;
	if (!expect(pr.children[0].children[0].name == "ClientVar", "Cookie child is ClientVar")) return false;
	if (!expect(field_str(pr.children[0].children[0], "VarName") == "NWUID" &&
			field_str(pr.children[0].children[0], "VarValue") == "abc" &&
			field_str(pr.children[0].children[0], "VarFNum") == "0",
			"first Cookie ClientVar VarFNum/VarName/VarValue")) return false;
	return true;
}

// End-to-end: build a handshake message, serialize via napi_stream_encode,
// round-trip-decode, and confirm structural equality.
bool check_handshake_wire_roundtrip() {
	using opennova::ClientVar;
	auto req = opennova::make_client_host_update(
		{{0, "ServerIP", "127.0.0.1"}, {0, "ServerPortNumber", "4444"}},  // Host
		{{0, "Slot0", "player"}});                                        // PlayerList
	std::vector<opennova::NapiMessage> stream = {req};
	std::vector<uint8_t> buf(opennova::napi_stream_size(stream) + 16, 0);
	size_t enc_size = 0;
	if (!expect(opennova::napi_stream_encode(stream, buf.data(), buf.size(), &enc_size) == 0,
			"encode handshake stream")) return false;
	std::vector<opennova::NapiMessage> decoded;
	size_t cons = 0;
	if (!expect(opennova::napi_stream_decode(buf.data(), enc_size, decoded, &cons) == 0,
			"decode handshake stream")) return false;
	if (!expect(decoded.size() == 1 && decoded[0].name == "ClientHostUpdate",
			"decoded root is ClientHostUpdate")) return false;
	if (!expect(decoded[0].children.size() == 2, "two children")) return false;
	// Both children survive as ClientVarLists with their VarList names.
	if (!expect(decoded[0].children[0].name == "ClientVarList", "child 0 ClientVarList")) return false;
	if (!expect(field_str(decoded[0].children[0], "VarList") == "Host", "child 0 VarList=Host")) return false;
	if (!expect(decoded[0].children[1].name == "ClientVarList", "child 1 ClientVarList")) return false;
	if (!expect(field_str(decoded[0].children[1], "VarList") == "PlayerList", "child 1 VarList=PlayerList")) return false;
	// The Host var-list's first ClientVar round-trips with its name/value.
	const auto &host_list = decoded[0].children[0];
	if (!expect(host_list.children.size() == 2, "Host var-list has 2 ClientVar")) return false;
	if (!expect(host_list.children[0].name == "ClientVar", "Host child is ClientVar")) return false;
	if (!expect(field_str(host_list.children[0], "VarName") == "ServerIP" &&
			field_str(host_list.children[0], "VarValue") == "127.0.0.1",
			"Host first ClientVar ServerIP")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_error_nwec_mapping()) return 1;
	if (!check_error_from_code()) return 1;
	if (!check_timing_constants()) return 1;
	if (!check_state_values()) return 1;
	if (!check_handshake_names()) return 1;
	if (!check_handshake_wire_roundtrip()) return 1;
	std::printf("OK: session state + NWEC map + handshake builders + wire roundtrip\n");
	return 0;
}
