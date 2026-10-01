#include <net/npwire/ingame_encode.h>
#include <net/npwire/wire_handle.h>
#include <base/io/le.h>
#include <formats/wac/command.h>

#include <algorithm>
#include <limits>

// Encoders for the in-match S2C replication tags — the symmetric partners to
// ingame_decode.cpp. Faithful structural ports of the original server-side
// serializers. Verified inverse pair:
//   encode_pool3_sync_batch  ← [orig: NetPacket_SerializeEntityPoolToPacket @ 0x503460]
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

// [orig: NetPacket_SerializeEntityPoolToPacket @ 0x503460]
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
		if (rec.movement_val)    flags |= kPool3SyncHasMovementVal;    // entry+16 raw BAM heading [orig: 0x50357e] (D-NET-59)
		if (rec.orientation_val) flags |= kPool3SyncHasOrientationVal; // entry+0   [orig: 0x503593]
		if (rec.ammo_count)      flags |= kPool3SyncHasAmmoCount;      // entry+290 [orig: 0x5035b7]
		if (rec.team_byte)       flags |= kPool3SyncHasTeamByte;       // entry+354 [orig: 0x5035f1]
		if (rec.weapon_type)     flags |= kPool3SyncHasWeaponType;     // entry+640 [orig: 0x50360b]
		if (rec.score_byte)      flags |= kPool3SyncHasScoreByte;      // entry+672 [orig: 0x503633]

		w.u8(flags);                    // [orig: *flags_location = flags @ 0x503660]
		w.u32(uint32_t(rec.pos_x));     // entry+4  [orig: 0x503553]
		w.u32(uint32_t(rec.pos_y));     // entry+8  [orig: 0x503565]
		w.u32(uint32_t(rec.pos_z));     // entry+12 [orig: 0x503577]

		if (flags & kPool3SyncHasMovementVal) w.u32(rec.movement_val);    // [orig: 0x50358f] (D-NET-59)
		if (flags & kPool3SyncHasOrientationVal) w.u32(rec.orientation_val); // [orig: 0x5035a9]
		if (flags & kPool3SyncHasAmmoCount) w.u16(rec.ammo_count);      // [orig: 0x5035cc]
		w.u16(rec.net_handle);                         // ALWAYS [orig: 0x5035e2, entry+124]
		if (flags & kPool3SyncHasTeamByte) w.u8(rec.team_byte);        // [orig: 0x503603]
		if (flags & kPool3SyncHasWeaponType) w.u16(rec.weapon_type);     // [orig: 0x50362a]
		if (flags & kPool3SyncHasScoreByte) w.u8(rec.score_byte);       // [orig: 0x503650]
	}

	return out;
}

