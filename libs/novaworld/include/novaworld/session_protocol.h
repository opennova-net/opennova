#pragma once

#include <string_view>

namespace opennova {

// Classify a ClientHello.pn into the session protocol family the host routes it as. "NOVAWORLDUDP" =
// the matchmaking lobby container; "JointOperations"/"JOINTOPERATIONS" = the in-match game protocol.
// The in-match reply dispatch itself lives in libs/npruntime (dispatch_session_replies over a
// NapiNPConnection) since P8 retired GameServerRuntime; this stays here as the shared PN classifier
// used by both the npruntime legs and the standalone server's lobby router.
enum class SessionProtocolKind {
	Unsupported,
	Lobby,
	JointOperations,
};

SessionProtocolKind classify_session_protocol(std::string_view pn);

} // namespace opennova
