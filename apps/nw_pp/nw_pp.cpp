// nw_pp — NovaWorld in-game packet pretty-printer.
//
// Reads a hexcap (the format `tools/net/pcap_to_hexcap.py` produces, also
// the env-var input for `nw_ingame_histogram_test`) and emits one line per
// outer datagram plus one structured block per inner protocol message. Drives
// the SAME outer-decode pipeline as `nw_ingame_histogram_test` and
// `nw_ingame_pool_records_test` — envelope CRC → outer NWU → per-session
// SCRK → 0x43/0x83 → reassembly — so what it prints is the exact byte stream
// the shipping libs see, not a parallel re-implementation.
//
// Pretty-print depth per tag:
//   S 0x0D  pool-entity spawn batch       — full §5.11 field map per record
//   S 0x20  bulk pool-3 entity sync       — full §5.12 field map per record
//   any other tag                         — header + hex sample (truncated)
//
// As we RE more tags we add their decoders here, alongside §5.x growth.
//
// CLI:
//   nw_pp <hexcap-path>                 # all frames
//   nw_pp <hexcap-path> 0x0d 0x20       # filter to listed S2C tags
//   NW_INGAME_HEXCAP=<path> nw_pp       # env-driven (matches the test convention)

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
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

std::string to_hex_sample(const uint8_t *p, size_t n, size_t cap = 48) {
	std::string s;
	char buf[4];
	for (size_t i = 0; i < n && i < cap; ++i) {
		std::snprintf(buf, sizeof(buf), "%02x", p[i]);
		s += buf;
		if (i + 1 < n && i + 1 < cap) s += ' ';
	}
	if (n > cap) s += " ..";
	return s;
}

// 16.16 fixed-point → decimal world units.
double fp16(int32_t v) { return double(v) / 65536.0; }

struct Cursor {
	const uint8_t *p = nullptr;
	const uint8_t *end = nullptr;
	bool ok = true;

	size_t pos(const uint8_t *base) const { return size_t(p - base); }
	size_t remaining() const { return ok ? size_t(end - p) : 0; }

	uint8_t u8() {
		if (!ok || p + 1 > end) { ok = false; return 0; }
		return *p++;
	}
	uint16_t u16() {
		if (!ok || p + 2 > end) { ok = false; return 0; }
		uint16_t v = uint16_t(p[0]) | uint16_t(p[1]) << 8; p += 2; return v;
	}
	uint32_t u32() {
		if (!ok || p + 4 > end) { ok = false; return 0; }
		uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
		             uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
		p += 4; return v;
	}
	std::string cstr() {
		std::string s;
		while (ok && p < end) {
			uint8_t c = *p++;
			if (c == 0) return s;
			s.push_back(char(c));
		}
		ok = false;
		return s;
	}
};

std::string handle_str(uint16_t h) {
	char buf[24];
	if (h == 0xFFFF) std::snprintf(buf, sizeof(buf), "0xFFFF=none");
	else std::snprintf(buf, sizeof(buf), "0x%04x p%u/s%u", h,
	                   unsigned(h >> 12), unsigned(h & 0xFFF));
	return buf;
}

