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
		if (rec.spawn_flags & 0x0010) rec.team_byte = c.u8();  // entity+354, BMS team (D-NET-58)
		if (rec.spawn_flags & 0x0100) rec.parent_handle = c.u16();
		if (rec.spawn_flags & 0x0200) rec.target_handle = c.u16();

		// Weapon block (0x0400). The retail handler reads the mask byte, then —
		// non-zero mask — one u16 per set bit (0xFFFF on a set bit skips storage
		// but still consumes the wire u16). It then ALWAYS consumes
		// extra_handle_0 + extra_handle_1: the mask==0 path `goto LABEL_110`
		// (@ 0x4330b1) reads both before falling through to the teamByte read.
		// D-NET-56: an earlier reading put the extras inside `if (mask)`, which
		// under-read by 4 B on the (0x400 set, mask==0) path. Retail servers
		// never emit that case (serialize_entity_pool_to_packet_0 only sets
		// 0x400 when the mask is non-zero), so the byte-witness capture didn't
		// exercise it — but the client handler reads it, so the port must too.
		// [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 LABEL_110)]
		if (rec.spawn_flags & 0x0400) {
			rec.weapon_mask = c.u8();
			if (rec.weapon_mask) {
				for (int b = 0; b < 8; ++b) {
					if (rec.weapon_mask & (1u << b))
						rec.weapon_handles[b] = c.u16();
				}
			}
			rec.extra_handle_0 = c.u16();
			rec.extra_handle_1 = c.u16();
		}

		rec.bone_byte = c.u8();  // entity+290, unconditional bone/other byte — NOT team (D-NET-58)

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

		if (rec.flags_byte & 0x01) rec.movement_val = c.u32();  // entitySlot+16, raw BAM heading — NOT parent (D-NET-59)
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

// S2C 0x40 minimap-overlay update / capture-zone state (§5.19).
// [orig: NapiNPClientMsg_0x040 @ 0x425A50 → MapOverlay_DecodeOverlayEntries @ 0x5BEBB0 (6-byte walker)]
bool decode_capture_zone_overlay(const uint8_t *body, size_t len,
                                 CaptureZoneOverlayBatch &out) {
	out = CaptureZoneOverlayBatch{};
	Cursor c{body, body + len, true};
	out.count = c.u8();
	for (uint8_t i = 0; i < out.count && c.ok; ++i) {
		CaptureZoneOverlay e;
		e.handle     = c.u16();
		e.param      = c.u8();
		e.icon_color = c.u8();
		e.flags      = c.u8();
		e.source     = c.u8();
		const bool record_ok = c.ok;
		out.entries.push_back(e);
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

// 5-B entity-packet sub-header used by S2C 0x0C and C2S 0x0C alike.
// [orig: dispatch_entity_packet_callback @ 0x4D6A80] reads it on the host side;
// [orig: Pool_SerializeEntityViaVTable @ 0x4D64E0] writes it on the sender side.
bool decode_entity_packet_sub_header(const uint8_t *body, size_t len,
                                     EntityPacketSubHeader &out,
                                     size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.handle       = c.u16();
	out.item_type_id = c.u16();
	out.sub_op       = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 5;
}

// §5.10 player extended uplink (mode 3/4, format 10). 43 B fixed body — i.e. the
// 48 B C2S 0x0C frame minus the 5 B sub-header. Cross-witnessed against the
// 2026-06-16b loopback frames cited in docs/net/novaworld-net-re.md §5.10.
// [orig: NetPacket_SerializePlayerState case 3/4 @ 0x4C09C0]
bool decode_player_extended_uplink(const uint8_t *body, size_t len,
                                   PlayerExtendedUplink &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.vehicle_handle = c.u16();
	out.pos_x          = int32_t(c.u32());
	out.pos_y          = int32_t(c.u32());
	out.pos_z          = int32_t(c.u32());
	out.heading        = int16_t(c.u16());
	out.pitch          = int16_t(c.u16());
	out.reserved_18    = c.u8();
	out.anim_slot_low  = c.u8();
	out.flags_xor      = c.u8();
	out.anim_def_1     = c.u8();
	out.anim_def_2     = c.u8();
	out.anim_def_3     = c.u8();
	out.reserved_24    = c.u8();
	out.stat_byte_0    = c.u8();
	out.stat_byte_1    = c.u8();
	out.weapon_id_0    = c.u16();
	out.fire_counter_0 = c.u16();
	out.weapon_id_1    = c.u16();
	out.fire_counter_1 = c.u16();
	out.weapon_id_2    = c.u16();
	out.fire_counter_2 = c.u16();
	out.weapon_id_3    = c.u16();
	out.fire_counter_3 = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 43;
}

// C2S 0x06 client-fired-round. 45 B fixed.
// [orig: NapiNPServerMsg_0x006_ClientFiredRound @ 0x513310]
bool decode_client_fired_round(const uint8_t *body, size_t len,
                               ClientFiredRound &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.current_tick    = c.u32();
	out.shooter_handle  = c.u16();
	out.fire_flags      = c.u8();
	out.adm_index       = c.u8();
	out.pos_x           = int32_t(c.u32());
	out.pos_y           = int32_t(c.u32());
	out.pos_z           = int32_t(c.u32());
	out.dir_x           = int32_t(c.u32());
	out.dir_y           = int32_t(c.u32());
	out.target_handle   = c.u16();
	out.hit_part        = c.u16();
	out.extra_byte1     = c.u8();
	out.extra_byte2     = c.u8();
	out.misc_byte       = c.u8();
	out.base_offset     = c.u16();
	out.offset_x        = c.u16();
	out.offset_y        = c.u16();
	out.offset_z        = c.u16();
	out.offset_w        = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 45;
}

// C2S 0x21 anti-cheat CRC reply. Handler reads u8 + u32 = 5 B effective; the
// 9-B body observed in capture has 4 trailing zero bytes that the handler
// never touches. We expose `consumed` so the caller can see the 5 vs 9 split.
// [orig: handle_anti_cheat_crc_check @ 0x502050]
bool decode_client_checksum_reply(const uint8_t *body, size_t len,
                                  ClientChecksumReply &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.player_index = c.u8();
	out.expected_crc = c.u32();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 5;
}

// §5.9.1 weapon-hit record. 17-20 B variable by flags gate (0x80, 0x40).
// [orig: NetPacket_DeserializeWeaponHit @ 0x42F270]
bool decode_weapon_hit_record(const uint8_t *body, size_t len,
                              WeaponHitRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.flags        = c.u8();
	out.adm_index    = c.u8();
	out.hit_subtype  = c.u8();
	if (out.flags & 0x80) {
		out.parent_byte = c.u8();
	}
	out.target_handle = c.u16();
	if (out.flags & 0x40) {
		out.weapon_handle = c.u16();
	}
	out.damage_extra_raw = c.u16();
	out.pos_x_compressed = c.u16();
	out.pos_y_compressed = c.u16();
	out.pos_z_compressed = c.u16();
	out.yaw_bam_high     = c.u16();
	out.pitch_bam_high   = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	const size_t expected = 17
		+ (out.has_parent_byte() ? 1u : 0u)
		+ (out.has_weapon_handle() ? 2u : 0u);
	return consumed == expected;
}

} // namespace opennova
