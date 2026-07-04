// Decodes the genuine NovaLogic NovaWorld lobby exchange captured against
// 207.178.209.204:64206 (fixtures/novaworld/nw204_lobby.hexcap — extracted from
// the user's Wireshark trace of a *successful* retail JO session, frames
// 8538-10651). This is the byte-level oracle for ADR 0010 real-NW parity: it
// strips the CRC envelope, removes the outer SESSION_NWU layer, recovers the
// retail client/server SCRK from the 0x42/0x82, decrypts the inner 0x43/0x83
// streams, and dumps every container so our builders can be matched field for
// field.
//
// It runs entirely through the SAME libs the client uses (napi envelope/tlv,
// novacrypto nwu, novaworld session/protocol parsers) — no re-implemented
// crypto — so what it prints is ground truth, not a guess.
//
// Key facts this pins (see docs/net/novaworld-net-re.md Wave 5):
//   - login is NOT a verify prerequisite (this whole exchange precedes any HTTP);
//   - the 0x42 join carries the CU var set; the 892B ClientRequestVerifyResult
//     carries SessIdString + a "Cookie" var-list of CD-key/hardware identity.

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;

namespace {

struct Datagram {
	std::string dir;     // "C2S" or "S2C"
	int frame = 0;
	std::vector<uint8_t> bytes;
};

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out) {
	if (hex.size() % 2 != 0) return false;
	out.clear();
	auto nib = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

std::string ascii_or_hex(const std::vector<uint8_t> &v) {
	bool printable = !v.empty();
	for (size_t i = 0; i < v.size(); ++i) {
		uint8_t c = v[i];
		if (c == 0 && i + 1 == v.size()) continue;  // trailing NUL ok
		if (c < 32 || c > 126) { printable = false; break; }
	}
	if (printable) {
		std::string s(v.begin(), v.end());
		while (!s.empty() && s.back() == '\0') s.pop_back();
		return "\"" + s + "\"";
	}
	std::string h = "hex[";
	char buf[4];
	for (size_t i = 0; i < v.size() && i < 64; ++i) {
		std::snprintf(buf, sizeof(buf), "%02x", v[i]);
		h += buf;
	}
	if (v.size() > 64) h += "...";
	h += "]";
	return h;
}

void dump_container(const NapiMessage &m, int indent) {
	std::string pad(static_cast<size_t>(indent) * 2, ' ');
	std::printf("%s<%s> fields=%zu children=%zu\n", pad.c_str(), m.name.c_str(),
	            m.fields.size(), m.children.size());
	for (const auto &f : m.fields) {
		std::printf("%s  .%s = %s (%zuB)\n", pad.c_str(), f.name.c_str(),
		            ascii_or_hex(f.data).c_str(), f.data.size());
	}
	for (const auto &c : m.children) dump_container(c, indent + 1);
}

// Outer decode: CRC-envelope strip + opcode peel + outer SESSION_NWU. Mirrors
// the anonymous decode_session_inbound() in client_session.cpp exactly. Works
// for BOTH directions because client and server both encode with nwu_decrypt,
// so the inverse (nwu_encrypt) decodes either way.
bool decode_outer(const std::vector<uint8_t> &raw, uint8_t &opcode,
                  std::vector<uint8_t> &body) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out) != 0) {
		return false;
	}
	stripped.resize(out);
	if (stripped.empty()) return false;
	opcode = stripped[0];
	body.assign(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	return true;
}

// Decode a 0x43/0x83 protocol packet's inner containers with the given SCRK.
void dump_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                   const char *label) {
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs)) {
		std::printf("    [%s] decode_protocol_packet_plaintext FAILED\n", label);
		return;
	}
	std::printf("    [%s] hdr session_id=0x%08x seq=%u ack=%u flags=%u  inner_msgs=%zu\n",
	            label, hdr.session_id, hdr.seq_num, hdr.ack_count,
	            hdr.connection_flags, msgs.size());
	for (const auto &pm : msgs) {
		std::printf("      msg flags=0x%02x full_tag=0x%03x len=%u%s%s\n",
		            pm.flags.raw, pm.full_tag, pm.length,
		            pm.flags.settings_update ? " [settings]" : "",
		            pm.flags.frag_cont ? " [frag_cont]" : "");
		if (pm.flags.settings_update || pm.full_tag != 0) continue;
		ProtocolReassemblyState rs;
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(rs, pm, assembled)) continue;
		if (assembled.empty()) continue;
		std::vector<NapiMessage> containers;
		size_t consumed = 0;
		if (napi_stream_decode(assembled.data(), assembled.size(), containers,
		                       &consumed) != 0) {
			std::printf("        napi_stream_decode FAILED (%zuB payload)\n",
			            assembled.size());
			continue;
		}
		for (const auto &c : containers) dump_container(c, 4);
	}
}

} // namespace

