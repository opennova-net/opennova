#include <novaworld/session_protocol.h>

#include <npwire/session_hello.h>

namespace opennova {

SessionProtocolKind classify_session_protocol(std::string_view pn) {
	if (pn == "NOVAWORLDUDP") {
		return SessionProtocolKind::Lobby;
	}
	if (is_jointoperations_protocol_name(pn)) {
		return SessionProtocolKind::JointOperations;
	}
	return SessionProtocolKind::Unsupported;
}

} // namespace opennova
