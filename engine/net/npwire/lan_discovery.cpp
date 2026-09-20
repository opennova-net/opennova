#include <net/npwire/lan_discovery.h>

#include <base/io/strutil.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <utility>

namespace opennova {

std::vector<uint8_t> build_lan_discovery_probe(uint32_t client_index) {
	// Retail LAN enumeration uses the ordinary JO game-session identity built
	// by CNapiNetwork_Init [orig: NapiNPSession_SendAnnouncePacket @0x61fa00;
	// CNapiNetwork_Init @0x4ca4a0]. This helper lives in the neutral wire layer;
	// no matchmaking/service configuration participates in discovery.
	const ClientHello hello = make_jointoperations_client_hello(client_index);
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
}

bool parse_lan_discovery_reply(const uint8_t *data, size_t size, uint32_t client_index,
                               LanDiscoveryServer &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!data || !nw_decode_inbound(data, size, opcode, body) ||
	    opcode != SESSION_OPCODE_SERVER_HELLO) {
		return false;
	}

	ServerHello hello;
	if (!parse_server_hello(body.data(), body.size(), hello)) return false;

	// A browse socket can receive unrelated UDP while its 30-second window is
	// open. The solicited 0x81 echoes the enumerator's CI (the host reads the
	// inbound CI TLV and writes it back), and retail resolves that CI against
	// the enumerator that issued it, returning on a miss; an absent CI TLV
	// resolves as id 0, which no live enumerator carries. Only 0x81 replies
	// reach this parser — the unsolicited 0x41 announces a retail host
	// broadcasts never do (the announce builder writes packet_header_byte =
	// 0x41), so the CI filter drops nothing a retail browser would list.
	// [orig: NapiNPProtocol_HandleClientHello @0x6213b0 ->
	//  NapiNPProtocol_SendServerInfoPacket @0x6204b0 (the CI echo @0x620563);
	//  Nwu_HandleServerHello @0x626d20 — sub_6227F0(connection, server_id)
	//  @0x627533, `if (!entry) return` @0x627541;
	//  NapiNPSession_SendAnnouncePacket @0x61fa00 @0x61fac4 (0x41)]
	if (client_index == 0 || hello.ci != client_index) return false;

	// The admission identity: PN and PV1 compared case-insensitively, PG
	// bytewise; PV2 is parsed and stored but is not a discovery gate (it is the
	// host's 0x42-time check, not the browser's). [orig: Nwu_HandleServerHello
	//  @0x626d20 — PN @0x62758b, PG @0x627591..0x627620, PV1 @0x627639]
	const ClientHello retail = make_jointoperations_client_hello(0);
	if (!hello.is_game_server || !strutil::iequals(hello.pn, retail.pn) ||
	    hello.pg != retail.pg || !strutil::iequals(hello.pv1, retail.pv1)) {
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
	if (client_index == 0 || port_min < 1 || port_max > 65535 || port_min > port_max) return false;
	stop();
	servers_.clear();
	index_by_endpoint_.clear();
	// One identity per browse window: retail keeps its connection identity
	// across the enumerator's re-announce pumps, so every burst repeats the
	// same probe bytes.
	client_index_ = client_index;
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
	// Strictly greater: `GetTickCount() - search_start > 0x7530` @0x55933a.
	if (browse_elapsed_s_ > kLanBrowseWindowSeconds) {
		stop();
		return false;
	}
	// A cold host that binds its port mid-window is only discoverable because
	// the enumerator keeps announcing. The re-announce is due strictly after
	// the interval [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 —
	// `tick_count - last_send_tick > interval`].
	if (announce_elapsed_s_ > kLanAnnounceIntervalSeconds) {
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
	if (!parse_lan_discovery_reply(data, size, client_index_, server)) return LanRowChange::kNone;
	const std::string key = endpoint_key(source_ip, source_port);
	// A listed host re-announcing: the row table hit adds and updates nothing
	// (the row text was built at first sighting) @0x5593c4..0x5593e3.
	if (index_by_endpoint_.count(key) != 0) return LanRowChange::kNone;
	// The per-pump walk stops at 32 examined sessions @0x5593b5.
	if (servers_.size() >= kLanBrowseMaxSessions) return LanRowChange::kNone;
	index_by_endpoint_.emplace(key, servers_.size());
	LanDiscoveryRow row;
	row.host_ip = source_ip;
	row.port = source_port;
	row.server = std::move(server);
	servers_.push_back(std::move(row));
	return LanRowChange::kAdded;
}

} // namespace opennova