// [orig: NetPacket_SerializeEntityPoolToPacket_0 @ 0x503940]
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
		if (rec.entity_flags)              f |= kPoolSpawnHasEntityFlags; // entity+36   [orig: 0x503ae6]
		if (rec.euler_z)                   f |= kPoolSpawnHasEulerZ; // entity+16 yaw [orig: 0x503b3c]
		if (rec.euler_x)                   f |= kPoolSpawnHasEulerX; // entity+20   [orig: 0x503b58]
		if (rec.euler_y)                   f |= kPoolSpawnHasEulerY; // entity+24   [orig: 0x503b74]
		if (rec.section_mask)              f |= kPoolSpawnHasSectionMask; // entity+308  [orig: 0x503b93]
		if (rec.team_byte)                 f |= kPoolSpawnHasTeamByte; // entity+354 team [orig: 0x503bb2] (D-NET-58)
		if (rec.parent_handle != wire_handle::kInvalid) f |= kPoolSpawnHasParentHandle; // entity+368  [orig: 0x503bd1]
		if (rec.target_handle != wire_handle::kInvalid) f |= kPoolSpawnHasTargetHandle; // entity+40   [orig: 0x503c27]
		if (rec.seat_mask)                 f |= kPoolSpawnHasMountOccupancy; // itemDef+604 [orig: 0x503c83]
		// The two pointer gates ride their presence fields: the AI slot
		// (entity+0x68) and the vehicle brain (entity+0x64), whatever the values.
		if (rec.has_ai_trailer)            f |= kPoolSpawnHasAiTrailer; // aiSlot      [orig: 0x503d53]
		if (rec.alert_byte)                f |= kPoolSpawnHasRefNum; // entity+533  [orig: 0x503e3c]
		if (rec.action_byte)               f |= kPoolSpawnHasSubType; // entity+532  [orig: 0x503e60]
		if (rec.has_sound_latch_byte)      f |= kPoolSpawnHasSoundLatchByte; // entity+100  [orig: 0x503e7f]
		// The zone block and the 0x4000 byte ride presence too: the zone number byte
		// and the def's SpawnPoint attrib / callbacks gate them, not the written values.
		if (rec.has_zone_number_rank)      f |= kPoolSpawnHasZoneNumberRank; // entity+538 != 0 [orig: 0x503ecc]
		else if (rec.has_zone_radius_alt)  f |= kPoolSpawnHasZoneRadiusAlt; // itemDef+0x40000 path [orig: 0x503f29]
		if (rec.has_difficulty_byte)       f |= kPoolSpawnHasDifficultyByte; // def callbacks [orig: 0x503f4c]

		w.u16(f);                       // spawn_flags  [orig: *flags_write_pos @ 0x503f90]
		w.u16(rec.slot_id);             // packed handle [orig: 0x503a3f]
		w.u16(rec.item_type_id);        // [orig: 0x503a57]
		w.cstr(rec.entity_name);        // AI: entity+244, else empty [orig: 0x503a64..]

		if (f & kPoolSpawnHasEntityFlags) w.u32(rec.entity_flags); // [orig: 0x503afc]
		w.u32(uint32_t(rec.pos_x));     // entity+4  [orig: 0x503b0f]
		w.u32(uint32_t(rec.pos_y));     // entity+8  [orig: 0x503b22]
		w.u32(uint32_t(rec.pos_z));     // entity+12 [orig: 0x503b35]

		if (f & kPoolSpawnHasEulerZ) w.u32(uint32_t(rec.euler_z));
		if (f & kPoolSpawnHasEulerX) w.u32(uint32_t(rec.euler_x));
		if (f & kPoolSpawnHasEulerY) w.u32(uint32_t(rec.euler_y));
		if (f & kPoolSpawnHasSectionMask) w.u32(uint32_t(rec.section_mask));
		if (f & kPoolSpawnHasTeamByte) w.u8(rec.team_byte);   // entity+354 team (D-NET-58)
		if (f & kPoolSpawnHasParentHandle) w.u16(rec.parent_handle);
		if (f & kPoolSpawnHasTargetHandle) w.u16(rec.target_handle);

		// Mount-occupancy block. Once 0x0400 is set the original walks retail
		// slots 0..7, then ALWAYS writes slot 8 + slot 9 (D-NET-56). The
		// encoder only sets 0x0400 when seat_mask != 0.
		if (f & kPoolSpawnHasMountOccupancy) {
			w.u8(rec.seat_mask);
			for (int b = 0; b < 8; ++b)
				if (rec.seat_mask & (1u << b))
					w.u16(rec.mount_handles[size_t(b)]); // [orig: 0x432f47.. mirror]
			w.u16(rec.mount_handle_8);  // entity+416 [orig: 0x503d0c]
			w.u16(rec.mount_handle_9);  // entity+418 [orig: 0x503d24]
		}

		w.u8(rec.bone_byte);            // ALWAYS, entity+290 bone/other byte [orig: 0x503d38] (D-NET-58)

		if (f & kPoolSpawnHasAiTrailer) { // AI trailer (D-NET-52: 4+4+cstr)
			w.u32(rec.ai_profile_1);    // aiSlot+16  [orig: 0x503d6f]
			w.u32(rec.ai_profile_2);    // aiSlot+20  [orig: 0x503d83]
			w.cstr(rec.ai_name);        // aiSlot+156 [orig: 0x503dab..]
		}
		if (f & kPoolSpawnHasRefNum) w.u8(rec.alert_byte);        // [orig: 0x503e50]
		if (f & kPoolSpawnHasSubType) w.u8(rec.action_byte);       // [orig: 0x503e77]
		if (f & kPoolSpawnHasSoundLatchByte) w.u8(rec.sound_latch_byte);  // [orig: 0x503ec0]

		if (f & kPoolSpawnHasZoneNumberRank) { // [orig: 0x503ee5 health block]
			w.u8(rec.zone_number_rank);      // entity+538 (the break-out byte)
			w.u16(rec.zone_radius);    // entity+350
		} else if (f & kPoolSpawnHasZoneRadiusAlt) {
			w.u16(rec.zone_radius);    // entity+350 [orig: 0x503f43]
		}
		if (f & kPoolSpawnHasDifficultyByte) w.u8(rec.difficulty_byte);   // entity+624 [orig: 0x503f7e]
	}

	return out;
}

