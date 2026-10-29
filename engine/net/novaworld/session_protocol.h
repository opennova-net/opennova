#pragma once

#include <string_view>

namespace opennova {

struct Protocol; // net/protocol/protocol.h

// Classify a ClientHello.pn into the session protocol family the host routes it as. "NOVAWORLDUDP" =
// the matchmaking lobby container; a protocol's in-match name ("JOINTOPERATIONS",
// "DELTAFORCEXTREME2", any case) = that in-match game protocol. The in-match reply
// dispatch itself lives in engine/runtime/inmatch (dispatch_session_replies over a
// NapiNPConnection) since P8 retired GameServerRuntime; this stays here as the shared PN classifier
// used by both the inmatch legs and the standalone server's lobby router.
enum class SessionProtocolKind {
	Unsupported,
	Lobby,
	Game,
};

struct SessionProtocol {
	SessionProtocolKind kind = SessionProtocolKind::Unsupported;
	// The in-match protocol the PN names (Game only); it may still be incomplete
	// (protocol_complete), which no consumer speaks.
	const Protocol *protocol = nullptr;
};

SessionProtocol classify_session_protocol(std::string_view pn);

} // namespace opennova
