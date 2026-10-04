#include <net/novaworld/client_session.h>

#include <base/io/le.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <net/napi/envelope.h>
#include <net/napi/tlv.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/outgoing_packets.h>
#include <net/npwire/protocol_message.h> // decode_cs_config_update
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace opennova {

namespace {

// The 16-byte NOVAWORLDUDP protocol GUID the real server validates (16 bytes
// at proto+284 in BOTH HandleClientHello @ 0x6213B0 and HandleClientJoin @
// 0x62B750). Built by CNapiGameSession_InitNPConnection @ 0x4d3be0 via
//   sub_62E750(dst, -655487758, 58574, 17549, 144,180,29,66,179,100,171,113)
// which lays out [u32 version LE][u16 port_a LE][u16 port_b LE][8 bytes].
// Computed from those literals so the byte order is exact regardless of host.
std::array<uint8_t, 16> novaworldudp_pg() {
	std::array<uint8_t, 16> pg{};
	const uint32_t version = static_cast<uint32_t>(-655487758);  // 0xD8EE0CF2
	const uint16_t port_a = 58574;  // 0xE4CE
	const uint16_t port_b = 17549;  // 0x448D
	pg[0] = static_cast<uint8_t>(version & 0xFFu);
	pg[1] = static_cast<uint8_t>((version >> 8) & 0xFFu);
	pg[2] = static_cast<uint8_t>((version >> 16) & 0xFFu);
	pg[3] = static_cast<uint8_t>((version >> 24) & 0xFFu);
	pg[4] = static_cast<uint8_t>(port_a & 0xFFu);
	pg[5] = static_cast<uint8_t>((port_a >> 8) & 0xFFu);
	pg[6] = static_cast<uint8_t>(port_b & 0xFFu);
	pg[7] = static_cast<uint8_t>((port_b >> 8) & 0xFFu);
	pg[8] = 144; pg[9] = 180; pg[10] = 29; pg[11] = 66;
	pg[12] = 179; pg[13] = 100; pg[14] = 171; pg[15] = 113;
	return pg;
}

// Pick the PG GUID that matches the protocol name carried in the hello/auth.
std::array<uint8_t, 16> pg_for_pn(const std::string &pn) {
	if (is_jointoperations_protocol_name(pn))
		return jointoperations_protocol_guid();
	return novaworldudp_pg();
}

NapiField str_field(const char *name, const std::string &value) {
	NapiField f;
	f.name = name;
	f.data.assign(value.begin(), value.end());
	return f;
}

// Napi_CopyString(dst, src, N): at most N-1 characters, always NUL-terminated.
std::string copy_capped(const std::string &s, size_t n) {
	return s.size() < n ? s : s.substr(0, n - 1);
}

// The retail atol over a param value: strtol base 10.
int atol_field(const NapiField &f) {
	return static_cast<int>(std::strtol(field_to_string(f).c_str(), nullptr, 10));
}

// CNapiVarList_SetOrCreate keyed on (VarFNum, name): update the match in place, else append.
// [orig: CNapiVarList_SetOrCreate @0x6318c0 -> NapiLinkedList_FindByTypeAndName @0x6304f0]
void set_or_create_var(std::vector<ClientVar> &list, const ClientVar &var) {
	for (ClientVar &v : list) {
		if (v.fnum == var.fnum && strutil::iequals(v.name, var.name)) {
			v.value = var.value;
			return;
		}
	}
	list.push_back(var);
}

// NapiLinkedList_RemoveByUserData: drop every var a slot owns. [orig: @0x630810]
void remove_slot_vars(std::vector<ClientVar> &list, int slot) {
	list.erase(std::remove_if(list.begin(), list.end(),
	                          [slot](const ClientVar &v) { return v.fnum == slot; }),
	           list.end());
}

} // namespace

std::vector<ClientSession::Config::CuVar> make_novaworld_join_cu(const NovaWorldJoinCu &in) {
	// Order and contents mirror CNapiGameSession_ConnectToNovaWorld @ 0x4d4640
	// (session+316 var list) exactly. type=2 on every chunk (retail .204 wire).
	using CuVar = ClientSession::Config::CuVar;
	return {
	    CuVar{"Application", in.application, 2},
	    CuVar{"BuildDateAndTime", "Jul 21 2009 18:54:41", 2},
	    CuVar{"Debug", "0", 2},
	    CuVar{"CountryName", "", 2},   // empty on the join; locale rides the verify Cookie
	    CuVar{"Language", "", 2},
	    CuVar{"TimeZoneBias", "", 2},
	    CuVar{"GateTag", in.gate_tag, 2},
	    CuVar{"MetTag", in.met_tag, 2},
	    CuVar{"UdpCode1", in.udp_code1, 2},
	    CuVar{"UdpCode2", in.udp_code2, 2},
	    CuVar{"MaxPacketSize", std::to_string(in.max_packet_size), 2},
	};
}

ClientSession::Config ClientSession::Config::jointoperations() {
	Config c;
	const ClientHello retail = make_jointoperations_client_hello(c.client_index);
	c.co = retail.co;
	c.ap = retail.ap;
	c.bdat = retail.bdat;
	c.pn = retail.pn;
	c.pv1 = retail.pv1;
	c.pv2 = retail.pv2;
	c.pg = retail.pg;
	c.use_default_pg = false;
	return c;
}

ClientSession::ClientSession() : ClientSession(Config{}) {}

// The per-session SCRK is retail's random 63-draw key (one generator in the tree,
// nw_session_framing.h). [orig: CNapiNPConnection_GenerateTxKey @0x61dfe0]
ClientSession::ClientSession(Config config)
	: cfg_(std::move(config)),
	  client_scrk_(make_dev_scrk()) {}

std::vector<uint8_t> ClientSession::start() {
	state_ = State::Hello;
	host_state_ = HostState::Idle;
	play_state_ = PlayState::Idle;
	// A fresh connect: the state, its flags and the hosting/playing word back to 0, then the
	// connection start's state 1 [orig: CNapiGameSession_InitNPConnection @0x4d40d0..0x4d412c;
	//  CNapiGameSession_InitPlayerConnection @0x4d43e3].
	lobby_state_ = 0;
	session_flags_ = 0;
	session_role_ = kSessionRoleNone;
	set_lobby_state(1);
	server_hk_ = 0;
	server_sk_ = 0;
	server_scrk_.clear();
	server_nwuid_.clear();
	server_web_domain_.clear();
	sess_id_string_.clear();
	last_error_.clear();
	cs_ = novaworld_service_cs_config(cfg_.max_packet_size);
	host_result_ = ServerResultFields{};
	play_result_ = ServerResultFields{};
	host_gsid_.clear();
	host_requires_join_ticket_ = 0;
	notices_.clear();
	mission_exit_reason_ = 0;
	disconnect_event_ = DisconnectEvent{};
	disconnect_latched_ = false;
	disconnected_by_peer_ = false;
	tossed_datagrams_ = 0;
	receive_clock_armed_ = false;
	glsvss_deadline_ms_ = 0;
	glsvss_poll_ms_ = 0;
	// Outbound 0x43 seq is 1-based, matching genuine NovaWorld: the retail
	// client's first protocol packet is seq=1 (capture frame 9739). Starting at
	// 0 makes ack=0 ambiguous with "acked nothing" in the peer's reliable-delivery
	// layer, which stalls the verify exchange against live NW (NW-S5).
	reset_sequencing();
	// A fresh connection object: an empty outgoing queue and no pending ACK
	// [orig: CNapiGameSession_InitNPConnection @0x4d3f70 -> CNapiNPConnection_Create @0x62acb0].
	send_queue_.clear();
	ack_pending_ = false;
	last_recv_activity_ms_ = 0;
	sent_client_connected_ = false;
	reassembly_ = ProtocolReassemblyState{};
	// A fresh connection: no counted disconnects and no reconnect pending
	// [orig: InitNPConnection @0x4d408f (+0x710 = 0); NapiNPConnection_Create @0x62acb0].
	reconnect_ = Reconnect{};
	dcnt_ = 0;
	rcnt_ = 0;
	host_setup_.clear();
	host_list_.clear();
	player_list_.clear();
	play_setup_.clear();
	reconnect_counter_ = 0;
	return build_client_hello();
}

std::vector<uint8_t> ClientSession::retransmit_stage_datagram() {
	// A reconnect's 0x41 / 0x42 ride the session's own pump (pump()).
	if (reconnecting()) return {};
	switch (state_) {
	case State::Hello: return build_client_hello();
	case State::Auth: return build_client_auth();
	default: return {};
	}
}

// The shared identity struct-fill (declared in client_session.h) — used by ClientSession below AND by
// inmatch::JoinerConnection (engine/runtime/inmatch). The two builders differ only in the CO source and the framing
// envelope, so the fill lives here once. [orig: one CNapiNPConnection identity block @0x61fe20.]
ClientHello make_client_hello(const ClientSession::Config &cfg, std::string_view co) {
	ClientHello hello;
	hello.nvs  = cfg.nvs;
	hello.co   = std::string(co);
	hello.ap   = cfg.ap;
	hello.bdat = cfg.bdat;
	hello.pn   = cfg.pn;
	hello.pv1  = cfg.pv1;
	hello.pv2  = cfg.pv2;
	hello.ci   = cfg.client_index;
	// PG — the 16-byte protocol GUID the real server validates (HandleClientHello @0x6213B0 compares 16
	// bytes at proto+284): the GUID for `pn` (NOVAWORLDUDP lobby vs JointOperations game), or the
	// explicit cfg.pg when use_default_pg is false. CO/AP/BDAT are read but never validated.
	hello.pg = cfg.use_default_pg ? pg_for_pn(cfg.pn) : cfg.pg;
	hello.pg_present = true;
	// eip/epn stay 0 (retail zeroes them).
	return hello;
}

ClientAuth make_client_auth(const ClientSession::Config &cfg, std::string_view co, uint32_t server_hk,
                            std::string_view client_scrk) {
	ClientAuth auth;
	// Identity block — the real server re-validates NVS/PN/PG/PV1 (+PV2 in its is_server branch) on the
	// 0x42 join exactly as on the 0x41 hello (HandleClientJoin @0x62B750); without it real NW silently
	// drops the join (return 0, no ServerAuth -> session_join timeout). Retail's 0x42 builder
	// (CNapiNPConnection_SendClientJoin @0x61fe20) emits the SAME identity block as the hello.
	auth.nvs  = cfg.nvs;
	auth.co   = std::string(co);
	auth.ap   = cfg.ap;
	auth.bdat = cfg.bdat;
	auth.pn   = cfg.pn;
	auth.pg   = cfg.use_default_pg ? pg_for_pn(cfg.pn) : cfg.pg;
	auth.pg_present = true;
	auth.pv1  = cfg.pv1;
	auth.pv2  = cfg.pv2;
	auth.ci   = cfg.client_index;
	auth.hk   = server_hk;        // echo ServerHello.hk
	auth.ck   = cfg.client_key;
	auth.na   = cfg.na;
	auth.scrk = std::string(client_scrk);
	// CU chunks (NW-S3) — the gate-issued session-auth codes + client env the live NW server validates.
	// Empty for the OpenNova server (permissive callbacks); the binding fills cfg.cu_vars from the gate
	// response when targeting live NW.
	for (const auto &v : cfg.cu_vars) {
		auth.cu.push_back(make_client_cu_chunk(v.type, v.name, v.value));
	}
	// sip/spn stay 0 (retail omits the tag when 0).
	return auth;
}

std::vector<uint8_t> ClientSession::build_client_hello() {
	last_framed_send_ms_ = clock_ms_;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                               client_hello_to_bytes(make_client_hello(cfg_, cfg_.co)));
}

