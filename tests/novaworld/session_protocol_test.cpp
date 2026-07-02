#include <novaworld/session_protocol.h>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// classify_session_protocol is the shared PN router (the in-match reply dispatch moved to libs/npruntime
// with the P8 retirement of GameServerRuntime; the npruntime handshake test exercises that path).
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

} // namespace

int main() {
	return check_protocol_classifier_accepts_lobby_and_jointoperations() ? 0 : 1;
}
