#include <napi/session.h>
#include <napi/tlv.h>

#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
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
	if (!expect(novaworld_error_from_code(9999) == NovaWorldError::TimeoutPoll, "unknown falls through to TimeoutPoll (NWEC13-adjacent)")) return false;
	return true;
}

// Timing constants match the witnessed immediates.
bool check_timing_constants() {
	using namespace opennova;
	if (!expect(SESSION_CONNECT_TIMEOUT_MS == 20000u, "connect timeout 20s (0x4E20)")) return false;
	if (!expect(SESSION_HANDSHAKE_RETRANSMIT_MS == 1300u, "handshake retransmit 1300ms")) return false;
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

	auto hr = make_client_host_request(NapiMessage{}, NapiMessage{}, NapiMessage{});
	if (!expect(hr.name == "ClientHostRequest", "ClientHostRequest name")) return false;
	if (!expect(hr.children.size() == 3, "ClientHostRequest has 3 children")) return false;
	if (!expect(hr.children[0].name == "HostSetup", "child 0 HostSetup")) return false;
	if (!expect(hr.children[1].name == "Host", "child 1 Host")) return false;
	if (!expect(hr.children[2].name == "PlayerList", "child 2 PlayerList")) return false;

	auto hu = make_client_host_update(NapiMessage{}, NapiMessage{});
	if (!expect(hu.name == "ClientHostUpdate", "ClientHostUpdate name")) return false;
	if (!expect(hu.children.size() == 2, "2 children")) return false;
	if (!expect(hu.children[0].name == "Host", "child 0 Host")) return false;
	if (!expect(hu.children[1].name == "PlayerList", "child 1 PlayerList")) return false;

	auto pr = make_client_play_request(NapiMessage{}, NapiMessage{});
	if (!expect(pr.name == "ClientPlayRequest", "ClientPlayRequest name")) return false;
	if (!expect(pr.children.size() == 2, "2 children")) return false;
	if (!expect(pr.children[0].name == "PlaySetup", "child 0 PlaySetup")) return false;
	if (!expect(pr.children[1].name == "Cookie", "child 1 Cookie")) return false;
	return true;
}

// End-to-end: build a handshake message, serialize via napi_stream_encode,
// round-trip-decode, and confirm structural equality.
bool check_handshake_wire_roundtrip() {
	opennova::NapiMessage host;
	host.fields.push_back({"Addr", {0x7F, 0x00, 0x00, 0x01}});
	host.fields.push_back({"Port", {0x5C, 0x11}}); // 0x115C = 4444
	opennova::NapiMessage players;
	opennova::NapiField slot;
	slot.name = "Slot0";
	slot.data = {'p', 'l', 'a', 'y', 'e', 'r'};
	players.fields.push_back(slot);

	auto req = opennova::make_client_host_update(host, players);
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
	if (!expect(decoded[0].children[0].name == "Host", "Host child")) return false;
	if (!expect(decoded[0].children[1].name == "PlayerList", "PlayerList child")) return false;
	if (!expect(decoded[0].children[0].fields.size() == 2, "Host has 2 fields")) return false;
	if (!expect(decoded[0].children[0].fields[0].name == "Addr", "Addr field")) return false;
	if (!expect(decoded[0].children[0].fields[0].data == std::vector<uint8_t>({0x7F, 0x00, 0x00, 0x01}), "Addr bytes")) return false;
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
