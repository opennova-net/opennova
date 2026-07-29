#include "npwire/ingame_encode.h"
#include <io/le.h>

// Encoders for the in-match S2C replication tags — the symmetric partners to
// ingame_decode.cpp. Faithful structural ports of the original server-side
// serializers. Verified inverse pair:
//   encode_pool3_sync_batch  ← [orig: serialize_entity_pool_to_packet @ 0x503460]
//   decode_pool3_sync_batch  ← [orig: NapiNPClientMsg_0x020          @ 0x425C00]
// Both handlers were decompiled and field-mapped (docs/net/novaworld-net-re.md
// §5.12); the encode side writes exactly the byte stream the decode side reads.

namespace opennova {

namespace {

// Little-endian byte writer — the inverse of ingame_decode.cpp's `Cursor`. No
// bounds cap here: the original's per-datagram 650 B limit is enforced by the
// host emit loop's cursor walk (see ingame_encode.h), so this serializes the
// exact records handed to it.
struct Writer {
	std::vector<uint8_t> &out;
	void u8(uint8_t v) { opennova::io::append_u8(out, v); }
	void u16(uint16_t v) { opennova::io::append_u16_le(out, v); }
	void u32(uint32_t v) { opennova::io::append_u32_le(out, v); }
	// NUL-terminated string — the inverse of Cursor::cstr().
	void cstr(const std::string &s) {
		for (char ch : s) out.push_back(uint8_t(ch));
		out.push_back(0);
	}
	// Capped VARIABLE-length NUL-terminated string: strlen+1 bytes on the wire, truncated to
	// (max_chars-1) chars. The retail slot-state writer emits strlen+1
	// (NetPacket_SerializePlayerSync0x46 @0x505f9b) and the client reads strlen+1 with the
	// same char cap (NapiNPClientMsg_PlayerSync @0x431370) — never a fixed-width field. (Renamed
	// from the misnomer `cstr_fixed`.)
	void cstr_capped(const std::string &s, size_t max_chars) {
		const size_t cap = max_chars > 0 ? max_chars - 1 : 0;
		const size_t n = s.size() < cap ? s.size() : cap;
		for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(s[i]));
		out.push_back(0);
	}
};

} // namespace

// [orig: serialize_entity_pool_to_packet @ 0x503460]
std::vector<uint8_t> encode_pool3_sync_batch(const Pool3SyncBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};

	// Header `[u16 start_index][u16 count]`. The original writes the pool
	// cursor as the start index (`*packet_buf = cursor_start @ 0x5034c5`) and
	// patches the count after the loop (`packet_buf[1] = buf_sizea @ 0x5036bf`);
	// the count is the number of slots emitted, INCLUDING empty-slot sentinels.
	w.u16(batch.start_index);
	w.u16(uint16_t(batch.records.size()));

	for (const Pool3SyncRecord &rec : batch.records) {
		// Empty-slot sentinel: the original's `else { *write_ptr++ = 0; }`
		// branch (@ 0x503673) writes a bare `[u16 0]` and advances. The decoder
		// reads item_type_id == 0 and skips the body.
		if (rec.is_empty_slot || rec.item_type_id == 0) {
			w.u16(0);
			continue;
		}
		w.u16(rec.item_type_id); // [orig: ref_ptr+80 -> *write_ptr @ 0x50352b]

		// The flag byte is DERIVED from non-zero source fields — the original
		// sets each gate bit inside `if (value) { flags |= bit; <write>; }`, so
		// a clear bit means "field was zero", not "field absent from state".
		// Field->bit map matches NapiNPClientMsg_0x020 / §5.12 exactly.
		uint8_t flags = 0;
		if (rec.movement_val)    flags |= 0x01; // entry+16 raw BAM heading [orig: 0x50357e] (D-NET-59)
		if (rec.orientation_val) flags |= 0x02; // entry+0   [orig: 0x503593]
		if (rec.ammo_count)      flags |= 0x04; // entry+290 [orig: 0x5035b7]
		if (rec.team_byte)       flags |= 0x08; // entry+354 [orig: 0x5035f1]
		if (rec.weapon_type)     flags |= 0x10; // entry+640 [orig: 0x50360b]
		if (rec.score_byte)      flags |= 0x20; // entry+672 [orig: 0x503633]

		w.u8(flags);                    // [orig: *flags_location = flags @ 0x503660]
		w.u32(uint32_t(rec.pos_x));     // entry+4  [orig: 0x503553]
		w.u32(uint32_t(rec.pos_y));     // entry+8  [orig: 0x503565]
		w.u32(uint32_t(rec.pos_z));     // entry+12 [orig: 0x503577]

		if (flags & 0x01) w.u32(rec.movement_val);    // [orig: 0x50358f] (D-NET-59)
		if (flags & 0x02) w.u32(rec.orientation_val); // [orig: 0x5035a9]
		if (flags & 0x04) w.u16(rec.ammo_count);      // [orig: 0x5035cc]
		w.u16(rec.net_handle);                         // ALWAYS [orig: 0x5035e2, entry+124]
		if (flags & 0x08) w.u8(rec.team_byte);        // [orig: 0x503603]
		if (flags & 0x10) w.u16(rec.weapon_type);     // [orig: 0x50362a]
		if (flags & 0x20) w.u8(rec.score_byte);       // [orig: 0x503650]
	}

	return out;
}

