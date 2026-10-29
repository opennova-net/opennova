#include <net/npwire/lan_discovery.h>

#include <net/napi/envelope.h> // the manager send's envelope over the rendezvous

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace opennova {

std::string lan_discovery_row_label(const std::string &server_name, const std::string &expansion,
		int32_t players, int32_t max_players) {
	// A %s stops at its first NUL; the row's copy of the expansion is 32 bytes.
	const std::string name = server_name.substr(0, server_name.find('\0'));
	const std::string variant =
			expansion.substr(0, std::min<size_t>(expansion.find('\0'), 31));
	const std::string counts =
			" (" + std::to_string(players) + "/" + std::to_string(max_players) + ")";
	std::string label = variant.empty() ? name + counts : name + " - " + variant + counts;
	if (label.size() > 255) {
		label.resize(255);
	}
	return label;
}

std::vector<uint8_t> build_lan_discovery_probe(const Protocol &title, uint32_t client_index,
                                               uint32_t target_ip, uint32_t target_port) {
	// Retail LAN enumeration uses the title's ordinary game-session identity built
	// by CNapiNetwork_Init [orig: NapiNPSession_SendAnnouncePacket @0x61fa00;
	// CNapiNetwork_Init @0x4ca4a0]. This helper lives in the neutral wire layer;
	// no matchmaking/service configuration participates in discovery. The
	// announce names its own destination [orig: @0x61fcf5..0x61fd47].
	ClientHello hello = make_title_client_hello(title, client_index);
	hello.eip = target_ip;
	hello.epn = target_port;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
}

bool parse_lan_discovery_reply(const Protocol &title, const uint8_t *data, size_t size,
                               uint32_t client_index, LanDiscoveryServer &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!data || !nw_decode_inbound(data, size, opcode, body) ||
	    opcode != SESSION_OPCODE_SERVER_HELLO) {
		return false;
	}

	// No field is required: an HK-less reply lists with host key 0, which a join
	// from the row carries into a 0x42 that omits it (parse_server_hello).
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
	//  Nwu_HandleServerHello @0x626d20 - NapiNPProtocol_FindConnectionByIndex(protocol,
	//  server_id) @0x627533, `if (!entry) return` @0x627541;
	//  NapiNPSession_SendAnnouncePacket @0x61fa00 @0x61fac4 (0x41)]
	if (client_index == 0 || hello.ci != client_index) return false;

	// The admission identity: PN and PV1 compared case-insensitively, PG
	// bytewise; PV2 is parsed and stored but is not a discovery gate (it is the
	// host's 0x42-time check, not the browser's). [orig: Nwu_HandleServerHello
	//  @0x626d20 — PN @0x62758b, PG @0x627591..0x627620, PV1 @0x627639]
	// No game-server field is required: an SF-less reply reads SF 0 and lists,
	// the discovery callback gating the session on P2's channel type alone
	// (LanDiscoveryBrowser::create_record). [orig: Nwu_HandleServerHello @0x626d20:
	//  SF zeroed @0x626eb7, loaded @0x62721a; CNapiNetwork_OnSessionDiscovered
	//  @0x4c8470: the P2 gate @0x4c84a3..0x4c84ba, SF read @0x4c84f1]
	if (!server_hello_matches_identity(hello, make_title_client_hello(title, 0))) return false;

	LanDiscoveryServer parsed;
	parsed.server_name = std::move(hello.sn);
	parsed.session_id = std::move(hello.sus1);
	parsed.expansion = std::move(hello.sus2);
	parsed.gametype = hello.p1;
	parsed.server_flags = hello.p2;
	parsed.current_players = hello.np;
	parsed.max_players = hello.mp;
	// The session record keeps the HK and SF a join from this row connects with
	// [orig: Nwu_HandleServerHello @0x626d20 — HK into session+0x1F8 and SF into
	//  session+0x2C0 (e.g. @0x62776f / @0x6277c2), the fields InitFromSession @0x62639c reads]
	parsed.host_key = hello.hk;
	parsed.password_required = (hello.sf & 1u) != 0;
	out = std::move(parsed);
	return true;
}

namespace {

// A %s stops at the string's first NUL.
std::string c_text(const std::string &text) {
	return text.substr(0, text.find('\0'));
}

} // namespace