std::vector<uint8_t> ClientSession::build_client_auth() {
	last_framed_send_ms_ = clock_ms_;
	ClientAuth auth = make_client_auth(cfg_, cfg_.co, server_hk_, client_scrk_);
	// A re-join carries the counted disconnects and the last 0x82's RCNT (each only when
	// nonzero, so a first join is unchanged) [orig: SendClientJoin @0x62033d..0x62037f].
	auth.dcnt = dcnt_;
	auth.rcnt = rcnt_;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

// QueueMessage for one lobby statement: the serialized container is one layer-4 record (tag 0 /
// full_tag 0, LEN8/LEN16 by size) on the connection's outgoing queue. Both expiry parameters are
// 0, so the record is retained until the peer ACKs the packet that carries it; a statement longer
// than CS field 13 leaves as SplitAtLength FRAG records at the next build.
// [orig: CNapiGameSession_SendHostRequest @0x4d3834 (and every lobby Send*) ->
//  CNapiNPConnection_QueueMessage(conn, 0, 0, 0, 0, 0, payload, len, 1300) @0x628640 — state 1
//  or 5, else -1; flags & 8 clear -> NapiNPMessage_Create @0x627fc0 (the 1300 chunk is
//  DataTransfer's)]
bool ClientSession::queue_lobby_record(const NapiMessage &container) {
	if (!receive_clock_armed_ || (state_ != State::Verifying && state_ != State::Verified))
		return false;
	std::vector<NapiMessage> stream{container};
	std::vector<uint8_t> stream_bytes(napi_stream_size(stream));
	size_t stream_size = 0;
	if (napi_stream_encode(stream, stream_bytes.data(), stream_bytes.size(),
	                       &stream_size) != 0) {
		return false;
	}
	stream_bytes.resize(stream_size);
	ProtocolMessage record = make_protocol_message(0, std::move(stream_bytes));
	record.reliable = true;
	record.retention_flushes = 0;
	send_queue_.push_back(std::move(record));
	return true;
}

bool ClientSession::queue_statement(const NapiMessage &container) {
	// Host registration (and any post-verify lobby traffic) rides the same connection the
	// verify exchange used. Gate it on Verified so a caller can't queue a lobby container
	// before the session is lobby-ready.
	if (state_ != State::Verified) return false;
	return queue_lobby_record(container);
}

std::vector<ClientVar> ClientSession::cookie_vars() const {
	std::vector<ClientVar> cookie;
	// Retail rebuilds from the current browser jar before every Cookie-bearing
	// statement, including the play leg after NWJoin.
	// [orig: CNapiSession_ReadLocaleInfo @ 0x4ce390; ConnectOrHost @ 0x4d543a]
	if (!cfg_.cookie_vars) return cookie;
	for (const auto &kv : cfg_.cookie_vars()) {
		const std::string &value =
		    (kv.first == "NWUID" && kv.second.empty()) ? server_nwuid_ : kv.second;
		cookie.push_back({0, kv.first, value});
	}
	return cookie;
}

bool ClientSession::queue_verify_request() {
	// ClientRequestVerifyResult: SessIdString (empty on the first pass) + the
	// "Cookie" var-list. Witnessed in the genuine .204 capture (frame 10166): a
	// ClientVarList(VarList="Cookie") child whose ClientVar children each carry
	// VarFNum="0" / VarName / VarValue. NWUID is echoed from the
	// ServerSessionInit. CNapiGameSession_SendVerifyRequest @ 0x4d3620.
	NapiMessage req;
	req.name = "ClientRequestVerifyResult";
	req.fields.push_back(str_field("SessIdString", sess_id_string_));
	// The ClientVarList(VarList="Cookie") parent is emitted UNCONDITIONALLY —
	// retail serializes the var list with includeAll=1, so a client with no
	// cookie vars configured sends an EMPTY parent, not an absent one (closes
	// D-NET-20). [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 ->
	// NapiStatement_SerializeVarList @ 0x4d0660]
	req.children.push_back(make_client_var_list("Cookie", cookie_vars()));
	return queue_lobby_record(req);
}

bool ClientSession::queue_glsvss_request() {
	return queue_lobby_record(make_client_glsvss_request(cfg_.glsvss_request, cookie_vars()));
}

bool ClientSession::handle_datagram(const uint8_t *data, size_t len,
                                    std::vector<std::vector<uint8_t>> &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(data, len, opcode, body)) {
		// A datagram the envelope/NWU layer rejects is tossed: retail's receive pump hands it to
		// the error callback (Network_LogTossedPacket, a pure "NAPI TOSSED N BYTES FROM ..." log
		// line under _connectlog) and keeps receiving; no connection is touched.
		// [orig: NapiNPManager_PumpReceive @0x623010 @0x623206; Network_LogTossedPacket @0x4c4580]
		++tossed_datagrams_;
		io::logf(io::LogLevel::kWarn, "lobby: NAPI TOSSED %zu BYTES (bad session envelope)", len);
		return state_ != State::Error;
	}
	switch (opcode) {
	case SESSION_OPCODE_SERVER_HELLO:
		// The first connect's hello, or a reconnect's: a ServerHello re-joins while the
		// connection is down and armed, probing or already in the 0x42 leg.
		// [orig: Nwu_HandleServerHello @0x627e3e..0x627e5f — !conn_flag0 && is_client &&
		//  conn_flag1 && +0x710]
		if (state_ == State::Hello || reconnecting()) on_server_hello(body, out);
		break;
	case SESSION_OPCODE_SERVER_AUTH:
		if (state_ == State::Auth) on_server_auth(body);
		break;
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
		if (state_ == State::Verifying || state_ == State::Verified) {
			on_server_protocol_message(body, out);
		}
		break;
	case SESSION_OPCODE_SERVER_GOODBYE:
		if (state_ == State::Verifying || state_ == State::Verified) on_server_goodbye(body, out);
		break;
	case SESSION_OPCODE_SERVER_RESEND_LIST:
		if (state_ == State::Verifying || state_ == State::Verified) on_server_resend_list(body, out);
		break;
	default:
		// Unexpected opcode for a client — ignore (server-only opcodes never
		// arrive here; truly unknown ones are not fatal).
		break;
	}
	return state_ != State::Error;
}

