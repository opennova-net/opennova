#pragma once

#include <cstdint>
#include <vector>

#include <npwire/entity_class.h> // EntityClass

namespace opennova::netsim {

// One decoded S2C 0x0A tag-2 fire descriptor, lifted into absolute fixed-point
// coordinates. It is a remote ROUND SPAWN, not a hit/impact notification: the
// receiving client re-simulates it visually and authority remains on the host.
struct ClientRoundEvent {
	uint8_t flags = 0;
	uint8_t adm_index = 0;
	uint8_t subtype = 0;
	uint8_t slot_byte = 0;
	uint16_t shooter_handle = 0xFFFF;
	uint16_t target_handle = 0xFFFF;
	uint16_t shot_seq = 0;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	int32_t origin_z = 0;
	int32_t dir_yaw_bam = 0;
	int32_t dir_pitch_bam = 0;
};

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
	// Full client-side entity+0x10 heading. A fresh compact yaw sample re-seeds
	// this from yaw_byte<<24; body-local effects can then retain sub-byte motion
	// (notably the PRNG-signed recoil half-step) without changing the wire sample.
	int32_t heading_bam = 0;
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
	// A body-anim state that was applied and then OVERWRITTEN by a later record
	// before the presenter drained this row. Retail applies each record's anim
	// byte through the receive arbitration as it decodes [orig: @0x4c1153]; our
	// snapshot seam samples once per render frame, so a 1-2 tick transition (a
	// tapped prone roll: the wire byte is `pending ?: current` and flips as soon
	// as the follow-up state queues on the authority) would otherwise never
	// reach presentation. -1 = none. The presenter consumes it first, then the
	// current state, and clears it (ClientState::clear_anim_pulses).
	int16_t anim_state_pulse = -1;
	uint8_t anim_pulse_ratio = 0;
	// entity+0x2B0, the peer's equipped AdmDef index (the player compact's off-16
	// `anim_def_index`). Retail's client stores it straight onto the peer entity and its
	// body updater re-reads it every selection pass to pick that peer's upper-body hold
	// pose — so this byte, not any replicated anim id, is how an observer knows what a
	// remote player is holding. 0xFF = none. [orig: client store @0x4c11f2]
	uint8_t equipped_adm_index = 0xFF;
	// BMS team (1=Blue/2=Red) from every world-stream record that carries
	// entity+354 (pool-0 0x0C, pool-1 0x0D, pool-2 0x10, pool-3 0x20).
	// A decoded flag-gated zero is assigned too; 0xFF means no team-bearing
	// record has been witnessed yet.
	uint8_t team = 0xFF;
	// Pool-1 0x0D zone block, retained exactly as received. The packed byte is
	// zoneNumber + 32*rank at entity+538; radius lands at entity+350. Both stay
	// zero for non-zone pool-1 rows and for all other pools.
	uint8_t zone_number_rank = 0;
	uint16_t zone_radius = 0;
	// The 0x0A off-12 movement-input byte; remote players are motor-driven from it,
	// and bits 6/7 are lean L/R. [orig: pack @0x4df68f; write @0x4c0c9c]
	uint8_t move_input = 0;
	// Locally integrated lean angle (BAM32): only the bits ride the wire — both ends
	// integrate the angle per body tick (decay then ramp, equilibrium ~±0x30000000).
	// [orig: decay lean -= (lean+8)>>4 @0x4b5c97, then the ramp @0x4b7dbf/@0x4b7dd6
	//  (∓0x3000000/tick)]
	int32_t lean_angle = 0;
	// The remote ARMS DIP. Retail's reload broadcast does NOT put a peer into the
	// 65/66 reload pose on a pure client: the 0x49 handler branches on the entity's
	// item type and, for a remote PERSON, stamps the dip window and returns without
	// ever touching the +0x372 reload window that the pose selector reads. So an
	// observer's whole feedback for "that player reloaded" is this dip.
	// [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42c0a0 — the remote-person branch
	//  @0x42c105/@0x42c109 stamps entity+0x371 = 80 @0x42c10b and returns @0x42c113;
	//  the pose window entity+0x372 is written only by WeaponSlot_ReloadAmmo @0x54173c]
	int32_t arms_dip_ticks = 0;
	// The term the dip drives. entity+0x36C carried the IDB name headLookDecay, which is
	// a misnomer — it is not a head-look term at all. It is a PITCH OFFSET accumulator
	// that lands directly in the entity's own Pitch, and from there in every overlay
	// matrix and the held-weapon attach basis. Verified at the consumer:
	// `Pitch = savedPitch + [0x36C] + 2*[0x380]`
	// [orig: @0x4b1bd4..0x4b1bf5, sibling reads @0x4b1c1b / @0x4b1d96]. Renamed to
	// pitchKickAccum in the IDB and pitch_kick_accum across the port on 2026-07-27.
	// [orig: entity+0x36C, driven @0x4b5cb7, eased @0x4b5cc7..0x4b5cd5]
	int32_t pitch_kick_accum = 0;
	// Remote recoil accumulator (entity+0x380). Round receive applies the
	// stance-indexed ammo impulse after calculating that shot's spread; the
	// client body pass decays it later in the same frame.
	int32_t recoil_pitch = 0;
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
	// Every compact entity record folded from an 0x0A. The replication heartbeat: it
	// climbing means peers' poses are still arriving, flat means they are not.
	std::uint32_t compact_records_applied = 0;
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
	// Consume-once drain for the per-row anim transition pulses: the presenter
	// calls this after building a snapshot so each pulse dispatches exactly one
	// frame (see ClientEntityState::anim_state_pulse).
	void clear_anim_pulses();
};

} // namespace opennova::netsim