// [orig: NetPacket_SerializePool2StaticToBuffer @0x5042f0] (the §5.2a world-stream phase-1 static serializer) — the inverse of
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
		if (rec.euler_z)      f |= kStaticEntityHasEulerZ; // entity+16 yaw heading (32-bit BAM)
		if (rec.euler_x)      f |= kStaticEntityHasEulerX; // entity+20
		if (rec.euler_y)      f |= kStaticEntityHasEulerY; // entity+24
		if (rec.section_mask) f |= kStaticEntityHasSectionMask; // entity+308
		if (rec.team_byte)    f |= kStaticEntityHasTeamByte; // entity+354 (D-NET-58/62)
		if (rec.entity_flags) f |= kStaticEntityHasEntityFlags; // entity+36 Flags dword (D-NET-147)
		if (rec.bone_a)       f |= kStaticEntityHasRefNum; // entity+533 (D-NET-94)
		if (rec.bone_b)       f |= kStaticEntityHasSubType; // entity+532 (D-NET-94)
		if (rec.has_score_flag) f |= kStaticEntityHasScoreFlag; // entity+624, the def callback gate [orig: 0x504554]
		// attach_ref is written when `weapon_byte != 0 || flags & 0x200`; force the 0x200
		// gate only when attach_ref is populated but weapon_byte is zero (else weapon_byte
		// already triggers the write and 0x200 would be redundant).
		if (rec.attach_ref && rec.weapon_byte == 0) f |= kStaticEntityHasAttachRef;

		w.u16(f);
		w.u32(uint32_t(rec.pos_x)); // entity+4
		w.u32(uint32_t(rec.pos_y)); // entity+8
		w.u32(uint32_t(rec.pos_z)); // entity+12

		if (f & kStaticEntityHasEulerZ) w.u32(uint32_t(rec.euler_z));
		if (f & kStaticEntityHasEulerX) w.u32(uint32_t(rec.euler_x));
		if (f & kStaticEntityHasEulerY) w.u32(uint32_t(rec.euler_y));
		if (f & kStaticEntityHasSectionMask) w.u32(uint32_t(rec.section_mask));
		if (f & kStaticEntityHasTeamByte) w.u8(rec.team_byte);
		if (f & kStaticEntityHasEntityFlags) w.u32(rec.entity_flags);
		w.u8(rec.ammo_count);          // ALWAYS, entity+290
		if (f & kStaticEntityHasRefNum) w.u8(rec.bone_a);
		if (f & kStaticEntityHasSubType) w.u8(rec.bone_b);
		if (f & kStaticEntityHasScoreFlag) w.u8(rec.score_flag);
		w.u8(rec.weapon_byte);         // ALWAYS, entity+538
		if (rec.weapon_byte != 0 || (f & kStaticEntityHasAttachRef)) w.u16(rec.attach_ref); // entity+350
	}

	return out;
}

// [orig: Terrain_SerializeTiles @ 0x6080F0] — the inverse of decode_terrain_load_batch
// (§5.37, D-NET-83). One paged chunk of the terrain-tile (.til) load stream. The header chunk
// writes the wire start word 0xFFFF, end_index, then the `'til0'` magic + total tile_count +
// hdr2/hdr3; continuation chunks write `[start_index][end_index]`. Every chunk then writes its
// (end_index - start_index) opaque 12-B tile entries. Byte-identical round-trip with the decoder.
std::vector<uint8_t> encode_terrain_load_batch(const TerrainLoadBatch &batch) {
	std::vector<uint8_t> out;
	Writer w{out};

	if (batch.has_header) {
		w.u16(kTerrainFirstChunkStartWord);                        // first-chunk sentinel (start_index := 0)
		w.u16(batch.end_index);
		w.u32(batch.magic != 0 ? batch.magic : kTerrainTileMagic); // 'til0' (reader bails on mismatch)
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
		w.u32(rec.owner_connection_id);
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
		w.u8(rec.player_slot_id);
		w.u8(rec.alert_level);
		w.u8(rec.sub_type);
		w.u8(rec.weapon_type);
		w.u8(rec.parent_slot);
		w.u16(rec.parent_handle);
	}
	return out;
}

