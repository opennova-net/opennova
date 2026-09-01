#include <net/npwire/wire_capture.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

namespace opennova {
namespace {

// Strip the NAPI envelope (CRC) and apply the outer NWU transform, leaving the
// session opcode in `opcode` and the post-opcode body in `body`. The outer
// transform DECRYPTS via nwu_encrypt (the names are swapped — see
// reference_nwu_names_swapped). Identical to the decode_outer that lived in
// nw_pp / the cross-validation tests before this was factored out.
bool decode_outer(const std::vector<uint8_t> &raw, uint8_t &opcode,
                  std::vector<uint8_t> &body) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out) != 0)
		return false;
	stripped.resize(out);
	if (stripped.empty()) return false;
	opcode = stripped[0];
	body.assign(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	return true;
}

// Per-direction reassembly state + the "pending" bookkeeping that lets a message
// fragmented across datagrams be reported under its FIRST fragment's tag/frame
// (the convention nw_pp established).
struct DirState {
	ProtocolReassemblyState rs;
	bool have_pending = false;
	uint16_t pending_tag = 0;
	bool pending_settings = false;
	int pending_first_frame = 0;
};

// Per-session state: its OWN SCRK pair + per-direction reassembly.
//
// IDENTITY (measured 2026-08-24, Kutu gateway capture): the client-side UDP port
// is NOT a session identity in a server-side capture - 42 different clients'
// ClientAuths arrived on one port (32768), so a port-keyed table overwrites the
// stored SCRK on every join and all but the newest client then decrypts with the
// wrong key. Nothing rejects that: parse_protocol_messages is deliberately
// tolerant [orig: CNapiNPConnection_ParseMessages @0x625bc0], so wrong-key bytes
// are emitted as well-formed-looking messages with arbitrary tags.
//
// The protocol packet header's session_id is readable WITHOUT the key (it sits
// ahead of the SCRK-encrypted inner region) and carries the PEER's local key
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00 — bytes 0..3 compared to
// conn->session_keys.local_key; the outbound stamp is modeled at
// session/protocol_message.cpp hdr.session_id = crypto.session_id]:
//   S2C header session_id == that client's ClientAuth.ck
//   C2S header session_id == that connection's ServerAuth.sk
// Verified on the Kutu capture: all 28 distinct S2C session_ids seen on the
// shared port are ClientAuth ck values (28/28, zero unmatched).
//
// So sessions are keyed by the auth-derived identity when it is known, and fall
// back to the historical port key otherwise (hexcaps / port-less crafted caps /
// traffic whose auth is not in the cut) - that path behaves exactly as before.
struct Session {
	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
};

// Identity = (client UDP port, client key). Neither half is sufficient alone,
// both measured on the Kutu capture (55 ClientAuths):
//   * port alone -> 9 buckets: 42 clients collide on port 32768 and overwrite
//     each other's SCRK (the bug this replaced).
//   * client key alone -> 47 buckets, but 8 keys are reused by DIFFERENT
//     clients (the key generator is a rolling pool, not random), which merges
//     sessions the port keying had kept apart and truncates their streams.
//   * (port, key) -> 52 buckets, no merge that port keying did not already
//     make; the 3 residual collisions are same-port rejoins with a recycled
//     key, which the port keying merged too (no regression, smaller residual).
// Bit 63 marks the auth space so it can never alias a bare port key (< 65536)
// used by the fallback path.
constexpr uint64_t kAuthIdBit = uint64_t(1) << 63;
inline uint64_t auth_identity(int client_port, uint32_t client_key) {
	return kAuthIdBit | (uint64_t(uint32_t(client_port)) << 32) | client_key;
}

struct SessionTable {
	std::unordered_map<uint64_t, Session> by_id;
	// (client port, ServerAuth.sk) -> identity, for routing the C2S direction.
	std::map<std::pair<int, uint32_t>, uint64_t> id_by_server_key;
};

// Drive one datagram through the outer-decode pipeline, appending any completed
// in-game messages to `out`. Shared by the live CaptureDecoder and the batch
// function so both produce identical output. State lives in `sessions`, keyed as
// above.
void process_datagram(const CaptureDatagram &d, SessionTable &sessions,
                      CaptureDecodeResult &out);

bool process(const std::vector<uint8_t> &body, const std::string &scrk, char dir,
             DirState &st, int frame, int session, CaptureDecodeResult &out) {
	if (scrk.empty()) return false;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr, msgs))
		return false;
	CapturedSessionPacket packet;
	packet.frame_index = frame;
	packet.dir = dir;
	packet.session = session;
	packet.header = hdr;
	packet.tags.reserve(msgs.size());
	packet.records.reserve(msgs.size());
	for (const ProtocolMessage &pm : msgs) {
		packet.tags.push_back(pm.full_tag);
		CapturedProtocolRecord record;
		record.full_tag = pm.full_tag;
		record.raw_flags = pm.flags.raw;
		record.encoded_length = pm.length;
		record.skip_bytes = pm.skip_bytes;
		packet.records.push_back(std::move(record));
	}
	out.session_packets.push_back(std::move(packet));
	for (const auto &pm : msgs) {
		if (!st.have_pending) {
			st.pending_tag = uint16_t(pm.full_tag);
			st.pending_settings = pm.flags.settings_update;
			st.pending_first_frame = frame;
			st.have_pending = true;
		}
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(st.rs, pm, assembled)) continue;
		InGameMessage m;
		m.frame_index = st.pending_first_frame;
		m.dir = dir;
		m.tag = st.pending_tag;
		m.settings_update = st.pending_settings;
		m.session = session;
		m.payload = std::move(assembled);
		out.messages.push_back(std::move(m));
		st.have_pending = false;
	}
	return true;
}

