#pragma once

#include <string_view>

namespace opennova {

struct TitleIdentity; // net/title/title_identity.h

// Classify a ClientHello.pn into the session protocol family the host routes it as. "NOVAWORLDUDP" =
// the matchmaking lobby container; a title's in-match protocol name ("JOINTOPERATIONS",
// "DELTAFORCEXTREME2", any case) = the in-match game protocol of that title. The in-match reply
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
	// The title whose in-match protocol the PN names (Game only); its identity may still be
	// incomplete (title_identity_complete), which no consumer speaks.
	const TitleIdentity *title = nullptr;
};

SessionProtocol classify_session_protocol(std::string_view pn);

} // namespace opennova