// [orig: NetPacket_SerializeObjectToBuffer @ 0x504d10] — the S2C 0x18 reply body. The
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
	w.cstr(rec.entity_name);       // entity+244 / g_EmptyStr [0x504e5a / 0x504e7c]
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
	w.u8(rec.player_slot_id);         // entity+340 [0x5051fd]
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
	if (rec.flags_byte & kVehicleCompactFlagDeadPose) {
		w.u16(uint16_t(rec.euler_y));           // entity+24 Euler Y  [orig: 0x460d4c]
		w.u16(uint16_t(rec.euler_x));           // entity+20 Euler X  [orig: 0x460d52 -> 0x460e10]
	} else {
		w.u16(rec.vertical_velocity);           // entity+0xA0 slideDecay, compressed
		                                        // [orig: 0x460d5a -> 0x460d7b]
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
	if (rec.flags & kRoundEventHasSlotByte) w.u8(rec.slot_byte);
	w.u16(rec.shooter_handle);
	if (rec.flags & kRoundEventHasTargetHandle) w.u16(rec.target_handle);
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
// 12-B anchor, flags1/flags2, the flags2&3 sub-block (weapon/timer/env/objective),
// the 7-B tail, the conditional phase-8 mounted-ammo record, then the event loop
// (tag=1 per-entity compact via the §5.10b class dispatch, tag=2 fired-round) and the
// tag=0 terminator. Known null-callback classes deliberately emit a header-only
// tag=1 record; Guided/Unknown records are skipped because they have no fixed
// 0x0A compact width. The listen host's own player gets the 14/25-byte
// header-only form (see the declaration) [orig: @0x4ff9cd / @0x50f07e].
std::vector<uint8_t> encode_frame_update(const FrameUpdate &fu,
                                         bool authority_recipient) {
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

	if (authority_recipient) {
		// The local gate: only the phase-0 weapon/reload/uniform block survives
		// [orig: NetPacket_WritePlayerState @0x4ff81b writes it before the
		//  local test @0x4ff9cd returns; NetPacket_SerializeEntityStatesToPacket
		//  @0x50f07e writes nothing for the local player].
		if ((fu.flags2 & kFrameFlags2SubBlockCycleMask) == 0) {
			w.u8(fu.weapon.preround_timer); w.u8(fu.weapon.slot_state360); w.u8(fu.weapon.slot_state368);
			w.u8(fu.weapon.slot_state364); w.u8(fu.weapon.slot_state356); w.u8(fu.weapon.slot_state460);
			w.u8(fu.weapon.reload_seconds);
			w.u32(uint32_t(fu.weapon.uniform_team_mask));
		}
		return out;
	}

	switch (fu.flags2 & kFrameFlags2SubBlockCycleMask) {
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
		w.u8(fu.env.quake_ticks); w.u8(fu.env.cloud_scroll); w.u8(fu.env.rain_pct);
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

	// Conditional mounted-weapon ammo record.
	if ((fu.flags2 & 0x0F) == 8) {
		w.u16(fu.passenger.mount_handle);
		if (fu.passenger.mount_handle != 0xFFFF) {
			w.u16(fu.passenger.clip);
			w.u16(fu.passenger.reserve);
		}
	}

	// Event loop: tag=1 per-entity compacts, then tag=2 round events, then tag=0.
	// (The original interleaves 1/2 pairs under the send budget
	// [orig: NetPacket_SerializeEntityStatesToPacket @0x50f070]; the retail decode loop
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
// §5.1 reply-body encoders (relocated from runtime/inmatch/server_message_dispatch.cpp so encode + decode
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
	if (field_flags & kPlayerSyncHasName)
		w.cstr_capped(ctx.player_name, 32); // name (variable-length, [orig: slot+40 @0x505f9b])
	if (field_flags & kPlayerSyncHasTeamString)
		w.cstr_capped(ctx.clan_tag, 16);    // team-string — retail ALWAYS writes "" here (@0x505ff7)
	if (field_flags & kPlayerSyncHasVehicleName)
		w.cstr_capped(std::string(), 16);   // vehicle-name — "" for an on-foot player (@0x50601f)
	if (field_flags & kPlayerSyncHasTeamByte)
		w.u8(ctx.team); // team byte [orig: slot+416; client -> playerSlot+14 + entity+354]
	if (field_flags & kPlayerSyncHasDownedState)
		w.u8(ctx.downed_state);
	if (field_flags & kPlayerSyncHasVehicleScore)
		w.u8(0);        // vehicle score byte [orig: vehicle+156 when mounted, else 0 @0x50613b]
	if (field_flags & kPlayerSyncHasLateJoinFlag)
		w.u8(ctx.spectator_in_game); // spectator in game [orig: slot+4 && slot+100567 && !slot+100579 @0x506197..0x5061cc]
	if (field_flags & kPlayerSyncHasSquad)
		w.u8(ctx.squad_leader); // the squad leader [orig: slot+100576, seeded 0xFF by Server_PlayerAdd @0x51cf0a]
	if (field_flags & kPlayerSyncHasSide)
		w.u8(ctx.fireteam);     // the fireteam [orig: slot+100577]
	if (field_flags & kPlayerSyncHasQuality)
		w.u8(ctx.quality); // quality [orig: slot+418 @0x506213; client clamps <=4 @0x431370]
	if (field_flags & kPlayerSyncHasAccountId)
		w.u32(0);       // NovaWorld account netId [orig: the slot connection's napi_player_data+420
		                //  @0x506257, else 0 @0x506246] — 0 = a LAN account (no clan-roster node);
		                // the NovaWorld-account value rides with the host clan-roster port

	return out;
}

std::vector<uint8_t> encode_player_sync_removal(uint8_t slot, bool with_ack) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(slot);
	// 0x8000 removal + optional 0x4000 ack (golden's 0xC000): the client clears the slot and, with the
	// ack bit, requests the next one — so the walk terminates at max_players. [orig: NapiNPClientMsg_PlayerSync
	// @0x431370 — a 0x8000 record reads NO entity slot / fields, just [u8 slot][u16 flags].]
	w.u16(static_cast<uint16_t>(kPlayerSyncRemoval | (with_ack ? kPlayerSyncAck : 0u)));
	return out;
}

// (encode_player_spawn — the invented unconditional S2C 0x51 "spawn confirm" — was REMOVED,
// D-NET-148: the original only sends 0x51 for a pending g_TeamChangeEntityList entry
// [orig: NapiNPServerMsg_0x029 @0x514F10 -> NetPacket_WriteEntityPacket @0x506bb0], and the client
// FIELD-PARSES it (NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 rebinds CharacterEntity from
// the packed char id) — a zeroed id re-bound the joiner to a vehicle archetype: the DBuggy1
// shadow. Port the real record from @0x506bb0 when the team-change flow lands.)

// [orig: NetPacket_SerializeScoreboard0x16 @0x504b80 (write) / NapiNPClientMsg_PlayerList
// @0x42FAE0 (read)] — round-trips through decode_player_list. Byte 0 is a FLAGS byte (bit0
// team-mode, bit1 timed-scores — the old `max` reading was a misnomer); the trailer carries
// the LIVE [inGameCount][spectatorCount] pair. The HUD "Number of players" = accepted rows −
// spectatorCount (g_ScoreboardRowCount − g_ScoreboardSpectatorCount) — a hardcoded trailer pinned
// every client's count at 2 (the v31 HUD-count defect, D-NET-158). Rows are accepted only for
// 0x46-known slots; bit0 of each row and the trailer count identify live spectators.
std::vector<uint8_t> encode_player_list(const PlayerListFrame &frame) {
	std::vector<uint8_t> out;
	Writer w{out};
	const size_t player_count = std::min<size_t>(frame.players.size(), 0xFFu);
	w.u8(frame.flags);
	w.u8(static_cast<uint8_t>(player_count));
	for (size_t i = 0; i < player_count; ++i) {
		const PlayerListEntry &e = frame.players[i];
		w.u8(e.slot);
		w.u16(e.status_flags);
		w.u16(e.score1);
		w.u16(e.score2);
		w.u8(static_cast<uint8_t>((e.team << 1) | (e.spectator ? 1u : 0u)));
	}
	w.u8(frame.team_count);
	for (size_t i = 0; i < size_t(frame.team_count) + 1; ++i) {
		const PlayerListTeamRow row =
				i < frame.teams.size() ? frame.teams[i] : PlayerListTeamRow{};
		w.u16(row.score1);
		w.u16(row.score2);
		w.u8(row.koth_hold);
		w.u8(row.ctf_flag);
	}
	w.u8(frame.in_game_count);
	w.u8(frame.spectator_count);
	return out;
}

std::vector<uint8_t> encode_end_round_header(const EndRoundHeader &header,
		bool non_team_form) {
	std::vector<uint8_t> out;
	Writer w{out};
	// Exact retail order and widths; the form is session state, never length.
	// [orig: EndRoundScoreboard_SerializeHeader @0x505280 — form pick
	// @0x5052a6; client NapiNPClientMsg_0x01D @0x430840]
	if (non_team_form) {
		// Top three frozen-board rows: three name C-strings, then the three
		// i16 primary scores the board builder stored at entry+0x40.
		// [orig: @0x5052bf..0x505381]
		for (const std::string &name : header.player_names) w.cstr(name);
		for (const int16_t score : header.player_scores)
			w.u16(static_cast<uint16_t>(score));
	} else {
		w.u8(static_cast<uint8_t>(header.winner_team));
		w.u16(static_cast<uint16_t>(header.team_score_0));
		w.u16(static_cast<uint16_t>(header.team_score_1));
	}
	w.u8(header.draw);
	w.u8(static_cast<uint8_t>(header.player_index));
	return out;
}

std::vector<uint8_t> encode_end_round_stats(const EndRoundStats &stats) {
	std::vector<uint8_t> out;
	Writer w{out};
	const size_t field_count = std::min<size_t>(stats.team_fields.size(), 0x7Fu);
	const size_t player_count = std::min<size_t>(stats.players.size(), 0xFFu);
	const size_t team_row_count = std::min<size_t>(stats.team_rows.size(), 0x7Fu);

	w.u8(static_cast<uint8_t>(stats.winner_team));
	w.u16(static_cast<uint16_t>(stats.team_score_0));
	w.u16(static_cast<uint16_t>(stats.team_score_1));
	w.u8(static_cast<uint8_t>(field_count));
	for (size_t i = 0; i < field_count; ++i) {
		w.u8(stats.team_fields[i].first);
		w.u8(stats.team_fields[i].second);
	}

	w.u8(static_cast<uint8_t>(player_count));
	for (size_t i = 0; i < player_count; ++i) {
		const EndRoundPlayerRow &row = stats.players[i];
		w.u8(row.slot);
		w.cstr(row.name);
		w.cstr(row.clan);
		w.cstr(row.tag);
		w.u8(row.team);
		w.u8(row.player_class);
		w.u16(static_cast<uint16_t>(row.kills));
		w.u16(static_cast<uint16_t>(row.deaths));
		w.u16(static_cast<uint16_t>(row.assists));
		w.u16(static_cast<uint16_t>(row.score));
		w.u16(static_cast<uint16_t>(row.captures));
		w.u16(static_cast<uint16_t>(row.flags));
		w.u16(static_cast<uint16_t>(row.special));
		for (size_t f = 0; f < field_count; ++f)
			w.u16(static_cast<uint16_t>(
					f < row.per_team.size() ? row.per_team[f] : 0));
	}

	// The trailing matrix writes every word each row carries (the producer
	// fills g_ScoreTeamCount CONFIGURED columns per row), not the declared
	// active count the player rows are gated on. The retail client reads the
	// declared count per row (decode_end_round_stats), so rows after row 0
	// misalign whenever a configured column is inactive; that asymmetry is
	// retail's own. [orig: Server_BuildEndOfRoundScoreboard — the count byte
	// @0x509581, the row/column loops @0x5095A0..0x5095CC with no Block[]
	// gate versus the player-row gate @0x509534]
	w.u8(static_cast<uint8_t>(team_row_count));
	for (size_t i = 0; i < team_row_count; ++i) {
		for (const int16_t value : stats.team_rows[i])
			w.u16(static_cast<uint16_t>(value));
	}
	return out;
}

std::vector<uint8_t> encode_end_round_stats_chunk(
		const std::vector<uint8_t> &board, uint16_t offset) {
	const size_t total = std::min<size_t>(
			board.size(), std::numeric_limits<uint16_t>::max());
	// Retail's writer returns zero and the server emits no 0x56 when the
	// requested offset is past the frozen stream. Offset == size is valid and
	// returns the four-byte terminal envelope.
	// [orig: NetPacket_WriteReplayStreamChunk @0x506F60]
	if (size_t(offset) > total) return {};
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(static_cast<uint16_t>(total));
	w.u16(offset);
	const size_t count = std::min<size_t>(200, total - size_t(offset));
	out.insert(out.end(), board.begin() + offset, board.begin() + offset + count);
	return out;
}

std::vector<uint8_t> encode_end_round_stats_request(uint16_t offset) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(offset);
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

std::vector<uint8_t> encode_mounted_weapon_slot_selection(
		const MountedWeaponSlotSelection &selection) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(selection.use_parent_slot ? 1u : 0u);
	return out;
}