// [orig: serialize_entity_pool_to_packet_0 @ 0x503940]
std::vector<uint8_t> encode_pool_spawn_batch(const PoolSpawnBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};

	// Header is `[u16 count]` only — the 0x0D handler reads `entityCount = *packetData`
	// with no start index (contrast 0x20). [orig: 0x432c71]
	w.u16(uint16_t(batch.records.size()));

	for (const PoolSpawnRecord &rec : batch.records) {
		// spawn_flags DERIVED from populated fields (the original sets each bit
		// inside `if (value) { flags |= bit; <write> }`). See ingame_encode.h.
		uint16_t f = 0;
		if (rec.entity_flags)              f |= 0x0020; // entity+36   [orig: 0x503ae6]
		if (rec.euler_z)                   f |= 0x0001; // entity+16 yaw [orig: 0x503b3c]
		if (rec.euler_x)                   f |= 0x0002; // entity+20   [orig: 0x503b58]
		if (rec.euler_y)                   f |= 0x0004; // entity+24   [orig: 0x503b74]
		if (rec.section_mask)              f |= 0x0008; // entity+308  [orig: 0x503b93]
		if (rec.team_byte)                 f |= 0x0010; // entity+354 team [orig: 0x503bb2] (D-NET-58)
		if (rec.parent_handle != 0xFFFF)   f |= 0x0100; // entity+368  [orig: 0x503bd1]
		if (rec.target_handle != 0xFFFF)   f |= 0x0200; // entity+40   [orig: 0x503c27]
		if (rec.weapon_mask)               f |= 0x0400; // itemDef+604 [orig: 0x503c83]
		if (!rec.ai_name.empty() || rec.ai_profile_1 || rec.ai_profile_2)
		                                   f |= 0x0800; // aiSlot      [orig: 0x503d53]
		if (rec.alert_byte)                f |= 0x0040; // entity+533  [orig: 0x503e3c]
		if (rec.action_byte)               f |= 0x0080; // entity+532  [orig: 0x503e60]
		if (rec.weapon_type_byte)          f |= 0x1000; // entity+100  [orig: 0x503e7f]
		if (rec.zone_number_rank)               f |= 0x2000; // entity+538  [orig: 0x503ecc]
		else if (rec.zone_radius)         f |= 0x8000; // itemDef+0x40000 path [orig: 0x503f29]
		if (rec.difficulty_byte)           f |= 0x4000; // entity+624  [orig: 0x503f4c]

		w.u16(f);                       // spawn_flags  [orig: *flags_write_pos @ 0x503f90]
		w.u16(rec.slot_id);             // packed handle [orig: 0x503a3f]
		w.u16(rec.item_type_id);        // [orig: 0x503a57]
		w.cstr(rec.entity_name);        // AI: entity+244, else empty [orig: 0x503a64..]

		if (f & 0x0020) w.u32(rec.entity_flags); // [orig: 0x503afc]
		w.u32(uint32_t(rec.pos_x));     // entity+4  [orig: 0x503b0f]
		w.u32(uint32_t(rec.pos_y));     // entity+8  [orig: 0x503b22]
		w.u32(uint32_t(rec.pos_z));     // entity+12 [orig: 0x503b35]

		if (f & 0x0001) w.u32(uint32_t(rec.euler_z));
		if (f & 0x0002) w.u32(uint32_t(rec.euler_x));
		if (f & 0x0004) w.u32(uint32_t(rec.euler_y));
		if (f & 0x0008) w.u32(uint32_t(rec.section_mask));
		if (f & 0x0010) w.u8(rec.team_byte);   // entity+354 team (D-NET-58)
		if (f & 0x0100) w.u16(rec.parent_handle);
		if (f & 0x0200) w.u16(rec.target_handle);

		// Weapon block. Once 0x0400 is set the original walks bits 0..7 writing
		// one u16 per set bit, then ALWAYS writes extra_handle_0 + extra_handle_1
		// (D-NET-56). The encoder only sets 0x0400 when weapon_mask != 0.
		if (f & 0x0400) {
			w.u8(rec.weapon_mask);
			for (int b = 0; b < 8; ++b)
				if (rec.weapon_mask & (1u << b))
					w.u16(rec.weapon_handles[size_t(b)]); // [orig: 0x432f47.. mirror]
			w.u16(rec.extra_handle_0);  // entity+416 [orig: 0x503d0c]
			w.u16(rec.extra_handle_1);  // entity+418 [orig: 0x503d24]
		}

		w.u8(rec.bone_byte);            // ALWAYS, entity+290 bone/other byte [orig: 0x503d38] (D-NET-58)

		if (f & 0x0800) {               // AI trailer (D-NET-52: 4+4+cstr)
			w.u32(rec.ai_profile_1);    // aiSlot+16  [orig: 0x503d6f]
			w.u32(rec.ai_profile_2);    // aiSlot+20  [orig: 0x503d83]
			w.cstr(rec.ai_name);        // aiSlot+156 [orig: 0x503dab..]
		}
		if (f & 0x0040) w.u8(rec.alert_byte);        // [orig: 0x503e50]
		if (f & 0x0080) w.u8(rec.action_byte);       // [orig: 0x503e77]
		if (f & 0x1000) w.u8(rec.weapon_type_byte);  // [orig: 0x503ec0]

		if (f & 0x2000) {               // [orig: 0x503ee5 health block]
			w.u8(rec.zone_number_rank);      // entity+538 (the break-out byte)
			w.u16(rec.zone_radius);    // entity+350
		} else if (f & 0x8000) {
			w.u16(rec.zone_radius);    // entity+350 [orig: 0x503f43]
		}
		if (f & 0x4000) w.u8(rec.difficulty_byte);   // entity+624 [orig: 0x503f7e]
	}

	return out;
}