void ClientSession::on_server_hello(const std::vector<uint8_t> &body,
                                    std::vector<std::vector<uint8_t>> &out) {
	ServerHello sh;
	if (!parse_server_hello(body.data(), body.size(), sh)) {
		fail("bad ServerHello");
		return;
	}
	// CI is the client's correlation token. A shared UDP receive path can
	// observe a valid reply for another in-flight hello; ignore it without
	// advancing this connection's handshake.
	if (sh.ci != cfg_.client_index) return;
	if (reconnecting()) {
		// The re-join: the probe stops and InitFromSession re-keys the connection (a fresh CK
		// and SCRK, this hello's HK, the peer's keys and the CS template reset) into the 0x42
		// leg, DCNT/RCNT kept across it; the schedule's next check waits out the join.
		// [orig: Nwu_HandleServerHello @0x627e62..0x627eb3; CNapiNPConnection_InitFromSession
		//  @0x626320 — the keys @0x6263fb/@0x626401, the cs_dir copies @0x626449/@0x62645c,
		//  state 3 @0x626496]
		reconnect_.probing = false;
		cfg_.client_key = cfg_.next_client_key ? cfg_.next_client_key() : make_random_session_u32();
		client_scrk_ = cfg_.next_scrk ? cfg_.next_scrk() : make_dev_scrk();
		server_hk_ = sh.hk;
		server_sk_ = 0;
		server_scrk_.clear();
		cs_ = novaworld_service_cs_config(cfg_.max_packet_size);
		state_ = State::Auth;
		reconnect_.join_started_ms = clock_ms_;
		reconnect_.join_last_send_ms = clock_ms_;
		reconnect_.next_ms = clock_ms_ + SESSION_RECONNECT_FIRST_DELAY_MS + SESSION_JOIN_TIMEOUT_MS;
		out.push_back(build_client_auth());
		return;
	}
	server_hk_ = sh.hk;       // [Phase 1] the value we must echo in ClientAuth
	state_ = State::Auth;
	out.push_back(build_client_auth());
}

void ClientSession::on_server_auth(const std::vector<uint8_t> &body) {
	ServerAuth sa;
	if (!parse_server_auth(body.data(), body.size(), sa)) {
		fail("bad ServerAuth");
		return;
	}
	// ServerAuth echoes both correlation values chosen by this client. Do not
	// install a foreign connection's session/SCRK material.
	if (sa.ci != cfg_.client_index || sa.ck != cfg_.client_key) return;
	// The 0x82's reconnect count is kept (0 when absent) and rides the next re-join.
	// [orig: NapiNP_HandleServerJoinResponse @0x629e2e]
	rcnt_ = sa.rcnt;
	if (sa.cr != 1) {
		fail("ServerAuth rejected (cr=" + std::to_string(sa.cr) + ")");
		return;
	}
	server_sk_ = sa.sk;         // session_id for our outbound 0x43s
	server_scrk_ = sa.scrk;     // decrypts inbound 0x83 inner streams
	state_ = State::Verifying;
	sent_client_connected_ = false;

	// The 0x82 is ServerSessionInit (NapiNPConnection_SendSessionInit @ 0x620ef0,
	// opcode 0x82), not a distinct "ServerAuth". It carries the NWUID the client
	// must echo back in the verify "Cookie" var-list (NWUID entry); capture frame
	// 9729 -> 10166. CNapiGameSession_OnNovaWorldConnected @ 0x4d1570 reads it from
	// the SessionInit CU and installs it as the NWUID browser form field.
	for (const auto &kv : sa.cu) {
		if (kv.first == "NWUID") server_nwuid_ = kv.second;
		else if (kv.first == "NovaworldWebDomainNameAndPortNumber")
			server_web_domain_ = kv.second;
	}
	// The host's CS block overlays this connection's client-direction template (cs_dir0): each
	// CLIENT-direction (byte 1) entry stores its slot raw (the reap window, the send intervals,
	// the queue and pool bounds, the packet ceiling and budget); fields the 0x82 omits keep the
	// template. [orig: NapiNP_HandleServerJoinResponse @0x629840 — the template seed @0x6299ae,
	//  the CS overlay @0x629b4c..0x629b75]
	for (const CsField &field : sa.client_cs)
		apply_cs_field(cs_, field.field_index, static_cast<int32_t>(field.value));
	// State-5 entry initializes the reap clock: the timeout_ms window runs from the accepted
	// 0x82 onward and every admitted datagram refreshes it. [orig: @0x629eac conn_state = 5 ->
	//  CNapiNPConnection_OnStateChange @0x626060, the conn+0x5E8 store @0x62612f]
	last_receive_ms_ = clock_ms_;
	last_framed_send_ms_ = clock_ms_;
	receive_clock_armed_ = true;
	// The rest of the state-5 entry: the last connection's disconnect record and sequencing
	// start over, and the connection is up again (conn_flag0 set, conn_flag1 cleared).
	// [orig: CNapiNPConnection_OnStateChange @0x626060 — the record @0x626096..0x62609b, the
	//  sequence counters @0x62609d..0x6260af, conn_flag0/1 @0x626172..0x626176, the reconnect
	//  next @0x62617d]
	disconnect_event_ = DisconnectEvent{};
	disconnect_latched_ = false;
	disconnected_by_peer_ = false;
	reset_sequencing();
	ack_pending_ = false;
	last_recv_activity_ms_ = clock_ms_;
	reassembly_ = ProtocolReassemblyState{};
	reconnect_.was_connected = false;
	reconnect_.next_ms = 0;
	// OnNovaWorldConnected marks every var list for a whole resend. [orig: @0x4d1570]
	set_lobby_state(2);
}

// [orig: Nwu_HandleDisconnect @0x623ce0 — the leading dword is the RECEIVER's local key
//  (@0x623e74), then the lenient TLV walk @0x623eb2..0x623fbd; the response arm latches the
//  184-byte description when none is set and calls CNapiNPConnection_RequestDisconnect
//  @0x6240c4]
void ClientSession::on_server_goodbye(const std::vector<uint8_t> &body,
                                      std::vector<std::vector<uint8_t>> &out) {
	if (body.size() < 4 || opennova::io::read_u32_le(body.data()) != cfg_.client_key) return;
	DisconnectEvent event;
	if (!parse_disconnect_event(body.data() + 4, body.size() - 4, event)) return;
	last_receive_ms_ = clock_ms_;
	latch_disconnect(event);
	disconnected_by_peer_ = true;
	teardown_connection(out);
}

