#pragma once

#include <cstdint>
#include <vector>

#include <npwire/ingame_decode.h> // EntityClass

namespace opennova::netsim {

// One entity as the local client has DECODED it off the wire. Per ADR 0011 the
// present pass reads THIS, not the authoritative sim directly — so single-player
// renders exactly the state a networked peer would see.
struct ClientEntityState {
	uint16_t handle = 0;                          // (pool<<12)|slot
	uint16_t type_id = 0;
	EntityClass cls = EntityClass::Unknown;
	int32_t x = 0;                                // world i32 16.16 (decompressed
	int32_t y = 0;                                // compact position + the frame anchor)
	int32_t z = 0;
	uint8_t yaw_byte = 0;                         // coarse heading (compact high byte)
	// Last full engine-frame entity+20/+24 orientation witnessed on an existing
	// spawn or vehicle dead-pose record. Live vehicle compacts omit both, so they
	// intentionally persist for mounted seat-frame presentation.
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
	// Normalized raw bytes retained from the class-specific compact organic record.
	// carrier_handle is PlayerCompactRecord::carrier_handle for players and
	// InfantryCompactRecord::vehicle_slot_handle for infantry. For players it can
	// also name a standing-on ground entity; mount_bone distinguishes an actual
	// seat mount (retail detaches player bone 0). No new wire fields are introduced.
	uint16_t carrier_handle = 0xFFFF;
	uint8_t mount_bone = 0;                       // player vehicle_bone / infantry seat_bone_idx
	uint8_t seat_type = 0;                        // player-only seat attribute; 0 for infantry
	uint8_t pitch_byte = 0;                       // class-specific witnessed compact byte
	uint8_t aim_yaw_byte = 0;                     // infantry-only entity+720 byte
	uint8_t anim_state_id = 0;                    // player anim_state_id / infantry anim_byte
	uint8_t anim_channel_ratio = 0;               // player-only; 0 for infantry
	bool seen_this_frame = false;
};

// The decoded world the client holds after pumping the loopback. Positions are
// post-compression (lossy, ~|v|>>11 quantization) — exactly what the original
// client renders for its decoded peers. Callers must NOT "correct" them toward the
// authoritative value.
struct ClientState {
	int32_t anchor_x = 0;                         // latest 0x0A frame anchor
	int32_t anchor_y = 0;
	int32_t anchor_z = 0;
	int16_t local_health = 0;
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	ClientEntityState &upsert(uint16_t handle);
};

} // namespace opennova::netsim