// [orig: sub_5042F0] (the §5.2a world-stream phase-1 static serializer) — the inverse of
// decode_static_entity_batch (§5.9). Header `[u16 start_index][u16 count]`, then per record
// `[u16 item_type_id]` (0 ⇒ empty-slot sentinel, record ends), else `[u16 field_flags]
// [i32 x][i32 y][i32 z]`, the flag-gated optionals, the ALWAYS ammo_count, more flag-gated
// optionals, the ALWAYS weapon_byte, and attach_ref when `weapon_byte != 0 || flags & 0x200`.
// Like the pool-3 / pool-spawn encoders the flag word is DERIVED from non-zero source fields
// (the original sets each gate bit inside `if (value) { flags |= bit; <write> }`). Field→bit
// map matches decode_static_entity_batch exactly (D-NET-70/71, byte-validated).
// [orig: decode NapiNPClientMsg_0x010 @ 0x433400.]
std::vector<uint8_t> encode_static_entity_batch(const StaticEntityBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};

	// Header — start index is the pool cursor; count is the number of slots emitted,
	// INCLUDING empty-slot sentinels (the decoder loops `count` times, an empty slot is
	// one iteration that consumes a bare `[u16 0]`).
	w.u16(batch.start_index);
	w.u16(uint16_t(batch.records.size()));

	for (const StaticEntityRecord &rec : batch.records) {
		// Empty-slot sentinel: bare `[u16 0]`, no body (decode reads item_type_id == 0
		// and continues to the next record).
		if (rec.is_empty_slot || rec.item_type_id == 0) {
			w.u16(0);
			continue;
		}
		w.u16(rec.item_type_id);

		uint16_t f = 0;
		if (rec.euler_z)      f |= 0x0001; // entity+16 yaw heading (32-bit BAM)
		if (rec.euler_x)      f |= 0x0002; // entity+20
		if (rec.euler_y)      f |= 0x0004; // entity+24
		if (rec.section_mask) f |= 0x0008; // entity+308
		if (rec.team_byte)    f |= 0x0010; // entity+354 (D-NET-58/62)
		if (rec.entity_flags) f |= 0x0020; // entity+36 Flags dword (D-NET-147)
		if (rec.bone_a)       f |= 0x0040; // entity+533 (D-NET-94)
		if (rec.bone_b)       f |= 0x0080; // entity+532 (D-NET-94)
		if (rec.score_flag)   f |= 0x0100; // entity+624
		// attach_ref is written when `weapon_byte != 0 || flags & 0x200`; force the 0x200
		// gate only when attach_ref is populated but weapon_byte is zero (else weapon_byte
		// already triggers the write and 0x200 would be redundant).
		if (rec.attach_ref && rec.weapon_byte == 0) f |= 0x0200;

		w.u16(f);
		w.u32(uint32_t(rec.pos_x)); // entity+4
		w.u32(uint32_t(rec.pos_y)); // entity+8
		w.u32(uint32_t(rec.pos_z)); // entity+12

		if (f & 0x0001) w.u32(uint32_t(rec.euler_z));
		if (f & 0x0002) w.u32(uint32_t(rec.euler_x));
		if (f & 0x0004) w.u32(uint32_t(rec.euler_y));
		if (f & 0x0008) w.u32(uint32_t(rec.section_mask));
		if (f & 0x0010) w.u8(rec.team_byte);
		if (f & 0x0020) w.u32(rec.entity_flags);
		w.u8(rec.ammo_count);          // ALWAYS, entity+290
		if (f & 0x0040) w.u8(rec.bone_a);
		if (f & 0x0080) w.u8(rec.bone_b);
		if (f & 0x0100) w.u8(rec.score_flag);
		w.u8(rec.weapon_byte);         // ALWAYS, entity+538
		if (rec.weapon_byte != 0 || (f & 0x0200)) w.u16(rec.attach_ref); // entity+350
	}

	return out;
}

// [orig: serialize_terrain_tiles @ 0x6080F0] — the inverse of decode_terrain_load_batch
// (§5.37, D-NET-83). One paged chunk of the terrain-tile (.til) load stream. The header chunk
// writes the wire start word 0xFFFF, end_index, then the `'til0'` magic + total tile_count +
// hdr2/hdr3; continuation chunks write `[start_index][end_index]`. Every chunk then writes its
// (end_index - start_index) opaque 12-B tile entries. Byte-identical round-trip with the decoder.
std::vector<uint8_t> encode_terrain_load_batch(const TerrainLoadBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};

	if (batch.has_header) {
		w.u16(0xFFFFu);                                            // first-chunk sentinel (start_index := 0)
		w.u16(batch.end_index);
		w.u32(batch.magic != 0 ? batch.magic : 0x74696C30u);      // 'til0' (reader bails on mismatch)
		w.u32(batch.tile_count);                                   // total tiles in the full set
		w.u32(batch.header_field2);                                // g_TerrainTileData[2]
		w.u32(batch.header_field3);                                // g_TerrainTileData[3]
	} else {
		w.u16(batch.start_index);
		w.u16(batch.end_index);
	}

	for (const TerrainTileEntry &e : batch.tiles) {               // 12-B opaque records (never interpreted)
		w.u32(e.word0);
		w.u32(e.word1);
		w.u32(e.word2);
	}
	return out;
}

// [orig: NapiNPClientMsg_0x00C @ 0x42E730] — the inverse of decode_organic_spawn_batch
// (§5.23). Header u16 entity_count, then per record: u16 slot_id, u8 has_body, and (when
// has_body) the unconditional field block. A field-identical round-trip with the decoder.
std::vector<uint8_t> encode_organic_spawn_batch(const OrganicSpawnBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(batch.entity_count);
	for (const OrganicSpawnRecord &rec : batch.records) {
		w.u16(rec.slot_id);
		w.u8(rec.has_body ? 1 : 0);
		if (!rec.has_body) continue; // empty spawn ends after the has_body byte (@ 0x42e813)
		w.u16(rec.item_type_id);
		w.u32(rec.entity_flags);
		w.cstr(rec.entity_name);
		w.u16(rec.minimap_flags);
		w.u32(uint32_t(rec.pos_x));
		w.u32(uint32_t(rec.pos_y));
		w.u32(uint32_t(rec.pos_z));
		w.u32(uint32_t(rec.orientation));
		w.u8(rec.team);
		w.u8(rec.ai_state);
		w.u8(rec.anim_slot);
		w.u16(rec.net_id);
		w.u8(rec.player_class);
		w.u8(rec.ai_action);
		w.u8(rec.skip_byte);
		w.u8(rec.unused_byte);
		w.u8(rec.alert_level);
		w.u8(rec.sub_type);
		w.u8(rec.weapon_type);
		w.u8(rec.parent_slot);
		w.u16(rec.parent_handle);
	}
	return out;
}