// Every teardown of the connected connection (the peer's goodbye or description record, the
// reap, the punt): DCNT counts it, the 0x46 burst echoes the latched record to the peer, the
// session's disconnect callback runs, and the reconnect schedule restarts (the tail acts on it
// only while the hosting/playing word is set).
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 — ++DCNT under conn_flag0
//  @0x6253c9..0x6253dc; the state-5 burst @0x62549e..0x6254d3 then conn+0xC4 =
//  CNapiGameSession_OnDisconnect @0x6254d5..0x6254e7; conn_flag1 and the client's
//  gap/next restart @0x6254f7..0x625525; conn_flag0 cleared @0x625535]
void ClientSession::teardown_connection(std::vector<std::vector<uint8_t>> &out) {
	const bool connected = receive_clock_armed_;
	if (connected) {
		++dcnt_;
		const std::vector<uint8_t> goodbye = nw_encode_outbound(
				SESSION_OPCODE_CLIENT_GOODBYE,
				disconnect_latched_ ? client_goodbye_to_bytes(server_sk_, disconnect_event_)
				                    : client_goodbye_to_bytes(server_sk_));
		for (size_t i = 0; i < disconnect_burst_count(); ++i) out.push_back(goodbye);
	}
	on_disconnected();
	if (connected) {
		reconnect_.was_connected = true;
		reconnect_.gap_ms = SESSION_RECONNECT_GAP_INITIAL_MS;
		reconnect_.next_ms = clock_ms_ + SESSION_RECONNECT_FIRST_DELAY_MS;
	}
	receive_clock_armed_ = false;
}

bool ClientSession::reconnecting() const {
	return !receive_clock_armed_ && reconnect_.was_connected && reconnect_armed();
}

// The disconnect callback drops the session to state 0 (so neither the host nor the play leg
// stands) and leaves the hosting/playing word alone; it resets the session strings the
// connection fed: the web domain to "???", the NWUID and the GSID (the in-match SUS1) cleared
// until a reconnect's 0x82 and re-host re-supply them.
// [orig: CNapiGameSession_OnDisconnect @0x4cfaa0 — SetState(0) @0x4cfb68; the web domain
//  @0x4cfb8e; the NWUID @0x4cfbe3; the GSID global and np_protocol->server_user_string1
//  @0x4cfc3b..0x4cfc54]
void ClientSession::on_disconnected() {
	state_ = State::Closed;
	set_lobby_state(0);
	host_state_ = HostState::Idle;
	play_state_ = PlayState::Idle;
	server_web_domain_ = "???";
	server_nwuid_.clear();
	host_gsid_.clear();
}

void ClientSession::latch_disconnect(const DisconnectEvent &event) {
	// Every latch site keeps the FIRST record (`if (!valid) copy; valid = 1`).
	if (disconnect_latched_) return;
	disconnect_event_ = event;
	disconnect_latched_ = true;
	last_error_ = event.ddstr.empty() ? event.dstr : event.ddstr;
}

// [orig: CGameSession_SetState @0x4ce140 — a no-op on the current state @0x4ce165; the
//  leaving state's flag clear @0x4ce17c..0x4ce1bc, the entered state's set
//  @0x4ce1ea..0x4ce288 (state 0 zeroes the word); case 4 arms the GLSVSS deadline from
//  GLSVSSRIMS (from any state) or GLSVSSAGRMS (returning from state 8) when a request
//  string is set]
void ClientSession::set_lobby_state(int state) {
	if (state == lobby_state_) return;
	const int previous = lobby_state_;
	switch (previous) {
	case 1: session_flags_ &= ~0x1u; break;
	case 2:
	case 3: session_flags_ &= ~0x2u; break;
	case 4: session_flags_ &= ~0x1Au; break;
	case 5: session_flags_ &= ~0x3Au; break;
	case 6: session_flags_ &= ~0x4Au; break;
	case 7: session_flags_ &= ~0x9Au; break;
	case 8: session_flags_ &= ~0x10Au; break;
	default: break;
	}
	lobby_state_ = state;
	switch (state) {
	case 0: session_flags_ = 0; break;
	case 1: session_flags_ |= 0x1u; break;
	case 2:
	case 3: session_flags_ |= 0x2u; break;
	case 4: session_flags_ |= 0x1Au; break;
	case 5: session_flags_ |= 0x3Au; break;
	case 6: session_flags_ |= 0x4Au; break;
	case 7: session_flags_ |= 0x9Au; break;
	case 8: session_flags_ |= 0x10Au; break;
	default: break;
	}
	if (state != 4) return;
	if (cfg_.glsvss_request.empty()) return;
	const int32_t interval = previous == 8 ? cfg_.glsvss_agrms_ms : cfg_.glsvss_rims_ms;
	if (interval <= 0) return;
	glsvss_deadline_ms_ = clock_ms_ + static_cast<uint32_t>(interval);
}

void ClientSession::process_periodic_update() {
	if (state_ == State::Verifying && !sent_client_connected_) {
		// Queue the lobby verify handshake's bare ClientConnected
		// (CNapiGameSession_SendClientConnected @ 0x4cfe30 — a "ClientConnected"
		// statement with zero fields; capture frame 9778). Retail queues this from its
		// periodic-update tick after conn_state==5 && session==2, after that tick's protocol
		// pump, so it builds on the next one. The one-shot flag prevents subsequent periodic
		// updates from repeating the statement.
		NapiMessage connected;
		connected.name = "ClientConnected";
		queue_lobby_record(connected);
		sent_client_connected_ = true;
		set_lobby_state(3);
		return;
	}
	// The GLSVSS poll: NP connection up (state 5) and the session in state 4, at most once per
	// SESSION_GLSVSS_POLL_MS; a passed deadline re-arms from GLSVSSRIMS (or disarms when the
	// interval is <= 0), rebuilds the Cookie and queues the request.
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 @0x4d44cb..0x4d4532]
	if (state_ != State::Verified || lobby_state_ != 4) return;
	if (glsvss_poll_ms_ != 0 && clock_ms_ - glsvss_poll_ms_ <= SESSION_GLSVSS_POLL_MS) return;
	glsvss_poll_ms_ = clock_ms_;
	if (cfg_.glsvss_request.empty() || glsvss_deadline_ms_ == 0) return;
	if (clock_ms_ <= glsvss_deadline_ms_) return;
	glsvss_deadline_ms_ = cfg_.glsvss_rims_ms > 0
	                      ? clock_ms_ + static_cast<uint32_t>(cfg_.glsvss_rims_ms) : 0;
	queue_glsvss_request();
}

// The lobby connection's PumpFlags pass (the periodic update pumps with every flag): the send
// leg, then the state machine's case 5 (the CLNTTMOUT reap, then PumpSendIntervals), then the
// flush counter.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d442e (NapiNPProtocol_Pump(proto, -1, 250));
//  CNapiNPConnection_PumpFlags @0x629780 — 0x20 @0x6297b7, 0x40 @0x6297c3, 0x80 @0x6297d5;
//  PumpStateMachine @0x6292e0 case 5 @0x6295a2..0x62961c]
void ClientSession::pump(std::vector<std::vector<uint8_t>> &out) {
	if (!receive_clock_armed_ || (state_ != State::Verifying && state_ != State::Verified)) {
		pump_reconnect(out);
		return;
	}
	// The send leg: with the NOVAWORLDUDP template's zero holdoff and zero send interval, the
	// queue and a pending ACK build on every pump.
	// [orig: PumpEnumeratorAndSend @0x629279..0x6292bb — conn_state 5, holdoff @0x62927b, the
	//  send interval @0x629298, `queued > 0 || has_pending_out` @0x6292a9 ->
	//  BuildOutgoingPackets @0x6292b4 + PrunePacketQueue @0x6292bb]
	build_outgoing_packets(out);
	if (!receive_clock_armed_) return; // a full pool tore the connection down
	if (cs_.timeout_ms >= 0) {
		const uint32_t elapsed = clock_ms_ - last_receive_ms_;
		if (elapsed > static_cast<uint32_t>(cs_.timeout_ms)) {
			latch_disconnect(make_disconnect_event(2, 3, elapsed,
					static_cast<uint32_t>(cs_.timeout_ms), "", 0, "NP.C:PT:CLNTTMOUT"));
			// RequestDisconnect -> the teardown [orig: @0x62940a]
			teardown_connection(out);
			return;
		}
	}
	// PumpSendIntervals. ACTIVE: records still retained and more than active_send_interval_ms
	// since the last packet -> a build with the pending flag set (a header-only packet when the
	// queue is empty, whose fresh sequence makes the peer ask for what it lost). Timed
	// MISSING-SEQUENCE: packet_queue_interval_ms set (the template's -1 leaves it off), packets
	// held and more than that since the last admitted packet -> the 0x44. EMPTY: nothing
	// retained or held and more than idle_send_interval_ms since the last packet -> the forced
	// header-only keepalive.
	// [orig: CNapiNPConnection_PumpSendIntervals @0x628fd0 — the active leg @0x628ff1..0x629017,
	//  the timed leg @0x628fe9..0x629034, the empty leg @0x629041..0x629067, the build
	//  @0x62906f..0x629089, SendMissingSeqList @0x629091 + the clock @0x62909f]
	const bool retained = seq_.retained_outbound_message_count > 0;
	const bool held = !seq_.queued_inbound.empty();
	bool build = cs_.active_send_interval_ms >= 0 && retained &&
	             clock_ms_ - last_framed_send_ms_ > static_cast<uint32_t>(cs_.active_send_interval_ms);
	const bool missing = cs_.packet_queue_interval_ms >= 0 && held &&
	                     clock_ms_ - last_recv_activity_ms_ >
	                             static_cast<uint32_t>(cs_.packet_queue_interval_ms);
	if (!build && !missing && cs_.idle_send_interval_ms >= 0 && !retained && !held &&
	    clock_ms_ - last_framed_send_ms_ > static_cast<uint32_t>(cs_.idle_send_interval_ms)) {
		build = true;
	}
	if (build) {
		ack_pending_ = true;
		build_outgoing_packets(out);
		if (!receive_clock_armed_) return;
	}
	if (missing) {
		send_missing_sequence_list(out);
		last_recv_activity_ms_ = clock_ms_;
	}
	advance_session_send_flush_counter(seq_);
}

// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430 — the ACK-only packet when the
//  queue is empty and has_pending_out is set @0x62847e..0x62848c, the per-packet send stamp
//  @0x628605, has_pending_out cleared once the queue count is zero @0x62861f..0x628629;
//  PrunePacketQueue @0x6292bb; a full pool: NapiNPMessage_Create @0x628099..0x628112 ->
//  CNapiNPConnection_RequestDisconnect @0x61e0f0, a client-side connection's SetState(6)
//  @0x61e0fa]
void ClientSession::build_outgoing_packets(std::vector<std::vector<uint8_t>> &out) {
	if (send_queue_.empty() && !ack_pending_) return;
	OutgoingPacketPlan plan = plan_outgoing_packets(seq_, send_queue_,
			packet_ceiling_bytes(cs_.max_packet_bytes),
			max_packets_per_build(cs_.max_packets_per_tick), OutgoingOverflow::StopBuild);
	if (plan.overflow) {
		latch_disconnect(make_disconnect_event(2, 4, plan.overflow_count,
				static_cast<uint32_t>(seq_.outbound_message_limit), "", 0, "NP.C:MSGCRE"));
		send_queue_.clear();
		ack_pending_ = false;
		teardown_connection(out);
		return;
	}
	if (plan.encode_failed) {
		io::logf(io::LogLevel::kWarn, "lobby: a queued statement failed to encode; %zu record(s) dropped",
		         plan.unbuilt.size());
		plan.unbuilt.clear();
	}
	send_queue_ = std::move(plan.unbuilt);
	if (plan.packets.empty()) plan.packets.emplace_back();
	for (const std::vector<ProtocolMessage> &packet : plan.packets) {
		std::vector<uint8_t> body_out;
		// Lobby C2S direction: encrypt with our client_scrk, stamp session_id = the server's SK
		// (the peer's local_key, advertised in ServerAuth). Shared seq/ack framing (ADR 0013).
		if (!frame_session_packet(seq_, SessionCrypto{client_scrk_, {}, server_sk_}, packet,
		                          body_out)) {
			break;
		}
		last_framed_send_ms_ = clock_ms_;
		out.push_back(nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body_out)));
	}
	if (send_queue_.empty()) ack_pending_ = false;
	prune_session_send_boundary(seq_);
}

// Each requested sequence is rebuilt from its retained records under that old sequence (with
// the current ACK) and sent at once, inside the receive pump; 0 asks for the next one. A packet
// whose records the ACK already retired goes out header-only.
// [orig: NapiNP_HandleResendList @0x623800 — the receiver's key @0x623870, the walk
//  @0x623980..0x6239da -> CNapiNPConnection_SendSessionPacket @0x6239b6]
void ClientSession::on_server_resend_list(const std::vector<uint8_t> &body,
                                          std::vector<std::vector<uint8_t>> &out) {
	std::vector<uint32_t> requested;
	if (!decode_session_resend_list(body.data(), body.size(), cfg_.client_key, requested)) return;
	for (uint32_t requested_sequence : requested) {
		const uint32_t sequence = requested_sequence == 0 ? seq_.next_outbound_seq : requested_sequence;
		std::vector<uint8_t> body_out;
		if (!frame_session_packet_for_sequence(seq_, SessionCrypto{client_scrk_, {}, server_sk_},
		                                       sequence, body_out)) {
			continue;
		}
		out.push_back(nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body_out)));
	}
}

void ClientSession::finish_receive_batch(std::vector<std::vector<uint8_t>> &out) {
	if (!receive_clock_armed_ || (state_ != State::Verifying && state_ != State::Verified)) return;
	if (!seq_.missing_request_pending) return;
	seq_.missing_request_pending = false;
	if (seq_.queued_inbound.empty()) return;
	send_missing_sequence_list(out);
}

// [orig: CNapiNPConnection_SendMissingSeqList @0x623560 — the peer's key, then up to sixteen
//  sequences from BuildMissingSeqList @0x6234b0]
void ClientSession::send_missing_sequence_list(std::vector<std::vector<uint8_t>> &out) {
	const std::vector<uint32_t> missing = build_session_missing_sequence_list(seq_, false);
	std::vector<uint8_t> body;
	if (!encode_session_resend_list(server_sk_, missing, body)) return;
	out.push_back(nw_encode_outbound(SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(body)));
}

// A fresh NP connection's sequencing: the counters start over at the state-5 entry and the
// connection runs the ordered receive gate with its template's queue and pool bounds.
// [orig: CNapiNPConnection_OnStateChange @0x626060 — the counters @0x62609d..0x6260b5;
//  NapiNPProtocol_HandleSessionPacket @0x626be0..0x626c3a (the gate); NapiNPMessage_Create
//  @0x628048 (the pool)]
void ClientSession::reset_sequencing() {
	seq_ = SessionSequencing{1, 0};
	seq_.ordered_recovery_enabled = true;
	sync_session_sequencing_limits(seq_, cs_);
}

// The reconnect, in the connection pump's order: the enumerator's re-announce first, then the
// state machine's 0x42 leg and its schedule tail.
// [orig: CNapiNPConnection_PumpFlags @0x629780 — PumpEnumeratorAndSend @0x6297b7 ahead of
//  PumpStateMachine @0x6297c3]
void ClientSession::pump_reconnect(std::vector<std::vector<uint8_t>> &out) {
	if (!reconnecting()) return;
	// The probe: the same-CI 0x41 at once and then every SESSION_CONNECT_RETRANSMIT_MS while
	// the window is open. [orig: PumpEnumeratorAndSend @0x6290c0 @0x6290e0..0x629170 —
	//  `!last_send_tick || now - last_send_tick > interval`, NapiNPSession_SendAnnouncePacket
	//  @0x61fa00 to each UDPNOVAWORLD session]
	if (reconnect_.probing &&
	    (!reconnect_.probe_sent || clock_ms_ - reconnect_.last_probe_ms > SESSION_CONNECT_RETRANSMIT_MS)) {
		reconnect_.probe_sent = true;
		reconnect_.last_probe_ms = clock_ms_;
		out.push_back(build_client_hello());
	}
	// Case 3, the re-join: the 0x42 re-sent every SESSION_CONNECT_RETRANSMIT_MS until
	// SESSION_JOIN_TIMEOUT_MS from the state-3 entry, which drops back to the down state with
	// no teardown (the connection never came up). [orig: PumpStateMachine @0x629508..0x629595,
	//  the timeout @0x629525..0x629564]
	if (state_ == State::Auth) {
		if (clock_ms_ - reconnect_.join_started_ms > SESSION_JOIN_TIMEOUT_MS) {
			state_ = State::Closed;
		} else if (clock_ms_ - reconnect_.join_last_send_ms > SESSION_CONNECT_RETRANSMIT_MS) {
			reconnect_.join_last_send_ms = clock_ms_;
			out.push_back(build_client_auth());
		}
	}
	// The schedule tail: past `next`, a re-join in flight pushes the check out by the gap plus
	// the join timeout; a down connection opens a SESSION_RECONNECT_PROBE_WINDOW_MS probe
	// window, and a window that ran out closes and waits the gap, which grows each time.
	// [orig: PumpStateMachine @0x629487..0x6296cb — state 3 @0x6294ec..0x629501, the window
	//  @0x629632..0x629676, the window's end @0x62968b..0x629693, the gap growth
	//  @0x6296a3..0x6296cb]
	if (clock_ms_ <= reconnect_.next_ms) return;
	if (state_ == State::Auth) {
		reconnect_.next_ms = clock_ms_ + reconnect_.gap_ms + SESSION_JOIN_TIMEOUT_MS;
		grow_reconnect_gap();
		return;
	}
	if (reconnect_.probing) {
		if (clock_ms_ <= reconnect_.window_start_ms + SESSION_RECONNECT_PROBE_WINDOW_MS) return;
		reconnect_.probing = false;
		reconnect_.next_ms = clock_ms_ + reconnect_.gap_ms;
		grow_reconnect_gap();
		return;
	}
	reconnect_.probing = true;
	reconnect_.window_start_ms = clock_ms_;
	reconnect_.probe_sent = false;
	reconnect_.next_ms = clock_ms_ + SESSION_RECONNECT_PROBE_WINDOW_MS;
}

