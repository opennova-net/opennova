// Byte-witness the §5.11 (S2C 0x0D pool-entity spawn batch) and §5.12 (S2C
// 0x20 bulk pool-3 entity sync) field maps against a live loopback capture.
//
// Drives the SAME outer-decode pipeline as `nw_ingame_histogram_test` —
// envelope CRC -> outer NWU -> per-session SCRK -> 0x43/0x83 -> protocol
// dispatch / reassembly. For every completed S2C payload tagged 0x0D or 0x20
// it then walks the body record-by-record per the IDA decompile of the retail
// handlers, asserting:
//
//   - 37 × S2C 0x0D payloads + 29 × S2C 0x20 payloads in the capture (matches
//     the 2026-06-16b loopback; user-confirmed counts driving Stage C).
//   - Every payload's body is consumed EXACTLY by the documented walker — a
//     non-zero leftover means the field map is wrong (or, equivalently, the
//     builder is wrong: D-NET-52..55).
//   - Position fields land in plausible 16.16 world bounds.
//
// Skips cleanly when `NW_INGAME_HEXCAP` is unset (CI stays green; see
// `tools/net/pcap_to_hexcap.py` for the converter). Hexcap line format is
// "<srcport> <frame> <udp_payload_hex>", same as the histogram test consumes.
//
// References:
//   - docs/net/novaworld-net-re.md §5.11 (this commit) — full per-flag field map.
//   - [orig: NapiNPClientMsg_0x00D @ 0x432C40]  — S2C 0x0D handler.
//   - [orig: NapiNPClientMsg_0x020 @ 0x425C00]  — S2C 0x20 handler.

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

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

// Position fields are i32 16.16 world coordinates. A Laba-Laba mission's world
// half-width is well under 30 km in engine units — anything bigger means we
// mis-read the field offset. (30000 * 0x10000 still fits in an int32, unlike
// 50000 * 0x10000 which overflows.)
bool position_plausible(int32_t v) {
	constexpr int32_t kHalfWorld = 30000 * 0x10000;
	return v > -kHalfWorld && v < kHalfWorld;
}

// Bounded cursor: every read is bounds-checked vs `end`; on underflow we set
// `ok=false` and stop advancing so the caller sees the exact byte where we ran
// out (mirrors the retail handlers' `cursor + N <= end` guards).
struct Cursor {
	const uint8_t *p = nullptr;
	const uint8_t *end = nullptr;
	bool ok = true;

	size_t remaining() const { return ok ? size_t(end - p) : 0; }