// [orig: serialize_object_to_buffer @ 0x504d10] — the S2C 0x18 reply body. The
// original recomputes the leading handle from the entity pointer (pool scan) and
// resolves the three link pointers to handles (0xFFFF when null); here both arrive
// pre-resolved on the record. An itemDef-null slot sends type_id/item_type 0 and
// the empty name — the same bytes this encoder produces from a default record.
std::vector<uint8_t> encode_full_entity_spawn(const FullEntitySpawnRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(rec.slot_id);            // [0x504d5f]
	w.u16(rec.item_type_id);       // itemDef+0x50 low16 [0x504d79]
	w.u8(rec.item_type);           // itemDef+0x5C low8  [0x504dc8]
	w.u8(rec.team);                // entity+354         [0x504dee]
	w.u16(rec.minimap_flags);      // entity+36          [0x504e00]
	w.u32(rec.entity_flags);       // entity+120         [0x504e12]
	w.cstr(rec.entity_name);       // entity+244 / g_empty_str [0x504e5a / 0x504e7c]
	w.u16(rec.parent_vehicle_handle); // entity+368 → handle [0x504ec5]
	w.u16(rec.ground_entity_handle);  // entity+40  → handle [0x504f3a]
	w.u16(rec.parent_entity_handle);  // entity+364 → handle [0x504fb4]
	w.u8(rec.seat_mask);           // itemDef+604 [0x505004]
	for (int bit = 0; bit < 8; ++bit) { // one occupant handle per set bit [0x50504c..0x5050da]
		if ((rec.seat_mask & (1u << bit)) != 0) w.u16(rec.mount_handles[bit]);
	}
	w.u16(rec.mount_handle_8);     // entity+416 [0x505118]
	w.u16(rec.mount_handle_9);     // entity+418 [0x50512e]
	w.u32(uint32_t(rec.pos_x));    // entity+4  [0x505140]
	w.u32(uint32_t(rec.pos_y));    // entity+8  [0x505153]
	w.u32(uint32_t(rec.pos_z));    // entity+12 [0x505164]
	w.u16(rec.heading_hi);         // entity+18 [0x505176]
	w.u16(rec.pitch_hi);           // entity+22 [0x505189]
	w.u8(rec.ai_state);            // entity+692 [0x50519e]
	w.u8(rec.anim_slot);           // entity+884 [0x5051b2]
	w.u16(rec.net_id);             // entity+348 [0x5051c6]
	w.u8(rec.player_class);        // entity+660 [0x5051db]
	w.u8(0);                       // hard 0 in the original [0x5051e8]
	w.u8(rec.unused_byte);         // entity+340 [0x5051fd]
	w.u8(rec.alert_level);         // entity+533 [0x505211]
	w.u8(rec.sub_type);            // entity+532 [0x50522c]
	return out;
}

// [orig: NetPacket_SerializeInfantryEntityState case 1 (write, type 11) @ 0x4C0320]
// Fixed 14 B; positions are the already-compressed u16s (see ingame_encode.h).
std::vector<uint8_t> encode_infantry_compact_record(const InfantryCompactRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(rec.seat_bone_idx);         // entity+343 if mounted else 0 [orig: 0x4c0700 / 0x4c072e]
	w.u16(rec.vehicle_slot_handle);  // (pool<<12)|slot or 0xFFFF    [orig: 0x4c0780 / 0x4c07b1]
	w.u16(rec.pos_x_compressed);     // CompressFixedPoint(world/local) [orig: 0x4c07f2 / 0x4c0884]
	w.u16(rec.pos_y_compressed);     // [orig: 0x4c0817 / 0x4c089e]
	w.u16(rec.pos_z_compressed);     // [orig: 0x4c083c / 0x4c08c5]
	w.u8(rec.yaw_byte);              // (entity+16 + 0x800000) >> 24  [orig: 0x4c08f8]
	w.u8(rec.flags_byte);            // entity+36                     [orig: 0x4c0917]
	w.u8(rec.pitch_byte);            // entity+748 (clamped)          [orig: 0x4c0960]
	w.u8(rec.aim_yaw_byte);          // entity+720                    [orig: 0x4c0983]
	w.u8(rec.anim_byte);             // entity+696 ?: entity+700      [orig: 0x4c09ae]
	return out; // 14 B
}

// [orig: Entity_SerializeVehicleState case 1 (write, type 11) @ 0x460560]
// 15 B mounted / 21 B unmounted; positions are already-compressed u16s.
std::vector<uint8_t> encode_vehicle_compact_record(const VehicleCompactRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(rec.parent_slot_handle);    // (pool<<12)|slot or 0xFFFF  [orig: 0x460ba1 / 0x460c61]
	w.u16(rec.pos_x_compressed);      // CompressFixedPoint(local/world) [orig: 0x460bda / 0x460c92]
	w.u16(rec.pos_y_compressed);      // [orig: 0x460bff / 0x460cbc]
	w.u16(rec.pos_z_compressed);      // [orig: 0x460c24 / 0x460ce6]
	w.u16(uint16_t(rec.euler_z));     // Euler Z, (v+0x8000)>>16 BAM high  [orig: 0x460d0a]
	w.u8(rec.flags_byte);             // entity+36                   [orig: 0x460d22]

	// Mounted (entity+36 & 4) emits the other two Euler components; unmounted emits
	// the full turret/weapon-aim block. The shared trailing write @ 0x460e10 carries
	// Euler X when mounted (src 0x460d52) and the weapon heading BAM when not
	// (src 0x460def). [orig: branch @ 0x460d2f]
	if (rec.flags_byte & 0x04) {
		w.u16(uint16_t(rec.euler_y));           // entity+24 Euler Y  [orig: 0x460d4c]
		w.u16(uint16_t(rec.euler_x));           // entity+20 Euler X  [orig: 0x460d52 -> 0x460e10]
	} else {
		w.u16(rec.weapon_x);                    // entity+160         [orig: 0x460d7b]
		w.u16(rec.health_word);                 // entity+286 vehicle HEALTH u16 [orig: 0x460d9b;
		                                        // read stores it @0x460aff — 0 kills the vehicle]
		w.u16(rec.weapon_aim_y);                // vehicleData[136]   [orig: 0x460dc2]
		w.u16(rec.weapon_aim_z);                // vehicleData[135]   [orig: 0x460de9]
		w.u16(uint16_t(rec.weapon_heading_bam));// vehicleData[132]   [orig: 0x460def -> 0x460e10]
	}
	return out; // 15 or 21 B
}

