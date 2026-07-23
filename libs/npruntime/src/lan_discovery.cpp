#include "npruntime/lan_discovery.h"

#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <utility>

namespace opennova::np {

std::vector<uint8_t> build_lan_discovery_probe(uint32_t client_index) {
	// Retail LAN enumeration uses the ordinary JO game-session identity built
	// by CNapiNetwork_Init @ 0x4ca4a0. This helper lives in the neutral wire
	// layer; no matchmaking/service configuration participates in discovery.
	const ClientHello hello = make_jointoperations_client_hello(client_index);
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
}

bool parse_lan_discovery_reply(const uint8_t *data, size_t size, LanDiscoveryServer &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!data || !nw_decode_inbound(data, size, opcode, body) ||
	    opcode != SESSION_OPCODE_SERVER_HELLO) {
		return false;
	}

	ServerHello hello;
	if (!parse_server_hello(body.data(), body.size(), hello)) return false;

	// A browse socket can receive unrelated UDP while its 30-second window is
	// open. Accept only a game-server 0x81 carrying the retail JO protocol
	// identity. Do not correlate CI: retail's ServerHello.CI is the host node
	// index (not an echo of the enumerator's random ClientHello.CI).
	const ClientHello retail = make_jointoperations_client_hello(0);
	if (!hello.is_game_server || hello.pn != retail.pn || hello.pg != retail.pg ||
	    hello.pv1 != retail.pv1 || hello.pv2 != retail.pv2) {
		return false;
	}

	LanDiscoveryServer parsed;
	parsed.server_name = std::move(hello.sn);
	parsed.session_id = std::move(hello.sus1);
	parsed.expansion = std::move(hello.sus2);
	parsed.gametype = hello.p1;
	parsed.current_players = hello.np;
	parsed.max_players = hello.mp;
	out = std::move(parsed);
	return true;
}

} // namespace opennova::np
