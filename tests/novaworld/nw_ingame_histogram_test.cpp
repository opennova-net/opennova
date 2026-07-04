// In-game JOINTOPERATIONS protocol surveyor.
//
// Decodes a loopback capture of a real host+join+play session (host and joiner
// both on 192.168.10.120; in-game session :32768 host <-> :32769 joiner) and
// builds a per-(direction, msg_id) histogram of the in-match NAPI messages so
// the high-volume tags can be reverse-engineered field-for-field against the
// §4 dispatch tables in docs/net/novaworld-net-re.md.
//
// It runs entirely through the SAME libs the client uses (napi envelope/tlv,
// novacrypto nwu, novaworld session/protocol parsers) — no re-implemented
// crypto — so what it prints is ground truth, not a guess. Mirrors the decode
// pipeline of nw204_lobby_decode_test.cpp; the only new code is the streaming
// aggregation.
//
// Asset-gated: reads the hexcap path from env NW_INGAME_HEXCAP (one datagram
// per line: "<srcport> <frame> <udp_payload_hex>"). Skips cleanly when unset so
// CI stays green until a sanitized fixture slice is committed.

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;

namespace {

struct Datagram {
	int srcport = 0;
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

std::string to_hex(const uint8_t *p, size_t n, size_t cap = 96) {
	std::string h;
	char buf[4];
	for (size_t i = 0; i < n && i < cap; ++i) {
		std::snprintf(buf, sizeof(buf), "%02x", p[i]);
		h += buf;
	}
	if (n > cap) h += "..";
	return h;
}

// Outer decode: CRC-envelope strip + opcode peel + outer SESSION_NWU. Identical
// to nw204_lobby_decode_test's decode_outer(). Returns false when the bytes are
// not a valid NAPI envelope (CRC mismatch / too short) — those get bucketed as
// "raw / non-envelope" for separate analysis.
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

// Per (direction, msg_id) aggregate.
struct Stat {
	uint64_t count = 0;
	uint64_t total_bytes = 0;
	std::map<size_t, uint64_t> lens;   // payload length -> occurrences
	std::vector<std::string> samples;  // up to 4 representative payloads
	int first_frame = -1;
	int last_frame = -1;
};

// Per-direction reassembly + the tag of the in-flight (possibly fragmented)
// message so completed payloads aggregate under the tag of their FIRST chunk.
struct DirState {
	ProtocolReassemblyState rs;
	bool have_pending = false;
	int pending_tag = 0;
	int pending_first_frame = 0;
};

std::map<std::pair<char, int>, Stat> g_hist;  // key = (dir 'C'/'S', tag)
int g_decode_fail = 0;
int g_no_scrk = 0;

void record(char dir, int tag, const std::vector<uint8_t> &payload, int frame) {
	Stat &s = g_hist[{dir, tag}];
	s.count++;
	s.total_bytes += payload.size();
	s.lens[payload.size()]++;
	if (s.first_frame < 0) s.first_frame = frame;
	s.last_frame = frame;
	if (s.samples.size() < 4)
		s.samples.push_back(to_hex(payload.data(), payload.size()));
}

// Decode one 0x43/0x83 packet body (already outer-decrypted) and dispatch each
// inner message by full_tag, reassembling fragments across packets.
void process_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                      char dir, DirState &st, int frame) {
	if (scrk.empty()) { g_no_scrk++; return; }
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs)) {
		g_decode_fail++;
		return;
	}
	for (const auto &pm : msgs) {
		// Settings packets (flag 0x80) dispatch through the high table; mark
		// them so they bucket apart from the same low tag.
		int tag = static_cast<int>(pm.full_tag);
		if (pm.flags.settings_update) tag |= 0x1000;
		if (!st.have_pending) {
			st.pending_tag = tag;
			st.pending_first_frame = frame;
			st.have_pending = true;
		}
		std::vector<uint8_t> assembled;
		if (reassemble_protocol_payload(st.rs, pm, assembled)) {
			record(dir, st.pending_tag, assembled, st.pending_first_frame);
			st.have_pending = false;
		}
	}
}

