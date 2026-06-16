// nw_pp — NovaWorld in-game packet pretty-printer.
//
// Reads a pcap/pcapng OR a hexcap (the format `tools/net/pcap_to_hexcap.py`
// produces; the env-var input for `nw_ingame_histogram_test`) and emits one
// line per outer datagram plus one structured block per inner protocol
// message. Drives the SAME outer-decode pipeline as `nw_ingame_histogram_test`
// and `nw_ingame_pool_records_test` — envelope CRC → outer NWU → per-session
// SCRK → 0x43/0x83 → reassembly — so what it prints is the exact byte stream
// the shipping libs see, not a parallel re-implementation.
//
// Input format is auto-detected from the path suffix: `.pcap` / `.pcapng`
// are parsed natively (no Wireshark / tshark required) for the loopback-UDP
// subset of the link-layer space — Ethernet, BSD-loopback (NULL/LOOP),
// raw-IP, IPv4 only. IP fragmentation is not reassembled (loopback MTU is
// 65535 so we don't see it in practice; fragments are dropped with a
// stderr warning). Anything else is read as hexcap text.
//
// Tag-specific decoders live in `libs/novaworld/include/novaworld/ingame_decode.h`
// (shared with `nw_ingame_pool_records_test` and the future real handlers).
// As new tags get field maps in docs/net/novaworld-net-re.md, their decoders
// land there and a printer for them lands here.
//
// CLI:
//   nw_pp <capture-path>                # pcapng/pcap/hexcap all accepted
//   nw_pp <capture-path> 0x0d 0x20      # filter to listed S2C tags
//   NW_INGAME_HEXCAP=<hexcap> nw_pp     # env-driven, hexcap only (test contract)

#include <def/def.h>
#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/ingame_decode.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <scr/scr.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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

bool ends_with_icase(const std::string &s, const char *suffix) {
	const size_t sl = std::strlen(suffix);
	if (s.size() < sl) return false;
	for (size_t i = 0; i < sl; ++i) {
		const char a = std::tolower(static_cast<unsigned char>(s[s.size() - sl + i]));
		const char b = std::tolower(static_cast<unsigned char>(suffix[i]));
		if (a != b) return false;
	}
	return true;
}

bool is_pcap_path(const std::string &p) {
	return ends_with_icase(p, ".pcap") || ends_with_icase(p, ".pcapng");
}

// Native pcap / pcapng reader for the loopback-UDP slice we care about.
//
// Spec sources:
//   pcap legacy   — wireshark.org/docs/man-pages/pcap-savefile.html
//   pcapng        — github.com/pcapng/pcapng (PCAP Next Generation block
//                   format spec). We only need Section Header Block (SHB,
//                   type 0x0A0D0D0A), Interface Description Block (IDB,
//                   type 0x00000001), and Enhanced Packet Block (EPB,
//                   type 0x00000006). Simple Packet Blocks (0x3) and
//                   legacy Packet Blocks (0x2) are tolerated.
//   linktype enum — tcpdump.org/linktypes.html
//
// We strip the data-link header to expose the IPv4 header, parse the UDP
// header out of that, and emit (srcport, frame_index, udp_payload). IP
// fragments are dropped (loopback MTU = 65535 so they're not expected).