// [orig: NetPacket_SerializePlayerState case 1 (write compact, type 11) @ 0x4C09C0; §5.10]
// 18 B fixed; the inverse of decode_player_compact_record (see ingame_encode.h on
// why the layout is cited from the landed §5.10 grill).
std::vector<uint8_t> encode_player_compact_record(const PlayerCompactRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(rec.vehicle_bone);       // entity+0x157
	w.u8(rec.seat_type);          // local seat-type byte
	w.u16(rec.carrier_handle);    // (pool<<12)|slot or 0xFFFF
	w.u16(rec.pos_x_compressed);  // CompressFixedPoint(entity+4; vehicle-local if mounted)
	w.u16(rec.pos_y_compressed);  // entity+8
	w.u16(rec.pos_z_compressed);  // entity+0xC
	w.u8(rec.yaw_byte);           // entity+0x14 high byte
	w.u8(rec.pitch_byte);
	w.u8(rec.move_input_byte);      // entity+0x12C
	w.u8(rec.state_flags);        // entity+0x24 (bit2 spawning, bit4 mounted)
	w.u8(rec.anim_state_id);          // entity+0x2B8 ?: entity+0x2BC [orig: @0x4c0cc7]
	w.u8(rec.anim_channel_ratio); // entity+0x188 channel ratio [orig: @0x4c0cf2]
	w.u8(rec.anim_def_index);     // entity+0x2B0 [orig: @0x4c0d59]
	w.u8(rec.health_class_byte);  // -> Entity_SetHealthFromDifficultyByte on read
	return out; // 18 B
}

// [orig: Entity_SerializeGuidedMissileState write modes (case 1 / case 3) @ 0x447C50]
// Write ONE field group — the inverse of decode_guided_field_group. `full` =
// WriteFull (mode 1); the delta path (WriteDelta, mode 3) drops the target handle
// from group 4 and writes only the target handle for group 3.
std::vector<uint8_t> encode_guided_field_group(GuidedMode mode,
                                               GuidedFieldGroup group,
                                               const GuidedRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	const bool full = (mode == GuidedMode::WriteFull);
	switch (group) {
	case GuidedFieldGroup::Status:
	case GuidedFieldGroup::ClearTarget:
		w.u8(0);                          // [orig: LABEL_3 @ 0x447c98] 1-byte marker
		break;
	case GuidedFieldGroup::TargetPos:     // [orig: 0x447cd6 (full) / 0x447fdb (delta)]
		w.u16(rec.target_slot);           // entity+724
		if (full) {                       // full writes pos; delta writes target only
			w.u32(uint32_t(rec.pos_x));   // entity+700
			w.u32(uint32_t(rec.pos_y));   // entity+704
			w.u32(uint32_t(rec.pos_z));   // entity+708
		}
		break;
	case GuidedFieldGroup::TargetTypePos: // [orig: 0x447d85 (full) / 0x447d9c (delta)]
		if (full) w.u16(rec.target_slot); // entity+724 (full only)
		w.u32(rec.weapon_type);           // entity+698 (4-B field, low u16 significant)
		w.u32(uint32_t(rec.pos_x));
		w.u32(uint32_t(rec.pos_y));
		w.u32(uint32_t(rec.pos_z));
		break;
	case GuidedFieldGroup::Pos:           // [orig: 0x447ced]
		w.u32(uint32_t(rec.pos_x));
		w.u32(uint32_t(rec.pos_y));
		w.u32(uint32_t(rec.pos_z));
		break;
	case GuidedFieldGroup::AttachOffsets: // [orig: 0x447df3] entity+740/744/748
		w.u32(uint32_t(rec.attach_x));
		w.u32(uint32_t(rec.attach_y));
		w.u32(uint32_t(rec.attach_z));
		break;
	}
	return out;
}

// §5.9.1 round-event record — the host write side of the tag-2 stream.
// [orig: NetPacket_SerializeRoundEvent @ 0x504820]. 17-20 B by the flags gate
// (0x80 adds slot_byte @0x504919, 0x40 adds target_handle @0x504953).
std::vector<uint8_t> encode_round_event_record(const RoundEventRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(rec.flags);
	w.u8(rec.adm_index);
	w.u8(rec.subtype);
	if (rec.flags & 0x80) w.u8(rec.slot_byte);
	w.u16(rec.shooter_handle);
	if (rec.flags & 0x40) w.u16(rec.target_handle);
	w.u16(rec.shot_seq);
	w.u16(rec.pos_x_compressed);
	w.u16(rec.pos_y_compressed);
	w.u16(rec.pos_z_compressed);
	w.u16(rec.yaw_bam_high);
	w.u16(rec.pitch_bam_high);
	return out;
}

