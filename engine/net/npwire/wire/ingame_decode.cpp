#include <base/io/byte_reader.h>
#include <net/npwire/ingame_decode.h>

#include <net/npwire/wire_handle.h>

#include <net/npwire/wire_cursor.h>

#include <formats/wac/command.h>

#include <cmath>
#include <cstring>
#include <base/io/fixed.h>

// Decoders for S2C 0x0D / 0x20 — see docs/net/novaworld-net-re.md §5.11/§5.12.
// Cross-witnessed byte-exact against the 2026-06-16b loopback capture
// (437 × 0x0D / 792 × 0x20 records, zero leftover bytes) when it was RE'd.

namespace opennova {

namespace {

// The 22-bit fixed-point euler trig both transforms sample: each BAM angle
// through the x87 sin/cos scaled by 2^22 (fild loads the dword signed; ftol
// truncates), roll/pitch/yaw.
struct EulerQ22 {
	int32_t sr, cr, sp, cp, sy, cy;
};

EulerQ22 euler_q22(uint32_t yaw_bam, uint32_t pitch_bam, uint32_t roll_bam) {
	const double k = 6.283185307179586476925286766559 / 4294967296.0; // 2pi / 2^32
	auto q = [k](uint32_t bam, int32_t &s, int32_t &c) {
		const double a = double(int32_t(bam)) * k; // fild loads the dword signed
		s = int32_t(std::sin(a) * io::kQ22One);      // *2^22, ftol truncates
		c = int32_t(std::cos(a) * io::kQ22One);
	};
	EulerQ22 t;
	q(roll_bam, t.sr, t.cr);
	q(pitch_bam, t.sp, t.cp);
	q(yaw_bam, t.sy, t.cy);
	return t;
}

// (a*b) >> 22 (imul + shrd ,22)
inline int32_t mul_q22(int32_t a, int32_t b) {
	return int32_t((int64_t(a) * int64_t(b)) >> 22);
}

} // namespace

// [orig: Entity_TransformLocalToWorld @ 0x43BD00] — see ingame_decode.h. Euler
// roll(X)->pitch(Y)->yaw(Z) rotation of the local offset in 22-bit fixed-point,
// then add the parent's world position. The intermediate assignments mirror the
// disassembly's imul/shrd-22 chain; the angle->radian + sin/cos*2^22 reproduces
// the x87 trig (last-bit FPU rounding is a platform primitive).
WorldPose network_transform_local_to_world(int32_t lx, int32_t ly, int32_t lz,
                                           int32_t px, int32_t py, int32_t pz,
                                           uint32_t yaw_bam, uint32_t pitch_bam,
                                           uint32_t roll_bam) {
	const auto [sr, cr, sp, cp, sy, cy] = euler_q22(yaw_bam, pitch_bam, roll_bam);
	const auto m = mul_q22;
	const int32_t ry = m(ly, cr) - m(lz, sr);  // after roll about X: y'
	const int32_t rz = m(ly, sr) + m(lz, cr);  //                     z'
	const int32_t pxr = m(lx, cp) - m(rz, sp); // after pitch about Y: x''
	const int32_t zr = m(lx, sp) + m(rz, cp);  //                      z''
	WorldPose w;                               // after yaw about Z + parent world
	w.x = m(pxr, cy) - m(ry, sy) + px;
	w.y = m(pxr, sy) + m(ry, cy) + py;
	w.z = zr + pz;
	return w;
}

// [orig: Entity_TransformWorldToLocal @ 0x43BB50] — see ingame_decode.h. The exact
// transpose of the chain above, applied in reverse order (yaw, pitch, roll). The
// disassembly stores sines scaled by -2^22 (dbl_7C57B0) and subtracts the products
// (@0x4c... via imul/shrd-22); writing the sines positive and flipping those signs is
// the identical arithmetic. Position only — the original's out[3..5] pose tail is
// heading subtraction + pitch/roll pass-through, composed by callers.
WorldPose network_transform_world_to_local(int32_t wx, int32_t wy, int32_t wz,
                                           int32_t px, int32_t py, int32_t pz,
                                           uint32_t yaw_bam, uint32_t pitch_bam,
                                           uint32_t roll_bam) {
	const auto [sr, cr, sp, cp, sy, cy] = euler_q22(yaw_bam, pitch_bam, roll_bam);
	const auto m = mul_q22;
	const int32_t dx = wx - px;                // @0x43bb65-0x43bb78: delta first
	const int32_t dy = wy - py;
	const int32_t dz = wz - pz;
	const int32_t tx = m(dx, cy) + m(dy, sy);  // yaw^-1 about Z (@0x43bc17-0x43bc38)
	const int32_t ty = m(dy, cy) - m(dx, sy);  //                (@0x43bc44-0x43bc88)
	WorldPose l;
	l.x = m(tx, cp) + m(dz, sp);               // pitch^-1 about Y (@0x43bc6e-0x43bcb2)
	const int32_t tz = m(dz, cp) - m(tx, sp);  //                  (@0x43bc90-0x43bcae)
	l.y = m(ty, cr) + m(tz, sr);               // roll^-1 about X (@0x43bcb4-0x43bcd0)
	l.z = m(tz, cr) - m(ty, sr);               //                 (@0x43bcd3-0x43bcf0)
	return l;
}

namespace {

using opennova::npwire_detail::Cursor;

// Bounded cursor — every read is bounds-checked vs `end`. On underflow we
// flip `ok=false` and stop advancing; the caller sees the exact byte where
// the decode ran out. Mirrors the retail handlers' `cursor + N <= end`
// pattern (NapiNPClientMsg_0x00D / _0x020 both bail to a final-return on
// short reads, leaving whatever was already stored in place).

} // namespace

bool decode_pool_spawn_batch(const uint8_t *body, size_t len,
                              PoolSpawnBatch &out) {
	out.records.clear();
	out.last_record_partial = false;
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
			out.last_record_partial = true;
			out.records.push_back(std::move(rec));
			return false;
		}
		if (wire_handle::is_batch_end_sentinel(rec.slot_id)) {
			// Retail handler returns immediately on the sentinel — without
			// storing this record. Match that: stop the loop here.
			out.sentinel_ended_early = true;
			return (c.p == c.end);
		}

		rec.item_type_id = c.u16();
		rec.entity_name = c.cstr();
		if (!c.ok) {
			out.last_record_partial = true;
			out.records.push_back(std::move(rec));
			return false;
		}

		if (rec.spawn_flags & kPoolSpawnHasEntityFlags) rec.entity_flags = c.u32();
		rec.pos_x = int32_t(c.u32());
		rec.pos_y = int32_t(c.u32());
		rec.pos_z = int32_t(c.u32());

		if (rec.spawn_flags & kPoolSpawnHasEulerZ) rec.euler_z = int32_t(c.u32()); // entity+16 yaw heading
		if (rec.spawn_flags & kPoolSpawnHasEulerX) rec.euler_x = int32_t(c.u32()); // entity+20
		if (rec.spawn_flags & kPoolSpawnHasEulerY) rec.euler_y = int32_t(c.u32()); // entity+24
		if (rec.spawn_flags & kPoolSpawnHasSectionMask) rec.section_mask = int32_t(c.u32());
		if (rec.spawn_flags & kPoolSpawnHasTeamByte) rec.team_byte = c.u8();  // entity+354, BMS team (D-NET-58)
		if (rec.spawn_flags & kPoolSpawnHasParentHandle) rec.parent_handle = c.u16();
		if (rec.spawn_flags & kPoolSpawnHasTargetHandle) rec.target_handle = c.u16();

		// Mount-occupancy block (0x0400). The retail handler reads the seat-mask
		// byte, then — for a non-zero mask — one occupant handle per set bit
		// for retail slots 0..7. It then ALWAYS consumes slots 8 and 9: the
		// mask==0 path `goto LABEL_110`
		// (@ 0x4330b1) reads both before falling through to the teamByte read.
		// D-NET-56: an earlier reading put the extras inside `if (mask)`, which
		// under-read by 4 B on the (0x400 set, mask==0) path. Retail servers
		// never emit that case (NetPacket_SerializeEntityPoolToPacket_0 only sets
		// 0x400 when the mask is non-zero), so the byte-witness capture didn't
		// exercise it — but the client handler reads it, so the port must too.
		// [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 LABEL_110)]
		if (rec.spawn_flags & kPoolSpawnHasMountOccupancy) {
			rec.seat_mask = c.u8();
			if (rec.seat_mask) {
				for (int b = 0; b < 8; ++b) {
					if (rec.seat_mask & (1u << b))
						rec.mount_handles[b] = c.u16();
				}
			}
			rec.mount_handle_8 = c.u16();
			rec.mount_handle_9 = c.u16();
		}

		rec.bone_byte = c.u8();  // entity+290, unconditional bone/other byte — NOT team (D-NET-58)

		if (rec.spawn_flags & kPoolSpawnHasAiTrailer) {
			rec.has_ai_trailer = true;
			rec.ai_profile_1 = c.u32();
			rec.ai_profile_2 = c.u32();
			rec.ai_name = c.cstr();
		}
		if (rec.spawn_flags & kPoolSpawnHasRefNum) rec.alert_byte = c.u8();
		if (rec.spawn_flags & kPoolSpawnHasSubType) rec.action_byte = c.u8();
		if (rec.spawn_flags & kPoolSpawnHasSoundLatchByte) {
			rec.has_sound_latch_byte = true;
			rec.sound_latch_byte = c.u8();
		}

		if (rec.spawn_flags & kPoolSpawnHasZoneNumberRank) {
			rec.has_zone_number_rank = true;
			rec.zone_number_rank = c.u8();
			rec.zone_radius = c.u16();
		} else if (rec.spawn_flags & kPoolSpawnHasZoneRadiusAlt) {
			rec.has_zone_radius_alt = true;
			rec.zone_radius = c.u16();
		}
		if (rec.spawn_flags & kPoolSpawnHasDifficultyByte) {
			rec.has_difficulty_byte = true;
			rec.difficulty_byte = c.u8();
		}

