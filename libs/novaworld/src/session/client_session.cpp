#include <novaworld/client_session.h>

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <npwire/session_keys.h>

#include <array>
#include <cstdlib>
#include <utility>
#include <npwire/nw_session_framing.h>

namespace opennova {

namespace {

// A 61-char [A-Z0-9] client SCRK matching retail's length + alphabet
// (notes/retail_capture_findings.md). Retail randomizes per session; we
// derive deterministically from the client key so dev/test runs are
// reproducible — only the length + alphabet matter on the wire.
std::string make_client_scrk(uint32_t seed) {
	static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; // 36
	uint32_t x = seed ? seed : 0x9E3779B9u;
	std::string out;
	out.reserve(61);
	for (int i = 0; i < 61; ++i) {
		x = x * 1664525u + 1013904223u; // Numerical Recipes LCG
		out.push_back(alphabet[(x >> 24) % 36u]);
	}
	return out;
}

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
	    CuVar{"MaxPacketSize", in.max_packet_size, 2},
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

ClientSession::ClientSession(Config config)
	: cfg_(std::move(config)),
	  client_scrk_(make_client_scrk(cfg_.client_key)) {}

std::vector<uint8_t> ClientSession::start() {
	state_ = State::Hello;
	server_hk_ = 0;
	server_sk_ = 0;
	server_scrk_.clear();
	server_nwuid_.clear();
	server_web_domain_.clear();
	sess_id_string_.clear();
	last_error_.clear();
	// Outbound 0x43 seq is 1-based, matching genuine NovaWorld: the retail
	// client's first protocol packet is seq=1 (capture frame 9739). Starting at
	// 0 makes ack=0 ambiguous with "acked nothing" in the peer's reliable-delivery
	// layer, which stalls the verify exchange against live NW (NW-S5).
	seq_ = SessionSequencing{1, 0};
	sent_client_connected_ = false;
	sent_verify_request_ = false;
	reassembly_ = ProtocolReassemblyState{};
	return build_client_hello();
}

// The shared identity struct-fill (declared in client_session.h) — used by ClientSession below AND by
// np::JoinerConnection (libs/npruntime). The two builders differ only in the CO source and the framing
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
	// (NapiNPConnection_SendClientHello @0x61fe20) emits the SAME identity block as the hello.
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
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                               client_hello_to_bytes(make_client_hello(cfg_, cfg_.co)));
}

std::vector<uint8_t> ClientSession::build_client_auth() {
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

std::vector<uint8_t> ClientSession::build_verify_request() {
	// ClientRequestVerifyResult: SessIdString (empty on the first pass) + the
	// "Cookie" var-list when configured. Witnessed in the genuine .204 capture
	// (frame 10166): a ClientVarList(VarList="Cookie") child whose ClientVar
	// children each carry VarFNum="0" / VarName / VarValue. NWUID is echoed from
	// the ServerSessionInit. CNapiGameSession_SendVerifyRequest @ 0x4d3620.
	auto str_field = [](const char *name, const std::string &value) {
		NapiField f;
		f.name = name;
		f.data.assign(value.begin(), value.end());
		return f;
	};

	NapiMessage req;
	req.name = "ClientRequestVerifyResult";
	req.fields.push_back(str_field("SessIdString", sess_id_string_));

	// The ClientVarList(VarList="Cookie") parent is emitted UNCONDITIONALLY —
	// retail serializes the var list with includeAll=1, so a client with no
	// cookie vars configured sends an EMPTY parent, not an absent one (closes
	// D-NET-20). [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 ->
	// NapiStatement_SerializeVarList @ 0x4d0660]
	NapiMessage var_list;
	var_list.name = "ClientVarList";
	var_list.fields.push_back(str_field("VarList", "Cookie"));
	for (const auto &kv : cfg_.verify_cookie_vars) {
		const std::string &value =
		    (kv.first == "NWUID" && kv.second.empty()) ? server_nwuid_
		                                               : kv.second;
		NapiMessage var;
		var.name = "ClientVar";
		var.fields.push_back(str_field("VarFNum", "0"));
		var.fields.push_back(str_field("VarName", kv.first));
		var.fields.push_back(str_field("VarValue", value));
		var_list.children.push_back(std::move(var));
	}
	req.children.push_back(std::move(var_list));

	return build_lobby_packet(req);
}

bool ClientSession::handle_datagram(const uint8_t *data, size_t len,
                                    std::vector<std::vector<uint8_t>> &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(data, len, opcode, body)) {
		fail("bad session envelope");
		return false;
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
	sent_verify_request_ = false;

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
}

void ClientSession::process_periodic_update(
		std::vector<std::vector<uint8_t>> &out) {
	if (state_ != State::Verifying || sent_client_connected_) return;

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

	const size_t out_before = out.size();
	const bool had_messages = !messages.empty();
	for (const auto &pm : messages) {
		if (pm.flags.settings_update) continue;   // socket tuning, ignore
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

void ClientSession::dispatch_server_container(const NapiMessage &container,
                                              std::vector<std::vector<uint8_t>> &out) {
	if (container.name == "ServerStartVerify") {
		if (!sent_verify_request_) {
			sent_verify_request_ = true;
			out.push_back(build_verify_request());
		}
		return;
	}
	if (container.name == "ServerVerifyResult") {
		std::string success, sess;
		for (const auto &f : container.fields) {
			if (f.name == "Success") success = field_to_string(f);
			else if (f.name == "SessIdString") sess = field_to_string(f);
		}
		// Retail parses Success with atol() and treats ANY non-zero integer as
		// success (leading-whitespace/sign tolerant), not an exact "1" match.
		// [orig: CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800 (atol @ 0x76ab0a)]
		if (std::strtol(success.c_str(), nullptr, 10) != 0) {
			sess_id_string_ = sess;
			state_ = State::Verified;
		} else {
			fail("ServerVerifyResult rejected (Success=" + success + ")");
		}
		return;
	}
	// Other containers (e.g. the embedded ConnectCommands var-list) carry no
	// client action in Phase 1 — the browse/host/play legs (ADR 0010 P2+)
	// handle their own reply containers.
}

std::vector<uint8_t> ClientSession::build_heartbeat() {
	// Heartbeat: a header-only (empty inner) 0x43 in the lobby C2S direction. Shared framing (ADR 0013).
	std::vector<uint8_t> body_out;
	if (!frame_session_packet(seq_, SessionCrypto{client_scrk_, {}, server_sk_}, {}, body_out)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE,
	                               std::move(body_out));
}

std::vector<uint8_t> ClientSession::build_goodbye() {
	// The leading dword is the receiver's local session key (ServerAuth.SK),
	// followed by retail's zeroed disconnect-stat TLVs. CI is not part of this
	// packet and using it makes a keyed receiver reject the leave.
	std::vector<uint8_t> body = client_goodbye_to_bytes(server_sk_);
	state_ = State::Closed;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE, std::move(body));
}

void ClientSession::fail(std::string reason) {
	last_error_ = std::move(reason);
	state_ = State::Error;
}

} // namespace opennova
