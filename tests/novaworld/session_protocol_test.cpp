#include <novaworld/session_protocol.h>

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

bool has_tag(const std::vector<opennova::ProtocolMessage> &messages, uint8_t tag) {
	for (const opennova::ProtocolMessage &message : messages) {
		if (message.tag == tag) {
			return true;
		}
	}
	return false;
}

bool check_protocol_classifier_accepts_lobby_and_jointoperations() {
	using opennova::SessionProtocolKind;
	if (!expect(opennova::classify_session_protocol("NOVAWORLDUDP") ==
	                    SessionProtocolKind::Lobby,
	            "NOVAWORLDUDP classifies as lobby")) return false;
	if (!expect(opennova::classify_session_protocol("JointOperations") ==
	                    SessionProtocolKind::JointOperations,
	            "JointOperations classifies as in-match")) return false;
	if (!expect(opennova::classify_session_protocol("JOINTOPERATIONS") ==
	                    SessionProtocolKind::JointOperations,
	            "JOINTOPERATIONS classifies as in-match")) return false;
	if (!expect(opennova::classify_session_protocol("jop:cus2") ==
	                    SessionProtocolKind::Unsupported,
	            "gate tags are not session protocol names")) return false;
	return true;
}

bool check_jointoperations_dispatch_routes_to_game_runtime() {
	opennova::GameServerRuntime runtime;
	runtime.start();

	const auto result = opennova::dispatch_in_match_session_messages(
			opennova::SessionProtocolKind::JointOperations,
			runtime,
			"127.0.0.1:32768",
			{opennova::make_protocol_message(0x37, {})},
			100);

	if (!expect(result.handled, "JointOperations messages are handled")) return false;
	if (!expect(has_tag(result.replies, 0x75), "mission request emits 0x75 ack")) return false;
	if (!expect(has_tag(result.replies, 0x64), "mission request emits 0x64 mission chunk")) return false;
	const auto snapshot = runtime.snapshot("127.0.0.1:32768");
	if (!expect(snapshot.session_count == 1, "runtime owns the peer session")) return false;
	if (!expect(snapshot.primary_session.phase == "mission_ready",
	            "runtime session enters mission_ready")) return false;
	return true;
}

bool check_non_game_dispatch_is_not_handled() {
	opennova::GameServerRuntime runtime;
	runtime.start();
	const auto result = opennova::dispatch_in_match_session_messages(
			opennova::SessionProtocolKind::Lobby,
			runtime,
			"127.0.0.1:32768",
			{opennova::make_protocol_message(0x37, {})},
			100);
	if (!expect(!result.handled, "lobby protocol is not handled by game dispatcher")) return false;
	if (!expect(result.replies.empty(), "non-game dispatch produces no replies")) return false;
	if (!expect(runtime.snapshot("127.0.0.1:32768").session_count == 0,
	            "non-game dispatch does not open runtime sessions")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_protocol_classifier_accepts_lobby_and_jointoperations() && ok;
	ok = check_jointoperations_dispatch_routes_to_game_runtime() && ok;
	ok = check_non_game_dispatch_is_not_handled() && ok;
	return ok ? 0 : 1;
}