// Build a complete S2C 0x0A frame from a FrameUpdate — the inverse of the
// decode_frame_update walk (§5.9). [orig: NapiNPClientMsg_0x00A @ 0x42FEC0].
// 12-B anchor, flags1/flags2, the flags2&3 sub-block (aim/timer/env/objective),
// the 7-B tail, the conditional passenger record, then the event loop (tag=1
// per-entity compact via the §5.10b class dispatch, tag=2 weapon-hit) and the
// tag=0 terminator. Known null-callback classes deliberately emit a header-only
// tag=1 record; Guided/Unknown records are skipped because they have no fixed
// 0x0A compact width.
std::vector<uint8_t> encode_frame_update(const FrameUpdate &fu) {
	std::vector<uint8_t> out;
	Writer w{out};
	auto append = [&](const std::vector<uint8_t> &v) {
		out.insert(out.end(), v.begin(), v.end());
	};

	w.u32(uint32_t(fu.anchor_x));
	w.u32(uint32_t(fu.anchor_y));
	w.u32(uint32_t(fu.anchor_z));
	w.u8(fu.flags1);
	w.u8(fu.flags2);

	switch (fu.flags2 & 0x03) {
	case 0: // weapon/reload/uniform (11 B) [orig: NetPacket_WritePlayerState @0x4ff81b]
		w.u8(fu.weapon.preround_timer); w.u8(fu.weapon.slot_state360); w.u8(fu.weapon.slot_state368);
		w.u8(fu.weapon.slot_state364); w.u8(fu.weapon.slot_state356); w.u8(fu.weapon.slot_state460);
		w.u8(fu.weapon.reload_seconds);
		w.u32(uint32_t(fu.weapon.uniform_team_mask));
		break;
	case 1: // timer (6 B)
		w.u8(fu.timer.state0); w.u8(fu.timer.state1);
		w.u8(fu.timer.state2); w.u8(fu.timer.state3);
		w.u16(uint16_t(fu.timer.timer_seconds));
		break;
	case 2: // env (11 B)
		w.u16(fu.env.fog_dist); w.u16(fu.env.fog_accel); w.u16(fu.env.tod_fixed);
		w.u8(fu.env.quake_ticks); w.u8(fu.env.cloud_scroll); w.u8(fu.env.cloud_param2);
		w.u8(fu.env.overcast); w.u8(fu.env.env_param);
		break;
	default: // sub-block 3: 16 B only when the off-wire objective-gametype gate is active
		if (fu.objective.present) {
			w.u32(uint32_t(fu.objective.state[0]));
			w.u32(uint32_t(fu.objective.state[1]));
			w.u32(uint32_t(fu.objective.state[2]));
			w.u32(uint32_t(fu.objective.state[3]));
		}
		break;
	}

	// 7-byte tail (local-player state).
	w.u8(fu.state_flag_byte);
	w.u16(fu.mount_handle);
	w.u16(uint16_t(fu.health));
	w.u16(uint16_t(fu.state_word));

	// Conditional passenger record.
	if ((fu.flags2 & 0x0F) == 8) {
		w.u16(fu.passenger.handle);
		if (fu.passenger.handle != 0xFFFF) {
			w.u16(fu.passenger.seat_yaw);
			w.u16(fu.passenger.seat_pitch);
		}
	}

	// Event loop: tag=1 per-entity compacts, then tag=2 round events, then tag=0.
	// (The original interleaves 1/2 pairs under the send budget
	// [orig: serialize_entity_states_to_packet @0x50f070]; the retail decode loop
	// is tag-driven, so grouped order is read identically.)
	for (const FrameUpdateRecord &rec : fu.records) {
		switch (rec.cls) {
		case EntityClass::Player:
			w.u8(1);
			w.u16(rec.handle);
			w.u16(rec.type_id);
			append(encode_player_compact_record(rec.player));
			break;
		case EntityClass::Vehicle:
			w.u8(1);
			w.u16(rec.handle);
			w.u16(rec.type_id);
			append(encode_vehicle_compact_record(rec.vehicle));
			break;
		case EntityClass::Infantry:
			w.u8(1);
			w.u16(rec.handle);
			w.u16(rec.type_id);
			append(encode_infantry_compact_record(rec.infantry));
			break;
		case EntityClass::NoNetworkCallback:
			w.u8(1);
			w.u16(rec.handle);
			w.u16(rec.type_id);
			break;
		default:
			break;
		}
	}
	for (const RoundEventRecord &ev : fu.round_events) {
		w.u8(2);
		append(encode_round_event_record(ev));
	}
	w.u8(0); // event-loop terminator
	return out;
}

// [orig: Pool_SerializeEntityViaVTable @ 0x4D64E0] — the 5-byte C2S 0x0C sub-header.
std::vector<uint8_t> encode_entity_packet_sub_header(const EntityPacketSubHeader &hdr) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(hdr.handle);
	w.u16(hdr.item_type_id);
	w.u8(hdr.sub_op);
	return out;
}

// [orig: Entity_FireWeaponAndSendPacket @0x42bd80] -- the fixed 45-byte
// descriptor queued by the joiner's locally predicted fire action.
void set_client_fired_round_pose(
		ClientFiredRound &round,
		const std::array<int32_t, 5> &fire_pose,
		const std::array<int32_t, 5> &shooter_pose) {
	const auto rounded_high_word = [](int32_t angle) {
		const uint32_t high =
				(static_cast<uint32_t>(angle) + 0x8000u) >> 16;
		return high < 0x8000u
				? static_cast<int32_t>(high)
				: static_cast<int32_t>(high) - 0x10000;
	};
	const auto low_word_delta = [](int32_t fire, int32_t shooter) {
		return static_cast<uint16_t>(
				static_cast<uint16_t>(fire) - static_cast<uint16_t>(shooter));
	};
	round.pos_x = fire_pose[0];
	round.pos_y = fire_pose[1];
	round.pos_z = fire_pose[2];
	round.dir_x = rounded_high_word(fire_pose[3]);
	round.dir_y = rounded_high_word(fire_pose[4]);
	round.delta_x = low_word_delta(fire_pose[0], shooter_pose[0]);
	round.delta_y = low_word_delta(fire_pose[1], shooter_pose[1]);
	round.delta_z = low_word_delta(fire_pose[2], shooter_pose[2]);
	round.delta_yaw = low_word_delta(fire_pose[3], shooter_pose[3]);
	round.delta_pitch = low_word_delta(fire_pose[4], shooter_pose[4]);
}