namespace pcap_io {

constexpr uint32_t PCAP_MAGIC_LE   = 0xa1b2c3d4u;
constexpr uint32_t PCAP_MAGIC_BE   = 0xd4c3b2a1u;
constexpr uint32_t PCAP_MAGIC_NSEC_LE = 0xa1b23c4du;  // nanosecond ts
constexpr uint32_t PCAP_MAGIC_NSEC_BE = 0x4d3cb2a1u;

constexpr uint32_t PCAPNG_BLOCK_SHB = 0x0a0d0d0au;
constexpr uint32_t PCAPNG_BLOCK_IDB = 0x00000001u;
constexpr uint32_t PCAPNG_BLOCK_EPB = 0x00000006u;
constexpr uint32_t PCAPNG_BLOCK_SPB = 0x00000003u;
constexpr uint32_t PCAPNG_BLOCK_PB  = 0x00000002u;

constexpr uint32_t LINKTYPE_NULL     = 0;    // BSD loopback: 4-byte AF_xxx (host order)
constexpr uint32_t LINKTYPE_ETHERNET = 1;    // 14-byte Ethernet II header
constexpr uint32_t LINKTYPE_RAW      = 101;  // raw IP
constexpr uint32_t LINKTYPE_LOOP     = 108;  // OpenBSD loopback: 4-byte AF_xxx (BE)
constexpr uint32_t LINKTYPE_IPV4     = 228;  // raw IPv4

constexpr uint16_t AF_INET_LINUX = 2;        // most ports
constexpr uint16_t AF_INET_DARWIN = 2;       // same value on macOS

uint16_t read_u16_le(const uint8_t *p) {
	return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t read_u32_le(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
	       uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t read_u32_be(const uint8_t *p) {
	return uint32_t(p[3]) | uint32_t(p[2]) << 8 |
	       uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
}
uint16_t read_u16_be(const uint8_t *p) {
	return uint16_t(p[1]) | uint16_t(p[0]) << 8;
}

// Peel the data-link header. Returns the offset into `frame` where the
// IPv4 header starts, or -1 if not IPv4 / unsupported linktype.
int strip_link_header(uint32_t linktype, const uint8_t *frame, size_t len) {
	switch (linktype) {
	case LINKTYPE_NULL:
	case LINKTYPE_LOOP: {
		if (len < 4) return -1;
		// NULL: host byte order; LOOP: big-endian. The AF value is small
		// (2 = AF_INET) so we can detect by checking both orders.
		uint32_t af_le = read_u32_le(frame);
		uint32_t af_be = read_u32_be(frame);
		uint32_t af = (af_le < 256) ? af_le : af_be;
		if (af != AF_INET_LINUX) return -1;
		return 4;
	}
	case LINKTYPE_ETHERNET: {
		if (len < 14) return -1;
		const uint16_t ethertype = read_u16_be(frame + 12);
		if (ethertype != 0x0800) return -1;  // not IPv4
		return 14;
	}
	case LINKTYPE_RAW:
	case LINKTYPE_IPV4:
		return 0;
	default:
		return -1;
	}
}

// Read the whole file into a buffer (capture files are typically <100 MiB
// and fit easily; the alternative would be streaming, which complicates
// pcapng block traversal).
bool slurp(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamsize n = f.tellg();
	if (n < 0) return false;
	out.resize(size_t(n));
	f.seekg(0);
	f.read(reinterpret_cast<char *>(out.data()), n);
	return f.good() || f.eof();
}

// Per-extracted-UDP-datagram callback. (srcport, frame_index, payload).
using UdpCb = std::function<void(int, int, const uint8_t *, size_t)>;

void extract_ipv4_udp(const uint8_t *pkt, size_t len, uint32_t linktype,
                       int frame_index, UdpCb &cb,
                       int &fragments_dropped) {
	const int ip_off = strip_link_header(linktype, pkt, len);
	if (ip_off < 0 || size_t(ip_off) + 20 > len) return;
	const uint8_t *ip = pkt + ip_off;
	// IPv4: low nibble of byte 0 is IHL in 32-bit words.
	if ((ip[0] >> 4) != 4) return;
	const size_t ihl = size_t(ip[0] & 0x0F) * 4;
	if (ihl < 20 || size_t(ip_off) + ihl > len) return;
	if (ip[9] != 17) return;  // not UDP
	// IP fragmentation: drop if fragment offset != 0 or MF flag set.
	const uint16_t flags_frag = read_u16_be(ip + 6);
	const uint16_t frag_offset = flags_frag & 0x1FFF;
	const bool mf = (flags_frag & 0x2000) != 0;
	if (frag_offset != 0 || mf) { fragments_dropped++; return; }
	const uint16_t ip_total_len = read_u16_be(ip + 2);
	const size_t udp_off = size_t(ip_off) + ihl;
	if (udp_off + 8 > len) return;
	const uint8_t *udp = pkt + udp_off;
	const uint16_t srcport = read_u16_be(udp + 0);
	const uint16_t udp_len = read_u16_be(udp + 4);
	if (udp_len < 8) return;
	// Trust UDP length over captured length when it's plausible (truncates
	// trailing pad bytes some link layers add).
	const size_t payload_off = udp_off + 8;
	size_t payload_len = udp_len - 8;
	const size_t cap_avail = (ip_off + ip_total_len <= int(len))
	                          ? size_t(ip_off + ip_total_len - int(udp_off + 8))
	                          : (len - payload_off);
	if (payload_len > cap_avail) payload_len = cap_avail;
	if (payload_off + payload_len > len) return;
	cb(int(srcport), frame_index, pkt + payload_off, payload_len);
}

bool load(const std::string &path, UdpCb cb) {
	std::vector<uint8_t> buf;
	if (!slurp(path, buf)) {
		std::fprintf(stderr, "FAILED to read %s\n", path.c_str());
		return false;
	}
	if (buf.size() < 24) return false;

	const uint32_t magic_le = read_u32_le(buf.data());
	int fragments_dropped = 0;
	int frame_index = 0;

	// Legacy pcap?
	if (magic_le == PCAP_MAGIC_LE || magic_le == PCAP_MAGIC_BE ||
	    magic_le == PCAP_MAGIC_NSEC_LE || magic_le == PCAP_MAGIC_NSEC_BE) {
		const bool be = (magic_le == PCAP_MAGIC_BE ||
		                 magic_le == PCAP_MAGIC_NSEC_BE);
		auto r32 = [&](const uint8_t *p) {
			return be ? read_u32_be(p) : read_u32_le(p);
		};
		const uint32_t linktype = r32(buf.data() + 20);
		size_t off = 24;
		while (off + 16 <= buf.size()) {
			const uint32_t incl_len = r32(buf.data() + off + 8);
			if (off + 16 + incl_len > buf.size()) break;
			frame_index++;
			extract_ipv4_udp(buf.data() + off + 16, incl_len, linktype,
			                 frame_index, cb, fragments_dropped);
			off += 16 + incl_len;
		}
	} else if (read_u32_le(buf.data()) == PCAPNG_BLOCK_SHB) {
		// pcapng. Byte-order magic in the SHB body tells us endianness.
		// We support little-endian captures (the dominant case) — big-
		// endian would mirror but we haven't seen one in practice.
		if (buf.size() < 28) return false;
		const uint32_t bom = read_u32_le(buf.data() + 8);
		if (bom != 0x1A2B3C4Du) {
			std::fprintf(stderr, "big-endian pcapng not supported\n");
			return false;
		}
		uint32_t cur_linktype = LINKTYPE_ETHERNET;
		size_t off = 0;
		while (off + 8 <= buf.size()) {
			const uint32_t btype = read_u32_le(buf.data() + off);
			const uint32_t blen = read_u32_le(buf.data() + off + 4);
			if (blen < 12 || off + blen > buf.size()) break;
			const uint8_t *body = buf.data() + off + 8;
			const size_t body_len = blen - 12;  // minus type+len*2
			if (btype == PCAPNG_BLOCK_IDB && body_len >= 8) {
				cur_linktype = read_u32_le(body) & 0xFFFF;
			} else if (btype == PCAPNG_BLOCK_EPB && body_len >= 20) {
				// EPB: interface_id(4) ts_high(4) ts_low(4) cap_len(4)
				//      pkt_len(4) data(cap_len, padded to 4) options
				const uint32_t cap_len = read_u32_le(body + 12);
				if (20 + cap_len <= body_len) {
					frame_index++;
					extract_ipv4_udp(body + 20, cap_len, cur_linktype,
					                 frame_index, cb, fragments_dropped);
				}
			} else if (btype == PCAPNG_BLOCK_SPB && body_len >= 4) {
				const uint32_t pkt_len = read_u32_le(body);
				if (4 + pkt_len <= body_len) {
					frame_index++;
					extract_ipv4_udp(body + 4, pkt_len, cur_linktype,
					                 frame_index, cb, fragments_dropped);
				}
			}
			off += blen;
		}
	} else {
		std::fprintf(stderr, "%s: not a pcap or pcapng file\n", path.c_str());
		return false;
	}

	if (fragments_dropped > 0)
		std::fprintf(stderr, "warning: dropped %d IP-fragment packet(s)\n",
		             fragments_dropped);
	return true;
}

} // namespace pcap_io

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

double fp16(int32_t v) { return double(v) / 65536.0; }

// Pool taxonomy from docs/engine-primer.md + docs/world/world-wac-ai-re.md:
// 0=organics (player + dynamic spawned units), 1=items (vehicles, spawn
// points, props), 2=buildings/static, 3=markers (waypoints, nav, objectives).
const char *pool_label(unsigned p) {
	switch (p) {
		case 0: return "organics";
		case 1: return "items";
		case 2: return "buildings";
		case 3: return "markers";
		default: return "?";
	}
}

std::string handle_str(uint16_t h) {
	char buf[40];
	if (h == 0xFFFF) std::snprintf(buf, sizeof(buf), "0xFFFF=none");
	else std::snprintf(buf, sizeof(buf), "0x%04x p%u(%s)/s%u", h,
	                   unsigned(h >> 12), pool_label(unsigned(h >> 12)),
	                   unsigned(h & 0xFFF));
	return buf;
}

// Tag labels from docs/net/novaworld-net-re.md §4 dispatch tables. Short
// human names just to orient the reader of nw_pp output; not exhaustive,
// just the tags we've seen flow in real captures.
const char *tag_label(char dir, int tag) {
	if (dir == 'S') {
		switch (tag) {
			case 0x00: return "init";
			case 0x02: return "post-handshake";
			case 0x0A: return "per-frame-update";       // §5.9
			case 0x0B: return "BMS-header";              // §5.4
			case 0x0C: return "entity-spawn-batch";
			case 0x0D: return "pool-spawn";              // §5.11
			case 0x0F: return "world-state-load";
			case 0x10: return "static-entity-batch";    // §5.9
			case 0x16: return "player-list";
			case 0x1A: return "wait-for-game-start-ack";
			case 0x1D: return "spawn-success-gate";      // §5.2
			case 0x1E: return "game-event";
			case 0x20: return "pool3-sync";              // §5.12
			case 0x26: return "kill-sync";
			case 0x40: return "capture-zone-state";
			case 0x45: return "terrain-load";
			case 0x46: return "player-sync";
			case 0x4E: return "kill-by-slot";
			case 0x57: return "rtt-echo";
			case 0x5A: return "weapon-loadout";
			case 0x60: return "file-chunk";
			case 0x61: return "session-key";
			case 0x64: return "mission-chunk";
			case 0x6F: return "cinematic-camera";
			case 0x7B: return "full-player-info";
			default: return nullptr;
		}
	} else {
		switch (tag) {
			case 0x00: return "JOIN";
			case 0x06: return "fired-round";
			case 0x0C: return "entity-input";            // §5.9
			case 0x0D: return "replication-ack";
			case 0x0F: return "input-frame";
			case 0x16: return "chat";
			case 0x21: return "checksum-reply";
			case 0x22: return "burst";
			case 0x23: return "burst";
			case 0x28: return "burst";
			case 0x29: return "burst";
			case 0x2C: return "rtt-consumed";
			default: return nullptr;
		}
	}
}

// ItemDef.id → display_name resolution. Populated when --items <path> is
// given on the CLI. Resolves type_ids in 0x0D / 0x20 record dumps so the
// reader sees "type=0x04bd [d_5ton truck]" instead of just a hex id.
std::unordered_map<int, std::string> g_item_names;

std::string type_str(uint16_t type) {
	// display_name field is 128 bytes in DefItemDef; a 192-byte stack
	// buffer comfortably holds the longest name + the "0xNNNN[...]" wrap.
	char buf[192];
	auto it = g_item_names.find(int(type));
	if (it == g_item_names.end()) {
		std::snprintf(buf, sizeof(buf), "0x%04x", type);
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "0x%04x[%s]", type, it->second.c_str());
	return buf;
}

// Load items.def into g_item_names. Accepts plaintext or SCR-encrypted
// input — the libs/scr decryptor expects the SCR magic in the first 3
// bytes, otherwise we treat the file as plaintext .def. Returns count
// of items loaded, 0 on any failure.
size_t load_items_def(const char *path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		std::fprintf(stderr, "items: failed to open %s\n", path);
		return 0;
	}
	const std::streamsize n = f.tellg();
	if (n <= 0) return 0;
	std::vector<uint8_t> raw(static_cast<size_t>(n));
	f.seekg(0);
	if (!f.read(reinterpret_cast<char *>(raw.data()), n)) return 0;

	const uint8_t *plain = raw.data();
	size_t plain_size = raw.size();
	std::vector<uint8_t> decrypted;
	if (scr_is_scr(raw.data(), raw.size())) {
		decrypted.resize(raw.size());
		size_t out_size = decrypted.size();
		if (scr_decrypt_buf(raw.data(), raw.size(), decrypted.data(),
		                    &out_size, SCR_KEY_JO_DFX2) != 0) {
			std::fprintf(stderr, "items: SCR decrypt failed for %s\n", path);
			return 0;
		}
		decrypted.resize(out_size);
		plain = decrypted.data();
		plain_size = decrypted.size();
	}

	DefItemsFile items{};
	if (def_parse_items_memory(plain, plain_size, &items) != 0) {
		std::fprintf(stderr, "items: def_parse_items_memory failed\n");
		return 0;
	}
	// The wire `itemTypeId` is `items.def.id - 100000` (cross-witnessed
	// 2026-06-16: wire 0x050b=1291 ↔ items.def `id 101291` "Drivable Dune
	// Buggy", wire 0x14B9=5305 ↔ id 105305 "Player #1 (Multiplayer)",
	// wire 0x04b0=1200 ↔ id 101200 "Drivable Indonesian LCT"). Engine
	// loader presumably folds the 100000 offset out before storing the
	// runtime `gItemDefs[i].id` field that `ItemList_FindIndexByTypeId
	// @ 0x49E100` compares against.
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &it = items.entries[i];
		const int wire_id = it.id - 100000;
		if (wire_id >= 0 && wire_id < 0x10000)
			g_item_names[wire_id] = it.display_name;
	}
	const size_t loaded = items.count;
	def_free_items(&items);
	return loaded;
}

