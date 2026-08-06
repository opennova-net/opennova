#include "npwire/serverlog_decode.h"

#include "../wire_cursor.h"

#include <cstring>

// `/PROFILE` .sph server-log decoder — see docs/net/novaworld-net-re.md §5.22.
// The chunk layout + per-record field maps are witnessed in the CServerLog_*
// writer cluster (Jointops.exe @ 0x4e1a10-0x4e1e00) and the per-frame loop in
// Game_ProcessMainFrame @ 0x5263f0.

namespace opennova {

namespace {

// Bounded cursor — mirrors ingame_decode.cpp. Every read is checked vs `end`;
// underflow flips ok=false and stops advancing.

using opennova::npwire_detail::Cursor;

inline bool tag_is(const uint8_t *p, const char (&t)[5]) {
	return std::memcmp(p, t, 4) == 0;
}

} // namespace

bool decode_server_log(const uint8_t *data, size_t len, ServerLogDocument &out) {
	out = ServerLogDocument{};
	if (!data) { return false; }

	size_t off = 0;
	while (off + 8 <= len) {
		const uint8_t *tag = data + off;
		// Length is the u16 at +4 (the high two header bytes are always zero —
		// the byte-trick writers strcpy only the low byte, the rest stays from
		// the zero-initialised buffer). It is the TOTAL chunk size incl header.
		uint16_t clen = uint16_t(data[off + 4]) | uint16_t(data[off + 5]) << 8;
		if (clen < 8 || off + clen > len) {
			out.leftover_bytes = len - off;
			return false;
		}

		const uint8_t *pl = data + off + 8;
		size_t pllen = size_t(clen) - 8;
		Cursor c{pl, pl + pllen, true};

		if (tag_is(tag, "NGEB")) {                 // BEGN — header
			out.version = c.u32();
			char m[17] = {0};
			for (int i = 0; i < 16 && c.ok; ++i) { m[i] = char(c.u8()); }
			out.mission.assign(m);                 // stops at first NUL
		} else if (tag_is(tag, "FEDP")) {          // PDEF — roster entry
			ServerLogPlayer pdef;
			pdef.net_id = c.u32();
			pdef.team = uint8_t(c.u32() & 0xFF);   // entity+354 (sign-ext on wire)
			uint32_t name_len = c.u32();
			std::string nm;
			for (uint32_t i = 0; i < name_len && c.ok; ++i) {
				uint8_t ch = c.u8();
				if (ch == 0) { break; }
				nm.push_back(char(ch));
			}
			pdef.name = nm;
			out.roster.push_back(pdef);
		} else if (tag_is(tag, "GEBF")) {          // FBEG — frame marker
			ServerLogFrame f;
			f.frame_index = c.u32();
			out.frames.push_back(f);
		} else if (tag_is(tag, "TADP")) {          // PDAT — entity sample
			ServerLogEntity e;
			e.net_id = c.u32();                    // +8
			int32_t neg_e2 = c.i32();              // +12 = -entity[2]
			int32_t e3 = c.i32();                  // +16 = entity[3]
			int32_t e1 = c.i32();                  // +20 = entity[1]
			e.yaw_bam = c.u32();                   // +24 = entity[4]
			e.angle2 = c.u32();                    // +28 = entity[6]
			(void)c.u32();                         // +32 = dead field
			e.flags = c.u32();                     // +36 = entity[9]
			e.vehicle_flag = c.u16();              // +40
			e.stat_byte = c.u16();                 // +42 (playerSlot+0x15F78)
			// Reconstruct the entity world position (entity+4/+8/+12).
			e.pos_x = e1;
			e.pos_y = -neg_e2;
			e.pos_z = e3;
			if (out.frames.empty()) { out.frames.emplace_back(); }
			out.frames.back().entities.push_back(e);
		} else if (tag_is(tag, "KRBP") || tag_is(tag, "MERP")) {  // PBRK / PREM
			ServerLogEvent ev;
			ev.kind = tag_is(tag, "KRBP") ? ServerLogEventKind::Death
			                              : ServerLogEventKind::Disconnect;
			ev.net_id = c.u32();
			ev.at_frame = out.frames.empty() ? 0 : out.frames.back().frame_index;
			out.events.push_back(ev);
		} else if (tag_is(tag, "CPSP")) {          // CDAT — entity-data blob
			++out.cdat_count;                      // body not modelled yet
		} else if (tag_is(tag, "DNE.")) {          // .END
			out.ended_clean = true;
			off += clen;
			break;
		}
		// Unknown tags are skipped by their length (forward-compatible).

		off += clen;
	}

	out.leftover_bytes = len - off;
	out.leftover_clean = out.ended_clean || off == len;
	return out.leftover_clean;
}

} // namespace opennova
