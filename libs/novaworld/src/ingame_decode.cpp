#include "novaworld/ingame_decode.h"

// Decoders for S2C 0x0D / 0x20 — see docs/net/novaworld-net-re.md §5.11/§5.12.
// Cross-witnessed byte-exact against the 2026-06-16b loopback by
// nw_ingame_pool_records_test (437 × 0x0D / 792 × 0x20 records, zero
// leftover bytes).

namespace opennova {

namespace {

// Bounded cursor — every read is bounds-checked vs `end`. On underflow we
// flip `ok=false` and stop advancing; the caller sees the exact byte where
// the decode ran out. Mirrors the retail handlers' `cursor + N <= end`
// pattern (NapiNPClientMsg_0x00D / _0x020 both bail to a final-return on
// short reads, leaving whatever was already stored in place).
struct Cursor {
	const uint8_t *p = nullptr;
	const uint8_t *end = nullptr;
	bool ok = true;

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

} // namespace

bool decode_pool_spawn_batch(const uint8_t *body, size_t len,
                              PoolSpawnBatch &out) {
	out.records.clear();
	out.sentinel_ended_early = false;
	out.entity_count = 0;

	Cursor c{body, body + len, true};
	out.entity_count = int16_t(c.u16());
	if (!c.ok) return false;
	if (out.entity_count <= 0) {
		// Empty batch — body of exactly 2 bytes is clean.
		return (c.p == c.end);
	}

	out.records.reserve(size_t(out.entity_count));
	for (int i = 0; i < out.entity_count; ++i) {
		PoolSpawnRecord rec;
		rec.spawn_flags = c.u16();
		rec.slot_id = c.u16();
		if (!c.ok) {
			out.records.push_back(std::move(rec));
			return false;
		}
		if (rec.slot_id == 0xFFFF || (rec.slot_id & 0xF000) >= 0x5000) {
			// Retail handler returns immediately on the sentinel — without
			// storing this record. Match that: stop the loop here.
			out.sentinel_ended_early = true;
			return (c.p == c.end);
		}

		rec.item_type_id = c.u16();
		rec.entity_name = c.cstr();
		if (!c.ok) {
			out.records.push_back(std::move(rec));
			return false;
		}

		if (rec.spawn_flags & 0x0020) rec.entity_flags = c.u32();
		rec.pos_x = int32_t(c.u32());
		rec.pos_y = int32_t(c.u32());
		rec.pos_z = int32_t(c.u32());

		if (rec.spawn_flags & 0x0001) rec.vel_x = int32_t(c.u32());
		if (rec.spawn_flags & 0x0002) rec.vel_y = int32_t(c.u32());
		if (rec.spawn_flags & 0x0004) rec.vel_z = int32_t(c.u32());
		if (rec.spawn_flags & 0x0008) rec.section_mask = int32_t(c.u32());
		if (rec.spawn_flags & 0x0010) rec.orient_byte = c.u8();
		if (rec.spawn_flags & 0x0100) rec.parent_handle = c.u16();
		if (rec.spawn_flags & 0x0200) rec.target_handle = c.u16();

		// Weapon block. The retail handler reads the mask byte
		// unconditionally inside the 0x400 branch; if the mask is zero the
		// extras are NOT consumed (the branch falls through to the
		// teamByte read). If the mask is non-zero, one u16 per set bit
		// (0xFFFF on a set bit skips storage but still consumes the wire
		// u16), then unconditional extra_handle_0 + extra_handle_1.
		if (rec.spawn_flags & 0x0400) {
			rec.weapon_mask = c.u8();
			if (rec.weapon_mask) {
				for (int b = 0; b < 8; ++b) {
					if (rec.weapon_mask & (1u << b))
						rec.weapon_handles[b] = c.u16();
				}
				rec.extra_handle_0 = c.u16();
				rec.extra_handle_1 = c.u16();
			}
		}

		rec.team_byte = c.u8();

		if (rec.spawn_flags & 0x0800) {
			rec.ai_profile_1 = c.u32();
			rec.ai_profile_2 = c.u32();
			rec.ai_name = c.cstr();
		}
		if (rec.spawn_flags & 0x0040) rec.alert_byte = c.u8();
		if (rec.spawn_flags & 0x0080) rec.action_byte = c.u8();
		if (rec.spawn_flags & 0x1000) rec.weapon_type_byte = c.u8();

		if (rec.spawn_flags & 0x2000) {
			rec.health_byte = c.u8();
			rec.health_short = c.u16();
		} else if (rec.spawn_flags & 0x8000) {
			rec.health_short = c.u16();
		}
		if (rec.spawn_flags & 0x4000) rec.difficulty_byte = c.u8();

		const bool record_ok = c.ok;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

bool decode_pool3_sync_batch(const uint8_t *body, size_t len,
                              Pool3SyncBatch &out) {
	out.records.clear();
	out.start_index = 0;
	out.entity_count = 0;

	Cursor c{body, body + len, true};
	out.start_index = c.u16();
	out.entity_count = int16_t(c.u16());
	if (!c.ok) return false;
	if (out.entity_count <= 0) return (c.p == c.end);

	out.records.reserve(size_t(out.entity_count));
	for (int i = 0; i < out.entity_count; ++i) {
		Pool3SyncRecord rec;
		rec.item_type_id = c.u16();
		if (!c.ok) {
			out.records.push_back(std::move(rec));
			return false;
		}
		if (rec.item_type_id == 0) {
			rec.is_empty_slot = true;
			out.records.push_back(std::move(rec));
			continue;
		}

		rec.flags_byte = c.u8();
		rec.pos_x = int32_t(c.u32());
		rec.pos_y = int32_t(c.u32());
		rec.pos_z = int32_t(c.u32());

		if (rec.flags_byte & 0x01) rec.parent_handle = c.u32();
		if (rec.flags_byte & 0x02) rec.orientation_val = c.u32();
		if (rec.flags_byte & 0x04) rec.ammo_count = c.u16();
		rec.net_handle = c.u16();
		if (rec.flags_byte & 0x08) rec.team_byte = c.u8();
		if (rec.flags_byte & 0x10) rec.weapon_type = c.u16();
		if (rec.flags_byte & 0x20) rec.score_byte = c.u8();

		const bool record_ok = c.ok;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

// ===========================================================================
// Per-entity compact records inside S2C 0x0A trailing event-loop tag==1.
// Field tables: docs/net/novaworld-net-re.md §5.10 / §5.13 / §5.14.
// ===========================================================================

// §5.10 player compact record (mode 2, format 11). 18 B fixed.
// [orig: NetPacket_SerializePlayerState case 1/2 @ 0x4C09C0]
bool decode_player_compact_record(const uint8_t *body, size_t len,
                                  PlayerCompactRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.vehicle_bone      = c.u8();
	out.seat_type         = c.u8();
	out.vehicle_handle    = c.u16();
	out.pos_x_compressed  = c.u16();
	out.pos_y_compressed  = c.u16();
	out.pos_z_compressed  = c.u16();
	out.yaw_byte          = c.u8();
	out.pitch_byte        = c.u8();
	out.anim_slot_low     = c.u8();
	out.state_flags       = c.u8();
	out.weapon_anim_state = c.u8();
	out.priority          = c.u8();
	out.anim_def_index    = c.u8();
	out.health_class_byte = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 18;
}

// §5.13 vehicle compact record (mode 2, format 11). 15 B mounted / 21 B not.
// [orig: Entity_SerializeMountedVehicleState @ 0x460560]
bool decode_vehicle_compact_record(const uint8_t *body, size_t len,
                                   VehicleCompactRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.parent_slot_handle = c.u16();
	out.pos_x_compressed   = c.u16();
	out.pos_y_compressed   = c.u16();
	out.pos_z_compressed   = c.u16();
	out.yaw_high           = int16_t(c.u16());
	out.flags_byte         = c.u8();
	out.is_mounted         = (out.flags_byte & 0x04) != 0;
	if (out.is_mounted) {
		out.secondary_heading = c.u16();
	} else {
		out.weapon_x_compressed       = c.u16();
		out.weapon_y_raw              = c.u16();
		out.weapon_z_compressed       = c.u16();
		out.weapon_heading_compressed = c.u16();
	}
	out.final_heading = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == (out.is_mounted ? size_t(15) : size_t(21));
}

// §5.14 infantry / AI compact record (mode 2, format 11). 14 B fixed.
// [orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320]
bool decode_infantry_compact_record(const uint8_t *body, size_t len,
                                    InfantryCompactRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.seat_bone_idx       = c.u8();
	out.vehicle_slot_handle = c.u16();
	out.pos_x_compressed    = c.u16();
	out.pos_y_compressed    = c.u16();
	out.pos_z_compressed    = c.u16();
	out.yaw_byte            = c.u8();
	out.flags_byte          = c.u8();
	out.pitch_byte          = c.u8();
	out.aim_yaw_byte        = c.u8();
	out.anim_byte           = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 14;
}

} // namespace opennova
