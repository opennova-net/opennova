#include <net/novaworld/session_protocol.h>

#include <net/npwire/session_hello.h>

namespace opennova {

// The PN field of a CLIENT_HELLO selects the message set (docs/net/novaworld-net-re.md
// §3): NOVAWORLDUDP is the lobby/browser service's own name (witnessed in the
// retail service captures, no Jointops.exe counterpart), and the game host's
// case-insensitive PN gate is [orig: NapiNPProtocol_HandleClientJoin @0x62B750 via
// Napi_StrCaseEqual @0x616e70] (session_hello.cpp's is_jointoperations_protocol_name).
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
