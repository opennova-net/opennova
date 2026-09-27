#include <net/novaworld/client_session.h>

#include <base/io/le.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <net/napi/envelope.h>
#include <net/napi/tlv.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h> // decode_cs_config_update
#include <net/npwire/session_keys.h>

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
	lobby_state_ = 0;
	server_hk_ = 0;
	server_sk_ = 0;
	server_scrk_.clear();
	server_nwuid_.clear();
	server_web_domain_.clear();
	sess_id_string_.clear();
	last_error_.clear();
	cs_ = ConnectionSettings{};
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
	seq_ = SessionSequencing{1, 0};
	sent_client_connected_ = false;
	reassembly_ = ProtocolReassemblyState{};
	return build_client_hello();
}

std::vector<uint8_t> ClientSession::retransmit_stage_datagram() {
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
	return nw_encode_outbound(
			SESSION_OPCODE_CLIENT_AUTH,
			client_auth_to_bytes(make_client_auth(cfg_, cfg_.co, server_hk_, client_scrk_)));
}

std::vector<uint8_t> ClientSession::build_lobby_packet(const NapiMessage &container) {
	std::vector<NapiMessage> stream{container};
	std::vector<uint8_t> stream_bytes(napi_stream_size(stream));
	size_t stream_size = 0;
	if (napi_stream_encode(stream, stream_bytes.data(), stream_bytes.size(),
	                       &stream_size) != 0) {
		return {};
	}
	stream_bytes.resize(stream_size);

	// One inner message, layer-4 lobby (tag 0 / full_tag 0), LEN8/LEN16 by size.
	ProtocolMessage pm;
	pm.flags.raw   = (stream_size > 0xFFu) ? 0x40u : 0x20u;
	pm.flags.len16 = stream_size > 0xFFu;
	pm.flags.len8  = stream_size <= 0xFFu;
	pm.tag = 0;
	pm.full_tag = 0;
	pm.length = static_cast<uint32_t>(stream_size);
	pm.payload = std::move(stream_bytes);

	// Lobby C2S direction: encrypt with our client_scrk, stamp session_id = the server's SK (the peer's
	// local_key, advertised in ServerAuth — mirror of the server setting session_id = client's CK on its
	// 0x83). Shared seq/ack framing (ADR 0013).
	std::vector<uint8_t> body_out;
	if (!frame_session_packet(seq_, SessionCrypto{client_scrk_, {}, server_sk_}, {pm}, body_out)) {
		return {};
	}
	last_framed_send_ms_ = clock_ms_;
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE,
	                               std::move(body_out));
}