std::vector<uint8_t> encode_auto_medic_preference(
		const AutoMedicPreference &preference) {
	std::vector<uint8_t> out;
	Writer w{out};
	// The profile stores the inverse checkbox value: zero means Auto Medic on.
	// [orig: NetPacket_WriteAutoMedicPreference @0x42A400; OPTIONS_AUTOMEDIC
	// reads/writes @0x5549E7/@0x554E40]
	w.u32(preference.enabled ? 0u : 1u);
	return out;
}

std::vector<uint8_t> encode_medic_request(const MedicRequest &request) {
	std::vector<uint8_t> out;
	Writer w{out};
	// [orig: NetPacket_WriteEntityIndex32 @0x49B4EB]
	w.u32(request.entity_index);
	return out;
}

std::vector<uint8_t> encode_chat_broadcast(const ChatBroadcast &chat) {
	std::vector<uint8_t> out;
	Writer w{out};
	// [orig: NetPacket_WriteTwoBytesAndCString @0x5047A0: buffer[0] = the
	// channel (byte2), buffer[1] = the sender slot (byte1), then the C string]
	w.u8(static_cast<uint8_t>(chat.channel));
	w.u8(chat.sender_slot);
	w.cstr(chat.text);
	return out;
}

std::vector<uint8_t> encode_formatted_game_text(const FormattedGameText &text) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(static_cast<uint8_t>(text.subtype)); // [orig: @0x51d21e / @0x505a81]
	w.cstr(text.text);                        // [orig: @0x51d250 / @0x505b72]
	// The team byte rides only the player join/leave pair
	// [orig: `mov [esi], cl` @0x51d277; @0x505b95 for types 1/2].
	if (text.subtype == kGameTextPlayerJoined || text.subtype == kGameTextPlayerLeaving)
		w.u8(static_cast<uint8_t>(text.team));
	return out;
}