void print_pool_spawn_record(int index, const PoolSpawnRecord &r) {
	std::printf("        record %d flags=0x%04x slot=%s type=%s "
	            "name=%-12s pos=(%.1f, %.1f, %.1f)",
	            index, r.spawn_flags, handle_str(r.slot_id).c_str(),
	            type_str(r.item_type_id).c_str(),
	            ("\"" + r.entity_name + "\"").c_str(),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.spawn_flags & 0x0020) std::printf(" entity36=0x%08x", r.entity_flags);
	if (r.spawn_flags & 0x0001) std::printf(" velX");
	if (r.spawn_flags & 0x0002) std::printf(" velY");
	if (r.spawn_flags & 0x0004) std::printf(" velZ");
	if (r.spawn_flags & 0x0008) std::printf(" sectionMask");
	if (r.spawn_flags & 0x0010) std::printf(" orient=0x%02x", r.orient_byte);
	if (r.spawn_flags & 0x0100)
		std::printf(" parent=%s", handle_str(r.parent_handle).c_str());
	if (r.spawn_flags & 0x0200)
		std::printf(" target=%s", handle_str(r.target_handle).c_str());
	if (r.spawn_flags & 0x0400) {
		std::printf(" weapMask=0x%02x", r.weapon_mask);
		if (r.weapon_mask) {
			int wcount = 0;
			for (int b = 0; b < 8; ++b) if (r.weapon_mask & (1u << b)) wcount++;
			std::printf(" weapons=%d+2extra", wcount);
		}
	}
	std::printf(" team=0x%02x", r.team_byte);
	if (r.spawn_flags & 0x0800)
		std::printf(" AItrailer{p1=0x%08x p2=0x%08x name=\"%s\"}",
		            r.ai_profile_1, r.ai_profile_2, r.ai_name.c_str());
	if (r.spawn_flags & 0x0040) std::printf(" alert=0x%02x", r.alert_byte);
	if (r.spawn_flags & 0x0080) std::printf(" action=0x%02x", r.action_byte);
	if (r.spawn_flags & 0x1000)
		std::printf(" weapType=0x%02x", r.weapon_type_byte);
	if (r.spawn_flags & 0x2000)
		std::printf(" health=0x%02x/0x%04x", r.health_byte, r.health_short);
	else if (r.spawn_flags & 0x8000)
		std::printf(" healthShort=0x%04x", r.health_short);
	if (r.spawn_flags & 0x4000) std::printf(" diff=0x%02x", r.difficulty_byte);
	std::printf("\n");
}