std::vector<uint8_t> ClientSession::build_lobby_message(const NapiMessage &container) {
	// Host registration (and any post-verify lobby traffic) rides the same 0x43
	// envelope the verify exchange used. Gate it on Verified so a caller can't
	// emit a lobby container before the SCRK/session_id are established (which
	// would encrypt under a zero key the server rejects).
	if (state_ != State::Verified) return {};
	return build_lobby_packet(container);
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

std::vector<uint8_t> ClientSession::build_verify_request() {
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
	return build_lobby_packet(req);
}

std::vector<uint8_t> ClientSession::build_glsvss_request() {
	return build_lobby_packet(make_client_glsvss_request(cfg_.glsvss_request, cookie_vars()));
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
		if (state_ == State::Hello) on_server_hello(body, out);
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
		if (state_ == State::Verifying || state_ == State::Verified) on_server_goodbye(body);
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
	// The host's CS block overlays this connection's client-direction template (cs_dir0):
	// CLIENT-direction (byte 1) entries land on the reap window and the three send intervals;
	// fields the 0x82 omits keep the template. [orig: NapiNP_HandleServerJoinResponse
	//  @0x629840 — the template seed @0x6299ae, the CS overlay @0x629b4c..0x629b75]
	for (const CsField &field : sa.client_cs) {
		switch (field.field_index) {
		case 0: cs_.timeout_ms = static_cast<int32_t>(field.value); break;
		case 4: cs_.idle_send_interval_ms = static_cast<int32_t>(field.value); break;
		case 5: cs_.active_send_interval_ms = static_cast<int32_t>(field.value); break;
		case 6: cs_.packet_queue_interval_ms = static_cast<int32_t>(field.value); break;
		default: break;
		}
	}
	// State-5 entry initializes the reap clock: the timeout_ms window runs from the accepted
	// 0x82 onward and every admitted datagram refreshes it. [orig: @0x629eac conn_state = 5 ->
	//  CNapiNPConnection_OnStateChange @0x626060, the conn+0x5E8 store @0x62612f]
	last_receive_ms_ = clock_ms_;
	last_framed_send_ms_ = clock_ms_;
	receive_clock_armed_ = true;
	set_lobby_state(2);
}

// [orig: Nwu_HandleDisconnect @0x623ce0 — the leading dword is the RECEIVER's local key
//  (@0x623e74), then the lenient TLV walk @0x623eb2..0x623fbd; the response arm latches the
//  184-byte description when none is set and calls CNapiNPConnection_RequestDisconnect
//  @0x6240c4]
void ClientSession::on_server_goodbye(const std::vector<uint8_t> &body) {
	if (body.size() < 4 || opennova::io::read_u32_le(body.data()) != cfg_.client_key) return;
	DisconnectEvent event;
	if (!parse_disconnect_event(body.data() + 4, body.size() - 4, event)) return;
	last_receive_ms_ = clock_ms_;
	latch_disconnect(event);
	disconnected_by_peer_ = true;
	state_ = State::Closed;
}

void ClientSession::latch_disconnect(const DisconnectEvent &event) {
	// Every latch site keeps the FIRST record (`if (!valid) copy; valid = 1`).
	if (disconnect_latched_) return;
	disconnect_event_ = event;
	disconnect_latched_ = true;
	last_error_ = event.ddstr.empty() ? event.dstr : event.ddstr;
}

// [orig: CGameSession_SetState @0x4ce140 — case 4 arms the GLSVSS deadline from GLSVSSRIMS
//  (from any state) or GLSVSSAGRMS (returning from state 8) when a request string is set]
void ClientSession::set_lobby_state(int state) {
	const int previous = lobby_state_;
	lobby_state_ = state;
	if (state != 4) return;
	if (cfg_.glsvss_request.empty()) return;
	const int32_t interval = previous == 8 ? cfg_.glsvss_agrms_ms : cfg_.glsvss_rims_ms;
	if (interval <= 0) return;
	glsvss_deadline_ms_ = clock_ms_ + static_cast<uint32_t>(interval);
}

void ClientSession::process_periodic_update(
		std::vector<std::vector<uint8_t>> &out) {
	if (state_ == State::Verifying && !sent_client_connected_) {
		// Emit the lobby verify handshake's bare ClientConnected
		// (CNapiGameSession_SendClientConnected @ 0x4cfe30 — a "ClientConnected"
		// statement with zero fields; capture frame 9778). Retail emits this from its
		// periodic-update tick after conn_state==5 && session==2; this method owns
		// that boundary after the SessionInit handler completes. The one-shot flag
		// prevents subsequent periodic updates from repeating the statement.
		NapiMessage connected;
		connected.name = "ClientConnected";
		out.push_back(build_lobby_packet(connected));
		sent_client_connected_ = true;
		set_lobby_state(3);
		return;
	}
	// The GLSVSS poll: NP connection up (state 5) and the session in state 4, at most once per
	// SESSION_GLSVSS_POLL_MS; a passed deadline re-arms from GLSVSSRIMS (or disarms when the
	// interval is <= 0), rebuilds the Cookie and sends the request.
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 @0x4d44cb..0x4d4532]
	if (state_ != State::Verified || lobby_state_ != 4) return;
	if (glsvss_poll_ms_ != 0 && clock_ms_ - glsvss_poll_ms_ <= SESSION_GLSVSS_POLL_MS) return;
	glsvss_poll_ms_ = clock_ms_;
	if (cfg_.glsvss_request.empty() || glsvss_deadline_ms_ == 0) return;
	if (clock_ms_ <= glsvss_deadline_ms_) return;
	glsvss_deadline_ms_ = cfg_.glsvss_rims_ms > 0
	                      ? clock_ms_ + static_cast<uint32_t>(cfg_.glsvss_rims_ms) : 0;
	out.push_back(build_glsvss_request());
}

