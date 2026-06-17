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
		if (rec.parent_handle)   flags |= 0x01; // entry+16  [orig: 0x50357e]
		if (rec.orientation_val) flags |= 0x02; // entry+0   [orig: 0x503593]
		if (rec.ammo_count)      flags |= 0x04; // entry+290 [orig: 0x5035b7]
		if (rec.team_byte)       flags |= 0x08; // entry+354 [orig: 0x5035f1]
		if (rec.weapon_type)     flags |= 0x10; // entry+640 [orig: 0x50360b]
		if (rec.score_byte)      flags |= 0x20; // entry+672 [orig: 0x503633]

		w.u8(flags);                    // [orig: *flags_location = flags @ 0x503660]
		w.u32(uint32_t(rec.pos_x));     // entry+4  [orig: 0x503553]
		w.u32(uint32_t(rec.pos_y));     // entry+8  [orig: 0x503565]
		w.u32(uint32_t(rec.pos_z));     // entry+12 [orig: 0x503577]

		if (flags & 0x01) w.u32(rec.parent_handle);   // [orig: 0x50358f]
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
		if (rec.vel_x)                     f |= 0x0001; // entity+16   [orig: 0x503b3c]
		if (rec.vel_y)                     f |= 0x0002; // entity+20   [orig: 0x503b58]
		if (rec.vel_z)                     f |= 0x0004; // entity+24   [orig: 0x503b74]
		if (rec.section_mask)              f |= 0x0008; // entity+308  [orig: 0x503b93]
		if (rec.orient_byte)               f |= 0x0010; // entity+354  [orig: 0x503bb2]
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

		if (f & 0x0001) w.u32(uint32_t(rec.vel_x));
		if (f & 0x0002) w.u32(uint32_t(rec.vel_y));
		if (f & 0x0004) w.u32(uint32_t(rec.vel_z));
		if (f & 0x0008) w.u32(uint32_t(rec.section_mask));
		if (f & 0x0010) w.u8(rec.orient_byte);
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

		w.u8(rec.team_byte);            // ALWAYS, entity+290 [orig: 0x503d38]

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

} // namespace opennova