// [orig: @0x6296a3..0x6296cb — gap += step, clamped to [min, max]]
void ClientSession::grow_reconnect_gap() {
	const int32_t grown = static_cast<int32_t>(reconnect_.gap_ms + SESSION_RECONNECT_GAP_STEP_MS);
	reconnect_.gap_ms = static_cast<uint32_t>(grown < SESSION_RECONNECT_GAP_MIN_MS
			? SESSION_RECONNECT_GAP_MIN_MS
			: (grown > SESSION_RECONNECT_GAP_MAX_MS ? SESSION_RECONNECT_GAP_MAX_MS : grown));
}

// A nonzero direction byte stores each written slot into cs_dir0, raw; the sequencing bounds
// follow the block. [orig: CNapiNPConnection_HandleCSConfigUpdate @0x621940 — a nonzero
//  direction byte stores into cs_dir0 @0x6219C8]
void ClientSession::apply_cs_config_update(const ProtocolMessage &pm) {
	const CsConfigUpdate update = decode_cs_config_update(pm.payload.data(), pm.payload.size());
	if (!update.to_dir0) return;
	for (uint32_t slot = 0; slot < static_cast<uint32_t>(kCsConfigSlots); ++slot) {
		if ((update.written & (1u << slot)) != 0)
			apply_cs_field(cs_, slot, update.value[slot]);
	}
	sync_session_sequencing_limits(seq_, cs_);
}

void ClientSession::on_server_protocol_message(const std::vector<uint8_t> &body,
                                               std::vector<std::vector<uint8_t>> &out) {
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	SessionDeframeAdmission admission;
	// Lobby recv: decrypt inbound 0x83 with the server's SCRK. The ordered gate admits only the
	// next sequence: a later packet is held (and latches the missing-sequence request for the
	// receive tail), and the packets a closing gap releases drain in order.
	// [orig: NapiNPProtocol_HandleSessionPacket @0x626a00 — the receiver key @0x626b72, the gate
	//  and queue @0x626be0..0x626c3a]
	if (!deframe_session_packet(seq_, SessionCrypto{{}, server_scrk_, 0, cfg_.client_key},
	                            body.data(), body.size(), hdr, messages, &admission)) {
		fail("bad 0x83 protocol packet");
		return;
	}
	if (!admission.admitted) return;
	// The admitted packets' ACKs retire the records they cover, and each refreshes the reap
	// clock. [orig: CNapiNPConnection_ParseMessages @0x625d54 (the clocks),
	//  @0x625d6b..0x625db2 (the retained-record prune)]
	acknowledge_session_packets(seq_, admission.max_ack_count);
	last_receive_ms_ = clock_ms_;
	last_recv_activity_ms_ = clock_ms_;

	bool had_messages = false;
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		if (!packet.messages.empty()) had_messages = true;
		for (const ProtocolMessage &pm : packet.messages) {
			// The ONE settings-flagged record that is not connection tuning: the connection
			// description the peer sends to close the session. An active connection records it
			// and moves to state 6 (teardown) whatever the body parsed to.
			// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0 @0x621d53..0x621d6b]
			if (pm.flags.settings_update && pm.full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION) {
				DisconnectEvent event;
				parse_disconnect_event(pm.payload.data(), pm.payload.size(), event);
				latch_disconnect(event);
				disconnected_by_peer_ = true;
				teardown_connection(out);
				return;
			}
			if (pm.flags.settings_update) {
				if (pm.full_tag == (PROTOCOL_FULL_TAG_HIGH_BASE | hightag::CS_CONFIG_UPDATE))
					apply_cs_config_update(pm);
				continue;
			}
			if (pm.full_tag != 0) continue;           // only layer-4 lobby containers

			// FIRST/MID/FINAL records fold into one statement before dispatch (a long
			// ClientRequestVerifyResult reply spans several packets).
			// [orig: CNapiNPConnection_DispatchMessage @0x622570]
			std::vector<uint8_t> assembled;
			if (!reassemble_protocol_payload(reassembly_, pm, assembled)) continue;
			if (assembled.empty()) continue;          // ack-only

			std::vector<NapiMessage> containers;
			size_t consumed = 0;
			if (napi_stream_decode(assembled.data(), assembled.size(),
			                       containers, &consumed) != 0) {
				continue;
			}
			for (const auto &container : containers) {
				dispatch_server_container(container, out);
				if (state_ == State::Closed || state_ == State::Error) return;
			}
		}
	}

	// A packet that carried records sets the pending-ACK flag: the next send build carries the
	// ACK, on a packet of queued statements (ServerStartVerify -> the verify request) or alone as
	// a header-only packet (the settings-update packet, capture frame 9730; the terminal
	// ServerVerifyResult, frame 10620). A header-only packet draws no ACK.
	// [orig: CNapiNPConnection_ParseMessages @0x625dff (has_pending_out per record);
	//  BuildOutgoingPackets @0x62847e..0x62848c]
	if (had_messages) ack_pending_ = true;
}

