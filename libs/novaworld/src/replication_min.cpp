#include <novaworld/replication_min.h>
#include <novaworld/ingame_encode.h> // network_compress_fixedpoint, encode_frame_update

#include <algorithm>
#include <cstring>

namespace opennova {

namespace {

inline void push_u8(std::vector<uint8_t> &buf, uint8_t v) {
	buf.push_back(v);
}

inline void push_u16(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void push_u32(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void push_cstr(std::vector<uint8_t> &buf, const std::string &s, size_t max_chars) {
	const size_t n = std::min(s.size(), max_chars > 0 ? max_chars - 1 : 0);
	for (size_t i = 0; i < n; ++i) {
		buf.push_back(static_cast<uint8_t>(s[i]));
	}
	buf.push_back(0);
}

// Number of u32 entries in the team-scores array consumed by tag=0x0F's
// inner loop. Derived from `dword_B761E8 - dword_B75FE8 = 0x200` in the
// Jointops binary's data segment — see the Phase D.2.16 witness entry.
constexpr size_t TAG_0F_TEAM_SCORE_DWORDS = 128;

} // namespace

std::vector<uint8_t> build_tag_0f_game_start(const PlayerReplicationState &ctx) {
	// Phase D.0.18: retail-verbatim 624-byte tag=0x0F (combined from retail
	// capture3 frames 211497 + 211498, which retail splits into 572-byte
	// FRAG_CONT + 52-byte FRAG_END fragments). Our prior 539-byte version
	// was missing:
	//   - 33 bytes of "mystery data" between team_count and spawn_count
	//     (flags, weapon restrictions, AS-mode objectives — not yet decoded)
	//   - u16 spawn_count + 6 null-term spawn-point name strings
	// Without the spawn names, cmap.mnu has no clickable markers → user
	// can see the menu (thanks to byte_A821EF=1 from tag=0x08 bit 15) but
	// cannot pick a spawn point. Retail sends 6 dvxi5 spawn names:
	//   North Sea Village, Southside Jungle, Rocky Point, Point Doom,
	//   Ash Village, Katulus' Mound.
	// Bytes 4-15 (Position X/Y/Z as int32 LE) are patched from ctx.
		static constexpr uint8_t kRetailTag0fPayload[] = {
			0x1a, 0x6c, 0x7a, 0x07, 0x35, 0xb3, 0xb8, 0x01, 0xe1, 0x27, 0xb9, 0xfe,
			0x68, 0xde, 0x0b, 0x00, 0xaa, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
			0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
			0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x1c,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x4e,
			0x6f, 0x72, 0x74, 0x68, 0x20, 0x53, 0x65, 0x61, 0x20, 0x56, 0x69, 0x6c,
			0x6c, 0x61, 0x67, 0x65, 0x00, 0x53, 0x6f, 0x75, 0x74, 0x68, 0x73, 0x69,
			0x64, 0x65, 0x20, 0x4a, 0x75, 0x6e, 0x67, 0x6c, 0x65, 0x00, 0x52, 0x6f,
			0x63, 0x6b, 0x79, 0x20, 0x50, 0x6f, 0x69, 0x6e, 0x74, 0x00, 0x50, 0x6f,
			0x69, 0x6e, 0x74, 0x20, 0x44, 0x6f, 0x6f, 0x6d, 0x00, 0x41, 0x73, 0x68,
			0x20, 0x56, 0x69, 0x6c, 0x6c, 0x61, 0x67, 0x65, 0x00, 0x4b, 0x61, 0x74,
			0x75, 0x6c, 0x75, 0x73, 0x27, 0x20, 0x4d, 0x6f, 0x75, 0x6e, 0x64, 0x00
		};
	std::vector<uint8_t> buf(std::begin(kRetailTag0fPayload), std::end(kRetailTag0fPayload));
	// Patch bytes 4-15 (Position X/Y/Z int32 LE) with our spawn coord.
	auto put_u32_le = [&](size_t off, uint32_t v) {
		buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
		buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
		buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
		buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
	};
	put_u32_le(4, ctx.spawn_x);
	put_u32_le(8, ctx.spawn_y);
	put_u32_le(12, ctx.spawn_z);
	return buf;
}

std::vector<uint8_t> build_tag_16_player_list(const PlayerReplicationState &ctx) {
	// Phase D.0.11: matches retail capture frame ~82540 byte-for-byte
	// (31 bytes total, vs the original 19-byte minimal version).
	//
	// Witnessed at NapiNPClientMsg_0x016 @ 0x42FAE0:
	//   byte 0: dword_A823B8 (HUD time-vs-score format flag, bit 1)
	//   byte 1: player_count
	//   per-player (8 bytes):
	//     byte 0: player_id
	//     bytes 1-2,3-4,5-6: u16 fields (v42, v44, v45)
	//     byte 7: packed = (team << 1) | spectator_flag
	//             — HUD_DrawKillList @ 0x423a30 ONLY counts players whose
	//             entity team byte (set from team>>1) is 1, 2, 3, or 4.
	//             Sending team=0 in the original 19-byte version made the
	//             HUD report "0 players in server" (verified 2026-04-24
	//             00:35 test).
	//   byte 10: team_count
	//   per-team (6 bytes × team_count+1): u16 + u16 + u8 + u8
	//   2 trailing bytes: dword_A85B3C, dword_A85B40
	std::vector<uint8_t> buf;
	buf.reserve(32);
	push_u8(buf, 0x01);               // dword_A823B8 = 1 (matches retail; bit 1 selects format)
	push_u8(buf, 1);                  // player_count = 1
	// Per-player record (8 bytes):
	push_u8(buf, ctx.player_slot);    // player_id
	push_u16(buf, 0);                 // v42 (placement/score, discarded unless byte_A85B49 != 0)
	push_u16(buf, 0);                 // v44 → entry+40
	push_u16(buf, 0);                 // v45 → entry+44
	const uint8_t packed_team = static_cast<uint8_t>(ctx.team << 1); // packed (team<<1)|spectator. team=1 → 0x02.
	push_u8(buf, packed_team);
	// Team list — retail sends team_count=2 + 3 entries × 6 bytes = 19 bytes.
	push_u8(buf, 0x02);               // team_count = 2 (matches retail)
	for (int i = 0; i < 3; ++i) {     // (team_count + 1) iterations
		push_u16(buf, 0); push_u16(buf, 0);
		push_u8(buf, 0); push_u8(buf, 0);
	}
	push_u8(buf, 0x02);               // dword_A85B3C — matches retail (last byte before tail)
	push_u8(buf, 0x00);               // dword_A85B40
	return buf;
}

std::vector<uint8_t> build_tag_46_player_sync(const PlayerReplicationState &ctx) {
	std::vector<uint8_t> buf;
	buf.reserve(48);

	// Phase D.0.26.4: match retail's full tag=0x46 byte layout. Previously we
	// sent only flags=0x0007 (name|clan|team) which left the client's session
	// state stuck at "pending" (4), never advancing to "accepted" (6) — the
	// menu's click handler gates on that transition.
	//
	// Retail reference (capture3, jored as host slot 0, team 1, 34 bytes):
	//   00 f7 1c 00 6a 6f 72 65 64 00 00 41 2d 41 46 46 2d 45 36 43 30 43 46 00
	//   01 00 00 ff 00 01 00 00 00 00
	//
	// flags=0x1CF7 = bits 0,1,2,4,5,6,7,10,11,12 set. Each bit signals the
	// handler (`NapiNPClientMsg_0x046 @ 0x431370`) to read one field:
	//   bit  0 (0x0001) = name (cstr)
	//   bit  1 (0x0002) = clan (cstr)
	//   bit  2 (0x0004) = label (cstr) — alt-name like "A-AFF-HEXID"
	//   bit  4 (0x0010) = team (u8)
	//   bit  5 (0x0020) = squad (u8)
	//   bit  6 (0x0040) = ticket-validated (u8) — NHQ DLL's NWPEnter_AcceptPlayer
	//                     sets the parallel server-side byte to 1 to stop the
	//                     server from kicking the player; client-side retail
	//                     observed this as 0 on host slot (already validated)
	//   bit  7 (0x0080) = byte+48 (u8) — squad-leader marker, 0xFF = none
	//   bit 10 (0x0400) = byte+49 (u8) — zero in retail
	//   bit 11 (0x0800) = quality (u8) — connection-quality, retail sends 1
	//   bit 12 (0x1000) = entity_ref (u32) — null pointer in retail
	push_u8(buf, ctx.player_slot);
	const uint16_t flags = 0x1CF7u;
	push_u16(buf, flags);
	push_u8(buf, ctx.entity_slot);
	push_cstr(buf, ctx.player_name, 32);
	push_cstr(buf, ctx.clan_tag, 16);

	// Label — retail format is "A-AFF-HEXID" (12 chars + NUL). Stable placeholder
	// that won't collide with any retail-issued label.
	push_cstr(buf, std::string("A-A02-000000"), 16);

	push_u8(buf, ctx.team);  // team byte (entity+354)
	push_u8(buf, 0);         // squad (no squad)
	push_u8(buf, 0);         // ticket-validated (matches retail host slot)
	push_u8(buf, 0xFF);      // byte+48 — no squad leader
	push_u8(buf, 0);         // byte+49
	push_u8(buf, 1);         // quality — retail sends 1
	push_u32(buf, 0);        // entity_ref — null pointer
	return buf;
}

EntityBatchBuildResult build_tag_10_entity_batch(
		const std::vector<GameEntitySnapshot> &entities,
		size_t start_cursor,
		size_t max_payload_bytes) {
	EntityBatchBuildResult result;
	if (entities.empty()) {
		result.payload = build_tag_10_entity_batch_empty();
		return result;
	}

	constexpr size_t kHeaderBytes = 4;
	constexpr size_t kRecordBytes = 16;
	if (max_payload_bytes < kHeaderBytes + kRecordBytes) {
		max_payload_bytes = kHeaderBytes + kRecordBytes;
	}

	const size_t start = start_cursor % entities.size();
	result.payload.reserve(std::min(max_payload_bytes, kHeaderBytes + entities.size() * kRecordBytes));
	push_u16(result.payload, entities[start].slot);
	push_u16(result.payload, 0);

	size_t cursor = start;
	size_t count = 0;
	do {
		if (result.payload.size() + kRecordBytes > max_payload_bytes) {
			break;
		}
		const GameEntitySnapshot &entity = entities[cursor];
		push_u16(result.payload, entity.type_id);
		push_u16(result.payload, entity.flags);
		push_u32(result.payload, static_cast<uint32_t>(entity.x));
		push_u32(result.payload, static_cast<uint32_t>(entity.y));
		push_u32(result.payload, static_cast<uint32_t>(entity.z));
		++count;
		cursor = (cursor + 1) % entities.size();
	} while (cursor != start);

	result.payload[2] = static_cast<uint8_t>(count & 0xFFu);
	result.payload[3] = static_cast<uint8_t>((count >> 8) & 0xFFu);
	result.next_cursor = cursor;
	result.entity_count = count;
	return result;
}

std::vector<uint8_t> build_tag_10_entity_batch_empty() {
	std::vector<uint8_t> buf;
	buf.reserve(4);
	push_u16(buf, 0); // starting_slot
	push_u16(buf, 0); // count = 0 (no entities iterated)
	return buf;
}

std::vector<uint8_t> build_tag_57_rtt_request(uint32_t tick) {
	std::vector<uint8_t> buf;
	buf.reserve(5);
	push_u32(buf, tick);
	push_u8(buf, 1); // echo_request = 1 → client replies with tag=0x2C carrying our tick back
	return buf;
}

std::vector<uint8_t> build_tag_2a_chat_history(uint32_t a, uint32_t b, uint16_t c) {
	std::vector<uint8_t> buf;
	buf.reserve(10);
	push_u32(buf, a);
	push_u32(buf, b);
	push_u16(buf, c);
	return buf;
}

std::vector<uint8_t> build_tag_0a_world_reference(const PlayerReplicationState &ctx,
                                                  const std::vector<GameEntitySnapshot> &entities) {
	// Field-driven §5.9 S2C 0x0A frame (D-NET-50) — replaces the verbatim retail
	// blob (ADR 0003: no raw passthrough). The header anchor is the subject player's
	// world position; each replicated entity becomes a tag=1 compact record whose
	// positions are compressed relative to the anchor via network_compress_fixedpoint.
	// encode_frame_update emits the bytes, which decode_frame_update round-trips.
	// [orig: NapiNPClientMsg_0x00A @ 0x42FEC0 / NetPacket_SerializePlayerState case 1 @ 0x4C09C0]
	FrameUpdate fu;
	const int32_t ax = int32_t(ctx.spawn_x);
	const int32_t ay = int32_t(ctx.spawn_y);
	const int32_t az = int32_t(ctx.spawn_z);
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	fu.flags1 = 0x00;
	fu.flags2 = 0x00;          // sub-block 0 (aim) — the common gameplay frame
	fu.aim.present = true;     // local-player view left zeroed (not authored yet)
	fu.state_flag_byte = 0x00; // 7-byte tail: not mounted, full health, no extra state
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	fu.state_word = 0;

	// One compact record per replicated entity, position compressed vs the anchor.
	// Entities without a known §5.10b class are skipped (guided/unknown have no
	// 0x0A compact form — decode_frame_update fails closed on them).
	for (const GameEntitySnapshot &e : entities) {
		const uint16_t cx = network_compress_fixedpoint(e.x - ax);
		const uint16_t cy = network_compress_fixedpoint(e.y - ay);
		const uint16_t cz = network_compress_fixedpoint(e.z - az);
		// Coarse wire heading from the engine BAM (D-NET-86). Player/Infantry carry
		// the rounded high byte (v+0x800000)>>24 (entity+16 high); Vehicle carries the
		// rounded high i16 (v+0x8000)>>16.
		const uint8_t yaw_byte =
				uint8_t((uint32_t(e.euler_z) + 0x00800000u) >> 24);
		const int16_t yaw_bam16 =
				int16_t((uint32_t(e.euler_z) + 0x00008000u) >> 16);
		FrameUpdateRecord rec;
		rec.handle = uint16_t((uint16_t(e.pool) << 12) | (e.slot & 0x0FFFu));
		rec.type_id = e.type_id;
		rec.cls = e.entity_class;
		switch (e.entity_class) {
		case EntityClass::Player:
			rec.player.vehicle_handle = 0xFFFF;
			rec.player.pos_x_compressed = cx;
			rec.player.pos_y_compressed = cy;
			rec.player.pos_z_compressed = cz;
			rec.player.yaw_byte = yaw_byte;
			break;
		case EntityClass::Vehicle:
			rec.vehicle.parent_slot_handle = 0xFFFF;
			rec.vehicle.flags_byte = 0x00; // unmounted (world-relative position)
			rec.vehicle.pos_x_compressed = cx;
			rec.vehicle.pos_y_compressed = cy;
			rec.vehicle.pos_z_compressed = cz;
			rec.vehicle.euler_z = yaw_bam16;
			break;
		case EntityClass::Infantry:
			rec.infantry.vehicle_slot_handle = 0xFFFF;
			rec.infantry.pos_x_compressed = cx;
			rec.infantry.pos_y_compressed = cy;
			rec.infantry.pos_z_compressed = cz;
			rec.infantry.yaw_byte = yaw_byte;
			break;
		default:
			continue; // Guided / Unknown: not representable as a 0x0A compact
		}
		fu.records.push_back(std::move(rec));
	}
	return encode_frame_update(fu);
}

std::vector<uint8_t> build_tag_0a_player_state(const PlayerReplicationState &ctx,
                                               const std::vector<GameEntitySnapshot> &entities) {
	return build_tag_0a_world_reference(ctx, entities);
}

std::vector<uint8_t> build_tag_1e_game_event_post_spawn() {
	// Phase D.0.7: Verbatim 8-byte payload from retail capture frame ~82540
	// (the post-spawn burst). See `notes/net_verification_log.md` Phase D.0.7
	// for the wire-format witness from `GameEvent_BuildPayload @ 0x5054E0`.
	// event_type=0x3A, attacker=4, victim/aux=0xFF, pos=(0,0).
	static constexpr uint8_t kRetailTag1ePayload[8] = {
		0x3a, 0x04, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
	};
	return std::vector<uint8_t>(std::begin(kRetailTag1ePayload),
			std::end(kRetailTag1ePayload));
}

std::vector<uint8_t> build_tag_0d_local_player_spawn(const PlayerReplicationState &ctx) {
	// **DO NOT CALL FROM `add_initial_sync_player_state`.** Live 2026-04-25
	// test crashed JO_CLIENT at NapiNPClientMsg_0x00D + 0x730
	// (= 0x00433370, NULL deref reading from `v44`). Root cause:
	// `ItemList_FindIndexByTypeId(0x14B9)`'s ItemDef has the AI flag
	// (`[+84] & 0x100000`) set, which forces the receiver into the AI
	// branch at line 0x433327. That branch reads `v44` — only assigned a
	// non-NULL value when `flags & 0x800` is set on the wire (the
	// "extra cstring + u32 + u16" trailer block). Our flags=0x30
	// payload omits 0x800, so `v44 = NULL` and the strcpy crashes.
	// See `notes/dispatcher_findings.md` "tag=0x0D — verified wrong"
	// section for the full forensic trace.
	//
	// This builder is preserved as scaffolding for `build_tag_0d_spawn_points`
	// (the multi-entity pool-1 spawn-point variant); do not wire it back
	// into the local-player initial-sync flow without first either
	// (a) using a non-AI type_id, or (b) implementing the flags & 0x800
	// trailer with a valid AI cstring.
	//
	// tag=0x0D LOCAL_PLAYER_SPAWN. Wire format derived this session from
	// fresh decompile of `NapiNPClientMsg_0x00D @ 0x432C40` — see
	// `notes/dispatcher_findings.md` for the full per-flag-bit table.
	//
	// Handler read order (per-entity, after the u16 count prefix):
	//
	//   [u16 flags]                       // gates optional fields below
	//   [u16 slot_id]                     // (pool << 12) | slot; 0xFFFF terminates
	//   [u16 type_id]                     // ItemList_FindIndexByTypeId at apply
	//   [name\0]                          // cstring; for non-AI types content unused
	//   [u32 entity36]      if flags&0x20 // → entity[+36] (per-frame input gate)
	//   [u32 pos_x]         ALWAYS        // → entity[+4]
	//   [u32 pos_y]         ALWAYS        // → entity[+8]
	//   [u32 pos_z]         ALWAYS        // → entity[+12]
	//   [u32 yaw  ]         if flags&0x01 // → entity[+16]
	//   [u32 pitch]         if flags&0x02 // → entity[+20]
	//   [u32 roll ]         if flags&0x04 // → entity[+24]
	//   [u32 ?    ]         if flags&0x08 // → entity[+308]
	//   [u8  team ]         if flags&0x10 // → entity[+354]
	//   [u16 weap0]         if flags&0x100// → entity[+416]
	//   [u16 weap1]         if flags&0x200// → entity[+418]
	//   [u8 weapon_seat_flag + sub-refs]  if flags&0x400
	//                                    // (else: entity[+416]/[+418] default to -1)
	//   [u8  bone_attach]   ALWAYS        // → entity[+290]
	//   [further conditional fields per flags & 0x800/0x40/0x80/0x1000/0x2000/0x4000/0x8000]
	//
	// We use flags = 0x30 (entity36 + team), so the minimal valid payload
	// for one local-player record is exactly 27 bytes: 2(count) + 2(flags) +
	// 2(slot_id) + 2(type_id) + 1(name NUL) + 4(entity36) + 4(pos_x) +
	// 4(pos_y) + 4(pos_z) + 1(team) + 1(bone_attach).
	//
	// Earlier this builder also pushed two u16 weapon-slot zeros before
	// bone_attach. That was wrong — the receiver only reads weapon-slot
	// fields when flags & 0x100 / 0x200 / 0x400 are set. With flags=0x30
	// the extra 4 bytes corrupted alignment and crashed the receiver
	// (SYSDUMP at NapiNPClientMsg_0x00D + 0x730 — see `notes/dispatcher_findings.md`).
	//
	// Why we still emit this even though we already emit tag=0x0C player
	// spawn: the existing tag=0x0C path writes entity[+36] = entity_flags
	// (16-bit value with bit 8 = "PLAYER"). But `Player_BuildTag0CInputBody @
	// 0x42A550` line 0x42a59d gates tag=0x0C input emission on bit 1
	// of entity[+36]. tag=0x0D with flags=0x20 and entity36=0 is the
	// only known wire route to explicitly clear that bit on the local
	// player.
	std::vector<uint8_t> buf;
	buf.reserve(27);
	push_u16(buf, 1); // count = 1
	const uint16_t flags = 0x0010u | 0x0020u; // team + entity36
	push_u16(buf, flags);
	const uint16_t slot_id = static_cast<uint16_t>((0u << 12) | (ctx.player_slot & 0x0FFFu));
	push_u16(buf, slot_id);
	push_u16(buf, ctx.entity_type_id);
	push_cstr(buf, ctx.player_name, 32);
	push_u32(buf, 0);          // entity+36 = 0 (clears the movement gate)
	push_u32(buf, ctx.spawn_x);
	push_u32(buf, ctx.spawn_y);
	push_u32(buf, ctx.spawn_z);
	push_u8(buf, ctx.team);
	push_u8(buf, 0);           // bone_attach byte → entity+290
	return buf;
}

std::vector<uint8_t> build_tag_51_player_spawn(const PlayerReplicationState &ctx) {
	// Wire format per fresh 2026-04-25 decompile of
	// `NapiNPClientMsg_HandlePlayerSpawn @ 0x431BB0`:
	//
	//   [u16 team_u16]      // echoed by client as (team+1) in its tag=0x29 ack
	//   [u16 entity_slot]   // (pool << 12) | slot; matches our pool-0 player
	//   [u8  team_byte]     // → entity[+354] (the field menus check for "alive")
	//   [u16 weapon_index]  // → entity[+348] if entity has PLAYER flag (0x100)
	//   [u8  camera_byte]   // → entity[+884] if entity has PLAYER flag
	//
	// 8 bytes total. The handler at 0x431cb1 also unconditionally calls
	// `dword_B5CBB4 = 2;` and queues C2S tag=0x29 with the (team+1) echo —
	// our tag=0x29 dispatcher uses a state flag to break that loop.
	std::vector<uint8_t> buf;
	buf.reserve(8);
	push_u16(buf, ctx.team);
	const uint16_t slot = static_cast<uint16_t>((0u << 12) | (ctx.player_slot & 0x0FFFu));
	push_u16(buf, slot);
	push_u8(buf, static_cast<uint8_t>(ctx.team & 0xFF));
	push_u16(buf, 0);
	push_u8(buf, 0);
	return buf;
}

std::vector<uint8_t> build_tag_0d_spawn_points(const std::vector<SpawnPointEntity> &points) {
	// Phase D.0.26.2: multi-entity tag=0x0D body that registers .bms spawn
	// points into the client's pool 1. Same wire format as
	// `build_tag_0d_local_player_spawn` (see that function's comment for the
	// full field layout witnessed from `NapiNPClientMsg_0x00D @ 0x432C40`).
	//
	// Per entity we use flags = 0x0030 (team + entity+36) — minimum to set the
	// team byte (for `sub_4FE110` team match) and clear the movement/flag
	// field. Name is empty. The pool|slot handle targets pool 1 (where
	// .bms-placed spawn points live).
	std::vector<uint8_t> buf;
	// Guard against overflow: a u16 count field caps us at 65535 entries; we
	// never have anywhere near that many spawn points (.bms typically <= 16).
	if (points.size() > 0xFFFFu) {
		return buf;
	}
	// Estimate: 2 (count) + per-entity ~25B when name is empty.
	buf.reserve(2 + points.size() * 25);
	push_u16(buf, static_cast<uint16_t>(points.size()));
	const uint16_t flags = 0x0010u | 0x0020u;
	for (const SpawnPointEntity &p : points) {
		push_u16(buf, flags);
		const uint16_t slot_id = static_cast<uint16_t>(
				(static_cast<uint16_t>(p.pool) << 12) | (p.slot & 0x0FFFu));
		push_u16(buf, slot_id);
		push_u16(buf, p.type_id);
		// Name: empty string (just a NUL terminator). Spawn-point entities
		// don't consume a name slot the way vehicles ("d_5ton", "d_buggy") do
		// in retail capture4 — their appearance is driven by the item-def's
		// graphic field, not a wire-provided name.
		push_u8(buf, 0);
		// entity+36 = 0 (flags&0x20): clear entity flag field.
		push_u32(buf, 0);
		// Position (always present).
		push_u32(buf, static_cast<uint32_t>(p.x));
		push_u32(buf, static_cast<uint32_t>(p.y));
		push_u32(buf, static_cast<uint32_t>(p.z));
		// Team (flags&0x10).
		push_u8(buf, p.team);
		// Mandatory always-read bone/other byte. Weapon handles are gated by
		// flags&0x0400 and are absent for these 0x0030 records.
		push_u8(buf, 0);
	}
	return buf;
}

std::vector<uint8_t> build_tag_40_capture_zone_state(
		const std::vector<GameEntitySnapshot> &entities) {
	// Phase D.0.26.4/8 — tag=0x40 capture-point/spawn-marker broadcast. Wire
	// format per `MapOverlay_DecodeOverlayEntries @ 0x5BEBB0` (docs §5.19):
	//   [u8 count] + count × [u16 handle][u8 size][u8 b3][u8 b4][u8 b5]
	//
	// Retail capture3 (dvxi5 AS) observed bytes, indexed by the spawn-point's
	// team ownership state:
	//   team=0 (neutral):  b3=0x0c b4=0x10 b5=0xc2
	//   team=1 (blue):     b3=0x0a b4=0x10 b5=0x01
	//   team=2 (red):      b3=0x09 b4=0x10 b5=0x03
	//
	// IDA analysis says bytes 3-5 don't gate menu rendering (routing via bit
	// 0x10/0x40/0x20 of b3 is equivalent for 0x00 vs retail's 0x0c/0x0a/0x09;
	// filter `entry[3] & 0x40` passes either way). But retail's server emits
	// these per-team values faithfully, and matching byte-exact rules out one
	// class of divergence during Phase D.0.26 investigation.
	std::vector<uint8_t> buf;
	if (entities.size() > 0xFFu) {
		return buf; // count is u8
	}
	buf.reserve(1 + entities.size() * 6);
	push_u8(buf, static_cast<uint8_t>(entities.size()));
	for (const GameEntitySnapshot &p : entities) {
		const uint16_t handle = static_cast<uint16_t>(
				(static_cast<uint16_t>(p.pool) << 12) | (p.slot & 0x0FFFu));
		push_u16(buf, handle);
		push_u8(buf, 0);    // byte 2 (size)
		uint8_t b3, b4, b5;
		switch (p.team) {
			case 1:  b3 = 0x0a; b4 = 0x10; b5 = 0x01; break;
			case 2:  b3 = 0x09; b4 = 0x10; b5 = 0x03; break;
			default: b3 = 0x0c; b4 = 0x10; b5 = 0xc2; break; // neutral/unclaimed
		}
		push_u8(buf, b3);
		push_u8(buf, b4);
		push_u8(buf, b5);
	}
	return buf;
}

std::vector<uint8_t> build_tag_5a_weapon_loadout() {
	// Verbatim payload from retail capture frame 82537 (notes/retail_capture2_decoded.txt
	// line ~2917 of the seq=156 bundle that drops the player into active
	// gameplay state). Format per `NapiNPClientMsg_0x05A@0x4290e0`:
	//
	//   byte 0       = i8 class_index (here = 8 → loadout class slot)
	//   byte 1       = u8 first_weapon_id (here = 2)
	//   bytes 2..    = LOOP { u8 ammo, u8 alt_ammo, u8 ?, u8 next_weapon_id (0xFF terminates) }
	//   trailing 0xFF terminator
	//
	// Decoded entries (each = weapon_id + ammo + alt_ammo + ?):
	//   { id=0x02, ammo=0xFF, alt=0xFF, ?=0x00 }   ← knife (2)
	//   { id=0x03, ammo=0x05, alt=0xFF, ?=0x00 }   ← grenade (3) × 5
	//   { id=0x13, ammo=0x0A, alt=0xFF, ?=0x00 }   ← M4A1 (19) × 10 mags
	//   { id=0x33, ammo=0x03, alt=0xFF, ?=0x00 }   ← weapon 51 × 3
	//   { id=0x34, ammo=0x03, alt=0xFF, ?=0x00 }   ← weapon 52 × 3
	//   { id=0x35, ammo=0x03, alt=0xFF, ?=0x00 }   ← weapon 53 × 3
	//   { id=0x37, ammo=0x02, alt=0xFF, ?=0xFF }   ← weapon 55 × 2
	//   { id=0x38 } → terminator-style entry, no follow-up
	//   end: 0xFF×4 (final terminator + trailing zeroes)
	//
	// Side effect (load-bearing): handler sets dword_81474C = 0, which
	// unblocks Client_ProcessNetworkFrame's tag=0x2c heartbeat and
	// tag=0x0c position-update emission — i.e., this is what lets the
	// player MOVE.
	static constexpr uint8_t kRetailTag5aPayload[34] = {
		0x08, 0x02, 0xff, 0xff, 0x00, 0x03, 0x05, 0xff, 0x00, 0x13,
		0x0a, 0xff, 0x00, 0x33, 0x03, 0xff, 0x00, 0x34, 0x03, 0xff,
		0x00, 0x35, 0x03, 0xff, 0x00, 0x37, 0x02, 0xff, 0xff, 0x38,
		0xff, 0xff, 0xff, 0xff
	};
	return std::vector<uint8_t>(std::begin(kRetailTag5aPayload),
			std::end(kRetailTag5aPayload));
}

} // namespace opennova
