#include "npwire/wire_capture.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

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

// Per-session state keyed by the client-side UDP port (C2S src / S2C dst). Each
// session carries its OWN SCRK pair + per-direction reassembly, so a capture with
// N clients decodes correctly (a single global pair would clobber the prior
// client's keys). When ports are unavailable (hexcap / crafted port-less caps)
// every datagram keys to session 0 — the original single-session behavior.
struct Session {
	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
};

// Drive one datagram through the outer-decode pipeline, appending any completed
// in-game messages to `out`. Shared by the live CaptureDecoder and the batch
// function so both produce identical output. State lives in `sessions`, keyed as
// above.
void process_datagram(const CaptureDatagram &d,
                      std::unordered_map<int, Session> &sessions,
                      std::vector<InGameMessage> &out);

void process(const std::vector<uint8_t> &body, const std::string &scrk, char dir,
             DirState &st, int frame, int session, std::vector<InGameMessage> &out) {
	if (scrk.empty()) return;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr, msgs))
		return;
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
		out.push_back(std::move(m));
		st.have_pending = false;
	}
}

void process_datagram(const CaptureDatagram &d,
                      std::unordered_map<int, Session> &sessions,
                      std::vector<InGameMessage> &out) {
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!decode_outer(d.payload, op, body)) return;
	const bool is_server = (op == SESSION_OPCODE_SERVER_AUTH ||
	                        op == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE);
	const bool have_ports = (d.src_port != 0 && d.dst_port != 0);
	const int session_key = !have_ports ? 0 : (is_server ? d.dst_port : d.src_port);
	Session &s = sessions[session_key];
	switch (op) {
	case SESSION_OPCODE_CLIENT_AUTH: {
		ClientAuth a;
		if (parse_client_auth(body.data(), body.size(), a)) s.client_scrk = a.scrk;
		break;
	}
	case SESSION_OPCODE_SERVER_AUTH: {
		ServerAuth a;
		if (parse_server_auth(body.data(), body.size(), a)) s.server_scrk = a.scrk;
		break;
	}
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
		process(body, s.client_scrk, 'C', s.cstate, d.frame_index, session_key, out);
		break;
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
		process(body, s.server_scrk, 'S', s.sstate, d.frame_index, session_key, out);
		break;
	default:
		break;
	}
}

} // namespace

struct CaptureDecoder::Impl {
	std::unordered_map<int, Session> sessions;
};

CaptureDecoder::CaptureDecoder() : impl_(std::make_unique<Impl>()) {}
CaptureDecoder::~CaptureDecoder() = default;
CaptureDecoder::CaptureDecoder(CaptureDecoder &&) noexcept = default;
CaptureDecoder &CaptureDecoder::operator=(CaptureDecoder &&) noexcept = default;

std::vector<InGameMessage> CaptureDecoder::push(const CaptureDatagram &datagram) {
	std::vector<InGameMessage> out;
	process_datagram(datagram, impl_->sessions, out);
	return out;
}

std::vector<InGameMessage>
decode_capture_to_messages(const std::vector<CaptureDatagram> &datagrams) {
	std::vector<InGameMessage> out;
	std::unordered_map<int, Session> sessions;
	for (const auto &d : datagrams) process_datagram(d, sessions, out);
	return out;
}

} // namespace opennova
