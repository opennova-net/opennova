#include <net/novaworld/session_protocol.h>

#include <base/gameprofile/gameprofile.h>
#include <net/protocol/protocol.h>

#include <cstdio>
#include <string_view>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool is_game_of(std::string_view pn, const char *title_code) {
	const opennova::SessionProtocol protocol = opennova::classify_session_protocol(pn);
	return protocol.kind == opennova::SessionProtocolKind::Game && protocol.protocol != nullptr &&
			protocol.protocol == opennova::title_protocol(title_code);
}

// classify_session_protocol is the shared PN router (the in-match reply dispatch moved to engine/runtime/inmatch
// with the P8 retirement of GameServerRuntime; the npruntime handshake test exercises that path). A title's
// in-match PN routes as that title's game protocol, in any case; DFX2's routes although its identity is not yet
// witnessed in full (its consumers refuse it); an unknown PN does not route.
bool check_protocol_classifier_routes_each_title() {
	using opennova::SessionProtocolKind;
	if (!expect(opennova::classify_session_protocol("NOVAWORLDUDP").kind ==
	                    SessionProtocolKind::Lobby,
	            "NOVAWORLDUDP classifies as lobby")) return false;
	if (!expect(opennova::classify_session_protocol("NOVAWORLDUDP").protocol == nullptr,
	            "the lobby names no title")) return false;
	if (!expect(is_game_of("JointOperations", "jo"),
	            "JointOperations classifies as JO's in-match protocol")) return false;
	if (!expect(is_game_of("JOINTOPERATIONS", "jo"),
	            "JOINTOPERATIONS classifies as JO's in-match protocol")) return false;
	if (!expect(is_game_of("DELTAFORCEXTREME2", "dfx2"),
	            "DELTAFORCEXTREME2 classifies as DFX2's in-match protocol")) return false;
	if (!expect(is_game_of("deltaforcextreme2", "dfx2"),
	            "the PN compares without case")) return false;
	if (!expect(opennova::classify_session_protocol("jop:cus2").kind ==
	                    SessionProtocolKind::Unsupported,
	            "gate tags are not session protocol names")) return false;
	if (!expect(opennova::classify_session_protocol("BLACKHAWKDOWN").kind ==
	                    SessionProtocolKind::Unsupported,
	            "a PN no title witnesses does not route")) return false;
	if (!expect(opennova::classify_session_protocol("").kind == SessionProtocolKind::Unsupported,
	            "an empty PN does not route")) return false;
	return true;
}

} // namespace

int main() {
	return check_protocol_classifier_routes_each_title() ? 0 : 1;
}