CaptureDatagramClass classify_opcode(uint8_t opcode) {
	switch (opcode) {
	case SESSION_OPCODE_CLIENT_HELLO:
		return CaptureDatagramClass::ClientHello;
	case SESSION_OPCODE_CLIENT_AUTH:
		return CaptureDatagramClass::ClientAuth;
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
		return CaptureDatagramClass::ClientProtocol;
	case SESSION_OPCODE_SERVER_HELLO:
		return CaptureDatagramClass::ServerHello;
	case SESSION_OPCODE_SERVER_AUTH:
		return CaptureDatagramClass::ServerAuth;
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
		return CaptureDatagramClass::ServerProtocol;
	default:
		return CaptureDatagramClass::Unknown;
	}
}

void process_datagram(const CaptureDatagram &d, SessionTable &sessions,
                      CaptureDecodeResult &out) {
	CapturedDatagramResult result;
	result.frame_index = d.frame_index;
	result.src_port = d.src_port;
	result.dst_port = d.dst_port;
	result.payload_length = d.payload.size();

	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!decode_outer(d.payload, op, body)) {
		out.datagrams.push_back(result);
		return;
	}
	result.outer_decoded = true;
	result.opcode = op;
	result.datagram_class = classify_opcode(op);

	const bool is_server = result.datagram_class == CaptureDatagramClass::ServerHello ||
	                       result.datagram_class == CaptureDatagramClass::ServerAuth ||
	                       result.datagram_class == CaptureDatagramClass::ServerProtocol;
	const bool have_ports = (d.src_port != 0 && d.dst_port != 0);
	const int session_key = !have_ports ? 0 : (is_server ? d.dst_port : d.src_port);
	// Resolve the identity of a protocol packet from its (pre-SCRK) header
	// session_id; fall back to the port key when the peer's auth was not seen.
	const auto protocol_identity = [&](bool server_to_client) -> uint64_t {
		ProtocolPacketHeader hdr;
		if (!parse_protocol_packet_header(body.data(), body.size(), hdr))
			return uint64_t(session_key);
		if (server_to_client) {
			// session_id == the client's own key (ClientAuth.ck); the datagram's
			// client-side port is session_key, so the identity is direct.
			const uint64_t id = auth_identity(session_key, hdr.session_id);
			if (sessions.by_id.count(id)) return id;
		} else {
			// session_id == the server's key for this connection (ServerAuth.sk)
			const auto it =
					sessions.id_by_server_key.find({session_key, hdr.session_id});
			if (it != sessions.id_by_server_key.end()) return it->second;
		}
		return uint64_t(session_key);
	};

	switch (op) {
	case SESSION_OPCODE_CLIENT_HELLO: {
		ClientHello hello;
		result.decoded = parse_client_hello(body.data(), body.size(), hello);
		break;
	}
	case SESSION_OPCODE_CLIENT_AUTH: {
		ClientAuth a;
		result.decoded = parse_client_auth(body.data(), body.size(), a);
		if (result.decoded) {
			// Bind by the client key when present; the port bucket keeps the
			// historical behaviour for captures without one.
			const uint64_t id = a.ck ? auth_identity(session_key, a.ck)
			                         : uint64_t(session_key);
			sessions.by_id[id].client_scrk = a.scrk;
		}
		break;
	}
	case SESSION_OPCODE_SERVER_HELLO: {
		ServerHello hello;
		result.decoded = parse_server_hello(body.data(), body.size(), hello);
		break;
	}
	case SESSION_OPCODE_SERVER_AUTH: {
		ServerAuth a;
		result.decoded = parse_server_auth(body.data(), body.size(), a);
		if (result.decoded) {
			// ServerAuth echoes the client key and carries the server key, so
			// this is where both directions' routing is bound.
			const uint64_t id = a.ck ? auth_identity(session_key, a.ck)
			                         : uint64_t(session_key);
			sessions.by_id[id].server_scrk = a.scrk;
			if (a.sk) sessions.id_by_server_key[{session_key, a.sk}] = id;
		}
		break;
	}
	case SESSION_OPCODE_PROTOCOL_MESSAGE: {
		Session &s = sessions.by_id[protocol_identity(false)];
		result.decoded = process(body, s.client_scrk, 'C', s.cstate,
		                         d.frame_index, session_key, out);
		break;
	}
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE: {
		Session &s = sessions.by_id[protocol_identity(true)];
		result.decoded = process(body, s.server_scrk, 'S', s.sstate,
		                         d.frame_index, session_key, out);
		break;
	}
	default:
		break;
	}
	out.datagrams.push_back(std::move(result));
}

} // namespace

struct CaptureDecoder::Impl {
	SessionTable sessions;
};

CaptureDecoder::CaptureDecoder() : impl_(std::make_unique<Impl>()) {}
CaptureDecoder::~CaptureDecoder() = default;
CaptureDecoder::CaptureDecoder(CaptureDecoder &&) noexcept = default;
CaptureDecoder &CaptureDecoder::operator=(CaptureDecoder &&) noexcept = default;

std::vector<InGameMessage> CaptureDecoder::push(const CaptureDatagram &datagram) {
	return push_detailed(datagram).messages;
}

CaptureDecodeResult CaptureDecoder::push_detailed(const CaptureDatagram &datagram) {
	CaptureDecodeResult out;
	process_datagram(datagram, impl_->sessions, out);
	return out;
}

CaptureDecodeResult decode_capture(const std::vector<CaptureDatagram> &datagrams) {
	CaptureDecodeResult out;
	SessionTable sessions;
	for (const auto &d : datagrams) process_datagram(d, sessions, out);
	return out;
}

std::vector<InGameMessage>
decode_capture_to_messages(const std::vector<CaptureDatagram> &datagrams) {
	return decode_capture(datagrams).messages;
}

} // namespace opennova