void print_pool3_sync_record(uint16_t slot_idx, const Pool3SyncRecord &r) {
	if (r.is_empty_slot) {
		std::printf("        slot %u type=0 (empty-slot sentinel)\n",
		            unsigned(slot_idx));
		return;
	}
	std::printf("        slot %u type=%s flags=0x%02x "
	            "pos=(%.1f, %.1f, %.1f)",
	            unsigned(slot_idx), type_str(r.item_type_id).c_str(),
	            r.flags_byte, fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.flags_byte & 0x01) std::printf(" parent=0x%08x", r.parent_handle);
	if (r.flags_byte & 0x02) std::printf(" orient=0x%08x", r.orientation_val);
	if (r.flags_byte & 0x04) std::printf(" ammo=%u", unsigned(r.ammo_count));
	std::printf(" net=%s", handle_str(r.net_handle).c_str());
	if (r.flags_byte & 0x08) std::printf(" team=0x%02x", r.team_byte);
	if (r.flags_byte & 0x10) std::printf(" weapType=0x%04x", r.weapon_type);
	if (r.flags_byte & 0x20) std::printf(" score=0x%02x", r.score_byte);
	std::printf("\n");
}

void print_tag_0d(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	const bool clean = decode_pool_spawn_batch(body.data(), body.size(), batch);
	std::printf("        [0x0D] entityCount=%d (body %zu B%s%s)\n",
	            int(batch.entity_count), body.size(),
	            batch.sentinel_ended_early ? ", sentinel-ended" : "",
	            clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_pool_spawn_record(int(i), batch.records[i]);
}

void print_tag_20(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	const bool clean = decode_pool3_sync_batch(body.data(), body.size(), batch);
	std::printf("        [0x20] startIdx=%u entityCount=%d (body %zu B%s)\n",
	            unsigned(batch.start_index), int(batch.entity_count),
	            body.size(), clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_pool3_sync_record(uint16_t(batch.start_index + i),
		                        batch.records[i]);
}

struct DirState {
	ProtocolReassemblyState rs;
	bool have_pending = false;
	int pending_tag = 0;
	int pending_first_frame = 0;
};

void print_payload(char dir, int frame, int tag,
                   const std::vector<uint8_t> &payload) {
	const char *label = tag_label(dir, tag);
	std::printf("[%c f=%-4d tag=0x%02x%s%s%s len=%zu]\n", dir, frame, tag,
	            label ? "[" : "", label ? label : "", label ? "]" : "",
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
	const char *items_path = nullptr;
	std::set<int> tag_filter;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (std::strcmp(a, "--items") == 0 && i + 1 < argc) {
			items_path = argv[++i];
		} else if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
			tag_filter.insert(int(std::strtol(a, nullptr, 16)));
		} else if (!path) {
			path = a;
		}
	}
	if (!path) path = std::getenv("NW_INGAME_HEXCAP");
	if (!items_path) items_path = std::getenv("NW_PP_ITEMS");
	if (!path || !*path) {
		std::fprintf(stderr,
		             "usage: nw_pp <capture-path> [--items <items.def>] [0xNN ...]\n"
		             "       path is a .pcap / .pcapng (parsed natively)\n"
		             "       or a hexcap text file (one '<srcport> <frame> "
		             "<hex>' per line)\n"
		             "       NW_INGAME_HEXCAP env supplies a hexcap path\n"
		             "       --items / NW_PP_ITEMS gives a JO items.def "
		             "(plaintext or SCR-encrypted with the JO/DFX2 key)\n"
		             "       so type_ids in 0x0D/0x20 records show as names\n");
		return 1;
	}
	if (items_path && *items_path) {
		const size_t n = load_items_def(items_path);
		std::fprintf(stderr, "loaded %zu item names from %s\n", n, items_path);
	}

	std::vector<Datagram> dgrams;
	if (is_pcap_path(path)) {
		const bool ok = pcap_io::load(path,
			[&dgrams](int srcport, int frame, const uint8_t *p, size_t n) {
				Datagram d;
				d.srcport = srcport;
				d.frame = frame;
				d.bytes.assign(p, p + n);
				dgrams.push_back(std::move(d));
			});
		if (!ok) return 1;
	} else {
		std::ifstream file(path);
		if (!file) {
			std::fprintf(stderr, "FAILED to open %s\n", path);
			return 1;
		}
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