size_t modal_len(const Stat &s) {
	size_t best = 0;
	uint64_t best_n = 0;
	for (const auto &kv : s.lens)
		if (kv.second > best_n) { best_n = kv.second; best = kv.first; }
	return best;
}

} // namespace

int main() {
	const char *path = std::getenv("NW_INGAME_HEXCAP");
	if (!path || !*path) {
		std::printf("[skip] set NW_INGAME_HEXCAP to a '<srcport> <frame> <hex>' "
		            "capture to run the in-game surveyor\n");
		return 0;
	}
	std::ifstream file(path);
	if (!file) {
		std::printf("FAILED to open NW_INGAME_HEXCAP=%s\n", path);
		return 1;
	}

	std::vector<Datagram> dgrams;
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ls(line);
		Datagram d;
		std::string hex;
		if (!(ls >> d.srcport >> d.frame >> hex)) continue;
		if (!hex_to_bytes(hex, d.bytes)) continue;
		dgrams.push_back(std::move(d));
	}
	std::printf("loaded %zu datagrams from %s\n\n", dgrams.size(), path);

	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
	uint64_t opcode_counts[256] = {0};
	int envelope_ok = 0, raw_count = 0;
	std::set<std::string> hello_pn;

	// Raw (non-envelope) bucket — keyed by srcport, with length histogram.
	std::map<int, Stat> raw_by_port;

	bool printed_first_c = false, printed_first_s = false;

	for (const auto &d : dgrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.bytes, op, body)) {
			raw_count++;
			Stat &s = raw_by_port[d.srcport];
			s.count++;
			s.total_bytes += d.bytes.size();
			s.lens[d.bytes.size()]++;
			if (s.first_frame < 0) s.first_frame = d.frame;
			s.last_frame = d.frame;
			if (s.samples.size() < 6)
				s.samples.push_back(to_hex(d.bytes.data(), d.bytes.size()));
			continue;
		}
		envelope_ok++;
		opcode_counts[op]++;

		switch (op) {
		case SESSION_OPCODE_CLIENT_HELLO: {
			ClientHello h;
			if (parse_client_hello(body.data(), body.size(), h))
				hello_pn.insert("C:" + h.pn + " ap=" + h.ap + " pv1=" + h.pv1);
			break;
		}
		case SESSION_OPCODE_SERVER_HELLO: {
			ServerHello h;
			if (parse_server_hello(body.data(), body.size(), h))
				hello_pn.insert("S:" + h.pn + " sn=" + h.sn);
			break;
		}
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth a;
			if (parse_client_auth(body.data(), body.size(), a))
				client_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			ServerAuth a;
			if (parse_server_auth(body.data(), body.size(), a))
				server_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_PROTOCOL_MESSAGE: {  // 0x43 C2S (joiner -> host)
			if (!printed_first_c && !client_scrk.empty()) {
				ProtocolPacketHeader hdr;
				std::vector<ProtocolMessage> m;
				if (decode_protocol_packet_plaintext(body.data(), body.size(),
				                                     client_scrk, hdr, m)) {
					std::printf("first C2S 0x43 hdr: session_id=0x%08x seq=%u "
					            "ack=%u inner=%zu\n",
					            hdr.session_id, hdr.seq_num, hdr.ack_count,
					            m.size());
					printed_first_c = true;
				}
			}
			process_protocol(body, client_scrk, 'C', cstate, d.frame);
			break;
		}
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE: {  // 0x83 S2C (host -> joiner)
			if (!printed_first_s && !server_scrk.empty()) {
				ProtocolPacketHeader hdr;
				std::vector<ProtocolMessage> m;
				if (decode_protocol_packet_plaintext(body.data(), body.size(),
				                                     server_scrk, hdr, m)) {
					std::printf("first S2C 0x83 hdr: session_id=0x%08x seq=%u "
					            "ack=%u inner=%zu\n",
					            hdr.session_id, hdr.seq_num, hdr.ack_count,
					            m.size());
					printed_first_s = true;
				}
			}
			process_protocol(body, server_scrk, 'S', sstate, d.frame);
			break;
		}
		default:
			break;
		}
	}

	std::printf("\n=== envelope summary ===\n");
	std::printf("envelope-OK=%d  raw/non-envelope=%d\n", envelope_ok, raw_count);
	std::printf("opcodes:");
	for (int i = 0; i < 256; ++i)
		if (opcode_counts[i])
			std::printf(" 0x%02x=%llu", i,
			            (unsigned long long)opcode_counts[i]);
	std::printf("\nprotocol decode failures=%d  packets-before-scrk=%d\n",
	            g_decode_fail, g_no_scrk);

	std::printf("\n=== hellos (PN selects the message set) ===\n");
	for (const auto &s : hello_pn) std::printf("  %s\n", s.c_str());
	std::printf("client_scrk=\"%s\" (%zu)\n", client_scrk.c_str(),
	            client_scrk.size());
	std::printf("server_scrk=\"%s\" (%zu)\n", server_scrk.c_str(),
	            server_scrk.size());

	// In-game message histogram, sorted by total bytes then count.
	std::vector<std::pair<std::pair<char, int>, Stat>> rows(g_hist.begin(),
	                                                         g_hist.end());
	std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
		if (a.second.total_bytes != b.second.total_bytes)
			return a.second.total_bytes > b.second.total_bytes;
		return a.second.count > b.second.count;
	});
	std::printf("\n=== in-game message histogram (by total bytes) ===\n");
	std::printf("%-4s %-8s %7s %10s %6s %8s  %s\n", "dir", "msg_id", "count",
	            "totbytes", "modal", "frames", "distinct_lens");
	for (const auto &r : rows) {
		const char dir = r.first.first;
		const int tag = r.first.second;
		const Stat &s = r.second;
		std::string tagstr;
		char buf[32];
		if (tag & 0x1000)
			std::snprintf(buf, sizeof(buf), "0x%02x[set]", tag & 0xFF);
		else
			std::snprintf(buf, sizeof(buf), "0x%03x", tag);
		tagstr = buf;
		std::string lens;
		int shown = 0;
		for (const auto &kv : s.lens) {
			if (shown++ >= 8) { lens += "..."; break; }
			char lb[24];
			std::snprintf(lb, sizeof(lb), "%zu(%llu) ", kv.first,
			              (unsigned long long)kv.second);
			lens += lb;
		}
		std::printf("%-4c %-8s %7llu %10llu %6zu %4d-%-4d %s\n", dir,
		            tagstr.c_str(), (unsigned long long)s.count,
		            (unsigned long long)s.total_bytes, modal_len(s),
		            s.first_frame, s.last_frame, lens.c_str());
	}

	std::printf("\n=== representative samples per (dir,msg_id) ===\n");
	for (const auto &r : rows) {
		const Stat &s = r.second;
		char buf[32];
		if (r.first.second & 0x1000)
			std::snprintf(buf, sizeof(buf), "0x%02x[set]", r.first.second & 0xFF);
		else
			std::snprintf(buf, sizeof(buf), "0x%03x", r.first.second);
		std::printf("  %c %s:\n", r.first.first, buf);
		for (const auto &sample : s.samples)
			std::printf("      %s\n", sample.c_str());
	}

	std::printf("\n=== raw / non-envelope datagrams (separate framing?) ===\n");
	for (const auto &kv : raw_by_port) {
		const Stat &s = kv.second;
		std::printf("  srcport %d: %llu pkts, %llu bytes, frames %d-%d, lens:",
		            kv.first, (unsigned long long)s.count,
		            (unsigned long long)s.total_bytes, s.first_frame,
		            s.last_frame);
		int shown = 0;
		for (const auto &lk : s.lens) {
			if (shown++ >= 10) { std::printf(" ..."); break; }
			std::printf(" %zu(%llu)", lk.first, (unsigned long long)lk.second);
		}
		std::printf("\n");
		for (const auto &sample : s.samples)
			std::printf("      %s\n", sample.c_str());
	}

	return 0;
}