// [orig: WacScript_ExecuteBytecode @0x4F58B0 — payload build @0x4f5cd0..0x4f5dc2]
std::vector<uint8_t> encode_script_remote_command(const ScriptRemoteCommand &command) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(command.command_index); // @0x4f5cf9
	if (command.command_index >= wac::wac_command_count()) return out;
	const wac::CommandDef &def = wac::wac_commands()[command.command_index];
	for (int i = 0; i < def.argc; ++i) {
		const ScriptRemoteCommandArg *arg = static_cast<size_t>(i) < command.args.size()
				? &command.args[static_cast<size_t>(i)] : nullptr;
		switch (def.params[i]) {
		case wac::ParamType::Text:
		case wac::ParamType::Filename:
			w.cstr_capped(arg != nullptr ? arg->text : std::string(), 251); // 250 chars + NUL @0x4f5d73..0x4f5dab
			break;
		case wac::ParamType::Ssn:
			w.u16(static_cast<uint16_t>(arg != nullptr ? arg->value : 0u)); // @0x4f5d3d
			break;
		default:
			w.u32(arg != nullptr ? arg->value : 0u); // @0x4f5d59
			break;
		}
	}
	return out;
}

std::vector<uint8_t> encode_death_camera_target(
		const DeathCameraTarget &target) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u32(static_cast<uint32_t>(target.x));
	w.u32(static_cast<uint32_t>(target.y));
	w.u32(static_cast<uint32_t>(target.z));
	return out;
}