std::string net_session_removed_text(const PeerAddr &at, const std::string &session_name) {
	return "NET SESSION AT " + peer_addr_to_string(at) + " [" + c_text(session_name) +
	       "] HAS BEEN REMOVED.";
}

std::string net_session_discovered_text(const PeerAddr &at, const std::string &session_name) {
	return "NET SESSION DISCOVERED AT " + peer_addr_to_string(at) + " [" + c_text(session_name) +
	       "]";
}

uint32_t next_browse_connection_index() {
	(void)next_connection_index(); // the screen's create, destroyed unsent
	return next_connection_index(); // InitTransportConnection's, the enumerator's
}

// [orig: NetPacket_ValidateChannelType @0x4c49b0 — the flags arrive as int16]
bool lan_session_admits_connect_type(int connect_type, uint32_t server_flags) {
	const uint16_t flags = static_cast<uint16_t>(server_flags);
	if ((flags & 0x400u) != 0) return connect_type == kLanConnectTypeNovaWorld;
	if ((flags & 0x100u) != 0) return connect_type == kLanConnectTypeLan;
	if ((flags & 0x200u) != 0) return connect_type == kLanConnectTypeInternet;
	return true;
}

bool LanDiscoveryBrowser::begin(const Protocol &title, uint32_t client_index,
                                uint32_t enumerate_ip, int port_min, int port_max,
                                int connect_type) {
	if (client_index == 0 || port_min < 1 || port_max > 65535 || port_min > port_max) return false;
	stop();
	// The search drains the enumerator's records, then empties its row table.
	drain_records();
	servers_.clear();
	// One identity per browse window: retail keeps its connection identity
	// across the enumerator's re-announce pumps, so every burst repeats the
	// same probe bytes.
	title_ = &title;
	client_index_ = client_index;
	enumerate_ip_ = enumerate_ip;
	connect_type_ = connect_type;
	port_min_ = port_min;
	port_max_ = port_max;
	browse_elapsed_s_ = 0.0;
	announce_elapsed_s_ = 0.0;
	browsing_ = true;
	return true;
}

std::vector<uint8_t> LanDiscoveryBrowser::probe(int port) const {
	if (title_ == nullptr) return {};
	return build_lan_discovery_probe(*title_, client_index_, enumerate_ip_,
	                                 static_cast<uint32_t>(port < 0 ? 0 : port));
}

std::vector<LanBurstDatagram> LanDiscoveryBrowser::burst() const {
	std::vector<LanBurstDatagram> out;
	if (title_ == nullptr) return out;
	if (proxy_.enabled()) {
		LanBurstDatagram rendezvous;
		rendezvous.rendezvous = true;
		// PeerAddr packs octets as the inet_addr dword does (first octet low).
		rendezvous.node.ip = proxy_.node_addr;
		rendezvous.node.port = static_cast<uint16_t>(proxy_.node_port & 0xFFFFu);
		const std::array<uint8_t, kProxyRendezvousDatagramSize> bytes =
				build_proxy_rendezvous_datagram(proxy_);
		// The manager's send with its encode flag set: the 48 bytes go out in the NAPI envelope,
		// 52 on the wire, as every NWU datagram does (D-NET-1249).
		// [orig: CNapiNPConnection_SendPingPacket @0x61F8C0 - flags 1 pushed @0x61F9B1, the length
		//  0x30 @0x61F9B3, CNapiNPManager_SendTo called @0x61F9DA; CNapiNPManager_SendTo @0x61EC20 -
		//  NapiSocket_SendTo called @0x61EC7A; NapiSocket_SendTo @0x62CFA0 - the encode test
		//  @0x62CFEA, NapiSocket_EncodePacket called @0x62CFFF]
		rendezvous.bytes.resize(bytes.size() + 4);
		size_t encoded = 0;
		if (napi_envelope_encode(bytes.data(), bytes.size(), rendezvous.bytes.data(),
					rendezvous.bytes.size(), &encoded) == 0) {
			rendezvous.bytes.resize(encoded);
			out.push_back(std::move(rendezvous));
		}
	}
	for (int port = port_min_; port <= port_max_; ++port) {
		LanBurstDatagram announce;
		announce.port = port;
		announce.bytes = probe(port);
		out.push_back(std::move(announce));
	}
	return out;
}