std::vector<uint8_t> encode_client_fired_round(const ClientFiredRound &r) {
	std::vector<uint8_t> out;
	out.reserve(45);
	Writer w{out};
	w.u32(r.current_tick);
	w.u16(r.shooter_handle);
	w.u8(r.fire_flags);
	w.u8(r.adm_index);
	w.u32(static_cast<uint32_t>(r.pos_x));
	w.u32(static_cast<uint32_t>(r.pos_y));
	w.u32(static_cast<uint32_t>(r.pos_z));
	w.u32(static_cast<uint32_t>(r.dir_x));
	w.u32(static_cast<uint32_t>(r.dir_y));
	w.u16(r.target_handle);
	w.u16(r.hit_part);
	w.u8(r.extra_byte1);
	w.u8(r.extra_byte2);
	w.u8(r.misc_byte);
	w.u16(r.delta_x);
	w.u16(r.delta_y);
	w.u16(r.delta_z);
	w.u16(r.delta_yaw);
	w.u16(r.delta_pitch);
	return out;
}

// [orig: NetPacket_SerializePlayerState case 3/4 @ 0x4C09C0] -- the 43-B extended
// uplink body, the exact bytes decode_player_extended_uplink consumes. The host
// driver fills PlayerExtendedUplink from the joiner's owned entity; this writes the
// wire bytes. Positions are i32 16.16 -- CARRIER-LOCAL (and heading carrier-relative)
// when carrier_handle != 0xFFFF, absolute world otherwise (section 5.10, D-NET-151).
std::vector<uint8_t> encode_player_extended_uplink(const PlayerExtendedUplink &r) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(r.carrier_handle);
	w.u32(uint32_t(r.pos_x));
	w.u32(uint32_t(r.pos_y));
	w.u32(uint32_t(r.pos_z));
	w.u16(uint16_t(r.heading));
	w.u16(uint16_t(r.pitch));
	w.u8(r.anticheat_flags);
	w.u8(r.move_input_byte);
	w.u8(r.state_flags_byte);
	w.u8(r.analog_x);
	w.u8(r.analog_y);
	w.u8(r.analog_z);
	w.u8(r.equipped_adm_index);
	w.u8(r.stat_byte_0);
	w.u8(r.stat_byte_1);
	w.u16(r.priority_handle_0);
	w.u16(r.priority_score_0);
	w.u16(r.priority_handle_1);
	w.u16(r.priority_score_1);
	w.u16(r.priority_handle_2);
	w.u16(r.priority_score_2);
	w.u16(r.priority_handle_3);
	w.u16(r.priority_score_3);
	return out;
}

// ---------------------------------------------------------------------------
// §5.1 reply-body encoders (relocated from npruntime/server_message_dispatch.cpp so encode + decode
// share the lib). The host-side input is the PlayerReplicationState reply POD.
// ---------------------------------------------------------------------------

// [orig: NetPacket_SerializePlayerSync0x46 @0x505e80] — round-trips through decode_player_sync.
// The reply mask is serialized VERBATIM and gates each field, so the server answers exactly the
// requested fieldFlags (ack bit 0x4000 included) [orig: the per-bit field writes; ack echo
// @0x505f05]. Field order matches the witnessed write sites (ascending @0x505f9b..@0x506230)
// and the decode_player_sync read order.
std::vector<uint8_t> encode_player_sync(const PlayerReplicationState &ctx, uint16_t field_flags) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(ctx.player_slot);
	w.u16(field_flags);
	w.u8(static_cast<uint8_t>(ctx.entity_handle & 0x00FFu)); // pool-0 entity index [orig: Pool_GetIndexFromPtr @0x505f40]
	if (field_flags & 0x0001u)
		w.cstr_capped(ctx.player_name, 32); // name (variable-length, [orig: slot+40 @0x505f9b])
	if (field_flags & 0x0002u)
		w.cstr_capped(ctx.clan_tag, 16);    // team-string — retail ALWAYS writes "" here (@0x505ff7)
	if (field_flags & 0x0010u)
		w.cstr_capped(std::string(), 16);   // vehicle-name — "" for an on-foot player (@0x50601f)
	if (field_flags & 0x0004u)
		w.u8(ctx.team); // team byte [orig: slot+416; client -> playerSlot+14 + entity+354]
	if (field_flags & 0x0008u)
		w.u8(0);        // class/subtype byte (outside the 0x1CF7 set; slot-state default 0)
	if (field_flags & 0x0020u)
		w.u8(0);        // vehicle score byte [orig: vehicle+156 when mounted, else 0 @0x50613b]
	if (field_flags & 0x1000u)
		w.u8(0);        // late-join flag [orig: slot+100567 && !slot+100579 @0x506197]
	if (field_flags & 0x0040u)
		w.u8(0xFF);     // squad [orig: slot+100576, init -1 at Server_PlayerAdd @0x51d4e0]
	if (field_flags & 0x0080u)
		w.u8(0);        // side [orig: slot+100577]
	if (field_flags & 0x0400u)
		w.u8(1);        // quality [orig: slot+418; client clamps <=4 @0x431370]
	if (field_flags & 0x0800u)
		w.u32(0);       // vehicle timer dword [orig: vehicle_data+420 when mounted, else 0 @0x506230]
	return out;
}