std::vector<uint8_t> encode_player_downed_state(
		const PlayerDownedState &state) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(state.entity_handle);
	w.u8(static_cast<uint8_t>((state.revive_seconds & 0x7Fu) |
			(state.medic_request_active ? 0x80u : 0u)));
	return out;
}

std::vector<uint8_t> encode_deployed_item_spawn(
		const DeployedItemSpawn &spawn) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(spawn.item_id);
	w.u16(spawn.owner_handle);
	w.u16(spawn.friendly_item_id);
	w.u16(spawn.enemy_item_id);
	w.u16(spawn.slot_handle);
	w.u16(spawn.parent_handle);
	w.u32(static_cast<uint32_t>(spawn.pos_x));
	w.u32(static_cast<uint32_t>(spawn.pos_y));
	w.u32(static_cast<uint32_t>(spawn.pos_z));
	w.u16(spawn.angle_x);
	w.u16(spawn.angle_y);
	w.u16(spawn.angle_z);
	w.u16(spawn.reserved);
	return out;
}

std::vector<uint8_t> encode_entity_remove(const EntityRemove &removal) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(removal.entity_handle);
	return out;
}

// [orig: Server_BroadcastEntityActionPacket @0x5080D0 — the kind byte
//  @0x5080e1; kind 0 packs slot and is_win @0x508151/@0x508160, is_active
//  @0x50815b and the flag @0x508166; kind 1 packs the team @0x5080fd and the
//  key with its NUL @0x508134..0x50813e]
std::vector<uint8_t> encode_objective_notification(const ObjectiveNotification &notice) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(notice.kind);
	if (notice.kind == 0) {
		w.u32(static_cast<uint32_t>(notice.slot));
		w.u32(static_cast<uint32_t>(notice.is_win));
		w.u32(static_cast<uint32_t>(notice.is_active));
		w.u8(notice.flag);
	} else if (notice.kind == 1) {
		w.u32(static_cast<uint32_t>(notice.team));
		w.cstr(notice.key);
	}
	return out;
}

// [orig: NetPacket_SerializeEntityWithParentAndTarget @0x505810]
std::vector<uint8_t> encode_objective_entity_state(
		const ObjectiveEntityState &state) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(state.entity_handle);
	w.u8(state.flags_byte);
	w.u32(static_cast<uint32_t>(state.pos_x));
	w.u32(static_cast<uint32_t>(state.pos_y));
	w.u32(static_cast<uint32_t>(state.pos_z));
	w.u16(state.attach_handle);
	w.u16(state.ground_handle);
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
//  order of NapiNPClientMsg_TeamAssign (0x50) @ 0x431910]
std::vector<uint8_t> encode_play_sound(const PlaySoundCommand &cmd) {
	// [orig: NetPacket_WriteOverlayAction @0x505d50 — the type byte, the def's
	//  name copied as a C string, then (type 1 only) the three i16 coordinates
	//  taken from the caller's position triple.]
	std::vector<uint8_t> out;
	out.push_back(cmd.flag);
	out.insert(out.end(), cmd.sound_name.begin(), cmd.sound_name.end());
	out.push_back(0);
	if (cmd.flag == 1) {
		opennova::io::append_u16_le(out, static_cast<uint16_t>(cmd.pos_x));
		opennova::io::append_u16_le(out, static_cast<uint16_t>(cmd.pos_y));
		opennova::io::append_u16_le(out, static_cast<uint16_t>(cmd.pos_z));
	}
	return out;
}

std::vector<uint8_t> encode_tracked_player_voice(const TrackedPlayerVoice &voice) {
	// [orig: NapiNPServerMsg_HandleRadioCall — the dword payload
	//  @0x5143a2..0x51448f]
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(voice.event);
	w.u8(voice.player_index);
	w.u16(static_cast<uint16_t>(voice.location));
	return out;
}

std::vector<uint8_t> encode_minimap_overlay_batch(const MinimapOverlayBatch &batch) {
	// [orig: NetPacket_SerializeDesignations @0x5116A0 — handle @0x51171b, x/y/z
	//  @0x51175a / @0x51176d / @0x511786, seconds @0x511732, type @0x51178a,
	//  height @0x511744, the count @0x5117cf]
	std::vector<uint8_t> out;
	if (batch.entries.empty()) return out;
	Writer w{out};
	w.u8(static_cast<uint8_t>(batch.entries.size()));
	for (const MinimapOverlayBatch::Entry &e : batch.entries) {
		w.u16(e.handle);
		w.u16(static_cast<uint16_t>(e.x));
		w.u16(static_cast<uint16_t>(e.y));
		w.u16(static_cast<uint16_t>(e.z));
		w.u16(e.lifetime_s);
		w.u8(e.type);
		w.u8(e.height);
	}
	return out;
}

