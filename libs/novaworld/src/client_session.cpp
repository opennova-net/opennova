#include <novaworld/client_session.h>

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/session_keys.h>

#include <utility>

namespace opennova {

namespace {

// Encode an outbound NW-UDP datagram: NWU-encrypt the body (client-side
// encrypt is our nwu_decrypt — names swapped vs onnet), opcode-prefix, then
// CRC-envelope. Identical flow to the server's encode_outbound()
// (apps/novaworld_server/nw_udp_listener.cpp) and what the binding inlined
// before this extraction.
std::vector<uint8_t> encode_session_outbound(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) {
		nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	}
	std::vector<uint8_t> with_opcode;
	with_opcode.reserve(1 + body.size());
	with_opcode.push_back(opcode);
	with_opcode.insert(with_opcode.end(), body.begin(), body.end());
	std::vector<uint8_t> packet(with_opcode.size() + 4);
	size_t out_size = 0;
	if (napi_envelope_encode(with_opcode.data(), with_opcode.size(),
	                         packet.data(), packet.size(), &out_size) != 0) {
		return {};
	}
	packet.resize(out_size);
	return packet;
}

// Decode an inbound NW-UDP datagram: strip the CRC envelope, peel the
// plaintext opcode (byte 0), then NWU-decrypt the remaining body (client-side
// decrypt is our nwu_encrypt). Inverse of encode_session_outbound.
bool decode_session_inbound(const uint8_t *raw, size_t raw_len,
                            uint8_t &opcode_out, std::vector<uint8_t> &body_out) {
	std::vector<uint8_t> stripped(raw_len);
	size_t out_size = 0;
	if (napi_envelope_decode(raw, raw_len, stripped.data(), stripped.size(),
	                         &out_size) != 0) {
		return false;
	}
	stripped.resize(out_size);
	if (stripped.empty()) return false;
	opcode_out = stripped[0];
	body_out.assign(stripped.begin() + 1, stripped.end());
	if (!body_out.empty()) {
		nwu_encrypt(body_out.data(), body_out.size(), SESSION_NWU_KEY);
	}
	return true;
}

std::string field_to_string(const NapiField &f) {
	if (f.data.empty()) return {};
	const char *p = reinterpret_cast<const char *>(f.data.data());
	size_t n = f.data.size();
	while (n > 0 && p[n - 1] == '\0') --n;
	return std::string(p, n);
}

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

} // namespace

ClientSession::ClientSession() : ClientSession(Config{}) {}

ClientSession::ClientSession(Config config)
	: cfg_(std::move(config)),
	  client_scrk_(make_client_scrk(cfg_.client_key)) {}

std::vector<uint8_t> ClientSession::start() {
	state_ = State::Hello;
	server_hk_ = 0;
	server_sk_ = 0;
	server_scrk_.clear();
	sess_id_string_.clear();
	last_error_.clear();
	next_outbound_seq_ = 0;
	last_inbound_seq_ = 0;
	sent_verify_request_ = false;
	reassembly_ = ProtocolReassemblyState{};
	return build_client_hello();
}

std::vector<uint8_t> ClientSession::build_client_hello() {
	ClientHello hello;
	hello.nvs  = cfg_.nvs;
	hello.co   = cfg_.co;
	hello.ap   = cfg_.ap;
	hello.bdat = cfg_.bdat;
	hello.pn   = cfg_.pn;
	hello.pv1  = cfg_.pv1;
	hello.pv2  = cfg_.pv2;
	hello.ci   = cfg_.client_index;
	hello.eip  = 0;
	hello.epn  = 0;

	// PG — the 16-byte NOVAWORLDUDP protocol GUID the real server validates
	// (HandleClientHello @ 0x6213B0 compares 16 bytes at proto+284). Built by
	// CNapiGameSession_InitNPConnection @ 0x4d3be0 via
	//   sub_62E750(dst, -655487758, 58574, 17549, 144,180,29,66,179,100,171,113)
	// which lays out [u32 version LE][u16 port_a LE][u16 port_b LE][8 bytes].
	// Compute from those literals so the byte order is exact regardless of host.
	{
		const uint32_t version = static_cast<uint32_t>(-655487758);  // 0xD8EE0CF2
		const uint16_t port_a = 58574;  // 0xE4CE
		const uint16_t port_b = 17549;  // 0x448D
		hello.pg[0] = static_cast<uint8_t>(version & 0xFFu);
		hello.pg[1] = static_cast<uint8_t>((version >> 8) & 0xFFu);
		hello.pg[2] = static_cast<uint8_t>((version >> 16) & 0xFFu);
		hello.pg[3] = static_cast<uint8_t>((version >> 24) & 0xFFu);
		hello.pg[4] = static_cast<uint8_t>(port_a & 0xFFu);
		hello.pg[5] = static_cast<uint8_t>((port_a >> 8) & 0xFFu);
		hello.pg[6] = static_cast<uint8_t>(port_b & 0xFFu);
		hello.pg[7] = static_cast<uint8_t>((port_b >> 8) & 0xFFu);
		hello.pg[8] = 144; hello.pg[9] = 180; hello.pg[10] = 29; hello.pg[11] = 66;
		hello.pg[12] = 179; hello.pg[13] = 100; hello.pg[14] = 171; hello.pg[15] = 113;
		hello.pg_present = true;
	}

	return encode_session_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                               client_hello_to_bytes(hello));
}