void LanDiscoveryBrowser::stop() {
	browsing_ = false;
	browse_elapsed_s_ = 0.0;
	announce_elapsed_s_ = 0.0;
}

void LanDiscoveryBrowser::connect_log(const std::string &text) const {
	if (connect_log_) connect_log_(text);
}

void LanDiscoveryBrowser::drain_records() {
	// [orig: NapiNPEnumerator_DrainTimerList @0x622140 - the head destroyed until none
	//  is left, NapiNPTimer_Destroy (the call @0x62215E)]
	while (!records_.empty()) destroy_record(0);
}

void LanDiscoveryBrowser::adopt_records(LanDiscoveryBrowser &from) {
	if (&from == this) return;
	for (Record &record : from.records_) records_.push_back(std::move(record));
	from.records_.clear();
}

void LanDiscoveryBrowser::create_record(const PeerAddr &from, LanDiscoveryServer server) {
	// [orig: NapiNPSession_Create @0x624540 - the stamp GetTickCount into +0xB0C
	//  @0x6245AE, appended at the enumerator list's tail @0x6246DA..0x624701, the id
	//  `if (counter++ == -1) counter = 1` @0x624704..0x62470A, stored @0x624710;
	//  Nwu_HandleServerHello @0x626D20 - a sender's first record: the Create call
	//  @0x62767E, the discovered callback while +0x25 is clear @0x6278D7..0x627903; a
	//  replacement: the Create call @0x627947, stamped again @0x627BA6, the callback
	//  @0x627BAC..0x627BD4]
	Record record;
	if (++record_ids_ == 0) record_ids_ = 1;
	record.id = record_ids_;
	record.sender = from;
	record.host_key = server.host_key;
	// The record's SN is the reply's 64-byte copy (parse_server_hello's).
	// [orig: Nwu_HandleServerHello @0x626D20 - Napi_CopyString(.., 0x40) @0x627172, into
	//  the record's +0x1FC @0x627775]
	record.name = server.server_name;
	record.password_required = server.password_required;
	record.stamp_s = browse_elapsed_s_;
	// The discovered callback: a session whose P2 does not admit the browse's
	// connect type takes no node (no row, no slot in the walk) and no line.
	// [orig: CNapiNetwork_OnSessionDiscovered @0x4C8470 - NetPacket_ValidateChannelType
	//  @0x4C84A3..0x4C84BA, the node found by the record's id or allocated
	//  @0x4C84CB..0x4C84EA, its copy of the record @0x4C84F1..0x4C8601 (the name from
	//  the record's SN +508 @0x4C8511), the line @0x4C8617..0x4C8663]
	record.listed = lan_session_admits_connect_type(connect_type_, server.server_flags);
	if (record.listed) record.node = std::move(server);
	records_.push_back(std::move(record));
	if (records_.back().listed)
		connect_log(net_session_discovered_text(records_.back().sender, records_.back().name));
}

void LanDiscoveryBrowser::destroy_record(size_t index) {
	// The removed callback frees the record's node (its row stays: walk_rows), then
	// writes its line whatever the session's channel.
	// [orig: NapiNPTimer_Destroy @0x621E50 - the callback @0x621E5D..0x621E6F, the
	//  unlink @0x621E76..0x621EBD; CNapiNetwork_OnSessionRemoved @0x4C68C0 - the node
	//  @0x4C68F8..0x4C690D, the line @0x4C6926..0x4C6975]
	const Record record = std::move(records_[index]);
	records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(index));
	connect_log(net_session_removed_text(record.sender, record.name));
}

void LanDiscoveryBrowser::expire_records() {
	// The connection's state-2 pump while the enumerator runs: from the list's head,
	// every record whose last stamp is older than the lifetime goes.
	// [orig: CNapiNPConnection_PumpStateMachine @0x6292E0 - state 2 @0x629414, the
	//  enumerator's +0x2C @0x62941E, the lifetime +0x18 @0x629435 (+0x25 clear),
	//  `now - stamp(+0xB0C) > lifetime` @0x62945E..0x629468, NapiNPTimer_Destroy
	//  (the call @0x62946F)]
	for (size_t i = 0; i < records_.size();) {
		if (browse_elapsed_s_ - records_[i].stamp_s > kLanSessionRecordLifetimeSeconds)
			destroy_record(i);
		else
			++i;
	}
}