		const bool record_ok = c.ok;
		if (!record_ok) out.last_record_partial = true;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

bool decode_pool3_sync_batch(const uint8_t *body, size_t len,
                              Pool3SyncBatch &out) {
	out.records.clear();
	out.last_record_partial = false;
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
			out.last_record_partial = true;
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

		if (rec.flags_byte & kPool3SyncHasMovementVal) rec.movement_val = c.u32();  // entitySlot+16, raw BAM heading — NOT parent (D-NET-59)
		if (rec.flags_byte & kPool3SyncHasOrientationVal) rec.orientation_val = c.u32();
		if (rec.flags_byte & kPool3SyncHasAmmoCount) rec.ammo_count = c.u16();
		rec.net_handle = c.u16();
		if (rec.flags_byte & kPool3SyncHasTeamByte) rec.team_byte = c.u8();
		if (rec.flags_byte & kPool3SyncHasWeaponType) rec.weapon_type = c.u16();
		if (rec.flags_byte & kPool3SyncHasScoreByte) rec.score_byte = c.u8();

		const bool record_ok = c.ok;
		if (!record_ok) out.last_record_partial = true;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

// S2C 0x10 pool-2 static-entity batch (§5.9). Header [u16 startIndex][u16 count]
// like 0x20; each record is flags-first variable-length like 0x0D, with the
// itemTypeId == 0 empty-slot sentinel. ammoCount and weaponByte are UNCONDITIONAL;
// attachRef is read when weaponByte != 0 OR flags & 0x200.
// [orig: NapiNPClientMsg_0x010 @ 0x433400]
bool decode_static_entity_batch(const uint8_t *body, size_t len,
                                StaticEntityBatch &out) {
	out.records.clear();
	out.last_record_partial = false;
	out.start_index = 0;
	out.entity_count = 0;

	Cursor c{body, body + len, true};
	out.start_index = c.u16();
	out.entity_count = int16_t(c.u16());
	if (!c.ok) return false;
	if (out.entity_count <= 0) return (c.p == c.end);

	out.records.reserve(size_t(out.entity_count));
	for (int i = 0; i < out.entity_count; ++i) {
		StaticEntityRecord rec;
		rec.item_type_id = c.u16();
		if (!c.ok) {
			out.last_record_partial = true;
			out.records.push_back(std::move(rec));
			return false;
		}
		if (rec.item_type_id == 0) {
			rec.is_empty_slot = true;
			out.records.push_back(std::move(rec));
			continue;
		}

		rec.field_flags = c.u16();
		rec.pos_x = int32_t(c.u32());
		rec.pos_y = int32_t(c.u32());
		rec.pos_z = int32_t(c.u32());

		if (rec.field_flags & kStaticEntityHasEulerZ) rec.euler_z = int32_t(c.u32()); // entity+16 yaw heading
		if (rec.field_flags & kStaticEntityHasEulerX) rec.euler_x = int32_t(c.u32()); // entity+20
		if (rec.field_flags & kStaticEntityHasEulerY) rec.euler_y = int32_t(c.u32()); // entity+24
		if (rec.field_flags & kStaticEntityHasSectionMask) {
			rec.has_section_mask = true;
			rec.section_mask = int32_t(c.u32());
		}
		if (rec.field_flags & kStaticEntityHasTeamByte) rec.team_byte = c.u8();   // entity+354 (D-NET-58/62)
		if (rec.field_flags & kStaticEntityHasEntityFlags) rec.entity_flags = c.u32(); // entity+36 Flags (D-NET-147)
		rec.ammo_count = c.u8();                                  // entity+290, unconditional
		if (rec.field_flags & kStaticEntityHasRefNum) rec.bone_a = c.u8();
		if (rec.field_flags & kStaticEntityHasSubType) rec.bone_b = c.u8();
		if (rec.field_flags & kStaticEntityHasScoreFlag) {
			rec.has_score_flag = true;
			rec.score_flag = c.u8();
		}
		rec.weapon_byte = c.u8();                                 // entity+538, unconditional
		if (rec.weapon_byte != 0 || (rec.field_flags & kStaticEntityHasAttachRef)) rec.attach_ref = c.u16();

		const bool record_ok = c.ok;
		if (!record_ok) out.last_record_partial = true;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

// S2C 0x16 player-list scoreboard (§5.20). One message = the full list:
// header + player_count 8-B rows + team_count + (team_count+1) 6-B rows + a
// 2-byte trailer. [orig: NapiNPClientMsg_PlayerList @ 0x42FAE0]
bool decode_player_list(const uint8_t *body, size_t len, PlayerList &out) {
	out = PlayerList{};
	Cursor c{body, body + len, true};
	out.flags = c.u8();
	out.player_count = c.u8();
	if (!c.ok) return false;
	// The retail parser's 252-row table clamp: `total = count > 0xFC ? 252 : count`
	// [orig: NapiNPClientMsg_PlayerList @0x42FAE0, @0x42fb3a..0x42fb3c]; the team
	// table then parses from wherever the row loop stopped.
	const unsigned rows = out.player_count > 0xFCu ? 252u : unsigned(out.player_count);
	out.players.reserve(rows);
	for (unsigned i = 0; i < rows; ++i) {
		PlayerListRow r;
		r.slot_id = c.u8();
		r.status_flags = c.u16();
		r.score1 = c.u16();
		r.score2 = c.u16();
		r.flags = c.u8();
		out.players.push_back(r);
		if (!c.ok) return false;
	}
	out.team_count = c.u8();
	if (!c.ok) return false;
	const unsigned team_rows = unsigned(out.team_count) + 1;  // T0 neutral + per team
	out.teams.reserve(team_rows);
	for (unsigned i = 0; i < team_rows; ++i) {
		PlayerListTeamRow t;
		t.score1 = c.u16();
		t.score2 = c.u16();
		t.koth_hold = c.u8();
		t.ctf_flag = c.u8();
		out.teams.push_back(t);
		if (!c.ok) return false;
	}
	out.in_game_count = c.u8();
	out.spectator_count = c.u8();
	return (c.p == c.end);
}

// S2C 0x46 player-sync (§5.21). Header [u8 slot][u16 bitmask]; bit 0x8000 = a
// removal (no body). Otherwise [u8 entity_slot] then the present fields IN
// SOURCE ORDER (non-numeric). Bit 0x4000 carries no body (queue-ack signal).
// [orig: NapiNPClientMsg_PlayerSync @ 0x431370]
bool decode_player_sync(const uint8_t *body, size_t len, PlayerSync &out) {
	out = PlayerSync{};
	Cursor c{body, body + len, true};
	out.slot_id = c.u8();
	out.field_bitmask = c.u16();
	if (!c.ok) return false;
	const uint16_t m = out.field_bitmask;
	out.queue_ack = (m & kPlayerSyncAck) != 0;   // no body byte; applies to live AND removal rows
	if (m & kPlayerSyncRemoval) {
		out.removal = true;
		return (c.p == c.end);
	}
	out.entity_slot_id = c.u8();
	// Source order: name, clan, id, team, type|subtype, 0x20, 0x1000, 0x40, 0x80, quality, account netId.
	if (m & kPlayerSyncHasName) out.name = c.cstr();
	if (m & kPlayerSyncHasTeamString) out.clan = c.cstr();
	if (m & kPlayerSyncHasPcid) out.id_label = c.cstr();
	if (m & kPlayerSyncHasTeamByte) out.team = c.u8();
	if (m & kPlayerSyncHasDownedState) out.downed_state = c.u8();
	if (m & kPlayerSyncHasVehicleScore) out.field_0020 = c.u8();
	if (m & kPlayerSyncHasLateJoinFlag) out.field_1000 = c.u8();
	if (m & kPlayerSyncHasSquad) out.field_0040 = c.u8();
	if (m & kPlayerSyncHasSide) out.field_0080 = c.u8();
	if (m & kPlayerSyncHasQuality) out.quality = c.u8();
	if (m & kPlayerSyncHasAccountId) out.account_id = c.u32();  // -> slot dword 15 @0x431736
	return (c.p == c.end);
}

// S2C 0x0C pool-0 organic spawn batch (§5.23). Fully flat per record — no
// flag-gated optionals; slot-id-first; name parsed inline for every record.
// [orig: NapiNPClientMsg_0x00C @ 0x42E730]
bool decode_organic_spawn_batch(const uint8_t *body, size_t len,
                                OrganicSpawnBatch &out) {
	out.records.clear();
	out.last_record_partial = false;
	out.sentinel_ended_early = false;
	out.entity_count = 0;

	Cursor c{body, body + len, true};
	out.entity_count = c.u16();
	if (!c.ok) return false;
	if (out.entity_count == 0) return (c.p == c.end);

	out.records.reserve(out.entity_count);
	for (int i = 0; i < int(out.entity_count); ++i) {
		OrganicSpawnRecord rec;
		rec.slot_id = c.u16();
		if (!c.ok) { out.last_record_partial = true; out.records.push_back(std::move(rec)); return false; }
		// Sentinel: retail returns immediately, without storing this record
		// (@ 0x42e79d / 0x42e7b1). The slot >= pool.capacity guard is pool-state
		// dependent and not reproducible from the wire alone — the two value
		// sentinels below are.
		if (wire_handle::is_batch_end_sentinel(rec.slot_id)) {
			out.sentinel_ended_early = true;
			return (c.p == c.end);
		}
		rec.def_type = c.u8();
		if (!c.ok) { out.last_record_partial = true; out.records.push_back(std::move(rec)); return false; }
		if (!rec.has_body()) {
			// Empty spawn — the record ends after the def-type byte (@ 0x42e813).
			out.records.push_back(std::move(rec));
			continue;
		}

		rec.item_type_id = c.u16();
		rec.owner_connection_id = c.u32();
		rec.entity_name = c.cstr();
		rec.minimap_flags = c.u16();
		rec.pos_x = int32_t(c.u32());
		rec.pos_y = int32_t(c.u32());
		rec.pos_z = int32_t(c.u32());
		rec.orientation = int32_t(c.u32());
		rec.team = c.u8();        // entity+354 — BMS team (D-NET-58/62)
		rec.ai_state = c.u8();
		rec.anim_slot = c.u8();
		rec.net_id = c.u16();
		rec.player_class = c.u8();
		rec.ai_action = c.u8();
		rec.skip_byte = c.u8();   // discarded by the handler (@ 0x42e9f5)
		rec.player_slot_id = c.u8();
		rec.alert_level = c.u8();
		rec.sub_type = c.u8();
		rec.weapon_type = c.u8();
		rec.parent_slot = c.u8();
		rec.parent_handle = c.u16();

		const bool record_ok = c.ok;
		if (!record_ok) out.last_record_partial = true;
		out.records.push_back(std::move(rec));
		if (!record_ok) return false;
	}

	return (c.p == c.end);
}

bool decode_full_entity_spawn(const uint8_t *body, size_t len,
                              FullEntitySpawnRecord &out) {
	out = FullEntitySpawnRecord{};
	Cursor c{body, body + len, true};
	out.slot_id = c.u16();
	if (!c.ok) return false;
	// Sentinel: retail returns before reading anything else (@ 0x4337c5).
	if (out.slot_id == wire_handle::kInvalid) return (c.p == c.end);
	out.item_type_id = c.u16();
	out.item_type = c.u8();
	out.team = c.u8();
	out.minimap_flags = c.u16();
	out.entity_flags = c.u32();
	out.entity_name = c.cstr();
	out.parent_vehicle_handle = c.u16();
	out.ground_entity_handle = c.u16();
	out.parent_entity_handle = c.u16();
	// Seat block: the handler pre-fills mountHandles[0..7] with 0xFFFF, then
	// overwrites one per set mask bit (@ 0x433935..0x4339f2).
	out.seat_mask = c.u8();
	for (int bit = 0; bit < 8; ++bit) {
		if ((out.seat_mask & (1u << bit)) != 0) out.mount_handles[bit] = c.u16();
	}
	out.mount_handle_8 = c.u16();
	out.mount_handle_9 = c.u16();
	out.pos_x = int32_t(c.u32());
	out.pos_y = int32_t(c.u32());
	out.pos_z = int32_t(c.u32());
	out.heading_hi = c.u16();
	out.pitch_hi = c.u16();
	out.ai_state = c.u8();
	out.anim_slot = c.u8();
	out.net_id = c.u16();
	out.player_class = c.u8();
	out.skip_byte = c.u8();   // discarded by the handler (@ 0x433b26)
	out.player_slot_id = c.u8();
	out.alert_level = c.u8();
	out.sub_type = c.u8();
	return c.ok && (c.p == c.end);
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

// S2C 0x7E: two mission-briefing C strings, with no count or trailing fields.
// [orig: NapiNPClientMsg_ServerConfigStrings @0x425E20]
bool decode_server_config_strings(const uint8_t *body, size_t len,
                                  ServerConfigStrings &out) {
	out = ServerConfigStrings{};
	Cursor c{body, body + len, true};
	out.briefing3 = c.cstr();
	out.briefing2 = c.cstr();
	return c.ok && c.p == c.end;
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
	out.carrier_handle    = c.u16();
	out.pos_x_compressed  = c.u16();
	out.pos_y_compressed  = c.u16();
	out.pos_z_compressed  = c.u16();
	out.yaw_byte          = c.u8();
	out.pitch_byte        = c.u8();
	out.move_input_byte     = c.u8();
	out.state_flags       = c.u8();
	out.anim_state_id          = c.u8();
	out.anim_channel_ratio = c.u8();
	out.anim_def_index     = c.u8();
	out.health_class_byte = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 18;
}

// §5.13 vehicle compact record (mode 2, format 11). 15 B mounted / 21 B not.
// [orig: Entity_SerializeVehicleState @ 0x460560]
bool decode_vehicle_compact_record(const uint8_t *body, size_t len,
                                   VehicleCompactRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.parent_slot_handle = c.u16();
	out.pos_x_compressed   = c.u16();
	out.pos_y_compressed   = c.u16();
	out.pos_z_compressed   = c.u16();
	out.euler_z            = int16_t(c.u16());
	out.flags_byte         = c.u8();
	out.is_dead_pose         = (out.flags_byte & kVehicleCompactFlagDeadPose) != 0;
	if (out.is_dead_pose) {
		// Mounted: the remaining two Euler components (entity+584/+580).
		out.euler_y = int16_t(c.u16());
		out.euler_x = int16_t(c.u16());
	} else {
		// Live: vertical velocity (entity+0xA0) + vehicle HEALTH word (entity+286) + the
		// [136]/[135]/[132] prediction registers.
		out.vertical_velocity  = c.u16();
		out.health_word        = c.u16();
		out.weapon_aim_y       = c.u16();
		out.weapon_aim_z       = c.u16();
		out.weapon_heading_bam = int16_t(c.u16());
	}
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == (out.is_dead_pose ? size_t(15) : size_t(21));
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
// [orig: NetPacket_DispatchEntityPacketCallback @ 0x4D6A80] reads it on the host side;
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
	out.carrier_handle = c.u16();       // ground entity (any pool); 0xFFFF = free
	out.pos_x          = int32_t(c.u32());
	out.pos_y          = int32_t(c.u32());
	out.pos_z          = int32_t(c.u32());
	out.heading        = int16_t(c.u16());
	out.pitch          = int16_t(c.u16());
	out.anticheat_flags  = c.u8();      // host apply discards this byte
	out.move_input_byte  = c.u8();
	out.state_flags_byte = c.u8();      // raw entity+0x24 low byte [orig: replace-bits apply @0x4c1e4d]
	out.analog_x     = c.u8();
	out.analog_y     = c.u8();
	out.analog_z     = c.u8();
	out.equipped_adm_index = c.u8(); // entity+0x2B0 [orig: case-4 store @0x4C20A3]
	out.stat_byte_0    = c.u8();
	out.stat_byte_1    = c.u8();
	out.priority_handle_0 = c.u16();
	out.priority_score_0  = c.u16();
	out.priority_handle_1 = c.u16();
	out.priority_score_1  = c.u16();
	out.priority_handle_2 = c.u16();
	out.priority_score_2  = c.u16();
	out.priority_handle_3 = c.u16();
	out.priority_score_3  = c.u16();
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
	out.delta_x         = c.u16();
	out.delta_y         = c.u16();
	out.delta_z         = c.u16();
	out.delta_yaw       = c.u16();
	out.delta_pitch     = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 45;
}

// C2S 0x21 anti-cheat CRC reply. The handler reads u8 + u32 = 5 B; the 9-B
// body the client builds ends in the echoed challenge key, which the handler
// never touches. A 5-B body is still a valid handler read, so the key is
// optional here and `consumed` reports 5 or 9.
// [orig: NapiNPServerMsg_HandleAntiCheatCRCCheck @ 0x502050;
//  NetPacket_WriteEntityCRCChecksum @0x42B020, key echo @0x42B14D]
bool decode_client_checksum_reply(const uint8_t *body, size_t len,
                                  ClientChecksumReply &out, size_t &consumed) {
	consumed = 0;
	out = ClientChecksumReply{};
	Cursor c{body, body + len, true};
	out.player_index = c.u8();
	out.expected_crc = c.u32();
	if (!c.ok) return false;
	if (c.p + 4 <= c.end) {
		out.echoed_key = c.u32();
		out.has_echoed_key = true;
	}
	consumed = size_t(c.p - body);
	return true;
}

// §5.9.1 round-event record. 17-20 B variable by flags gate (0x80, 0x40).
// [orig: NetPacket_DeserializeRoundEvent @ 0x42F270 (client read); host write side
// NetPacket_SerializeRoundEvent @ 0x504820]
bool decode_round_event_record(const uint8_t *body, size_t len,
                               RoundEventRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.flags        = c.u8();
	out.adm_index    = c.u8();
	out.subtype      = c.u8();
	if (out.flags & kRoundEventHasSlotByte) {
		out.slot_byte = c.u8();
	}
	out.shooter_handle = c.u16();
	if (out.flags & kRoundEventHasTargetHandle) {
		out.target_handle = c.u16();
	}
	out.shot_seq         = c.u16();
	out.pos_x_compressed = c.u16();
	out.pos_y_compressed = c.u16();
	out.pos_z_compressed = c.u16();
	out.yaw_bam_high     = c.u16();
	out.pitch_bam_high   = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	const size_t expected = 17
		+ (out.has_slot_byte() ? 1u : 0u)
		+ (out.has_target_handle() ? 2u : 0u);
	return consumed == expected;
}

// §5.15 guided weapon record — read ONE field group. The write side lives in
// encode_guided_field_group. [orig: Entity_SerializeGuidedMissileState @ 0x447C50]
// (mode 2 read-full = the case-2 switch; mode 4 read-apply = the case-4 switch).
// See ingame_decode.h for the (mode × group) size matrix + the deferral note.
bool decode_guided_field_group(GuidedMode mode, GuidedFieldGroup group,
                               const uint8_t *body, size_t len,
                               GuidedRecord &out, size_t &consumed) {
	consumed = 0;
	// Decode handles the two READ modes only (full vs delta-apply); the write
	// modes 1/3 are encode_guided_field_group's job.
	if (mode != GuidedMode::ReadFull && mode != GuidedMode::ReadApply)
		return false;
	const bool full = (mode == GuidedMode::ReadFull);
	Cursor c{body, body + len, true};
	switch (group) {
	case GuidedFieldGroup::Status:        // [orig: 0x448039] entity+696|=1, +276|=0x1000
		out.launched = true;
		break;                            // 0 payload bytes
	case GuidedFieldGroup::ClearTarget:   // [orig: 0x447e73] clear target
		out.target_cleared = true;
		out.target_slot = 0xFFFF;
		break;                            // 0 payload bytes
	case GuidedFieldGroup::TargetPos:     // [orig: 0x447e93 (full) / 0x448052 (apply)]
		out.target_slot = c.u16();        // entity+724
		out.target_bound = true;
		if (full) {                       // full reads pos; apply reads target only
			out.pos_x = int32_t(c.u32()); // entity+700
			out.pos_y = int32_t(c.u32()); // entity+704
			out.pos_z = int32_t(c.u32()); // entity+708
		}
		break;
	case GuidedFieldGroup::TargetTypePos: // [orig: 0x447f39 (full) / 0x4480ab (apply)]
		if (full) out.target_slot = c.u16();    // entity+724 (full only)
		out.weapon_type = uint16_t(c.u32());     // entity+698 (low u16 of a 4-B field)
		out.pos_x = int32_t(c.u32());
		out.pos_y = int32_t(c.u32());
		out.pos_z = int32_t(c.u32());
		out.target_bound = true;
		break;
	case GuidedFieldGroup::Pos:           // [orig: 0x448135] read pos, clear target
		out.pos_x = int32_t(c.u32());
		out.pos_y = int32_t(c.u32());
		out.pos_z = int32_t(c.u32());
		out.target_cleared = true;
		out.target_slot = 0xFFFF;
		break;
	case GuidedFieldGroup::AttachOffsets: // [orig: 0x4481b1] entity+740/744/748
		out.attach_x = int32_t(c.u32());
		out.attach_y = int32_t(c.u32());
		out.attach_z = int32_t(c.u32());
		break;
	default:
		return false;
	}
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return true;
}

// Walk a S2C 0x0A body into a FrameUpdate. Structural port of the retail handler
// [orig: NapiNPClientMsg_0x00A @ 0x42FEC0] (the same walk nw_pp's print_tag_0a
// performs, returning data instead of printing). Sub-block widths IDA-witnessed:
// case 0 = 11 B, 1 = 6 B, 2 = 11 B (ENV), 3 = 0 B (objective-gametype gated).
bool decode_frame_update(const uint8_t *body, size_t len,
                         const std::function<EntityClass(uint16_t)> &class_of,
                         FrameUpdate &out, bool is_objective_gametype,
                         bool authority_recipient) {
	Cursor c{body, body + len, true};
	auto finish = [&](bool complete) {
		out.complete = complete;
		out.consumed = size_t(c.p - body);
		return complete;
	};

	// 12-byte reference header -> the position anchor (dword_A822E4/E8/EC).
	out.anchor_x = c.i32();
	out.anchor_y = c.i32();
	out.anchor_z = c.i32();
	out.flags1 = c.u8();    // loadprog / death-spectator signals
	out.flags2 = c.u8();
	out.sub_block = uint8_t(out.flags2 & kFrameFlags2SubBlockCycleMask);
	if (!c.ok) return finish(false);
	if (authority_recipient && out.sub_block != 0) {
		// The listen host's own frame: no sub-block, tail, or event loop follows
		// [orig: NapiNPClientMsg_0x00A @0x430174].
		return finish(true);
	}
	switch (out.sub_block) {
	case 0:
		// Local-player weapon/reload/uniform state (11 B) [orig: NetPacket_WritePlayerState
		// @0x4ff81b (writer) / 0x430054..0x430136 (reader)].
		out.weapon.preround_timer    = c.u8();
		out.weapon.slot_state360     = c.u8();
		out.weapon.slot_state368     = c.u8();
		out.weapon.slot_state364     = c.u8();
		out.weapon.slot_state356     = c.u8();
		out.weapon.slot_state460     = c.u8();
		out.weapon.reload_seconds    = c.u8();
		out.weapon.uniform_team_mask = c.i32();
		if (!c.ok) return finish(false);
		out.weapon.present           = true;
		break;
	case 1:
		// Round/game timer (6 B) [orig: 0x430191..0x430235].
		out.timer.state0        = c.u8();
		out.timer.state1        = c.u8();
		out.timer.state2        = c.u8();
		out.timer.state3        = c.u8();
		out.timer.timer_seconds = c.i16();
		if (!c.ok) return finish(false);
		out.timer.present       = true;
		break;
	case 2:
		// ENV snapshot (11 B): 3× u16 + 5× u8 [orig: 0x430244..0x43034C].
		out.env.fog_dist     = c.u16();
		out.env.fog_accel    = c.u16();
		out.env.tod_fixed    = c.u16();
		out.env.quake_ticks  = c.u8();
		out.env.cloud_scroll = c.u8();
		out.env.rain_pct = c.u8();
		out.env.overcast     = c.u8();
		out.env.env_param    = c.u8();
		if (!c.ok) return finish(false);
		out.env.present      = true;
		break;
	case 3:
		// Objective-gametype block (4× i32, 16 B) present ONLY when the host's
		// `g_GameType & 0x20000` bit is set [orig: gate @ 0x430361, body
		// @ 0x430363..0x4303D0]. That gate is NOT on the wire, so the caller supplies
		// the hint (default false). probe3 (Co-op, g_GameType 0x30020) is the first
		// capture to carry it (docs/net/novaworld-net-re.md D-NET-75); the 4 i32 land
		// in dword_AC86F4/F0/EC/E8.
		if (is_objective_gametype) {
			out.objective.state[0] = c.i32();
			out.objective.state[1] = c.i32();
			out.objective.state[2] = c.i32();
			out.objective.state[3] = c.i32();
			if (!c.ok) return finish(false);
			out.objective.present = true;
		}
		break;
	}

	// The listen host's own frame ends with the phase-0 block
	// [orig: NapiNPClientMsg_0x00A @0x430174].
	if (authority_recipient) return finish(true);

	// 7-byte fixed tail: state_flag u8, carried-object u16, health i16, state_word i16.
	out.state_flag_byte = c.u8();
	out.carried_handle  = c.u16();
	out.health          = c.i16();
	out.state_word      = c.i16();
	if (!c.ok) return finish(false);
	out.local_tail_present = true;

	// Conditional phase-8 mounted-ammo record (sub-block 0 + flags2 bit 3 set).
	if ((out.flags2 & kFrameFlags2RouteMask) == kFrameFlags2MountedAmmoRoute) {
		const uint16_t mount_handle = c.u16();
		if (!c.ok) return finish(false);
		if (mount_handle != 0xFFFF) {
			const uint16_t clip = c.u16();
			const uint16_t reserve = c.u16();
			if (!c.ok) return finish(false);
			out.passenger.has_mount = true;
			out.passenger.clip = clip;
			out.passenger.reserve = reserve;
		}
		out.passenger.mount_handle = mount_handle;
		// `present` means the complete conditional record was consumed. A short
		// handle/clip/reserve must never overwrite the last authoritative slot.
		out.passenger.present = true;
	}

	// Event loop: tag 0 = EOB, 1 = per-entity compact, 2 = round event.
	while (c.ok && c.p < c.end) {
		const uint8_t tag = c.u8();
		if (tag == 0) return finish(true);
		if (tag == 2) {
			RoundEventRecord re;
			size_t consumed = 0;
			if (!decode_round_event_record(c.p, size_t(c.end - c.p), re, consumed))
				return finish(false);
			c.p += consumed;
			out.round_events.push_back(re);
			continue;
		}
		if (tag == 1) {
			FrameUpdateRecord rec;
			rec.handle = c.u16();
			rec.type_id = c.u16();
			if (!c.ok) return finish(false);
			rec.cls = class_of ? class_of(rec.type_id) : EntityClass::Unknown;
			const size_t avail = size_t(c.end - c.p);
			size_t consumed = 0;
			bool ok = false;
			switch (rec.cls) {
			case EntityClass::Player:
				ok = decode_player_compact_record(c.p, avail, rec.player, consumed);
				break;
			case EntityClass::Vehicle:
				ok = decode_vehicle_compact_record(c.p, avail, rec.vehicle, consumed);
				break;
			case EntityClass::Infantry:
				ok = decode_infantry_compact_record(c.p, avail, rec.infantry, consumed);
				break;
			case EntityClass::NoNetworkCallback:
				// Known item class with ItemDef+0x164 == null: retail consumes only
				// the tag/handle/type header, then jumps back to the loop head.
				// [orig: 0x430814..0x43081D]
				ok = true;
				consumed = 0;
				break;
			default:
				// Guided (§5.15) or unresolved Unknown: not a fixed §5.10b compact.
				// Guided treats the subtype as a field-group selector and rejects
				// the 0x0A compact subtype 11; Unknown has no safe width.
				return finish(false);
			}
			if (!ok) return finish(false);
			c.p += consumed;
			out.records.push_back(std::move(rec));
			continue;
		}
		return finish(false); // unknown event tag — fail closed
	}
	return finish(c.ok);
}

// S2C 0x1E game event — 8 B fixed. [orig: NetPacket_HandleGameEvent @ 0x426270]
// S2C 0x61 tick seed. The retail handler reads the dword only when four bytes are
// present and otherwise seeds ZERO, then stores it unconditionally — a short body is a
// seed of 0, not an error, and 0 is itself the witnessed round-end disarm value.
// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 — the `keyData+4 <= keyData+dataLen`
//  guard @0x4297eb, the stores @0x4297f8 / @0x4297fd]
bool decode_tick_seed(const uint8_t *body, size_t len, uint32_t &out) {
	out = 0;
	if (body != nullptr && len >= 4) {
		out = static_cast<uint32_t>(body[0]) | (static_cast<uint32_t>(body[1]) << 8) |
		      (static_cast<uint32_t>(body[2]) << 16) | (static_cast<uint32_t>(body[3]) << 24);
	}
	return true;
}

bool decode_game_event(const uint8_t *body, size_t len, GameEventRecord &out,
                       size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.event_type     = c.u8();
	out.attacker_index = c.u8();
	out.victim_index   = c.u8();
	out.aux_index      = c.u8();
	out.pos_x          = c.i16();
	out.pos_y          = c.i16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 8;
}

// S2C 0x26 entity kill replication. [orig: NapiNPClientMsg_0x026 @ 0x42EC30]
// The handler is defensive: it reads the victim slot if 2 B are present and the
// hit section if a further 2 B are present. A missing section is zero; this
// decoder retains its explicit rejection of a missing entity word.
bool decode_kill_record(const uint8_t *body, size_t len, KillRecord &out,
                        size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.victim_slot = c.u16();
	if (!c.ok) return false;
	out.section = c.p + 2 <= c.end ? static_cast<int16_t>(c.u16()) : 0;
	consumed = size_t(c.p - body);
	return true;
}

// S2C 0x4E — one join-window kill-list page. [orig: NapiNPClientMsg_HandleBatchKill
// @ 0x431870] The handler kills every u16 slot after the leading word up to the
// buffer end (@0x4318a5..0x4318c2); the leading word is the host's resume index,
// echoed as the C2S 0x28 continuation's `start` (@0x4318ee), never a read limit.
bool decode_batch_kill(const uint8_t *body, size_t len, BatchKillBatch &out) {
	out = BatchKillBatch{};
	Cursor c{body, body + len, true};
	out.count = c.u16();
	if (!c.ok) return false;
	while (c.p + 2 <= c.end)
		out.slots.push_back(c.u16());
	return (c.p == c.end);
}

// S2C 0x5D empty-slot sweep. No count word: the handler walks `[i16 pool0Index]`
// pairs to the end of the body. Each index is a RAW pool-0 slot number
// (Pool_GetEntryUnchecked(0, idx)), not a packed handle.
// [orig: NapiNPClientMsg_DestroyEntityList @ 0x429730]
bool decode_destroy_entity_list(const uint8_t *body, size_t len,
                                DestroyEntityList &out) {
	out = DestroyEntityList{};
	Cursor c{body, body + len, true};
	while (c.p + 2 <= c.end)
		out.pool0_indices.push_back(c.u16());
	return (c.p == c.end);
}

// S2C 0x50 team assign. A short body leaves every REMAINING field at zero — the
// handler reads what arrived and never fails on a truncated tail.
// [orig: NapiNPClientMsg_TeamAssign (0x50) @ 0x431910]
bool decode_team_assign(const uint8_t *body, size_t len, TeamAssign &out,
                        size_t &consumed) {
	out = TeamAssign{};
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	if (!c.ok) return false; // the handle itself is the one mandatory field
	if (c.p + 1 <= c.end) out.team = c.u8();
	if (c.p + 2 <= c.end) out.net_id = c.u16();
	if (c.p + 1 <= c.end) out.anim_slot = c.u8();
	consumed = size_t(c.p - body);
	return true;
}

// S2C 0x51 team-change confirm: every field zero-fills and a short read does
// not advance, so a later narrower field still reads what remains; the handle
// defaults to 0, not the 0xFFFF sentinel. [orig: NapiNPClientMsg_HandlePlayerSpawn
// @0x431BB0 — index @0x431bca, handle @0x431bdd, team @0x431bed, NetId
// @0x431bfc, animSlot @0x431c11]
bool decode_team_change_confirm(const uint8_t *body, size_t len, TeamChangeConfirm &out) {
	out = TeamChangeConfirm{};
	size_t at = 0;
	const auto u16 = [&]() -> uint16_t {
		if (at + 2 > len) return uint16_t{0};
		const uint16_t v = static_cast<uint16_t>(body[at] | (body[at + 1] << 8));
		at += 2;
		return v;
	};
	const auto u8 = [&]() -> uint8_t {
		if (at + 1 > len) return uint8_t{0};
		return body[at++];
	};
	out.index = u16();
	out.assign.entity_handle = u16();
	out.assign.team = u8();
	out.assign.net_id = u16();
	out.assign.anim_slot = u8();
	return len >= 8;
}

// ===========================================================================
// Uncharacterized-tag bodies (D-NET-73 / D-NET-74). Field maps: docs
// §5.28-§5.33. Witnessed in Jointops.exe.kong.i64 this pass; see ingame_decode.h
// for the per-tag layout + [orig] cites.
// ===========================================================================

// S2C 0x5A weapon-loadout. [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290E0]
bool decode_weapon_loadout(const uint8_t *body, size_t len, WeaponLoadout &out) {
	out = WeaponLoadout{};
	Cursor c{body, body + len, true};
	out.avatar_class = c.u8();
	uint8_t type_id = c.u8();
	if (!c.ok) return false;
	// Chain of {typeId, ammoP, ammoS, ammoAlt} terminated by typeId == 0xFF.
	// Retail caps at 40 raw slots (@ 0x429155) before reading the ammo bytes.
	int raw = 0;
	while (type_id != 0xFF && raw < 40) {
		WeaponLoadoutSlot s;
		s.type_id        = type_id;
		s.ammo_primary   = c.u8();
		s.ammo_secondary = c.u8();
		s.ammo_alt       = c.u8();
		out.slots.push_back(s);
		type_id = c.u8();   // next type id (or the 0xFF terminator)
		++raw;
		if (!c.ok) return false;
	}
	return (c.p == c.end);
}

// S2C 0x6E spawn-wave/deploy-screen status.
// [orig: NetPacket_WriteSpawnWaveStatus @0x507490;
//        NapiNPClientMsg_HandleSquadRosterSync @0x429880]
bool decode_spawn_wave_status(const uint8_t *body, size_t len, SpawnWaveStatus &out) {
	out = SpawnWaveStatus{};
	Cursor c{body, body + len, true};
	out.group_count = c.u8();
	if (!c.ok) return false;
	out.groups.reserve(out.group_count);
	for (unsigned i = 0; i < out.group_count; ++i) {
		SpawnWaveGroup group;
		group.zone_handle    = c.u16();
		group.zone_index     = c.u16();
		group.queued_count   = c.u8();
		group.wave_countdown = c.u16();
		if (!c.ok) { out.groups.push_back(std::move(group)); return false; }
		group.members.reserve(group.queued_count);
		for (unsigned m = 0; m < group.queued_count; ++m)
			group.members.push_back(c.u16());
		const bool record_ok = c.ok;
		out.groups.push_back(std::move(group));
		if (!record_ok) return false;
	}
	return (c.p == c.end);
}

bool decode_score_delta_sound(const uint8_t *body, size_t len,
		ScoreDeltaSound &out) {
	out = ScoreDeltaSound{};
	Cursor c{body, body + len, true};
	out.score = c.i32();
	return c.ok && c.p == c.end;
}

// S2C 0x7B full player info. [orig: NapiNPClientMsg_HandlePlayerInfoFull @ 0x429BB0]
bool decode_full_player_info(const uint8_t *body, size_t len, FullPlayerInfo &out) {
	out = FullPlayerInfo{};
	Cursor c{body, body + len, true};
	out.player_name  = c.cstr();
	out.player_id    = c.cstr();
	out.server_name  = c.cstr();
	out.mission_name = c.cstr();
	out.map_file     = c.cstr();
	out.extra        = c.u32();
	out.motd         = c.cstr();
	out.game_name    = c.cstr();
	if (!c.ok) return false;
	return (c.p == c.end);
}

// S2C 0x0F world-state-load. [orig: NapiNPClientMsg_0x00F @ 0x42E200]
bool decode_world_state_load(const uint8_t *body, size_t len, WorldStateLoad &out,
                             bool is_waypoint_gametype) {
	out = WorldStateLoad{};
	Cursor c{body, body + len, true};
	out.session_tick = c.u32();
	out.pos_x = int32_t(c.u32());
	out.pos_y = int32_t(c.u32());
	out.pos_z = int32_t(c.u32());
	out.yaw   = c.i16();
	out.pitch = c.i16();
	out.roll  = c.i16();
	out.game_flags = c.u8();
	// Fixed 128-entry ammo-pool table (loop fills [g_LocalAmmoPools, data) @ 0x42e324).
	for (int i = 0; i < kWorldStateAmmoPoolCount; ++i)
		out.ammo_pools[i] = int32_t(c.u32());
	out.waypoint_count = c.u16();
	if (!c.ok) return false;
	// Waypoint records ride the wire ONLY for a waypoint gametype — an off-wire
	// host gate, so the caller supplies the hint (default false).
	if (is_waypoint_gametype) {
		out.waypoints.reserve(out.waypoint_count);
		for (unsigned i = 0; i < out.waypoint_count; ++i) {
			WorldStateWaypoint w;
			w.slot_id = c.u16();
			w.name_id = c.u16();
			w.pad     = c.u8();
			out.waypoints.push_back(w);
			if (!c.ok) return false;
		}
	}
	out.team_name_count = c.u16();
	if (!c.ok) return false;
	out.team_names.reserve(out.team_name_count);
	for (unsigned i = 0; i < out.team_name_count; ++i) {
		out.team_names.push_back(c.cstr());
		if (!c.ok) return false;
	}
	return (c.p == c.end);
}

// S2C 0x3A medic-reviving. The handler takes no arguments and reads no
// bytes — it latches the local entity's +0x1E0 word and runs two presentation
// legs — so any body length is accepted, exactly as retail ignores it.
// [orig: NapiNPClientMsg_0x03A @ 0x422680]
bool decode_medic_reviving(const uint8_t * /*body*/, size_t /*len*/) {
	return true;
}

// S2C 0x60 / 0x64 chunked file transfer (shared decoder).
// [orig: NapiNPClientMsg_HandleFileTransferChunk @ 0x432350 (0x60) /
//        NapiNPClientMsg_HandleMissionDataChunk @ 0x432410 (0x64)]
bool decode_file_transfer_chunk(const uint8_t *body, size_t len, FileTransferChunk &out) {
	out = FileTransferChunk{};
	Cursor c{body, body + len, true};
	out.transfer_id  = c.u32();
	out.total_size   = c.u32();
	out.chunk_offset = c.u32();
	if (!c.ok) return false;          // need the full 12-byte header
	out.chunk_size = size_t(c.end - c.p);
	out.chunk_data = c.p;             // remaining bytes are the raw payload slice
	return true;                      // 12 + chunk_size == len by construction
}

// C2S 0x22 player-sync request. [orig: NapiNPServerMsg_0x022 @ 0x514C90]
bool decode_burst_player_sync_request(const uint8_t *body, size_t len,
                                      BurstPlayerSyncRequest &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.slot        = c.u8();
	out.field_flags = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 3;
}

// C2S 0x23 visible-players request (empty body). [orig: NapiNPServerMsg_0x023_WeaponOverlayBroadcast @ 0x514D50]
bool decode_burst_visible_request(const uint8_t * /*body*/, size_t len, size_t &consumed) {
	consumed = 0;
	return len == 0;
}

// C2S 0x32 empty-slot sweep request. The host handler reads no fields — it walks
// pool 0 and answers S2C 0x5D — so this consumes nothing and accepts any body
// length (a stock client's exact filler, if any, is unwitnessed).
// [orig: NapiNPServerMsg_SendEmptySlots @ 0x51a600]
bool decode_empty_slots_request(const uint8_t * /*body*/, size_t /*len*/, size_t &consumed) {
	consumed = 0;
	return true;
}

// C2S 0x28 join-window kill-list request: [u32 windowMin][u32 windowMax][u16 start]
// (the IDB's "weapon loadout" field names are kept for the consumers).
// [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @ 0x51A550 — reads
//  @0x51a592 / @0x51a5a1 / @0x51a5b0, page builder call @0x51a5c0]
bool decode_burst_loadout_request(const uint8_t *body, size_t len,
                                  BurstLoadoutRequest &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.loadout_filter = c.u32();  // windowMin
	out.flags          = c.u32();  // windowMax
	out.extra          = c.u16();  // start (resume index)
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 10;
}

// C2S 0x29 team/spawn ack. [orig: emitted with team_index+1 by the client's 0x51 apply
// @0x431c99 and at deploy/team pick; the server reads it as a g_TeamChangeEntityList
// index — NapiNPServerMsg_0x029 @0x514F10 @0x514f7c — replying 0x51 only for a pending
// team-change entry (D-NET-148)]
bool decode_team_spawn_ack(const uint8_t *body, size_t len,
                           TeamSpawnAck &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.team_change_index = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 2;
}

// C2S 0x4C client quality/state byte (server clamps to 4 @ 0x5111fd).
// [orig: NapiNPServerMsg_0x04C @ 0x5111B0]
bool decode_burst_client_quality(const uint8_t *body, size_t len,
                                 BurstClientQuality &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	uint8_t v = c.u8();
	if (!c.ok) return false;
	out.value = v > 4 ? uint8_t(4) : v;
	consumed = size_t(c.p - body);
	return consumed == 1;
}

// ---------------------------------------------------------------------------
// Session/transport control pings (§5.34).
// ---------------------------------------------------------------------------

// S2C 0x57 / C2S 0x2C RTT ping/pong — 5-B `[u32 timestamp][u8 echo_flag]`.
// [orig: NapiNPClientMsg_0x057_RTT @ 0x432210 (S2C);
//        NapiNPServerMsg_HandlePingResponse @ 0x515070 (C2S 0x2C)]
bool decode_rtt_sample(const uint8_t *body, size_t len,
                       RttSample &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.timestamp = c.u32();
	out.echo_flag = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 5;
}

// S2C 0x68/0x43/0x39 periodic request trio — a single `[u32]` inbound scalar.
// Each handler reads the same 4-byte body and queues a different fixed reply
// (0x68→C2S 0x3D, 0x43→C2S 0x08, 0x39→C2S 0x1C); only the inbound parse lives
// here. [orig: 0x42DAA0 (0x68), 0x42FA90 (0x43), 0x42E6D0 (0x39)]
bool decode_u32_scalar(const uint8_t *body, size_t len,
                       uint32_t &out_value, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out_value = c.u32();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 4;
}

// ---------------------------------------------------------------------------
// Minimap / reload / lifecycle scalars (§5.35).
// ---------------------------------------------------------------------------

// S2C 0x6B minimap overlay batch — [u8 count] + count × 12-B records. The
// handler consumes ALL 12 bytes of each record: handle, whole-unit s16 x/y/z
// (<<16 at fold), lifetime in SECONDS (x62 at fold), the type byte, and the
// height byte (the ring radius source). The old "10 trailing bytes unread"
// gloss was a decompile artifact (D-NET-77, corrected 2026-08-13).
// [orig: NapiNPClientMsg_0x06B @ 0x425520 marshalling @0x42559f..0x4255dd]
bool decode_minimap_overlay_batch(const uint8_t *body, size_t len,
                                  MinimapOverlayBatch &out) {
	out = MinimapOverlayBatch{};
	Cursor c{body, body + len, true};
	uint8_t count = c.u8();
	if (!c.ok) return false;
	out.entries.reserve(count);
	for (uint8_t i = 0; i < count; ++i) {
		MinimapOverlayBatch::Entry e;
		e.handle = c.u16();
		e.x = static_cast<int16_t>(c.u16());
		e.y = static_cast<int16_t>(c.u16());
		e.z = static_cast<int16_t>(c.u16());
		e.lifetime_s = c.u16();
		e.type = c.u8();
		e.height = c.u8();
		if (!c.ok) return false;
		out.entries.push_back(e);
	}
	return size_t(c.p - body) == size_t(1) + size_t(12) * count;
}

// S2C 0x49 weapon-reload notification — [u16 handle][u16 reloadParam] (4 B).
// [orig: NapiNPClientMsg_WeaponReload_0x049 @ 0x42C0A0 (IDA-misnamed; reloads ammo)]
bool decode_weapon_reload(const uint8_t *body, size_t len,
                          WeaponReload &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	out.reload_param  = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 4;
}

// S2C 0x35 weapon pickup -- [u16 picker][u16 powerup] (4 B).
// [orig: NapiNPClientMsg_0x035 @0x4261A0 -- the two u16 reads @0x4261B8 /
//  @0x4261C8]
bool decode_weapon_pickup(const uint8_t *body, size_t len,
                          WeaponPickupNotice &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.picker_handle = c.u16();
	out.powerup_handle = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 4;
}

bool decode_mounted_weapon_slot_selection(
		const uint8_t *body, size_t len,
		MountedWeaponSlotSelection &out, size_t &consumed) {
	out = MountedWeaponSlotSelection{};
	consumed = 0;
	if (body == nullptr || len != 2) return false;
	const uint16_t value = static_cast<uint16_t>(body[0]) |
			(static_cast<uint16_t>(body[1]) << 8);
	out.use_parent_slot = value != 0;
	consumed = 2;
	return true;
}

bool decode_auto_medic_preference(
		const uint8_t *body, size_t len,
		AutoMedicPreference &out, size_t &consumed) {
	out = AutoMedicPreference{};
	consumed = 0;
	// The handler reads the first dword when at least four bytes arrived and
	// ignores any tail. [orig: NapiNPServerMsg_AutoMedicPreference @0x501C12]
	if (body == nullptr || len < 4) return false;
	const uint32_t disabled = static_cast<uint32_t>(body[0]) |
			(static_cast<uint32_t>(body[1]) << 8) |
			(static_cast<uint32_t>(body[2]) << 16) |
			(static_cast<uint32_t>(body[3]) << 24);
	out.enabled = disabled == 0;
	consumed = 4;
	return true;
}

bool decode_medic_request(const uint8_t *body, size_t len,
		MedicRequest &out, size_t &consumed) {
	out = MedicRequest{};
	consumed = 0;
	if (body == nullptr || len != 4) return false;
	out.entity_index = static_cast<uint32_t>(body[0]) |
			(static_cast<uint32_t>(body[1]) << 8) |
			(static_cast<uint32_t>(body[2]) << 16) |
			(static_cast<uint32_t>(body[3]) << 24);
	consumed = 4;
	return true;
}

// S2C 0x13 entity death (second path) — [u16 handle][i16 deathAnimStateId] (4 B).
// [orig: NetPacket_BuildDeathNotifyPayload @0x5036E0; NapiNPClientMsg_EntityDeath @ 0x42EB50]
bool decode_entity_death(const uint8_t *body, size_t len,
                         EntityDeathRecord &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	out.death_anim_state_id = c.i16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 4;
}

bool decode_death_camera_target(const uint8_t *body, size_t len,
		DeathCameraTarget &out, size_t &consumed) {
	out = DeathCameraTarget{};
	consumed = 0;
	if (body == nullptr || len != 12) return false;
	Cursor c{body, body + len, true};
	out.x = c.i32();
	out.y = c.i32();
	out.z = c.i32();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 12;
}

bool decode_player_downed_state(const uint8_t *body, size_t len,
		PlayerDownedState &out, size_t &consumed) {
	out = PlayerDownedState{};
	consumed = 0;
	if (body == nullptr || len != 3) return false;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	const uint8_t packed = c.u8();
	if (!c.ok) return false;
	out.revive_seconds = static_cast<uint8_t>(packed & 0x7Fu);
	out.medic_request_active = (packed & 0x80u) != 0;
	consumed = size_t(c.p - body);
	return consumed == 3;
}

// [orig: GameMode_DispatchRemoteCommand @0x4F81E0]
bool decode_script_remote_command(const uint8_t *body, size_t len,
		ScriptRemoteCommand &out, size_t &consumed) {
	out = ScriptRemoteCommand{};
	consumed = 0;
	if (body == nullptr || len < 2) return false;
	Cursor c{body, body + len, true};
	out.command_index = c.u16(); // @0x4f827e
	if (out.command_index >= wac::wac_command_count()) return false;
	const wac::CommandDef &def = wac::wac_commands()[out.command_index];
	out.args.resize(static_cast<size_t>(def.argc));
	for (int i = 0; i < def.argc; ++i) {
		ScriptRemoteCommandArg &arg = out.args[static_cast<size_t>(i)];
		const wac::ParamType type = def.params[i];
		if (type == wac::ParamType::Text || type == wac::ParamType::Filename) {
			// Byte by byte until the NUL; the 251st byte read is dropped in
			// favor of a NUL, and a short body ends the string with zero.
			// [@0x4f8385..0x4f83e9]
			size_t chars = 0;
			uint8_t ch = c.u8();
			while (ch != 0) {
				arg.text.push_back(static_cast<char>(ch));
				++chars;
				ch = c.u8();
				if (chars >= 250) break;
			}
		} else if (type == wac::ParamType::Ssn) {
			arg.value = c.u16(); // @0x4f8320..0x4f834a
		} else {
			arg.value = c.u32(); // @0x4f835a..0x4f837a
		}
	}
	out.read_error = !c.ok;
	consumed = size_t(c.p - body);
	return wac::cmd_is_replicated(def); // @0x4f8429
}

// S2C 0x30 entity-checksum request — [u8 entityId][u16 checksum] (3 B) → C2S 0x20.
// [orig: NapiNPClientMsg_HandleChecksumRequest @ 0x431170]
bool decode_entity_checksum_request(const uint8_t *body, size_t len,
                                    EntityChecksumRequest &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_id = c.u8();
	out.checksum  = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 3;
}

// S2C 0x31 loadout/ammo CRC request — [u8 ammoIndex][u16 xorKey] (3 B) → C2S 0x21.
// [orig: NapiNPClientMsg_0x031 @ 0x4311E0]
bool decode_loadout_crc_request(const uint8_t *body, size_t len,
                                LoadoutCrcRequest &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.ammo_index = c.u8();
	out.xor_key    = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 3;
}

// S2C 0x42 input/state-flags — [u16] (2 B). [orig: NapiNPClientMsg_0x042 @ 0x4281A0]
bool decode_input_state_flags(const uint8_t *body, size_t len,
                              uint16_t &out_flags, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out_flags = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 2;
}

// S2C 0x79 host network-quality scalar — [u8] (1 B).
// [orig: Server_TickUpdate @0x51E1B2..0x51E202 / NapiNPClientMsg_NetworkQuality @0x429B00]
bool decode_network_quality(const uint8_t *body, size_t len,
                            uint8_t &out_quality, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out_quality = c.u8();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 1;
}

// S2C 0x2A chat-history entry — [i32 a][i32 b][i16 c] (10 B).
// [orig: NapiNPClientMsg_0x02A @ 0x425BA0]
bool decode_chat_history_entry(const uint8_t *body, size_t len,
                               ChatHistoryEntry &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.field_a = c.i32();
	out.field_b = c.i32();
	out.field_c = c.i16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 10;
}

// ---------------------------------------------------------------------------
// Deployed-item spawn (0x59) + entity-routed sub-packet (0x44) (§5.36).
// ---------------------------------------------------------------------------

// S2C 0x12 destroy-one-entity — [u16 packed handle].
// [orig: NapiNPClientMsg_0x012 @ 0x425EE0 -> Entity_Destroy @ 0x43E810]
bool decode_entity_remove(const uint8_t *body, size_t len,
	                      EntityRemove &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 2;
}

// S2C 0x3F. The safe wire boundary rejects a short body where the retail
// guarded reads zero-fill. [orig: NapiNPClientMsg_0x03F @0x42BB20]
bool decode_objective_notification(const uint8_t *body, size_t len,
		ObjectiveNotification &out, size_t &consumed) {
	consumed = 0;
	out = ObjectiveNotification{};
	Cursor c{body, body + len, true};
	out.kind = c.u8();
	if (out.kind == 0) {
		out.slot = c.i32();
		out.is_win = c.i32();
		out.is_active = c.i32();
		out.flag = c.u8();
	} else if (out.kind == 1) {
		out.team = c.i32();
		out.key = c.cstr();
		// The 256-byte stack copy keeps 255 chars [orig: @0x42bbff..0x42bc1c].
		if (out.key.size() > 255) out.key.resize(255);
	}
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return true;
}

// S2C 0x2F flag/carryable state — fixed 19 bytes. Retail's guarded cursor
// zero-fills a short body; the safe wire boundary rejects it instead.
// [orig: NapiNPClientMsg_0x02F @0x430E10]
bool decode_objective_entity_state(const uint8_t *body, size_t len,
	                               ObjectiveEntityState &out,
	                               size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.entity_handle = c.u16();
	out.flags_byte = c.u8();
	out.pos_x = c.i32();
	out.pos_y = c.i32();
	out.pos_z = c.i32();
	out.attach_handle = c.u16();
	out.ground_handle = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 19;
}

// S2C 0x59 deployed-item spawn-or-update — fixed 32-B record (the handler
// reads 15 u16s = 30 B; 2 trailing reserved). pos = 3×i32 16.16; angles 3×u16
// (the yaw/pitch/roll high halves). [orig: NapiNPClientMsg_0x059 @ 0x4228E0
//  -> Entity_SpawnOrUpdateFromSlotPacket @ 0x546770]
bool decode_deployed_item_spawn(const uint8_t *body, size_t len,
                                DeployedItemSpawn &out, size_t &consumed) {
	consumed = 0;
	Cursor c{body, body + len, true};
	out.item_id          = c.u16();
	out.owner_handle     = c.u16();
	out.friendly_item_id = c.u16();
	out.enemy_item_id    = c.u16();
	out.slot_handle      = c.u16();
	out.parent_handle    = c.u16();
	out.pos_x            = c.i32();
	out.pos_y            = c.i32();
	out.pos_z            = c.i32();
	out.angle_x          = c.u16();
	out.angle_y          = c.u16();
	out.angle_z          = c.u16();
	out.reserved         = c.u16();
	if (!c.ok) return false;
	consumed = size_t(c.p - body);
	return consumed == 32;
}

// S2C 0x44 entity-routed sub-packet — 5-B sub-header + class-dependent body.
// [orig: NetPacket_DispatchToEntityByNetId @ 0x4D6960]
bool decode_entity_routed_packet(const uint8_t *body, size_t len,
                                 EntityRoutedPacket &out) {
	out = EntityRoutedPacket{};
	Cursor c{body, body + len, true};
	out.field0  = c.u16();
	out.net_id  = c.i16();
	out.subtype = c.u8();
	if (!c.ok) return false;       // need the 5-byte sub-header
	out.body = c.p;
	out.body_size = size_t(c.end - c.p);
	return true;
}

// S2C 0x45 terrain-tile load batch — §5.37. Paged stream the host sends during a
// client's initial-state load (phase 5 of Server_SendInitialGameStateToPlayer).
// Witnessed byte-exact against the writer + reader (52-tile header chunk = 644 B,
// 53-tile chunk = 640 B in operation_whitenoise). The 12-B tile entries are
// opaque copies the network layer never interprets (D-NET-83).
// [orig: Terrain_SerializeTiles @ 0x6080F0 / PolyTrn_LoadTileData @ 0x6081D0 /
//  NapiNPClientMsg_0x045 @ 0x422890]
bool decode_terrain_load_batch(const uint8_t *body, size_t len, TerrainLoadBatch &out) {
	out = TerrainLoadBatch{};
	Cursor c{body, body + len, true};
	const uint16_t first = c.u16();   // wire start word
	out.end_index = c.u16();
	if (!c.ok) return false;          // need the 4-byte [start][end] frame
	out.has_header = (first == kTerrainFirstChunkStartWord);
	if (out.has_header) {
		out.start_index = 0;
		out.magic = c.u32();          // 'til0'
		out.tile_count = c.u32();
		out.header_field2 = c.u32();
		out.header_field3 = c.u32();
		if (!c.ok) return false;
		if (out.magic != kTerrainTileMagic) return false;  // reader returns 1 (no load) otherwise
	} else {
		out.start_index = first;
	}
	if (out.end_index < out.start_index) return false;
	const uint32_t n = uint32_t(out.end_index) - out.start_index;
	out.tiles.reserve(n);
	for (uint32_t i = 0; i < n; ++i) {
		TerrainTileEntry e;
		e.word0 = c.u32();
		e.word1 = c.u32();
		e.word2 = c.u32();
		if (!c.ok) return false;
		out.tiles.push_back(e);
	}
	return c.p == c.end;  // consumed exactly
}

// §5.48 S2C 0x58 — [orig: SessionStatus_ParseFromBuffer @ 0x530ED0]. The client
// truncates the strings on store (31/63 kept) and keeps only the first 8 kv
// pairs with key <= 9; the decoder carries the wire values verbatim.
bool decode_session_status(const uint8_t *body, size_t len, SessionStatusBlock &out) {
	out = SessionStatusBlock{};
	Cursor c{body, body + len, true};
	out.server_name = c.cstr();
	out.mission_name = c.cstr();
	out.byte0 = c.u8();
	out.byte1 = c.u8();
	out.byte2 = c.u8();
	out.uptime_ms = c.u32();
	for (int i = 0; i < 39; ++i) out.stat_values[i] = c.i32();
	out.kv_count = c.u8();
	if (!c.ok) return false;
	// The retail reader is bounds-tolerant: each of the kv_count pairs reads 0
	// on underflow (@0x531055 for the key, @0x531067 for the value), keeps the
	// pair, and the record is stamped valid unconditionally (@0x5310AA), so a
	// wire count larger than the pairs actually present is legal — golden retail
	// sends it. Mirror that byte for byte: a partial final pair zero-fills the
	// missing bytes instead of failing the decode.
	for (uint8_t i = 0; i < out.kv_count; ++i) {
		SessionStatusKV kv;
		kv.key = (c.p + 1 <= c.end) ? c.u8() : 0;
		kv.value = (c.p + 4 <= c.end) ? c.u32() : 0;
		out.kv.push_back(kv);
	}
	// The retail parser stops here and the dispatcher never requires full
	// consumption. The writer's loop runs one pair past kv_count, so the body
	// ends in that unadvertised {0, 0} pair [orig: Server_BuildStatusReport
	// @0x530A60]; anything after it is surfaced as trailing bytes.
	if (c.p + 5 <= c.end) {
		out.writer_sentinel.key = c.u8();
		out.writer_sentinel.value = c.u32();
		out.has_writer_sentinel = true;
	}
	out.trailing_bytes = size_t(c.end - c.p);
	c.skip(out.trailing_bytes);
	return true;
}

// §5.49 S2C 0x6F — [orig: NapiNPClientMsg_ZoneTimerValue @ 0x428D60]. Fixed 15 B.
bool decode_zone_timer_value(const uint8_t *body, size_t len,
                             ZoneTimerValue &out, size_t &consumed) {
	out = ZoneTimerValue{};
	Cursor c{body, body + len, true};
	out.zone_handle = c.u16();
	out.mode = c.u8();
	out.value_s = c.i32();
	out.limit_s = c.i32();
	out.rate = c.i16();
	out.byte544 = c.u8();
	out.byte545 = c.u8();
	consumed = c.ok ? size_t(c.p - body) : 0;
	return c.ok;
}

// §5.49 S2C 0x53 — [orig: NapiNPClientMsg_ZoneTimerWindow @ 0x428AE0]. Fixed 9 B.
bool decode_zone_timer_window(const uint8_t *body, size_t len,
                              ZoneTimerWindow &out, size_t &consumed) {
	out = ZoneTimerWindow{};
	Cursor c{body, body + len, true};
	out.zone_handle = c.u16();
	out.mode_a = c.u8();
	out.mode_b = c.u8();
	out.start_s = c.u16();
	out.end_s = c.u16();
	out.rate = c.u8();
	consumed = c.ok ? size_t(c.p - body) : 0;
	return c.ok;
}

// §5.61 S2C 0x6C — [orig: NapiNPClientMsg_0x06C @ 0x428FC0]. Fixed 3 B.
bool decode_zone_presence_count(const uint8_t *body, size_t len,
                                ZonePresenceCount &out, size_t &consumed) {
	out = ZonePresenceCount{};
	Cursor c{body, body + len, true};
	out.zone_handle = c.u16();
	out.count = c.u8();
	consumed = c.ok ? size_t(c.p - body) : 0;
	return c.ok;
}

// S2C 0x6D — the contextual radio call. Retail defaults each absent field
// independently (event @0x430c8a, player index @0x430c9c, location word
// @0x430cbb) and still dispatches, so this decoder never fails.
// [orig: NapiNPClientMsg_HandleEntityDeath @ 0x430C50]
bool decode_tracked_player_voice(const uint8_t *body, size_t len, TrackedPlayerVoice &out) {
    io::ByteReader reader(body, len);
    out.event = reader.read_u8();
    out.player_index = reader.read_u8();
    out.location = reader.read_i16();
    return true;
}

// §5.50 S2C 0x34 — [orig: NapiNPClientMsg_PlaySoundByName @ 0x4283A0]. The
// position block exists on the wire only when flag == 1.
bool decode_play_sound(const uint8_t *body, size_t len, PlaySoundCommand &out) {
	out = PlaySoundCommand{};
	Cursor c{body, body + len, true};
	out.flag = c.u8();
	out.sound_name = c.cstr();
	if (out.flag == 1) {
		out.has_pos = true;
		out.pos_x = c.i16();
		out.pos_y = c.i16();
		out.pos_z = c.i16();
	}
	return c.ok && (c.p == c.end);
}

// §5.51 S2C 0x2C — [orig: NapiNPClientMsg_MissionMapNames @ 0x427E10].
bool decode_mission_map_names(const uint8_t *body, size_t len, MissionMapNames &out) {
	out = MissionMapNames{};
	Cursor c{body, body + len, true};
	out.session_name = c.cstr();
	out.map_file_name = c.cstr();
	return c.ok && (c.p == c.end);
}

// §5.52 C2S 0x0D — [orig: NapiNPServer_HandleChatMessage @ 0x513760].
bool decode_chat_uplink(const uint8_t *body, size_t len, ChatUplink &out) {
	out = ChatUplink{};
	Cursor c{body, body + len, true};
	out.channel = c.u8();
	out.text = c.cstr();
	return c.ok && (c.p == c.end);
}

// §5.52 S2C 0x14 — [orig: NapiNPClientMsg_ChatMessage @ 0x42F240].
bool decode_chat_broadcast(const uint8_t *body, size_t len, ChatBroadcast &out) {
	out = ChatBroadcast{};
	Cursor c{body, body + len, true};
	// [channel][sender_slot][cstr] — the dispatcher takes the roster index
	// FIRST and the switched channel SECOND, and the call passes body[1] then
	// body[0] [orig: NapiNPClientMsg_ChatMessage @0x42F240 ->
	// Chat_DispatchToChannel @0x42B910].
	out.channel = static_cast<int8_t>(c.u8());
	out.sender_slot = c.u8();
	out.text = c.cstr();
	return c.ok && (c.p == c.end);
}

// S2C 0x28 — [orig: NapiNPClientMsg_0x028 @0x425B40].
bool decode_dialog_line(const uint8_t *body, size_t len, DialogLine &out) {
	out = DialogLine{};
	const uint8_t *p = body;
	const uint8_t *const end = body + len;
	while (p < end && *p != 0) ++p;
	out.dialog_name.assign(reinterpret_cast<const char *>(body),
			static_cast<size_t>(p - body));
	const bool terminated = p < end;
	if (terminated) ++p;
	if (p + 2 <= end) {
		out.line = static_cast<int16_t>(p[0] | (p[1] << 8));
		p += 2;
	} else {
		return false;
	}
	return terminated && p == end;
}

// S2C 0x32 formatted game text. [orig: NapiNPClientMsg_0x032 @0x428060]
bool decode_formatted_game_text(const uint8_t *body, size_t len, FormattedGameText &out,
		bool *clean) {
	out = FormattedGameText{};
	const uint8_t *p = body;
	const uint8_t *const end = body + len;
	bool ok = true;
	// The subtype byte, 0 on an empty body [orig: @0x428075..0x42807d], read
	// signed into the switch [orig: movsx @0x428080].
	if (p + 1 <= end) out.subtype = static_cast<int8_t>(*p++);
	else ok = false;
	if (out.subtype < kGameTextPlayerJoined || out.subtype > kGameTextPlayerSpectating) {
		if (clean != nullptr) *clean = false;
		return false; // [orig: the default arm returns @0x428099]
	}
	// The text is the payload up to its NUL; an unterminated tail keeps what
	// is there (retail's strlen reads on past the body).
	const uint8_t *nul = p;
	while (nul < end && *nul != 0) ++nul;
	out.text.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(nul - p));
	if (nul >= end) ok = false;
	const uint8_t *after = nul < end ? nul + 1 : end;
	if (out.subtype == kGameTextPlayerJoined || out.subtype == kGameTextPlayerLeaving) {
		// The team byte past the NUL (clamped to the body end), 0 when short,
		// sign-extended [orig: @0x4280b8..0x4280d5].
		if (after + 1 <= end) out.team = static_cast<int8_t>(*after++);
		else ok = false;
	}
	if (after != end) ok = false;
	if (clean != nullptr) *clean = ok;
	return true;
}

bool decode_end_round_header(const uint8_t *body, size_t len,
		bool non_team_form, EndRoundHeader &out) {
	out = EndRoundHeader{};
	Cursor c{body, body + len, true};
	if (non_team_form) {
		// The in-session non-team form replaces the winner/team-score words
		// with the top three frozen-board rows. Retail keeps at most 31 chars
		// of each name (32-byte staging + Napi_CopyString(dst, 32) commit)
		// while the cursor advances by the sender's full string.
		// [orig: NapiNPClientMsg_0x01D @0x430889..0x4309af, commit
		// @0x430a70..0x430ac1]
		for (std::string &name : out.player_names) {
			name = c.cstr();
			if (name.size() > 31) name.resize(31);
		}
		for (int16_t &score : out.player_scores) score = c.i16();
	} else {
		out.winner_team = static_cast<int8_t>(c.u8());
		out.team_score_0 = c.i16();
		out.team_score_1 = c.i16();
	}
	out.draw = c.u8();
	out.player_index = static_cast<int8_t>(c.u8());
	return c.ok && c.p == c.end;
}

bool decode_end_round_stats_request(const uint8_t *body, size_t len,
		EndRoundStatsRequest &out) {
	out = EndRoundStatsRequest{};
	Cursor c{body, body + len, true};
	out.offset = c.u16();
	return c.ok && c.p == c.end;
}

// §5.68 S2C 0x56 envelope — [orig: NapiNPClientMsg_0x056 @0x431D10, the two
// u16 reads @0x431D4D / @0x431D61]. The remainder of the datagram is the chunk
// (at most 200 bytes: the server cuts it with NetPacket_WriteReplayStreamChunk
// @0x506F60 — `if (copyLen > 200) copyLen = 200` @0x506FB9).
bool decode_end_round_stats_chunk(const uint8_t *body, size_t len,
                                  EndRoundStatsChunk &out) {
	out = EndRoundStatsChunk{};
	Cursor c{body, body + len, true};
	out.total_size = c.u16();
	out.chunk_offset = c.u16();
	if (!c.ok) return false;
	out.chunk.assign(c.p, c.end);
	return true;
}

// §5.68 the reassembled board — [orig: the parse from @0x431E0B].
bool decode_end_round_stats(const uint8_t *data, size_t len, EndRoundStats &out) {
	out = EndRoundStats{};
	Cursor c{data, data + len, true};
	out.winner_team = static_cast<int8_t>(c.u8());
	out.team_score_0 = c.i16();
	out.team_score_1 = c.i16();

	// The declared team columns. This count sizes each row's per-team array
	// AND the trailing matrix, so a bad count corrupts everything after it.
	// Retail reads it as a SIGNED byte (movsx @0x431E69) and every loop it
	// drives is a `> 0` test [orig: @0x431E75 / @0x432091 / @0x432174], so a
	// byte >= 0x80 declares NO fields rather than 128..255 of them.
	const int field_count = static_cast<int8_t>(c.u8());
	if (!c.ok) return false;
	if (field_count > 0) out.team_fields.reserve(static_cast<size_t>(field_count));
	for (int i = 0; i < field_count; ++i) {
		const uint8_t f0 = c.u8();
		const uint8_t f1 = c.u8();
		if (!c.ok) return false;
		out.team_fields.emplace_back(f0, f1);
	}

	const uint8_t player_count = c.u8();
	if (!c.ok) return false;
	out.players.reserve(player_count);
	for (uint8_t i = 0; i < player_count; ++i) {
		EndRoundPlayerRow r;
		r.slot = c.u8();
		// Three NUL-terminated strings, each clipped to 31 chars by retail's
		// 32-byte copy loops [orig: @0x431F5A / @0x431F9E / @0x431FE2].
		r.name = c.cstr();
		r.clan = c.cstr();
		r.tag = c.cstr();
		if (r.name.size() > 31) r.name.resize(31);
		if (r.clan.size() > 31) r.clan.resize(31);
		if (r.tag.size() > 31) r.tag.resize(31);
		r.team = c.u8();
		r.player_class = c.u8();
		// WIRE order; retail's slot shuffle is the column layout, not this.
		r.kills = c.i16();
		r.deaths = c.i16();
		r.assists = c.i16();
		r.score = c.i16();
		r.captures = c.i16();
		r.flags = c.i16();
		r.special = c.i16();
		if (field_count > 0) r.per_team.reserve(static_cast<size_t>(field_count));
		for (int f = 0; f < field_count; ++f) r.per_team.push_back(c.i16());
		if (!c.ok) return false;
		out.players.push_back(std::move(r));
	}

	// The trailing per-team matrix. Retail reads its row count as a SIGNED
	// byte and skips the loop when negative [orig: the `(char)` cast and the
	// `> 0` test @0x4321A2].
	const int8_t team_row_count = static_cast<int8_t>(c.u8());
	if (!c.ok) return false;
	for (int8_t rrow = 0; rrow < team_row_count; ++rrow) {
		std::vector<int16_t> row;
		if (field_count > 0) row.reserve(static_cast<size_t>(field_count));
		for (int f = 0; f < field_count; ++f) row.push_back(c.i16());
		if (!c.ok) return false;
		out.team_rows.push_back(std::move(row));
	}
	return c.ok;
}

// §5.53 S2C 0x04 — [orig: NapiNPClientMsg_SessionSlotConfig @ 0x425410]. Fixed 24 B.
bool decode_session_slot_config(const uint8_t *body, size_t len, SessionSlotConfig &out) {
	out = SessionSlotConfig{};
	Cursor c{body, body + len, true};
	for (int i = 0; i < 4; ++i) out.skipped[i] = c.u32();
	out.session_config = c.u8();
	out.local_player_slot = c.u8();
	out.max_players = c.u8();
	out.skipped4 = c.u32();
	out.trailing = c.u8();
	return c.ok && (c.p == c.end);
}

// §5.54 S2C 0x08 — [orig: NapiNPClientMsg_HandleSessionConfig @ 0x4281D0]. Fixed 51 B.
bool decode_session_config(const uint8_t *body, size_t len, SessionConfig &out) {
	out = SessionConfig{};
	Cursor c{body, body + len, true};
	for (int i = 0; i < 10; ++i) out.fields[i] = c.i32();
	for (int i = 0; i < 7; ++i) out.bytes[i] = c.u8();
	out.bitflags = c.u32();
	return c.ok && (c.p == c.end);
}

// §5.55 S2C 0x02 — [orig: NapiNPClientMsg_HandleJoinResponse @ 0x42E0F0]. The
// handler reads 12 B and ignores the rest of the body (random filler); consume
// it explicitly so the coverage contract ("body accounted for") holds.
bool decode_join_padding_probe(const uint8_t *body, size_t len, JoinPaddingProbe &out) {
	out = JoinPaddingProbe{};
	Cursor c{body, body + len, true};
	out.pos_x = c.i32();
	out.pos_y = c.i32();
	out.padding_len = c.u32();
	if (!c.ok) return false;
	out.filler_bytes = size_t(c.end - c.p);
	c.skip(out.filler_bytes);
	return c.ok && (c.p == c.end);
}

// §5.56 C2S 0x2F — [orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790].
// Retail reads every field through a bounds-guarded cursor that substitutes
// ZERO past the end and stops the entry loop ONLY on the 0xFF terminator; it
// never checks for trailing bytes after that exit (@ 0x515a99), so a body with
// extra bytes after the terminator is accepted and processed. An UNTERMINATED
// list is rejected here as a deliberate crash-safe divergence: retail's
// zero-filled reads never produce 0xFF and its entry loop cannot exit
// (@ 0x5159c0 zero-fill feeding the @ 0x515a99 backedge).
bool decode_loadout_submit(const uint8_t *body, size_t len, LoadoutSubmit &out) {
	out = LoadoutSubmit{};
	Cursor c{body, body + len, true};
	out.team = c.u8();
	out.player_class = c.u8();
	out.weapon_slot_index = c.u32();
	while (c.ok) {
		const uint8_t adm = c.u8();
		if (!c.ok) break;
		if (adm == 0xFF) { out.terminated = true; break; }
		LoadoutSubmitEntry e;
		e.adm_index = adm;
		e.ammo_primary = c.u8();
		e.ammo_secondary = c.u8();
		e.variant = c.u8();
		if (c.ok) out.entries.push_back(e);
	}
	return c.ok && out.terminated;
}

// [orig: NapiNPClientMsg_HandleSpawnEffect @0x430B10]
// S2C 0x37 / C2S 0x1A door-row action — the one 5-byte body. Each field reads 0
// when the body runs short (client @0x431265 / @0x431275 / @0x431288; server
// @0x514b61 / @0x514b72 / @0x514b8c), so a short body decodes zero-filled and
// returns false — `number` 0 is dropped by every receiver's gate regardless.
// [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250;
//  NapiNPServerMsg_HandleVoteUpdate @0x514B20]
bool decode_door_slot_action(const uint8_t *body, size_t len,
                             DoorSlotAction &out, size_t &consumed) {
	out = DoorSlotAction{};
	consumed = 0;
	Cursor c{body, body + len, true};
	if (c.p + 2 <= c.end) out.entity_handle = c.u16();
	if (c.p + 2 <= c.end) out.state = c.i16();
	if (c.p + 1 <= c.end) out.number = c.u8();
	consumed = size_t(c.p - body);
	return consumed == 5;
}

// S2C 0x6A clan-roster update. [orig: NapiNPClientMsg_HandlePlayerJoinLeave @0x432510]
bool decode_clan_roster_update(const uint8_t *body, size_t len, ClanRosterUpdate &out) {
	out = ClanRosterUpdate{};
	Cursor c{body, body + len, true};
	if (c.p + 1 <= c.end) out.action = c.u8();                 // @0x43254b (0 on an empty body @0x432547)
	if (out.action == kClanRosterRemove) {
		if (c.p + 4 <= c.end) out.account_id = c.u32();        // @0x432570 (0 when short @0x43256c)
		return c.ok && c.p == c.end;
	}
	if (out.action != kClanRosterAdd && out.action != kClanRosterWalkReply)
		return false;                                          // any other action returns untouched @0x4325a1
	if (c.p + 4 <= c.end) out.account_id = c.u32();            // @0x4325b1 (left 0 when short)
	// The name copy keeps <= 64 chars, stopping at NUL or the body end
	// (@0x4325bc..0x4325db); the tag then starts past the FULL name's NUL
	// (@0x4325ea, clamped to the body end @0x4325f6) and keeps <= 8 chars
	// (@0x4325fc..0x43261b). An unterminated name is where retail's strlen
	// leaves the body: the partial name stands, the tag is empty, and this
	// decoder reports the body unclean.
	out.name = c.cstr();
	if (out.name.size() > kClanRosterNameChars) out.name.resize(kClanRosterNameChars);
	if (!c.ok) return false;
	out.tag = c.cstr();
	if (out.tag.size() > kClanRosterTagChars) out.tag.resize(kClanRosterTagChars);
	return c.ok && c.p == c.end;
}

// C2S 0x4E clan-roster walk request. [orig: NapiNPServerMsg_HandleMinimapSlotRequest
// @0x511210 — the u32 read @0x51124b, 0 when short @0x511247]
bool decode_clan_roster_walk_request(const uint8_t *body, size_t len,
                                     ClanRosterWalkRequest &out, size_t &consumed) {
	out = ClanRosterWalkRequest{};
	consumed = 0;
	Cursor c{body, body + len, true};
	if (c.p + 4 <= c.end) out.after_account_id = c.u32();
	consumed = size_t(c.p - body);
	return consumed == 4;
}

// S2C 0x70 vehicle-spawn availability list. [orig: NapiNPClientMsg_HandleWeaponLoadoutList
// @0x429a30 — leading byte @0x429a53 (0 when empty @0x429a4f), the row loop ends when
// fewer than two bytes remain @0x429a6a or on the 0 word @0x429a74, and each missing
// row byte reads 0 @0x429a7d / @0x429a8c]
bool decode_vehicle_spawn_availability(const uint8_t *body, size_t len,
                                       VehicleSpawnAvailabilityList &out) {
	out = VehicleSpawnAvailabilityList{};
	Cursor c{body, body + len, true};
	out.leading_byte = (c.p + 1 <= c.end) ? c.u8() : uint8_t(0);
	while (c.p + 2 <= c.end) {
		VehicleSpawnAvailabilityRow row;
		row.type_id = c.u16();
		if (row.type_id == 0) {
			out.terminated = true;
			break;
		}
		if (c.p + 1 <= c.end) row.available = c.u8();
		if (c.p + 1 <= c.end) row.max_count = c.u8();
		out.rows.push_back(row);
	}
	return c.ok && out.terminated && c.p == c.end;
}

// C2S 0x42 vehicle-spawn availability request: the host reads no fields.
// [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930]
bool decode_vehicle_spawn_availability_request(const uint8_t * /*body*/, size_t /*len*/,
                                               size_t &consumed) {
	consumed = 0;
	return true;
}

// C2S 0x40 vehicle-spawn request. [orig: NapiNPServerMsg_HandleVehicleSpawnRequest
// @0x51C4C0 — handle @0x51c51b (0 when short @0x51c517), type index @0x51c52a (0 when short)]
bool decode_vehicle_spawn_request(const uint8_t *body, size_t len,
                                  VehicleSpawnRequest &out, size_t &consumed) {
	out = VehicleSpawnRequest{};
	consumed = 0;
	Cursor c{body, body + len, true};
	if (c.p + 2 <= c.end) out.source_handle = c.u16();
	if (c.p + 1 <= c.end) out.type_index = c.u8();
	consumed = size_t(c.p - body);
	return consumed == 3;
}

bool decode_explosion_effect(const uint8_t *body, size_t len, ExplosionEffectRecord &out) {
    io::ByteReader reader(body,len);
    out.type=reader.read_u8(); out.count=reader.read_u8(); out.source=reader.read_u16();
    out.x=reader.read_i32(); out.y=reader.read_i32(); out.z=reader.read_i32();
    out.heading=reader.read_i16();
    return true; // independent absent-field defaults, then dispatch
}

} // namespace opennova