std::vector<uint8_t> encode_player_sync_removal(uint8_t slot, bool with_ack) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(slot);
	// 0x8000 removal + optional 0x4000 ack (golden's 0xC000): the client clears the slot and, with the
	// ack bit, requests the next one — so the walk terminates at max_players. [orig: NapiNPClientMsg_PlayerSync
	// @0x431370 — a 0x8000 record reads NO entity slot / fields, just [u8 slot][u16 flags].]
	w.u16(static_cast<uint16_t>(0x8000u | (with_ack ? 0x4000u : 0u)));
	return out;
}

// (encode_player_spawn — the invented unconditional S2C 0x51 "spawn confirm" — was REMOVED,
// D-NET-148: the original only sends 0x51 for a pending g_team_change_entity_list entry
// [orig: NapiNPServerMsg_0x029 @0x514F10 -> write_entity_packet @0x506bb0], and the client
// FIELD-PARSES it (NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 rebinds CharacterEntity from
// the packed char id) — a zeroed id re-bound the joiner to a vehicle archetype: the DBuggy1
// shadow. Port the real record from @0x506bb0 when the team-change flow lands.)

// [orig: NetPacket_SerializeScoreboard0x16 @0x504b80 (write) / NapiNPClientMsg_PlayerList
// @0x42FAE0 (read)] — round-trips through decode_player_list. Byte 0 is a FLAGS byte (bit0
// team-mode, bit1 timed-scores — the old `max` reading was a misnomer); the trailer carries
// the LIVE [inGameCount][spectatorCount] pair. The HUD "Number of players" = accepted rows −
// spectatorCount (g_scoreboard_row_count − g_scoreboard_spectator_count) — a hardcoded trailer pinned
// every client's count at 2 (the v31 HUD-count defect, D-NET-158). Rows are accepted only for
// 0x46-known slots; spectators are unmodeled (0).
std::vector<uint8_t> encode_player_list(const std::vector<PlayerListEntry> &players) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(0x01);                                  // flags: bit0 team-mode [orig: -> g_scoreboard_flags]
	w.u8(static_cast<uint8_t>(players.size()));  // row count
	for (const PlayerListEntry &e : players) {
		w.u8(e.slot);
		w.u16(0); // ping
		w.u16(0); // score
		w.u16(0); // deaths
		w.u8(static_cast<uint8_t>(e.team << 1)); // bits1+ team, bit0 SPECTATOR (unmodeled 0)
	}
	w.u8(0x02); // team_count = 2 (matches retail)
	for (int i = 0; i < 3; ++i) { // (team_count + 1) blocks {score, deaths, kothHold, ctfFlag}
		w.u16(0); w.u16(0);
		w.u8(0); w.u8(0);
	}
	w.u8(static_cast<uint8_t>(players.size())); // inGameCount  [orig: -> g_scoreboard_ingame_count]
	w.u8(0x00);                                 // spectatorCount [orig: -> g_scoreboard_spectator_count]
	return out;
}

// [orig: Server_SendWeaponSlotListToPlayer @ 0x502550] — one 4-byte group per weapon slot
// ([u8 admIdx @0x50273b][u8 ammo/status @0x502794][u8 alt @0x50286f][u8 restriction @0x50288a]),
// 0xFF terminator @0x5028b5, after the leading avatar-class byte (playerSlot+89820 @0x5025e6).
std::vector<uint8_t> encode_weapon_loadout(const WeaponLoadout &loadout) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(loadout.avatar_class);
	for (const WeaponLoadoutSlot &s : loadout.slots) {
		w.u8(s.type_id);
		w.u8(s.ammo_primary);
		w.u8(s.ammo_secondary);
		w.u8(s.ammo_alt);
	}
	w.u8(0xFF);
	return out;
}

// [orig: NetPacket_SendLoadoutSubmit @ 0x42cdc0 — header stores @0x42cea3..0x42ceae,
// 4-byte row group @0x42cf35/@0x42d021..0x42d045, terminator @0x42d076]
std::vector<uint8_t> encode_loadout_submit(const LoadoutSubmit &submit) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(submit.team);
	w.u8(submit.player_class);
	w.u32(submit.weapon_slot_index);
	for (const LoadoutSubmitEntry &e : submit.entries) {
		w.u8(e.adm_index);
		w.u8(e.ammo_primary);
		w.u8(e.ammo_secondary);
		w.u8(e.variant);
	}
	w.u8(0xFF);
	return out;
}

// [orig: NapiNPServerMsg_HandleReloadRequest @ 0x514DF0 — S2C 0x49 carries the same
// [u16 handle][u16 weaponSlotCombo] payload as the C2S 0x25 request it relays (§5.58)]
std::vector<uint8_t> encode_weapon_reload(const WeaponReload &reload) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(reload.entity_handle);
	w.u16(reload.reload_param);
	return out;
}

// [orig: NapiNPServerMsg_SendEmptySlots @ 0x51a600 -> the pool-0 walk + append
//  builder @ 0x5160f0 — a bare index run, no count word]
std::vector<uint8_t> encode_destroy_entity_list(const DestroyEntityList &list) {
	std::vector<uint8_t> out;
	Writer w{out};
	for (uint16_t index : list.pool0_indices) w.u16(index);
	return out;
}

// [orig: Server_ChangeEntityTeam @ 0x518D70; the client field order is the read
//  order of NapiNPClientMsg_0x050 @ 0x431910]
std::vector<uint8_t> encode_team_assign(const TeamAssign &assign) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(assign.entity_handle);
	w.u8(assign.team);
	// Both zero for a non-player entity in retail (the Flags & 0x100 gate @0x506b3d);
	// the caller owns that gate. [orig: write_entity_handle_packet @ 0x506ad0]
	w.u16(assign.net_id);
	w.u8(assign.anim_slot);
	return out;
}

} // namespace opennova
