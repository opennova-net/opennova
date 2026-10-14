#include <net/novaworld/session_protocol.h>

#include <net/title/title_identity.h>

namespace opennova {

// The PN field of a CLIENT_HELLO selects the message set (docs/net/novaworld-net-re.md
// §3): NOVAWORLDUDP is the lobby/browser service's own name (witnessed in the
// retail service captures, no Jointops.exe counterpart), and a game host's PN is its
// title's, compared case-insensitively [orig: NapiNPProtocol_HandleClientJoin @0x62B750 via
// Napi_StrCaseEqual @0x616e70] (title_identity_by_protocol_name).
SessionProtocol classify_session_protocol(std::string_view pn) {
	SessionProtocol out;
	if (pn == "NOVAWORLDUDP") {
		out.kind = SessionProtocolKind::Lobby;
	} else if (const TitleIdentity *title = title_identity_by_protocol_name(pn)) {
		out.kind = SessionProtocolKind::Game;
		out.title = title;
	}
	return out;
}

} // namespace opennova