	uint8_t u8() {
		if (!ok || p + 1 > end) { ok = false; return 0; }
		uint8_t v = p[0]; p += 1; return v;
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
	// NUL-terminated string. Advance past the NUL. Returns false if no NUL
	// found before `end`.
	bool cstr() {
		if (!ok) return false;
		while (p < end) {
			uint8_t c = *p++;
			if (c == 0) return true;
		}
		ok = false;
		return false;
	}
};

// Walker stats — accumulated across the whole capture and printed at exit so
// the doc gets cross-witnessed counts.
struct Stats {
	int payloads = 0;
	int payloads_clean = 0;       // body consumed exactly
	int entities = 0;
	int entities_with_trailer = 0; // AI trailer (0x0D only)
	int entities_empty_slot = 0;   // type_id == 0 sentinel (0x20 only)
	uint32_t any_flag_seen = 0;    // OR of all flag words observed
	int max_count_per_payload = 0;
	int bad_position = 0;
	int leftover_bytes = 0;
};

Stats g_stats_0d;
Stats g_stats_20;

// Walk one S2C 0x0D payload per the §5.11 field map.
//   [u16 entityCount]
//   loop entityCount: per-entity record (§5.11 table)
// Returns true if the body was consumed cleanly.
bool walk_tag_0d(const std::vector<uint8_t> &body) {
	Cursor c{body.data(), body.data() + body.size(), true};
	g_stats_0d.payloads++;

	int16_t entityCount = int16_t(c.u16());
	if (!c.ok) return false;
	if (entityCount <= 0) {
		// Empty batch — body of 2 bytes is legitimate.
		if (c.remaining() == 0) g_stats_0d.payloads_clean++;
		else g_stats_0d.leftover_bytes += int(c.remaining());
		return c.remaining() == 0;
	}
	if (entityCount > g_stats_0d.max_count_per_payload)
		g_stats_0d.max_count_per_payload = entityCount;

	for (int i = 0; i < entityCount; ++i) {
		const uint16_t spawnFlags = c.u16();
		const uint16_t slotId = c.u16();
		if (!c.ok) return false;
		// Retail's 0xFFFF / (slotId&0xF000)>=0x5000 / OOB → handler returns early.
		// Treat as a clean end-of-batch.
		if (slotId == 0xFFFF || (slotId & 0xF000) >= 0x5000) {
			if (c.remaining() == 0) g_stats_0d.payloads_clean++;
			else g_stats_0d.leftover_bytes += int(c.remaining());
			g_stats_0d.entities += i;
			return c.remaining() == 0;
		}

		(void)c.u16();              // itemTypeId
		if (!c.cstr()) return false; // entity name cstring
		g_stats_0d.any_flag_seen |= spawnFlags;

		if (spawnFlags & 0x0020) (void)c.u32(); // entity36

		const int32_t px = int32_t(c.u32());
		const int32_t py = int32_t(c.u32());
		const int32_t pz = int32_t(c.u32());
		if (!c.ok) return false;
		if (!position_plausible(px) || !position_plausible(py) ||
		    !position_plausible(pz))
			g_stats_0d.bad_position++;

		if (spawnFlags & 0x0001) (void)c.u32(); // velX
		if (spawnFlags & 0x0002) (void)c.u32(); // velY
		if (spawnFlags & 0x0004) (void)c.u32(); // velZ
		if (spawnFlags & 0x0008) (void)c.u32(); // sectionMask
		if (spawnFlags & 0x0010) (void)c.u8();  // orientByte
		if (spawnFlags & 0x0100) (void)c.u16(); // parentHandle
		if (spawnFlags & 0x0200) (void)c.u16(); // targetHandle

		// Weapon block: u8 mask + 1× u16 per set bit (0xFFFF skips storage
		// but still consumes a wire u16 only on a set bit, NOT on a clear
		// bit — handler advances cursor *inside* the per-bit gate). Then
		// always 2× u16 extra handles (within the block).
		if (spawnFlags & 0x0400) {
			const uint8_t mask = c.u8();
			if (!c.ok) return false;
			if (mask) {
				for (int b = 0; b < 8; ++b) {
					if (mask & (1u << b)) (void)c.u16();
				}
				(void)c.u16(); // extraHandle0
				(void)c.u16(); // extraHandle1
			}
		}

		(void)c.u8(); // teamByte (always) → entity+290

		if (spawnFlags & 0x0800) {
			// AI trailer (D-NET-52: was documented as [u16][u32][cstring];
			// actual handler @ 0x43311e/0x433131 reads two 4-byte fields).
			(void)c.u32(); // aiProfile1 → aiSlot+16
			(void)c.u32(); // aiProfile2 → aiSlot+20
			if (!c.cstr()) return false; // aiName → aiSlot+156
			g_stats_0d.entities_with_trailer++;
		}

		if (spawnFlags & 0x0040) (void)c.u8(); // alertByte → entity+533
		if (spawnFlags & 0x0080) (void)c.u8(); // actionByte → entity+532
		if (spawnFlags & 0x1000) (void)c.u8(); // weaponTypeByte → entity+176

		if (spawnFlags & 0x2000) {
			(void)c.u8();  // healthByte → entity+538
			(void)c.u16(); // healthShort → entity+350
		} else if (spawnFlags & 0x8000) {
			(void)c.u16(); // healthShort only
		}

		if (spawnFlags & 0x4000) (void)c.u8(); // difficultyByte → entity+624

		if (!c.ok) return false;
		g_stats_0d.entities++;
	}

	if (c.remaining() == 0) {
		g_stats_0d.payloads_clean++;
		return true;
	}
	g_stats_0d.leftover_bytes += int(c.remaining());
	return false;
}

// Walk one S2C 0x20 payload per the §5.12 field map.
//   [u16 startIndex][u16 entityCount]
//   loop entityCount: per-entity record (itemTypeId==0 → empty slot, body ends)
// Returns true if the body was consumed cleanly.
bool walk_tag_20(const std::vector<uint8_t> &body) {
	Cursor c{body.data(), body.data() + body.size(), true};
	g_stats_20.payloads++;

	(void)c.u16(); // startIndex
	int16_t entityCount = int16_t(c.u16());
	if (!c.ok) return false;
	if (entityCount <= 0) {
		if (c.remaining() == 0) g_stats_20.payloads_clean++;
		else g_stats_20.leftover_bytes += int(c.remaining());
		return c.remaining() == 0;
	}
	if (entityCount > g_stats_20.max_count_per_payload)
		g_stats_20.max_count_per_payload = entityCount;

	for (int i = 0; i < entityCount; ++i) {
		const uint16_t itemTypeId = c.u16();
		if (!c.ok) return false;
		if (itemTypeId == 0) {
			g_stats_20.entities_empty_slot++;
			g_stats_20.entities++;
			continue; // empty slot — no body for this record
		}

		const uint8_t flagsByte = c.u8();
		if (!c.ok) return false;
		g_stats_20.any_flag_seen |= flagsByte;

		const int32_t px = int32_t(c.u32());
		const int32_t py = int32_t(c.u32());
		const int32_t pz = int32_t(c.u32());
		if (!c.ok) return false;
		if (!position_plausible(px) || !position_plausible(py) ||
		    !position_plausible(pz))
			g_stats_20.bad_position++;

		if (flagsByte & 0x01) (void)c.u32(); // parentHandle  → entity+16
		if (flagsByte & 0x02) (void)c.u32(); // orientationVal → entity+0
		if (flagsByte & 0x04) (void)c.u16(); // ammoCount     → entity+290
		(void)c.u16();                       // netHandle ALWAYS → entity+124
		if (flagsByte & 0x08) (void)c.u8();  // teamByte      → entity+354
		if (flagsByte & 0x10) (void)c.u16(); // weaponType    → entity+640
		if (flagsByte & 0x20) (void)c.u8();  // scoreByte     → entity+672

		if (!c.ok) return false;
		g_stats_20.entities++;
	}

	if (c.remaining() == 0) {
		g_stats_20.payloads_clean++;
		return true;
	}
	g_stats_20.leftover_bytes += int(c.remaining());
	return false;
}

void process_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                      ProtocolReassemblyState &rs) {
	if (scrk.empty()) return;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs))
		return;
	for (const auto &pm : msgs) {
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(rs, pm, assembled)) continue;
		if (pm.flags.settings_update) continue; // not pool messages

		// Inner-tag dispatch: only S2C 0x0D / 0x20.
		const int tag = int(pm.full_tag);
		if (tag == 0x0D) walk_tag_0d(assembled);
		else if (tag == 0x20) walk_tag_20(assembled);
	}
}

} // namespace