std::vector<uint8_t> encode_team_assign(const TeamAssign &assign) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(assign.entity_handle);
	w.u8(assign.team);
	// Both zero for a non-player entity in retail (the Flags & 0x100 gate @0x506b3d);
	// the caller owns that gate. [orig: NetPacket_WriteEntityHandlePacket @ 0x506ad0]
	w.u16(assign.net_id);
	w.u8(assign.anim_slot);
	return out;
}

// [orig: NetPacket_WriteEntityPacket @0x506BB0 — the list index first @0x506BCE, then the
//  0x50 record's own fields in its order]
std::vector<uint8_t> encode_team_change_confirm(uint16_t index, const TeamAssign &assign) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(index);
	const std::vector<uint8_t> record = encode_team_assign(assign);
	out.insert(out.end(), record.begin(), record.end());
	return out;
}

// [orig: NetPacket_SerializeEntityEventToBuffer @0x5055A0]
std::vector<uint8_t> encode_explosion_effect(const ExplosionEffectRecord &event) {
    std::vector<uint8_t> out;
    Writer w{out}; w.u8(event.type); w.u8(event.count); w.u16(event.source);
    w.u32(uint32_t(event.x)); w.u32(uint32_t(event.y)); w.u32(uint32_t(event.z));
    w.u16(uint16_t(event.heading));
    return out;
}

// [orig: Server_CollectValidWeaponSlots @0x516000 — the leading word is reserved
//  @0x51607f, each eligible slot appended @0x5160a9, and the iterator's current
//  slot stored into the leading word after the loop @0x5160d7]
std::vector<uint8_t> encode_batch_kill(const BatchKillBatch &page) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(page.count);
	for (uint16_t slot : page.slots) w.u16(slot);
	return out;
}

// [orig: the 0x4E continuation @0x4318db (windowMin) / @0x4318e8 (windowMax) /
//  @0x4318ee (start), len 10 @0x4318f5; the 0x0F burst form @0x42e5d3..0x42e5f7]
std::vector<uint8_t> encode_burst_loadout_request(const BurstLoadoutRequest &request) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u32(request.loadout_filter);  // windowMin
	w.u32(request.flags);           // windowMax
	w.u16(request.extra);           // start
	return out;
}

// [orig: NetPacket_SerializeMinimapSlot @0x5073B0 — `*buf = type` @0x5073dd; types 1/3
//  write the node id @0x50741a then name @0x507440 and tag @0x507470 as
//  strlen+1 copies; type 2 writes the id only @0x5073fb (5 B); anything else
//  returns 0 @0x507408 and the caller sends nothing]
std::vector<uint8_t> encode_clan_roster_update(const ClanRosterUpdate &update) {
	std::vector<uint8_t> out;
	if (update.action == kClanRosterAdd || update.action == kClanRosterWalkReply) {
		Writer w{out};
		w.u8(update.action);
		w.u32(update.account_id);
		w.cstr_capped(update.name, kClanRosterNameChars + 1);  // node +16 is char[65]
		w.cstr_capped(update.tag, kClanRosterTagChars + 1);    // node +81 is char[9]
	} else if (update.action == kClanRosterRemove) {
		Writer w{out};
		w.u8(update.action);
		w.u32(update.account_id);
	}
	return out;
}

// [orig: NapiNPClientMsg_HandleGameStart @0x42E180 writes {0} @0x42e1c9, len 4
//  @0x42e1cf; NapiNPClientMsg_HandlePlayerJoinLeave @0x432510 writes the node id
//  @0x43265c, len 4 @0x432662]
std::vector<uint8_t> encode_clan_roster_walk_request(const ClanRosterWalkRequest &request) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u32(request.after_account_id);
	return out;
}

// [orig: Server_SendWeaponSlotActionPacket @0x50F9A0 — handle @0x50fa15, state
//  word @0x50fa2d, number byte @0x50fa34, len 5 @0x50fa3a; the same 5-byte layout
//  from NetPacket_WriteShortShortByte @0x42B2B0 and NetPacket_WriteTwoShortsAndByte
//  @0x505E00]
std::vector<uint8_t> encode_door_slot_action(const DoorSlotAction &action) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(action.entity_handle);
	w.u16(static_cast<uint16_t>(action.state));
	w.u8(action.number);
	return out;
}

// [orig: NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0 — the constant 3 @0x5105c3,
//  per row the type id @0x510660, avail @0x510674, max @0x510681, then the 0 word
//  @0x5106b8]
std::vector<uint8_t> encode_vehicle_spawn_availability(const VehicleSpawnAvailabilityList &list) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(kVehicleSpawnAvailabilityLeadingByte);
	for (const VehicleSpawnAvailabilityRow &row : list.rows) {
		w.u16(row.type_id);
		w.u8(row.available);
		w.u8(row.max_count);
	}
	w.u16(0);
	return out;
}

// [orig: the host read order NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0
//  — u16 handle @0x51c51b, u8 type index @0x51c52a]
std::vector<uint8_t> encode_vehicle_spawn_request(const VehicleSpawnRequest &request) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u16(request.source_handle);
	w.u8(request.type_index);
	return out;
}

} // namespace opennova