void LanDiscoveryBrowser::walk_rows() {
	// The LAN list's walk over the listed sessions in their discovery order: at most 32
	// examined; a session whose record id the row table holds is skipped, any other's
	// row is appended from its node. A row is never removed in the window, so a
	// replaced session lists again beside its old row, past the 32-row table too: nothing
	// bounds the append. (Retail's 33rd row writes past its table, over the options
	// screens' scratch binding table, which is rebuilt before any reader: no face; the
	// row itself is a list row a join selects by index.)
	// [orig: UI_ProcessLANSessionStateMachine @0x558de0 - the node walk
	//  @0x559384..0x559546 (the 32 break @0x5593B5, the step @0x559532..0x55953F), the key
	//  compare @0x5593C1..0x5593E3 against the node's record id +0x14, the append
	//  @0x5593FA..0x559524 (the key stored @0x559412, the count stepped @0x559521)]
	size_t examined = 0;
	for (const Record &record : records_) {
		if (!record.listed) continue;
		if (examined >= kLanBrowseMaxSessions) break;
		++examined;
		const bool keyed = std::any_of(servers_.begin(), servers_.end(),
				[&record](const LanDiscoveryRow &row) { return row.record_id == record.id; });
		if (keyed) continue;
		LanDiscoveryRow row;
		row.host_ip = peer_addr_ip_to_string(record.sender);
		row.port = record.sender.port;
		row.server = record.node;
		row.record_id = record.id;
		servers_.push_back(std::move(row));
	}
}

bool LanDiscoveryBrowser::join_session(size_t row, LanDiscoveryRow &out) const {
	// The join walks the listed sessions to the row's index.
	// [orig: UI_EnumerateAndJoinSession @0x569F50 - the walk @0x56A1AE..0x56A1D6;
	//  UI_JoinSelectedSession @0x5699D0 - @0x569A4B..0x569A76]
	size_t index = 0;
	for (const Record &record : records_) {
		if (!record.listed) continue;
		if (index++ != row) continue;
		// The node's copy as discovered; the dial's address, HK and SF are the record's.
		// [orig: CNapiNPConnection_InitFromSession @0x626320 - @0x62639C / @0x6263A8 /
		//  @0x6263AE..0x6263B7]
		out.host_ip = peer_addr_ip_to_string(record.sender);
		out.port = record.sender.port;
		out.server = record.node;
		out.server.host_key = record.host_key;
		out.server.password_required = record.password_required;
		out.record_id = record.id;
		return true;
	}
	return false;
}

bool LanDiscoveryBrowser::frame(double delta_seconds, const FrameLegs &legs) {
	if (!browsing_) return false;
	browse_elapsed_s_ += delta_seconds;
	announce_elapsed_s_ += delta_seconds;
	const auto enumerator = [&] {
		if (pump_enumerator() && legs.send_burst) legs.send_burst();
	};
	// The LAN screen's client send pump leads its menu frame [orig:
	//  UI_ProcessLANSessionStateMachine @0x558DE0 - @0x558E0E ahead of @0x559324].
	if (windowed_) enumerator();
	// The receive, then the Pump(-1)'s queue pass and walk [orig:
	//  CNapiNetwork_PumpTransportAndProtocol @0x4C6E00 - @0x4C6E2B, @0x4C6E3D].
	if (legs.receive) legs.receive();
	pump_queue();
	if (legs.held_walk) legs.held_walk();
	// The enumerate-and-join's Pump(-1) reaches the browse connection after the held nodes.
	if (!windowed_) enumerator();
	return end_frame();
}