int main() {
	const std::string path = std::string(FIXTURE_DIR) + "/nw204_lobby.hexcap";
	std::ifstream file(path);
	TEST_EXPECT(static_cast<bool>(file));

	std::vector<Datagram> dgrams;
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ls(line);
		Datagram d;
		std::string hex;
		ls >> d.dir >> d.frame >> hex;
		TEST_EXPECT(hex_to_bytes(hex, d.bytes));
		dgrams.push_back(std::move(d));
	}
	TEST_EXPECT(dgrams.size() == 11);

	std::string client_scrk, server_scrk;
	bool saw_client_connected = false, saw_verify_req = false,
	     saw_verify_result = false;
	std::vector<std::pair<std::string, std::string>> join_cu;

	for (const auto &d : dgrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.bytes, op, body)) {
			std::printf("frame %d [%s] %zuB: OUTER DECODE FAILED\n", d.frame,
			            d.dir.c_str(), d.bytes.size());
			TEST_EXPECT(false);
			continue;
		}
		std::printf("\n=== frame %d [%s] raw=%zuB op=0x%02x bodylen=%zu ===\n",
		            d.frame, d.dir.c_str(), d.bytes.size(), op, body.size());

		switch (op) {
		case SESSION_OPCODE_CLIENT_HELLO: {
			ClientHello h;
			TEST_EXPECT(parse_client_hello(body.data(), body.size(), h));
			std::printf("  ClientHello NVS=\"%s\" PN=\"%s\" PV1=\"%s\" CI=%u AP=\"%s\"\n",
			            h.nvs.c_str(), h.pn.c_str(), h.pv1.c_str(), h.ci,
			            h.ap.c_str());
			break;
		}
		case SESSION_OPCODE_SERVER_HELLO: {
			ServerHello h;
			TEST_EXPECT(parse_server_hello(body.data(), body.size(), h));
			std::printf("  ServerHello SN=\"%s\" PN=\"%s\" hk=0x%08x rip=0x%08x\n",
			            h.sn.c_str(), h.pn.c_str(), h.hk, h.rip);
			break;
		}
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth a;
			TEST_EXPECT(parse_client_auth(body.data(), body.size(), a));
			client_scrk = a.scrk;
			std::printf("  ClientAuth CI=%u HK=0x%08x CK=0x%08x NA=\"%s\" SCRK=\"%s\"(%zu) cu=%zu\n",
			            a.ci, a.hk, a.ck, a.na.c_str(), a.scrk.c_str(),
			            a.scrk.size(), a.cu.size());
			for (const auto &chunk : a.cu) {
				uint8_t type = 0;
				std::string name, value;
				if (parse_client_cu_chunk(chunk.data(), chunk.size(), type, name,
				                          value)) {
					std::printf("    CU type=%u %s = \"%s\"\n", type, name.c_str(),
					            value.c_str());
					join_cu.emplace_back(name, value);
				} else {
					std::printf("    CU <unparsed %zuB>\n", chunk.size());
				}
			}
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			ServerAuth a;
			TEST_EXPECT(parse_server_auth(body.data(), body.size(), a));
			server_scrk = a.scrk;
			std::printf("  ServerAuth(SessionInit) CI=%u CR=%u SK=0x%08x NA=\"%s\" SCRK=\"%s\"(%zu) cu=%zu\n",
			            a.ci, a.cr, a.sk, a.na.c_str(), a.scrk.c_str(),
			            a.scrk.size(), a.cu.size());
			for (const auto &kv : a.cu)
				std::printf("    CU %s = \"%s\"\n", kv.first.c_str(),
				            kv.second.c_str());
			break;
		}
		case SESSION_OPCODE_PROTOCOL_MESSAGE: {  // 0x43 C2S
			dump_protocol(body, client_scrk, "C->S 0x43");
			// Track which container we saw for assertions.
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> msgs;
			if (decode_protocol_packet_plaintext(body.data(), body.size(),
			                                     client_scrk, hdr, msgs)) {
				for (const auto &pm : msgs) {
					if (pm.flags.settings_update || pm.full_tag != 0) continue;
					ProtocolReassemblyState rs;
					std::vector<uint8_t> asm_;
					if (!reassemble_protocol_payload(rs, pm, asm_) || asm_.empty())
						continue;
					std::vector<NapiMessage> cs;
					size_t cons = 0;
					if (napi_stream_decode(asm_.data(), asm_.size(), cs, &cons) != 0)
						continue;
					for (const auto &c : cs) {
						if (c.name == "ClientConnected") saw_client_connected = true;
						if (c.name == "ClientRequestVerifyResult")
							saw_verify_req = true;
					}
				}
			}
			break;
		}
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE: {  // 0x83 S2C
			dump_protocol(body, server_scrk, "S->C 0x83");
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> msgs;
			if (decode_protocol_packet_plaintext(body.data(), body.size(),
			                                     server_scrk, hdr, msgs)) {
				for (const auto &pm : msgs) {
					if (pm.flags.settings_update || pm.full_tag != 0) continue;
					ProtocolReassemblyState rs;
					std::vector<uint8_t> asm_;
					if (!reassemble_protocol_payload(rs, pm, asm_) || asm_.empty())
						continue;
					std::vector<NapiMessage> cs;
					size_t cons = 0;
					if (napi_stream_decode(asm_.data(), asm_.size(), cs, &cons) != 0)
						continue;
					for (const auto &c : cs)
						if (c.name == "ServerVerifyResult") saw_verify_result = true;
				}
			}
			break;
		}
		default:
			std::printf("  (unhandled opcode)\n");
			break;
		}
	}

	std::printf("\n--- summary ---\n");
	std::printf("client_scrk=\"%s\" (%zu)\n", client_scrk.c_str(),
	            client_scrk.size());
	std::printf("server_scrk=\"%s\" (%zu)\n", server_scrk.c_str(),
	            server_scrk.size());
	std::printf("join CU vars: %zu\n", join_cu.size());

	// Structural ground-truth assertions (the oracle).
	TEST_EXPECT(!client_scrk.empty());
	TEST_EXPECT(!server_scrk.empty());
	TEST_EXPECT(!join_cu.empty());
	TEST_EXPECT(saw_client_connected);
	TEST_EXPECT(saw_verify_req);
	TEST_EXPECT(saw_verify_result);

	std::printf("nw204 lobby decode OK\n");
	return 0;
}