// Decode + print one S2C 0x0D payload per the §5.11 field map.
void print_tag_0d(const std::vector<uint8_t> &body) {
	Cursor c{body.data(), body.data() + body.size(), true};
	const uint8_t *base = body.data();
	int16_t entityCount = int16_t(c.u16());
	std::printf("        [0x0D] entityCount=%d (body %zu B)\n",
	            int(entityCount), body.size());
	if (entityCount <= 0) return;
	for (int i = 0; i < entityCount; ++i) {
		const size_t rec_off = c.pos(base);
		const uint16_t flags = c.u16();
		const uint16_t slot = c.u16();
		if (!c.ok) {
			std::printf("        record %d @+%zu TRUNCATED (header)\n",
			            i, rec_off);
			return;
		}
		if (slot == 0xFFFF || (slot & 0xF000) >= 0x5000) {
			std::printf("        record %d @+%zu sentinel slot=%s "
			            "(end-of-batch)\n", i, rec_off, handle_str(slot).c_str());
			return;
		}
		const uint16_t type = c.u16();
		std::string name = c.cstr();
		uint32_t entity36 = 0;
		if (flags & 0x0020) entity36 = c.u32();
		const int32_t px = int32_t(c.u32());
		const int32_t py = int32_t(c.u32());
		const int32_t pz = int32_t(c.u32());
		if (!c.ok) {
			std::printf("        record %d @+%zu TRUNCATED (position)\n",
			            i, rec_off);
			return;
		}
		std::printf("        record %d @+%zu flags=0x%04x slot=%s type=0x%04x "
		            "name=%-12s pos=(%.1f, %.1f, %.1f)",
		            i, rec_off, flags, handle_str(slot).c_str(), type,
		            ("\"" + name + "\"").c_str(), fp16(px), fp16(py), fp16(pz));
		if (flags & 0x0020) std::printf(" entity36=0x%08x", entity36);
		if (flags & 0x0001) { (void)c.u32(); std::printf(" velX"); }
		if (flags & 0x0002) { (void)c.u32(); std::printf(" velY"); }
		if (flags & 0x0004) { (void)c.u32(); std::printf(" velZ"); }
		if (flags & 0x0008) { (void)c.u32(); std::printf(" sectionMask"); }
		if (flags & 0x0010) { std::printf(" orient=0x%02x", c.u8()); }
		if (flags & 0x0100) { std::printf(" parent=%s", handle_str(c.u16()).c_str()); }
		if (flags & 0x0200) { std::printf(" target=%s", handle_str(c.u16()).c_str()); }
		if (flags & 0x0400) {
			const uint8_t mask = c.u8();
			std::printf(" weapMask=0x%02x", mask);
			if (mask) {
				int wcount = 0;
				for (int b = 0; b < 8; ++b) if (mask & (1u << b)) {
					(void)c.u16(); wcount++;
				}
				(void)c.u16(); (void)c.u16(); // extraHandle0/1
				std::printf(" weapons=%d+2extra", wcount);
			}
		}
		const uint8_t teamByte = c.u8(); std::printf(" team=0x%02x", teamByte);
		if (flags & 0x0800) {
			const uint32_t aiP1 = c.u32();
			const uint32_t aiP2 = c.u32();
			std::string aiName = c.cstr();
			std::printf(" AItrailer{p1=0x%08x p2=0x%08x name=\"%s\"}",
			            aiP1, aiP2, aiName.c_str());
		}
		if (flags & 0x0040) std::printf(" alert=0x%02x", c.u8());
		if (flags & 0x0080) std::printf(" action=0x%02x", c.u8());
		if (flags & 0x1000) std::printf(" weapType=0x%02x", c.u8());
		if (flags & 0x2000) {
			(void)c.u8(); (void)c.u16(); std::printf(" health[+s]");
		} else if (flags & 0x8000) {
			(void)c.u16(); std::printf(" healthShort");
		}
		if (flags & 0x4000) std::printf(" diff=0x%02x", c.u8());
		std::printf("\n");
		if (!c.ok) {
			std::printf("        record %d truncated mid-field\n", i);
			return;
		}
	}
	if (c.remaining() > 0)
		std::printf("        WARNING: %zu trailing bytes\n", c.remaining());
}

