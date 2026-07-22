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
	// Full/reconstructed entity+20 pitch plus entity+24 roll. Vehicles retain the
	// last spawn/dead-pose values because live compacts omit both. Infantry pitch
	// is reconstructed here from the compact aim target using retail's one-eighth
	// chase; Player live pitch remains in pitch_byte below.
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
	// Pool-1 0x0D entity+368 relationship. The spawn positions are absolute;
	// NetClientView captures this row's rigid parent-local pose after the whole
	// batch is present, then recomposes it from each decoded parent sample. This
	// is the retail path for NoNetworkCallback addeweap children.
	uint16_t parent_handle = 0xFFFF;
	int32_t parent_local_x = 0;
	int32_t parent_local_y = 0;
	int32_t parent_local_z = 0;
	uint8_t parent_local_yaw_byte = 0;
	int32_t parent_local_pitch_bam = 0;
	int32_t parent_local_roll_bam = 0;
	bool parent_pose_valid = false;
	// Raw entity flags from the latest compact organic record: PlayerCompactRecord::
	// state_flags or InfantryCompactRecord::flags_byte. Bit 0 is hidden and bit 1
	// is dead/undeployed. Spawns carry no compact flags, so `state_flags_known`
	// distinguishes an unwitnessed zero from a witnessed alive sample.
	uint8_t state_flags = 0;
	bool state_flags_known = false;
	// Last explicit compact health sample. `health_known` prevents a load-only
	// row from treating its default zero as death.
	uint16_t health_word = 0;
	bool health_known = false;
	// Advances on each witnessed dead -> alive edge. Keeping the epoch in the
	// decoded view preserves a respawn even if several 0x0A frames are folded by
	// one client pump before presentation runs.
	std::uint32_t respawn_revision = 0;
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
	// Advances only when the complete seven-byte recipient-local 0x0A tail was
	// decoded. frames_applied remains the lenient partial-presentation counter.
	std::uint32_t health_updates_applied = 0;
	// Phase-3 objective masks, present when g_GameType bit 0x20000 is active.
	// Text IDs remain mission-local; these four authoritative masks drive them.
	std::uint32_t objective_won = 0;
	std::uint32_t objective_lost = 0;
	std::uint32_t objective_show_win = 0;
	std::uint32_t objective_show_lose = 0;
	std::uint32_t objective_updates_applied = 0;
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	ClientEntityState &upsert(uint16_t handle);
};

} // namespace opennova::netsim
