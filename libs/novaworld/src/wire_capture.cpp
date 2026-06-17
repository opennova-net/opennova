#include "novaworld/wire_capture.h"

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

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

void process(const std::vector<uint8_t> &body, const std::string &scrk, char dir,
             DirState &st, int frame, std::vector<InGameMessage> &out) {
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
		m.payload = std::move(assembled);
		out.push_back(std::move(m));
		st.have_pending = false;
	}
}

} // namespace

std::vector<InGameMessage>
decode_capture_to_messages(const std::vector<CaptureDatagram> &datagrams) {
	std::vector<InGameMessage> out;
	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
	for (const auto &d : datagrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.payload, op, body)) continue;
		switch (op) {
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth a;
			if (parse_client_auth(body.data(), body.size(), a)) client_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			ServerAuth a;
			if (parse_server_auth(body.data(), body.size(), a)) server_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_PROTOCOL_MESSAGE:
			process(body, client_scrk, 'C', cstate, d.frame_index, out);
			break;
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			process(body, server_scrk, 'S', sstate, d.frame_index, out);
			break;
		default:
			break;
		}
	}
	return out;
}

} // namespace opennova