// Decode + print one S2C 0x20 payload per the §5.12 field map.
void print_tag_20(const std::vector<uint8_t> &body) {
	Cursor c{body.data(), body.data() + body.size(), true};
	const uint8_t *base = body.data();
	const uint16_t startIdx = c.u16();
	const int16_t entityCount = int16_t(c.u16());
	std::printf("        [0x20] startIdx=%u entityCount=%d (body %zu B)\n",
	            unsigned(startIdx), int(entityCount), body.size());
	if (entityCount <= 0) return;
	for (int i = 0; i < entityCount; ++i) {
		const size_t rec_off = c.pos(base);
		const uint16_t type = c.u16();
		if (!c.ok) {
			std::printf("        slot %u TRUNCATED (type)\n",
			            unsigned(startIdx) + i);
			return;
		}
		if (type == 0) {
			std::printf("        slot %u @+%zu type=0 (empty-slot sentinel)\n",
			            unsigned(startIdx) + i, rec_off);
			continue;
		}
		const uint8_t flags = c.u8();
		const int32_t px = int32_t(c.u32());
		const int32_t py = int32_t(c.u32());
		const int32_t pz = int32_t(c.u32());
		if (!c.ok) {
			std::printf("        slot %u @+%zu TRUNCATED (position)\n",
			            unsigned(startIdx) + i, rec_off);
			return;
		}
		std::printf("        slot %u @+%zu type=0x%04x flags=0x%02x "
		            "pos=(%.1f, %.1f, %.1f)",
		            unsigned(startIdx) + i, rec_off, type, flags,
		            fp16(px), fp16(py), fp16(pz));
		if (flags & 0x01) std::printf(" parent=0x%08x", c.u32());
		if (flags & 0x02) std::printf(" orient=0x%08x", c.u32());
		if (flags & 0x04) std::printf(" ammo=%u", unsigned(c.u16()));
		std::printf(" net=%s", handle_str(c.u16()).c_str());
		if (flags & 0x08) std::printf(" team=0x%02x", c.u8());
		if (flags & 0x10) std::printf(" weapType=0x%04x", c.u16());
		if (flags & 0x20) std::printf(" score=0x%02x", c.u8());
		std::printf("\n");
		if (!c.ok) {
			std::printf("        slot %u truncated mid-field\n",
			            unsigned(startIdx) + i);
			return;
		}
	}
	if (c.remaining() > 0)
		std::printf("        WARNING: %zu trailing bytes\n", c.remaining());
}

struct DirState {
	ProtocolReassemblyState rs;
	bool have_pending = false;
	int pending_tag = 0;
	int pending_first_frame = 0;
};

void print_payload(char dir, int frame, int tag,
                   const std::vector<uint8_t> &payload) {
	std::printf("[%c f=%-4d tag=0x%02x len=%zu]\n", dir, frame, tag,
	            payload.size());
	if (dir == 'S' && tag == 0x0D) print_tag_0d(payload);
	else if (dir == 'S' && tag == 0x20) print_tag_20(payload);
	else if (!payload.empty()) std::printf("        %s\n",
	                                       to_hex_sample(payload.data(),
	                                                     payload.size()).c_str());
}

void process_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                      char dir, DirState &st, int frame,
                      const std::set<int> &tag_filter) {
	if (scrk.empty()) return;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs))
		return;
	for (const auto &pm : msgs) {
		int tag = int(pm.full_tag);
		if (pm.flags.settings_update) tag |= 0x1000;
		if (!st.have_pending) {
			st.pending_tag = tag;
			st.pending_first_frame = frame;
			st.have_pending = true;
		}
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(st.rs, pm, assembled)) continue;
		if (tag_filter.empty() ||
		    tag_filter.count(st.pending_tag & 0xFF))
			print_payload(dir, st.pending_first_frame, st.pending_tag,
			              assembled);
		st.have_pending = false;
	}
}

} // namespace

int main(int argc, char *argv[]) {
	const char *path = nullptr;
	std::set<int> tag_filter;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
			tag_filter.insert(int(std::strtol(a, nullptr, 16)));
		} else if (!path) {
			path = a;
		}
	}
	if (!path) path = std::getenv("NW_INGAME_HEXCAP");
	if (!path || !*path) {
		std::fprintf(stderr,
		             "usage: nw_pp <hexcap> [0xNN ...]\n"
		             "       or set NW_INGAME_HEXCAP\n");
		return 1;
	}
	std::ifstream file(path);
	if (!file) {
		std::fprintf(stderr, "FAILED to open %s\n", path);
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
	std::fprintf(stderr, "loaded %zu datagrams from %s\n", dgrams.size(), path);
	if (!tag_filter.empty()) {
		std::fprintf(stderr, "tag filter:");
		for (int t : tag_filter) std::fprintf(stderr, " 0x%02x", t);
		std::fprintf(stderr, "\n");
	}

	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
	for (const auto &d : dgrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.bytes, op, body)) continue;
		switch (op) {
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
		case SESSION_OPCODE_PROTOCOL_MESSAGE:
			process_protocol(body, client_scrk, 'C', cstate, d.frame,
			                 tag_filter);
			break;
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			process_protocol(body, server_scrk, 'S', sstate, d.frame,
			                 tag_filter);
			break;
		default:
			break;
		}
	}
	return 0;
}