std::vector<uint8_t> ClientSession::build_client_auth() {
	ClientAuth auth;
	auth.ci   = cfg_.client_index;
	auth.hk   = server_hk_;   // [Phase 1] echo ServerHello.hk (was hardcoded 0)
	auth.ck   = cfg_.client_key;
	auth.na   = cfg_.na;
	auth.sip  = 0;
	auth.spn  = 0;
	auth.scrk = client_scrk_;
	return encode_session_outbound(SESSION_OPCODE_CLIENT_AUTH,
	                               client_auth_to_bytes(auth));
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

	ProtocolPacketHeader hdr;
	// session_id = peer's local_key = the server's SK (advertised in
	// ServerAuth). Mirror of the server setting session_id = client's CK on
	// its outbound 0x83 (nw_udp_listener.cpp + protocol_message.h witness).
	hdr.session_id = server_sk_;
	hdr.seq_num = next_outbound_seq_++;
	hdr.ack_count = last_inbound_seq_;
	hdr.connection_flags = 0;

	std::vector<uint8_t> body_out;
	if (!encode_protocol_packet_plaintext(hdr, {pm}, client_scrk_, body_out)) {
		return {};
	}
	return encode_session_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE,
	                               std::move(body_out));
}

bool ClientSession::handle_datagram(const uint8_t *data, size_t len,
                                    std::vector<std::vector<uint8_t>> &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!decode_session_inbound(data, len, opcode, body)) {
		fail("bad session envelope");
		return false;
	}
	switch (opcode) {
	case SESSION_OPCODE_SERVER_HELLO:
		if (state_ == State::Hello) on_server_hello(body, out);
		break;
	case SESSION_OPCODE_SERVER_AUTH:
		if (state_ == State::Auth) on_server_auth(body, out);
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
	server_hk_ = sh.hk;       // [Phase 1] the value we must echo in ClientAuth
	state_ = State::Auth;
	out.push_back(build_client_auth());
}

void ClientSession::on_server_auth(const std::vector<uint8_t> &body,
                                   std::vector<std::vector<uint8_t>> &out) {
	ServerAuth sa;
	if (!parse_server_auth(body.data(), body.size(), sa)) {
		fail("bad ServerAuth");
		return;
	}
	if (sa.cr != 1) {
		fail("ServerAuth rejected (cr=" + std::to_string(sa.cr) + ")");
		return;
	}
	server_sk_ = sa.sk;         // session_id for our outbound 0x43s
	server_scrk_ = sa.scrk;     // decrypts inbound 0x83 inner streams
	state_ = State::Verifying;
	sent_verify_request_ = false;

	// Kick off the lobby verify handshake. The server's handle_client_connected
	// ignores the container body, so a bare ClientConnected suffices for
	// OpenNova; retail re-sends identity here (a later parity refinement).
	NapiMessage connected;
	connected.name = "ClientConnected";
	out.push_back(build_lobby_packet(connected));
}

void ClientSession::on_server_protocol_message(const std::vector<uint8_t> &body,
                                               std::vector<std::vector<uint8_t>> &out) {
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), server_scrk_,
	                                      hdr, messages)) {
		fail("bad 0x83 protocol packet");
		return;
	}
	last_inbound_seq_ = hdr.seq_num;

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
}

void ClientSession::dispatch_server_container(const NapiMessage &container,
                                              std::vector<std::vector<uint8_t>> &out) {
	if (container.name == "ServerStartVerify") {
		if (!sent_verify_request_) {
			sent_verify_request_ = true;
			NapiMessage req;
			req.name = "ClientRequestVerifyResult";
			out.push_back(build_lobby_packet(req));
		}
		return;
	}
	if (container.name == "ServerVerifyResult") {
		std::string success, sess;
		for (const auto &f : container.fields) {
			if (f.name == "Success") success = field_to_string(f);
			else if (f.name == "SessIdString") sess = field_to_string(f);
		}
		if (success == "1") {
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
	ProtocolPacketHeader hdr;
	hdr.session_id = server_sk_;
	hdr.seq_num = next_outbound_seq_++;
	hdr.ack_count = last_inbound_seq_;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body_out;
	if (!encode_protocol_packet_plaintext(hdr, {}, client_scrk_, body_out)) {
		return {};
	}
	return encode_session_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE,
	                               std::move(body_out));
}

std::vector<uint8_t> ClientSession::build_goodbye() {
	std::vector<uint8_t> body(4);
	const uint32_t ci = cfg_.client_index;
	body[0] = static_cast<uint8_t>(ci & 0xFFu);
	body[1] = static_cast<uint8_t>((ci >> 8) & 0xFFu);
	body[2] = static_cast<uint8_t>((ci >> 16) & 0xFFu);
	body[3] = static_cast<uint8_t>((ci >> 24) & 0xFFu);
	state_ = State::Closed;
	return encode_session_outbound(SESSION_OPCODE_CLIENT_GOODBYE, std::move(body));
}

void ClientSession::fail(std::string reason) {
	last_error_ = std::move(reason);
	state_ = State::Error;
}

} // namespace opennova