bool LanDiscoveryBrowser::pump_enumerator() {
	if (!browsing_) return false;
	// A cold host that binds its port mid-window is only discoverable because
	// the enumerator keeps announcing. The re-announce is due strictly after
	// the interval [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 —
	// the interval select @0x6290ed (the session's +0x14 interval, which the client
	// browse sets to 3000: CNapiGameSession_InitTransportConnection @0x4c9f9d), the last
	// send read @0x629108, `!last_send || tick_count - last_send_tick > interval`
	// @0x629119, the restamp @0x62911f].
	bool announce_due = false;
	if (announce_elapsed_s_ > kLanAnnounceIntervalSeconds) {
		announce_elapsed_s_ = 0.0;
		announce_due = true;
	}
	// The 0x40 leg after the 0x20 leg [orig: CNapiNPConnection_PumpFlags @0x629780 -
	//  @0x6297B7, @0x6297C3].
	expire_records();
	return announce_due;
}

bool LanDiscoveryBrowser::end_frame() {
	if (!browsing_) return false;
	// Strictly greater: `GetTickCount() - search_start > 0x7530` @0x55933a; the
	// enumerate-and-join's loop has no window (set_windowed).
	const bool open = !(windowed_ && browse_elapsed_s_ > kLanBrowseWindowSeconds);
	if (!open) stop();
	walk_rows();
	return open;
}

bool LanDiscoveryBrowser::hello_passes(const uint8_t *data, size_t size, const PeerAddr &from,
		LanDiscoveryServer &out) const {
	// A reply's source must be routable back: an unspecified address cannot be joined.
	if (!browsing_ || title_ == nullptr || data == nullptr || size == 0 ||
	    static_cast<int>(from.port) < port_min_ || static_cast<int>(from.port) > port_max_ ||
	    from.ip == 0)
		return false;
	return parse_lan_discovery_reply(*title_, data, size, client_index_, out);
}

void LanDiscoveryBrowser::accept_reply(const uint8_t *data, size_t size, const PeerAddr &from) {
	if (title_ == nullptr || data == nullptr || size == 0) return;
	connect_log(napi_got_raw_text(static_cast<int32_t>(size), from));
	if (napi_punkbuster_datagram(data, size)) return;
	std::vector<uint8_t> unpacked;
	if (!nw_unpack_inbound(data, size, unpacked)) {
		connect_log(napi_tossed_text(static_cast<int32_t>(size), from, NapiTossReason::Unpack));
		return;
	}
	const int32_t unpacked_len = static_cast<int32_t>(unpacked.size());
	connect_log(napi_got_cooked_text(unpacked_len, from));
	if (napi_baffle_packet(unpacked)) {
		connect_log(napi_got_baffle_text(unpacked_len, from));
		return;
	}
	// The dispatch: the 0x81 handler's receive leg queues a reply that passes its gates.
	LanDiscoveryServer server;
	const bool taken = hello_passes(data, size, from, server);
	if (taken)
		queue_.push_back(QueuedReply{from, std::vector<uint8_t>(data, data + size), unpacked_len});
	connect_log(taken ? napi_got_protocol_text(unpacked_len, from,
	                            title_->pn != nullptr ? title_->pn : "")
	                  : napi_tossed_text(unpacked_len, from, NapiTossReason::NoProtocol));
}

void LanDiscoveryBrowser::pump_queue() {
	std::vector<QueuedReply> queued;
	queued.swap(queue_);
	for (const QueuedReply &reply : queued) {
		connect_log(net_got_text(reply.packet_bytes, reply.from, SESSION_OPCODE_SERVER_HELLO));
		LanDiscoveryServer server;
		if (hello_passes(reply.datagram.data(), reply.datagram.size(), reply.from, server))
			record_reply(reply.from, std::move(server));
	}
}

void LanDiscoveryBrowser::record_reply(const PeerAddr &from, LanDiscoveryServer server) {
	// The sender's record: the same HK and SN refresh it and stamp it again in place with
	// no callback (its node, the row's copy, is not refreshed); anything else replaces it.
	// [orig: Nwu_HandleServerHello @0x626D20 - the in-place copy @0x627BFA..0x627E24 (the
	//  SF into +0x2C0 @0x627D12), the stamp @0x627E34]
	for (size_t i = 0; i < records_.size(); ++i) {
		Record &record = records_[i];
		if (!(record.sender == from)) continue;
		if (record.host_key == server.host_key && record.name == server.server_name) {
			record.password_required = server.password_required;
			record.stamp_s = browse_elapsed_s_;
			return;
		}
		destroy_record(i);
		break;
	}
	create_record(from, std::move(server));
}

} // namespace opennova