// [orig: CNapiNPConnection_PumpStateMachine @0x6292e0 case 5 (the CLNTTMOUT reap over
//  cs_dir0.timeout_ms since the last admitted datagram, then PumpSendIntervals);
//  CNapiNPConnection_PumpSendIntervals @0x628fd0 — the empty leg @0x629041..0x629067:
//  idle_send_interval_ms >= 0, nothing queued, nothing retained, elapsed since the last
//  send > the interval -> one forced (header-only) packet]
void ClientSession::pump_send_intervals(std::vector<std::vector<uint8_t>> &out) {
	if (!receive_clock_armed_ || (state_ != State::Verifying && state_ != State::Verified)) return;
	if (cs_.timeout_ms >= 0) {
		const uint32_t elapsed = clock_ms_ - last_receive_ms_;
		if (elapsed > static_cast<uint32_t>(cs_.timeout_ms)) {
			latch_disconnect(make_disconnect_event(2, 3, elapsed,
					static_cast<uint32_t>(cs_.timeout_ms), "", 0, "NP.C:PT:CLNTTMOUT"));
			state_ = State::Closed;
			return;
		}
	}
	// The lobby ClientSession queues nothing between pumps and retains no reliable nodes
	// (D-NET-164), so the empty leg's two queue guards always hold here.
	if (cs_.idle_send_interval_ms < 0) return;
	if (clock_ms_ - last_framed_send_ms_ <= static_cast<uint32_t>(cs_.idle_send_interval_ms)) return;
	out.push_back(build_heartbeat());
}

// The lobby keeps the cs_dir0 slots it runs on: CS field 0 (the reap timeout) and
// fields 4/5/6 (the send-interval legs), each only when the update stored it.
// [orig: CNapiNPConnection_HandleCSConfigUpdate @0x621940 — a nonzero direction
//  byte stores into cs_dir0 @0x6219C8]
void ClientSession::apply_cs_config_update(const ProtocolMessage &pm) {
	const CsConfigUpdate update = decode_cs_config_update(pm.payload.data(), pm.payload.size());
	if (!update.to_dir0) return;
	const auto apply = [&](int slot, int32_t &field) {
		if ((update.written & (1u << slot)) != 0) field = update.value[static_cast<size_t>(slot)];
	};
	apply(0, cs_.timeout_ms);
	apply(4, cs_.idle_send_interval_ms);
	apply(5, cs_.active_send_interval_ms);
	apply(6, cs_.packet_queue_interval_ms);
}