// The client msginfo dispatch table [orig: the 14-entry {name, handler} table @0x82c0c8]:
// "ServerStartVerify" -> CNapiGameSession_SendLocaleAndVerify @0x4d57e0,
// "ServerVerifyResult" -> CNapiGameSession_HandleConnectVerifyResponse @0x4d5800,
// "ServerHostResult" -> CNapiGameSession_HandleHostVerifyResponse @0x4d59d0,
// "ServerPlayerEnterResult" -> CNapiGameSession_HandlePlayEnterResponse @0x4d1940,
// "ServerStopHosting" -> CNapiGameSession_HandleServerMessage @0x4d1c50,
// "ServerPlayResult" -> CNapiGameSession_HandleVerifyResponse @0x4d1e00,
// "ServerStopPlaying" -> CNapiGameSession_HandleServerDisconnectMsg @0x4d1fa0,
// "ServerLeaveNovaWorld" -> CNapiGameSession_HandlePuntNotification @0x4d20b0,
// "ServerCommand" -> CNapiGameSession_HandleServerCommand, "ServerGLSVSSResults" -> CNapiGameSession_HandleGLSVSSResults
// @0x4d3380; the four ServerNWUStat* entries are empty in retail.
void ClientSession::dispatch_server_container(const NapiMessage &container,
                                              std::vector<std::vector<uint8_t>> &out) {
	const std::string &name = container.name;
	if (strutil::iequals(name, "ServerStartVerify")) {
		// Every challenge is answered: the handler is a bare ReadLocaleInfo (the Cookie
		// rebuild) + SendVerifyRequest with no guard. [orig: SendLocaleAndVerify @0x4d57e0]
		queue_verify_request();
		return;
	}
	if (strutil::iequals(name, "ServerVerifyResult")) {
		std::string success, sess;
		for (const auto &f : container.fields) {
			if (strutil::iequals(f.name, "Success")) success = field_to_string(f);
			else if (strutil::iequals(f.name, "SessIdString")) sess = field_to_string(f);
		}
		// Retail parses Success with atol() and treats ANY non-zero integer as
		// success (leading-whitespace/sign tolerant), not an exact "1" match.
		// [orig: CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800 (atol @ 0x76ab0a)]
		if (std::strtol(success.c_str(), nullptr, 10) != 0) {
			sess_id_string_ = sess;
			state_ = State::Verified;
			set_lobby_state(4);
			// A first verify marks the session verified [orig: @0x4d5920..0x4d5940]. A word
			// already hosting or playing is a reconnect's re-verify, which re-requests at once:
			// the host counts its ReconnectCounter up and re-sends ClientHostRequest with
			// CurrentlyHosting=1 and every list, the joiner re-sends ClientPlayRequest with
			// CurrentlyPlaying=1 and its PlaySetup. [orig: @0x4d5961..0x4d5978 (++session+0x500,
			//  InitHeapsAndSerializeCounter, StartHostingSession(1)); @0x4d5998..0x4d599e
			//  (StartPlayingSession(1))]
			if (session_role_ == kSessionRoleNone) {
				session_role_ = kSessionRoleVerified;
			} else if (session_role_ == kSessionRoleHosting) {
				++reconnect_counter_;
				set_or_create_var(host_setup_, {0, "ReconnectCounter", std::to_string(reconnect_counter_)});
				send_host_request(1);
				Notice notice;
				notice.kind = Notice::Kind::Rehost;
				notices_.push_back(std::move(notice));
			} else if (session_role_ == kSessionRolePlaying) {
				send_play_request(1);
				Notice notice;
				notice.kind = Notice::Kind::Replay;
				notices_.push_back(std::move(notice));
			}
		} else {
			// The rejection drops the word and the state to 0 [orig: @0x4d58ac..0x4d58de].
			session_role_ = kSessionRoleNone;
			set_lobby_state(0);
			fail("ServerVerifyResult rejected (Success=" + success + ")");
		}
		return;
	}
	if (strutil::iequals(name, "ServerHostResult")) {
		// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d59d0]
		host_result_ = parse_server_result_fields(container);
		Notice notice;
		notice.kind = Notice::Kind::HostResult;
		notice.fields = host_result_;
		if (host_result_.success) {
			const auto host_commands = parse_host_commands(container);
			host_requires_join_ticket_ = 0;
			const auto ticket = host_commands.find("HostRequiresJoinTicket");
			if (ticket != host_commands.end())
				host_requires_join_ticket_ = static_cast<int>(std::strtol(ticket->second.c_str(), nullptr, 10));
			const auto gsid = host_commands.find("GSID");
			host_gsid_ = gsid != host_commands.end() ? copy_capped(gsid->second, 128) : std::string();
			host_state_ = HostState::Established;
			set_lobby_state(6);
			// [orig: @0x4d5c17..0x4d5c40 — any word 0..3 becomes 2]
			if (session_role_ >= kSessionRoleNone && session_role_ <= kSessionRolePlaying)
				session_role_ = kSessionRoleHosting;
		} else {
			host_state_ = HostState::Failed;
			set_lobby_state(4);
			// [orig: @0x4d5ae7..0x4d5b27 — any word 0..3 becomes 1]
			if (session_role_ >= kSessionRoleNone && session_role_ <= kSessionRolePlaying)
				session_role_ = kSessionRoleVerified;
		}
		notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerPlayResult")) {
		// [orig: CNapiGameSession_HandleVerifyResponse @0x4d1e00 — Success -> state 8]
		play_result_ = parse_server_result_fields(container);
		Notice notice;
		notice.kind = Notice::Kind::PlayResult;
		notice.fields = play_result_;
		if (play_result_.success) {
			play_state_ = PlayState::Playing;
			set_lobby_state(8);
			// [orig: @0x4d1f35..0x4d1f53 — an unsigned word <= 2 becomes 3]
			if (static_cast<uint32_t>(session_role_) <= 2u) session_role_ = kSessionRolePlaying;
		} else {
			play_state_ = PlayState::Failed;
			set_lobby_state(4);
			// [orig: @0x4d1eee..0x4d1f13 — any word 0..3 becomes 1]
			if (session_role_ >= kSessionRoleNone && session_role_ <= kSessionRolePlaying)
				session_role_ = kSessionRoleVerified;
		}
		notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerStopHosting")) {
		// [orig: CNapiGameSession_HandleServerMessage @0x4d1c50 — the triple, state 4, the
		//  52-row msgcode key]
		Notice notice;
		notice.kind = Notice::Kind::StopHosting;
		notice.fields = parse_server_result_fields(container);
		notice.msg_key = novaworld_server_msg_code_key(notice.fields.msg_code);
		host_result_ = notice.fields;
		host_state_ = HostState::Idle;
		set_lobby_state(4);
		// [orig: @0x4d1d29..0x4d1d4f — any word 0..3 becomes 1]
		if (session_role_ >= kSessionRoleNone && session_role_ <= kSessionRolePlaying)
			session_role_ = kSessionRoleVerified;
		notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerStopPlaying")) {
		// [orig: CNapiGameSession_HandleServerDisconnectMsg @0x4d1fa0 — the triple, state 4,
		//  g_MissionExitReason = 12]
		Notice notice;
		notice.kind = Notice::Kind::StopPlaying;
		notice.fields = parse_server_result_fields(container);
		play_result_ = notice.fields;
		play_state_ = PlayState::Idle;
		set_lobby_state(4);
		// [orig: @0x4d2057..0x4d207c — any word 0..3 becomes 1]
		if (session_role_ >= kSessionRoleNone && session_role_ <= kSessionRolePlaying)
			session_role_ = kSessionRoleVerified;
		mission_exit_reason_ = 12;
		notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerLeaveNovaWorld")) {
		// [orig: CNapiGameSession_HandlePuntNotification @0x4d20b0 — the triple, state 0,
		//  RequestDisconnect, g_MissionExitReason = 12, ERR_PUNTEDFROMNOVAWORLD]
		Notice notice;
		notice.kind = Notice::Kind::LeaveNovaWorld;
		notice.fields = parse_server_result_fields(container);
		host_state_ = HostState::Idle;
		play_state_ = PlayState::Idle;
		set_lobby_state(0);
		// [orig: @0x4d21cd..0x4d21ec — a word 1..3 becomes 0, the reconnect disarmed with it]
		session_role_ = kSessionRoleNone;
		mission_exit_reason_ = 12;
		disconnected_by_peer_ = true;
		// RequestDisconnect: the teardown's burst and disconnect callback.
		// [orig: HandlePuntNotification @0x4d20b0 -> CNapiNPConnection_RequestDisconnect]
		teardown_connection(out);
		notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerCommand")) {
		Notice notice;
		notice.kind = Notice::Kind::Command;
		if (parse_server_command(container, notice.command)) notices_.push_back(std::move(notice));
		return;
	}
	if (strutil::iequals(name, "ServerGLSVSSResults")) {
		// [orig: CNapiGameSession_HandleGLSVSSResults @0x4d3380 — acts only when the
		//  "GLSVSSResults" param is present (1024-char cap)]
		for (const auto &f : container.fields) {
			if (!strutil::iequals(f.name, "GLSVSSResults")) continue;
			Notice notice;
			notice.kind = Notice::Kind::GlsvssResults;
			notice.glsvss_results = copy_capped(field_to_string(f), 1024);
			notices_.push_back(std::move(notice));
			return;
		}
		return;
	}
	if (strutil::iequals(name, "ServerPlayerEnterResult")) {
		// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4d1940 — ConnectionId / Success /
		//  MsgCode atol'd, PlayerTicket (128) and AccessCodeList (0x2000) copied, IpAddress /
		//  PortNumber / MsgParam1 / MsgParam2 atol'd and dropped]
		Notice notice;
		notice.kind = Notice::Kind::PlayerEnterResult;
		for (const auto &f : container.fields) {
			if (strutil::iequals(f.name, "ConnectionId"))
				notice.player_enter.connection_id = static_cast<uint32_t>(atol_field(f));
			else if (strutil::iequals(f.name, "Success")) notice.player_enter.success = atol_field(f);
			else if (strutil::iequals(f.name, "MsgCode")) notice.player_enter.msg_code = atol_field(f);
			else if (strutil::iequals(f.name, "PlayerTicket"))
				notice.player_enter.player_ticket = copy_capped(field_to_string(f), 128);
			else if (strutil::iequals(f.name, "AccessCodeList"))
				notice.player_enter.access_code_list = copy_capped(field_to_string(f), 0x2000);
		}
		notices_.push_back(std::move(notice));
		return;
	}
	// ServerNWUStatGameOpenResult / GameContinueResult / GameMode / PlayerEnterResult: the
	// retail handlers @0x4ce0f0/@0x4ce100/@0x4ce110/@0x4ce120 are empty.
}

