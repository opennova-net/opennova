#include <net/novaworld/session_protocol.h>

#include <net/protocol/lobby_protocol.h>
#include <net/protocol/protocol.h>

namespace opennova {

// The PN field of a CLIENT_HELLO selects the message set (docs/net/novaworld-net-re.md
// §3): NOVAWORLDUDP is the lobby/browser service's own name (witnessed in the
// retail service captures, no Jointops.exe counterpart), and a game host's PN is its
// protocol's, compared case-insensitively [orig: NapiNPProtocol_HandleClientJoin @0x62B750 via
// Napi_StrCaseEqual @0x616e70] (protocol_by_pn).
SessionProtocol classify_session_protocol(std::string_view pn) {
	SessionProtocol out;
	if (pn == kLobbyProtocolName) {
		out.kind = SessionProtocolKind::Lobby;
	} else if (const Protocol *protocol = protocol_by_pn(pn)) {
		out.kind = SessionProtocolKind::Game;
		out.protocol = protocol;
	}
	return out;
}

} // namespace opennova
