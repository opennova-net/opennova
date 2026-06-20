#include "novaworld/ingame_encode.h"

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
	void u8(uint8_t v) { out.push_back(v); }
	void u16(uint16_t v) {
		out.push_back(uint8_t(v));
		out.push_back(uint8_t(v >> 8));
	}
	void u32(uint32_t v) {
		out.push_back(uint8_t(v));
		out.push_back(uint8_t(v >> 8));
		out.push_back(uint8_t(v >> 16));
		out.push_back(uint8_t(v >> 24));
	}
	// NUL-terminated string — the inverse of Cursor::cstr().
	void cstr(const std::string &s) {
		for (char ch : s) out.push_back(uint8_t(ch));
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
		if (rec.health_byte)               f |= 0x2000; // entity+538  [orig: 0x503ecc]
		else if (rec.health_short)         f |= 0x8000; // itemDef+0x40000 path [orig: 0x503f29]
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
			w.u8(rec.health_byte);      // entity+538 (the break-out byte)
			w.u16(rec.health_short);    // entity+350
		} else if (f & 0x8000) {
			w.u16(rec.health_short);    // entity+350 [orig: 0x503f43]
		}
		if (f & 0x4000) w.u8(rec.difficulty_byte);   // entity+624 [orig: 0x503f7e]
	}

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

// [orig: Entity_SerializeMountedVehicleState case 1 (write, type 11) @ 0x460560]
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
		w.u16(uint16_t(rec.turret_pitch_raw));  // entity+286 raw i16 [orig: 0x460d9b]
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
	w.u16(rec.vehicle_handle);    // (pool<<12)|slot or 0xFFFF
	w.u16(rec.pos_x_compressed);  // CompressFixedPoint(entity+4; vehicle-local if mounted)
	w.u16(rec.pos_y_compressed);  // entity+8
	w.u16(rec.pos_z_compressed);  // entity+0xC
	w.u8(rec.yaw_byte);           // entity+0x14 high byte
	w.u8(rec.pitch_byte);
	w.u8(rec.anim_slot_low);      // entity+0x12C
	w.u8(rec.state_flags);        // entity+0x24 (bit2 spawning, bit4 mounted)
	w.u8(rec.weapon_anim_state);  // entity+0x2B8 / 0x2BC
	w.u8(rec.priority);           // entity+0x377
	w.u8(rec.anim_def_index);     // entity+0x2B0
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

// §5.9.1 weapon-hit record — the inverse of decode_weapon_hit_record.
// [orig: NetPacket_DeserializeWeaponHit @ 0x42F270]. 17-20 B by the flags gate
// (0x80 adds parent_byte, 0x40 adds weapon_handle).
std::vector<uint8_t> encode_weapon_hit_record(const WeaponHitRecord &rec) {
	std::vector<uint8_t> out;
	Writer w{out};
	w.u8(rec.flags);
	w.u8(rec.adm_index);
	w.u8(rec.hit_subtype);
	if (rec.flags & 0x80) w.u8(rec.parent_byte);
	w.u16(rec.target_handle);
	if (rec.flags & 0x40) w.u16(rec.weapon_handle);
	w.u16(rec.damage_extra_raw);
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
	case 0: // aim (11 B)
		w.u8(fu.aim.view0); w.u8(fu.aim.view1); w.u8(fu.aim.view2);
		w.u8(fu.aim.view3); w.u8(fu.aim.view4); w.u8(fu.aim.view5);
		w.u8(fu.aim.target_slot);
		w.u32(uint32_t(fu.aim.aim_extra));
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
	default: // sub-block 3: the objective gate is wire-invisible → 0 B (matches decode)
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

	// Event loop: tag=1 per-entity compacts, then tag=2 weapon-hits, then tag=0.
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
	for (const WeaponHitRecord &hit : fu.hits) {
		w.u8(2);
		append(encode_weapon_hit_record(hit));
	}
	w.u8(0); // event-loop terminator
	return out;
}

} // namespace opennova
