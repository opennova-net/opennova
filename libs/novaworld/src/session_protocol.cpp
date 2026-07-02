#include <novaworld/session_protocol.h>

namespace opennova {

SessionProtocolKind classify_session_protocol(std::string_view pn) {
	if (pn == "NOVAWORLDUDP") {
		return SessionProtocolKind::Lobby;
	}
	if (pn == "JointOperations" || pn == "JOINTOPERATIONS") {
		return SessionProtocolKind::JointOperations;
	}
	return SessionProtocolKind::Unsupported;
}

} // namespace opennova