int main() {
	const char *path = std::getenv("NW_INGAME_HEXCAP");
	if (!path || !*path) {
		std::printf("[skip] set NW_INGAME_HEXCAP to a '<srcport> <frame> <hex>' "
		            "capture to run the pool-records witness\n");
		return 0;
	}
	std::ifstream file(path);
	if (!file) {
		std::printf("FAILED to open NW_INGAME_HEXCAP=%s\n", path);
		return 1;
	}

	struct Datagram {
		int srcport = 0;
		int frame = 0;
		std::vector<uint8_t> bytes;
	};
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
	std::printf("loaded %zu datagrams from %s\n", dgrams.size(), path);

	// Only S2C tags 0x0D / 0x20 are walked. The S2C SCRK comes out of
	// SERVER_AUTH; the host's per-session 0x83 packets carry the pool messages.
	std::string client_scrk, server_scrk;
	ProtocolReassemblyState s_rs;

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
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			process_protocol(body, server_scrk, s_rs);
			break;
		default:
			break;
		}
	}

	auto dump = [](const char *label, const Stats &s) {
		std::printf("\n=== %s ===\n", label);
		std::printf("  payloads:           %d (clean: %d, leftover bytes: %d)\n",
		            s.payloads, s.payloads_clean, s.leftover_bytes);
		std::printf("  entities total:     %d (max %d per payload)\n",
		            s.entities, s.max_count_per_payload);
		std::printf("  entities w/ trailer:%d\n", s.entities_with_trailer);
		std::printf("  entities empty slot:%d\n", s.entities_empty_slot);
		std::printf("  flag bits seen:     0x%x\n", s.any_flag_seen);
		std::printf("  bad positions:      %d\n", s.bad_position);
	};
	dump("S2C 0x0D pool-entity spawn batch (§5.11)", g_stats_0d);
	dump("S2C 0x20 bulk pool-3 entity sync (§5.12)", g_stats_20);

	// Assertions. Counts are pinned to the 2026-06-16b capture (the user
	// supplied this exact pcapng; it's the §5.10 cross-witness oracle).
	int failures = 0;
	auto check = [&](bool cond, const char *what) {
		if (!cond) { std::printf("FAIL: %s\n", what); failures++; }
	};
	check(g_stats_0d.payloads == 37, "S2C 0x0D payload count == 37");
	check(g_stats_0d.payloads_clean == 37,
	      "every S2C 0x0D payload consumed exactly");
	check(g_stats_0d.bad_position == 0,
	      "S2C 0x0D positions all within world bounds");
	check(g_stats_20.payloads == 29, "S2C 0x20 payload count == 29");
	check(g_stats_20.payloads_clean == 29,
	      "every S2C 0x20 payload consumed exactly");
	check(g_stats_20.bad_position == 0,
	      "S2C 0x20 positions all within world bounds");

	if (failures) {
		std::printf("\n%d assertion(s) failed\n", failures);
		return 1;
	}
	std::printf("\nPASS: §5.11 and §5.12 field maps consume the wire exactly.\n");
	return 0;
}