void ClientSession::on_server_protocol_message(const std::vector<uint8_t> &body,
                                               std::vector<std::vector<uint8_t>> &out) {
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	// Lobby recv: decrypt inbound 0x83 with the server's SCRK; deframe latches seq_.last_inbound_seq.
	if (!deframe_session_packet(seq_, SessionCrypto{{}, server_scrk_, 0, cfg_.client_key},
	                            body.data(), body.size(), hdr,
	                            messages)) {
		fail("bad 0x83 protocol packet");
		return;
	}
	// Every admitted session packet refreshes the reap clock [orig: ParseMessages @0x625d54].
	last_receive_ms_ = clock_ms_;

	const size_t out_before = out.size();
	const bool had_messages = !messages.empty();
	for (const auto &pm : messages) {
		// The ONE settings-flagged record that is not connection tuning: the connection
		// description the peer sends to close the session. An active connection records it and
		// moves to state 6 (teardown) whatever the body parsed to.
		// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0 @0x621d53..0x621d6b]
		if (pm.flags.settings_update && pm.full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION) {
			DisconnectEvent event;
			parse_disconnect_event(pm.payload.data(), pm.payload.size(), event);
			latch_disconnect(event);
			disconnected_by_peer_ = true;
			state_ = State::Closed;
			return;
		}
		if (pm.flags.settings_update) {
			if (pm.full_tag == (PROTOCOL_FULL_TAG_HIGH_BASE | hightag::CS_CONFIG_UPDATE))
				apply_cs_config_update(pm);
			continue;
		}
		if (pm.full_tag != 0) continue;           // only layer-4 lobby containers

		// Fragment reassembly (verify replies are single-fragment today, but
		// retail can split ConnectCommands; mirror the server's handling).
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

	// Acknowledge inbound data packets the genuine NovaWorld way: its reliable
	// layer expects a 0x43 carrying ack == the seq it just sent. When a packet
	// delivered content but drew no substantive reply (the settings-update
	// packet, capture frame 9730; or the terminal ServerVerifyResult, frame
	// 10620), emit a header-only ack so the peer advances — retail's p#1 and p#4.
	// A packet that already produced a reply (ServerStartVerify -> the verify
	// request) carries the ack itself, so no extra ack is sent.
	if (had_messages && out.size() == out_before) {
		out.push_back(build_heartbeat());
	}
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
		out.push_back(build_verify_request());
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
		} else {
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
		} else {
			host_state_ = HostState::Failed;
			set_lobby_state(4);
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
		} else {
			play_state_ = PlayState::Failed;
			set_lobby_state(4);
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
		mission_exit_reason_ = 12;
		disconnected_by_peer_ = true;
		state_ = State::Closed;
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

std::vector<uint8_t> ClientSession::build_host_request(const HostRegistration &cfg,
                                                       int currently_hosting) {
	// [orig: CNapiGameSession_StartHostingSession @0x4d4540 — state 4 -> 5, else -1]
	if (state_ != State::Verified || (host_state_ != HostState::Idle && host_state_ != HostState::Failed))
		return {};
	host_state_ = HostState::Requested;
	host_result_ = ServerResultFields{};
	set_lobby_state(5);
	return build_lobby_packet(make_host_request(cfg, cookie_vars(), currently_hosting));
}

std::vector<uint8_t> ClientSession::build_host_update(const std::vector<ClientVar> &host,
                                                      const std::vector<ClientVar> &player_list) {
	// [orig: CNapiGameSession_SendHostUpdate @0x4d3860]
	return build_lobby_message(make_client_host_update(host, player_list));
}

std::vector<uint8_t> ClientSession::build_host_player_added(const HostPlayerSlot &player) {
	if (host_state_ != HostState::Established) return {};
	return build_lobby_message(make_client_host_player_added(
			player.slot, player.player_name, player.ip_and_port, player.pcid, player.team,
			player.type));
}

std::vector<uint8_t> ClientSession::build_host_player_removed(int player_number) {
	if (host_state_ != HostState::Established) return {};
	return build_lobby_message(make_client_host_player_removed(player_number));
}

std::vector<uint8_t> ClientSession::build_player_enter_request(uint32_t connection_id,
                                                               uint32_t ip_address,
                                                               uint32_t port_number,
                                                               const std::string &join_ticket) {
	if (host_state_ != HostState::Established) return {};
	return build_lobby_message(
			make_client_player_enter_request(connection_id, ip_address, port_number, join_ticket));
}

std::vector<uint8_t> ClientSession::build_stop_hosting() {
	// [orig: CGameSession_StopHosting @0x4d0e60 — states 5/6 back to 4 + the statement]
	if (host_state_ != HostState::Requested && host_state_ != HostState::Established) return {};
	host_state_ = HostState::Idle;
	set_lobby_state(4);
	return build_lobby_message(make_client_stop_hosting());
}

std::vector<uint8_t> ClientSession::build_play_request(const std::vector<ClientVar> &play_setup) {
	// [orig: CNapiGameSession_StartPlayingSession @0x4d45e0 — state 4 -> 7, else -1;
	//  the reconnect flag is CurrentlyPlaying, 0 on the fresh path @0x4d5449]
	if (state_ != State::Verified || (play_state_ != PlayState::Idle && play_state_ != PlayState::Failed))
		return {};
	play_state_ = PlayState::Requested;
	play_result_ = ServerResultFields{};
	set_lobby_state(7);
	return build_lobby_packet(make_client_play_request(0, cookie_vars(), play_setup));
}

std::vector<uint8_t> ClientSession::build_stop_playing() {
	// [orig: CGameSession_StopPlaying @0x4d0ec0 — states 7/8 back to 4 + the statement]
	if (play_state_ != PlayState::Requested && play_state_ != PlayState::Playing) return {};
	play_state_ = PlayState::Idle;
	set_lobby_state(4);
	return build_lobby_message(make_client_stop_playing());
}

std::vector<uint8_t> ClientSession::build_heartbeat() {
	// Heartbeat: a header-only (empty inner) 0x43 in the lobby C2S direction. Shared framing (ADR 0013).
	std::vector<uint8_t> body_out;
	if (!frame_session_packet(seq_, SessionCrypto{client_scrk_, {}, server_sk_}, {}, body_out)) {
		return {};
	}
	last_framed_send_ms_ = clock_ms_;
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE,
	                               std::move(body_out));
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
	state_ = State::Closed;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE, std::move(body));
}

void ClientSession::fail(std::string reason) {
	last_error_ = std::move(reason);
	state_ = State::Error;
}

} // namespace opennova