std::vector<ClientSession::Notice> ClientSession::take_notices() {
	std::vector<Notice> drained;
	drained.swap(notices_);
	return drained;
}

// BuildHostVarLists then StartHostingSession: the lists start over (HostSetup with
// ReconnectCounter zeroed, the Host list's first run, an empty PlayerList) and the request goes.
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50 (ReconnectCounter = 0 @0x4d0b7b) ->
//  StartHostingSession(0) @0x4d5113]
bool ClientSession::request_hosting(const HostRegistration &cfg, int currently_hosting) {
	if (state_ != State::Verified || (host_state_ != HostState::Idle && host_state_ != HostState::Failed))
		return false;
	reconnect_counter_ = 0;
	HostRegistration setup = cfg;
	setup.reconnect_counter = reconnect_counter_;
	host_setup_ = make_host_setup_var_list(setup);
	host_list_ = make_host_var_list(setup, HostLobbyText{}, /*full=*/false);
	player_list_.clear();
	return send_host_request(currently_hosting);
}

// [orig: CNapiGameSession_StartHostingSession @0x4d4540 — state 4 -> 5 @0x4d4561, then
//  SendHostRequest @0x4d3700 (CurrentlyHosting, VarCheck, Cookie, HostSetup, Host, PlayerList);
//  a refused queue drops back to state 4 and fails @0x4d4574..0x4d4583]
bool ClientSession::send_host_request(int currently_hosting) {
	if (state_ != State::Verified || (host_state_ != HostState::Idle && host_state_ != HostState::Failed))
		return false;
	const HostState previous = host_state_;
	host_state_ = HostState::Requested;
	host_result_ = ServerResultFields{};
	set_lobby_state(5);
	if (queue_lobby_record(make_client_host_request(currently_hosting, cookie_vars(), host_setup_,
	                                                host_list_, player_list_))) {
		return true;
	}
	host_state_ = previous;
	set_lobby_state(4);
	return false;
}

// The Host and PlayerList vars land in the session's lists (SetOrCreate) and the update
// carries them. [orig: Lobby_UpdateServerInfo @0x4fe8c0 (the SetOrCreates) ->
//  CNapiGameSession_SendHostUpdate @0x4d3860]
bool ClientSession::send_host_update(const std::vector<ClientVar> &host,
                                     const std::vector<ClientVar> &player_list) {
	for (const ClientVar &v : host) set_or_create_var(host_list_, v);
	for (const ClientVar &v : player_list) set_or_create_var(player_list_, v);
	return queue_statement(make_client_host_update(host, player_list));
}

// The slot's five vars replace whatever it held, then the statement goes only in state 6.
// [orig: Server_PlayerAdd @0x51d421..0x51d4aa (RemoveByUserData + the five SetOrCreates) ->
//  the state-6 wrapper @0x4d0e20]
bool ClientSession::send_host_player_added(const HostPlayerSlot &player) {
	remove_slot_vars(player_list_, player.slot);
	for (const ClientVar &v : make_player_list({player})) player_list_.push_back(v);
	if (host_state_ != HostState::Established) return false;
	return queue_statement(make_client_host_player_added(
			player.slot, player.player_name, player.ip_and_port, player.pcid, player.team,
			player.type));
}

// [orig: the slot's vars dropped (NapiLinkedList_RemoveByUserData @0x630810), then the
//  state-6 wrapper @0x4d0e40]
bool ClientSession::send_host_player_removed(int player_number) {
	remove_slot_vars(player_list_, player_number);
	if (host_state_ != HostState::Established) return false;
	return queue_statement(make_client_host_player_removed(player_number));
}

bool ClientSession::send_player_enter_request(uint32_t connection_id, uint32_t ip_address,
                                              uint32_t port_number,
                                              const std::string &join_ticket) {
	if (host_state_ != HostState::Established) return false;
	return queue_statement(
			make_client_player_enter_request(connection_id, ip_address, port_number, join_ticket));
}

bool ClientSession::stop_hosting() {
	// [orig: CGameSession_StopHosting @0x4d0e60 — states 5/6 back to 4 + the statement
	//  @0x4d0e63..0x4d0e86]
	bool queued = false;
	if (host_state_ == HostState::Requested || host_state_ == HostState::Established) {
		host_state_ = HostState::Idle;
		set_lobby_state(4);
		queued = queue_statement(make_client_stop_hosting());
	}
	// A hosting word steps back to verified whatever the state, so a session torn down
	// mid-hosting stops its reconnect's re-host too [orig: @0x4d0e89..0x4d0e9e].
	if (session_role_ == kSessionRoleHosting) session_role_ = kSessionRoleVerified;
	return queued;
}

// ConnectOrHost rebuilds the PlaySetup list, then StartPlayingSession sends it.
// [orig: ConnectOrHost @0x4d53bb..0x4d542b (g_SessionConnectVarList) -> StartPlayingSession(0)
//  @0x4d5449]
bool ClientSession::request_playing(const std::vector<ClientVar> &play_setup,
                                    int currently_playing) {
	play_setup_ = play_setup;
	return send_play_request(currently_playing);
}

// [orig: CNapiGameSession_StartPlayingSession @0x4d45e0 — state 4 -> 7 and the MsgCode
//  cleared, else -1; SendPlayRequest @0x4d3920 — CurrentlyPlaying, Cookie, PlaySetup; a
//  refused queue drops back to state 4]
bool ClientSession::send_play_request(int currently_playing) {
	if (state_ != State::Verified || (play_state_ != PlayState::Idle && play_state_ != PlayState::Failed))
		return false;
	const PlayState previous = play_state_;
	play_state_ = PlayState::Requested;
	play_result_ = ServerResultFields{};
	set_lobby_state(7);
	if (queue_lobby_record(make_client_play_request(currently_playing, cookie_vars(), play_setup_)))
		return true;
	play_state_ = previous;
	set_lobby_state(4);
	return false;
}

bool ClientSession::stop_playing() {
	// [orig: CGameSession_StopPlaying @0x4d0ec0 — states 7/8 back to 4 + the statement; the
	//  NovaWorld menu re-entered after a match runs it, UI_ProcessLANSessionStateMachine @0x558e80]
	bool queued = false;
	if (play_state_ == PlayState::Requested || play_state_ == PlayState::Playing) {
		play_state_ = PlayState::Idle;
		set_lobby_state(4);
		queued = queue_statement(make_client_stop_playing());
	}
	// A playing word steps back to verified whatever the state, so a session torn down
	// mid-play stops its reconnect's re-play too [orig: @0x4d0ee9..0x4d0efe].
	if (session_role_ == kSessionRolePlaying) session_role_ = kSessionRoleVerified;
	return queued;
}

std::vector<uint8_t> ClientSession::build_goodbye() {
	// The leading dword is the receiver's local session key (ServerAuth.SK),
	// followed by the latched disconnect record (a peer punt / the reap) or, for a
	// user leave, the un-latched zero record retail sends
	// [orig: CNapiGameSession_ResetToDisconnected @0x4D0890 ->
	//  CNapiNPConnection_RequestDisconnect @0x61E0F0 (no latch) ->
	//  CNapiNPConnection_Destroy @0x62A4B0 -> TeardownActiveConnection @0x6253C0 ->
	//  SendDisconnectPacket @0x61F2A0 (writes disconnect_event unconditionally)].
	// CI is not part of this packet and using it makes a keyed receiver reject
	// the leave.
	std::vector<uint8_t> body = disconnect_latched_
			? client_goodbye_to_bytes(server_sk_, disconnect_event_)
			: client_goodbye_to_bytes(server_sk_);
	// The reset drops the hosting/playing word to 0 (the reconnect disarmed with it), and the
	// connection's teardown runs the disconnect callback. [orig: @0x4d08fc..0x4d0927]
	session_role_ = kSessionRoleNone;
	on_disconnected();
	receive_clock_armed_ = false;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE, std::move(body));
}

void ClientSession::fail(std::string reason) {
	last_error_ = std::move(reason);
	state_ = State::Error;
}

} // namespace opennova
