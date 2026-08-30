#include <net/npruntime/lan_discovery.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <utility>

namespace opennova::np {

std::vector<uint8_t> build_lan_discovery_probe(uint32_t client_index) {
	// Retail LAN enumeration uses the ordinary JO game-session identity built
	// by CNapiNetwork_Init [orig: NapiNPSession_SendAnnouncePacket @0x61fa00;
	// CNapiNetwork_Init @0x4ca4a0]. This helper lives in the neutral wire layer;
	// no matchmaking/service configuration participates in discovery.
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
	// identity. CI is deliberately NOT used to filter here: the solicited 0x81 does
	// echo the enumerator's CI (`NapiNPProtocol_HandleClientHello @0x6213b0` reads
	// the inbound CI TLV and hands it to `NapiNPProtocol_SendServerInfoPacket
	// @0x6204b0`, which writes it back at @0x620563), but a browse window also
	// collects UNSOLICITED announces from `NapiNPSession_SendAnnouncePacket
	// @0x61fa00`, whose CI is the announcing session's own field and is omitted
	// entirely when zero. Correlating would drop those rows.
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
	parsed.server_flags = hello.p2;
	parsed.current_players = hello.np;
	parsed.max_players = hello.mp;
	out = std::move(parsed);
	return true;
}

namespace {

// A reply's source must be routable back: an empty or unspecified address
// cannot be joined.
bool usable_address(const std::string &address) {
	return !address.empty() && address != "0.0.0.0" && address != "::";
}

std::string endpoint_key(const std::string &address, int port) {
	return address + ":" + std::to_string(port);
}

} // namespace

bool LanDiscoveryBrowser::begin(uint32_t client_index, int port_min, int port_max) {
	if (port_min < 1 || port_max > 65535 || port_min > port_max) return false;
	stop();
	servers_.clear();
	index_by_endpoint_.clear();
	// One identity per browse window: retail keeps its connection identity
	// across the enumerator's re-announce pumps, so every burst repeats the
	// same probe bytes.
	probe_ = build_lan_discovery_probe(client_index);
	port_min_ = port_min;
	port_max_ = port_max;
	browse_elapsed_s_ = 0.0;
	announce_elapsed_s_ = 0.0;
	browsing_ = true;
	return true;
}

void LanDiscoveryBrowser::stop() {
	browsing_ = false;
	browse_elapsed_s_ = 0.0;
	announce_elapsed_s_ = 0.0;
}

bool LanDiscoveryBrowser::advance(double delta_seconds, bool &announce_due) {
	announce_due = false;
	if (!browsing_) return false;
	browse_elapsed_s_ += delta_seconds;
	announce_elapsed_s_ += delta_seconds;
	if (browse_elapsed_s_ >= kLanBrowseWindowSeconds) {
		stop();
		return false;
	}
	// A cold host that binds its port mid-window is only discoverable because
	// the enumerator keeps announcing.
	if (announce_elapsed_s_ >= kLanAnnounceIntervalSeconds) {
		announce_elapsed_s_ = 0.0;
		announce_due = true;
	}
	return true;
}

LanRowChange LanDiscoveryBrowser::accept_reply(const uint8_t *data, size_t size,
                                               const std::string &source_ip,
                                               int source_port) {
	if (!browsing_ || data == nullptr || size == 0 || source_port < port_min_ ||
	    source_port > port_max_ || !usable_address(source_ip))
		return LanRowChange::kNone;
	LanDiscoveryServer server;
	if (!parse_lan_discovery_reply(data, size, server)) return LanRowChange::kNone;
	const std::string key = endpoint_key(source_ip, source_port);
	const auto it = index_by_endpoint_.find(key);
	if (it != index_by_endpoint_.end()) {
		// The host re-announces every browse interval and its row data is live
		// state (player count, mission rotation) — refresh the stored row in
		// place and report a change only when something actually changed, so a
		// stale first-seen row does not survive the whole browse window.
		LanDiscoveryRow &row = servers_[it->second];
		if (row.server == server) return LanRowChange::kNone;
		row.server = std::move(server);
		return LanRowChange::kUpdated;
	}
	index_by_endpoint_.emplace(key, servers_.size());
	LanDiscoveryRow row;
	row.host_ip = source_ip;
	row.port = source_port;
	row.server = std::move(server);
	servers_.push_back(std::move(row));
	return LanRowChange::kAdded;
}

} // namespace opennova::np
