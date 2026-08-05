#pragma once

#include <array>
#include <cstdint>
#include <string>
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
// Vehicle compact flags_byte dead-pose/wreck bit — the vehicle-class analog of
// the organic dead bit 1 (a wreck keeps replicating the short frozen-pose form
// with this bit set) [orig: the §5.13 short-form select; decoder
// is_dead_pose = (flags_byte & 4)].
inline constexpr uint8_t kVehicleFlagDeadPose = 0x04;

// renders exactly the state a networked peer would see.
struct ClientEntityState {
	uint16_t handle = 0;                          // (pool<<12)|slot
	uint16_t type_id = 0;
	EntityClass cls = EntityClass::Unknown;
	// Load-stream identity retained by the canonical replica fold. Presentation,
	// history, and the header-only native-world materializer share this metadata;
	// it never participates in compact record sizing. Keeping it on the decoded
	// row lets every consumer use one entity model instead of decoding spawn
	// batches twice.
	std::string name;
	uint16_t net_id = 0xFFFF;
	uint8_t spawn_tag = 0;
	int32_t x = 0;                                // world i32 16.16 (decompressed
	int32_t y = 0;                                // compact position + the frame anchor)
	int32_t z = 0;
	uint8_t yaw_byte = 0;                         // coarse heading (compact high byte)
	// Full client-side entity+0x10 heading (BAM32) — presentation reads THIS,
	// not yaw_byte. Snap mode (host/SP fold) re-seeds it from each compact
	// sample (yaw_byte << 24 / vehicle euler_z << 16); remote-motion mode
	// maintains it via the per-class chase (the mover cluster below). Body-local
	// effects can then retain sub-byte motion (notably the PRNG-signed recoil
	// half-step) without changing the wire sample.
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
	bool team_known = false;
	// Spawn batches gate full orientation fields. Compact Person/Vehicle records
	// always make heading known; retaining the gate keeps replay history honest
	// for spawn-only static/marker rows.
	bool heading_known = false;
	// Pool-1 0x0D zone block, retained exactly as received. The packed byte is
	// zoneNumber + 32*rank at entity+538; radius lands at entity+350. Pool-2's
	// identically placed weapon_byte/attach_ref fields land here too.
	uint8_t zone_number_rank = 0;
	uint16_t zone_radius = 0;
	// Full load-stream entity metadata needed to construct a client-side World
	// row when the joiner loaded only the 616-byte BMS header. These are kept
	// separate from the live compact state_flags byte: the 0x0D/0x10 dword is
	// entity+36 in full, and truncating it loses Building/NoShadow/etc.
	uint32_t spawn_entity_flags = 0;
	uint32_t spawn_section_mask = 0;
	uint16_t spawn_ammo_count = 0;
	uint8_t spawn_ref_num = 0;
	uint8_t spawn_sub_type = 0;
	// Pool-1 0x0D's fixed retail mountHandles image. Slots 0..7 are
	// selected by spawn_mount_mask; slots 8/9 are the block's two
	// unconditional tail handles. Preserve all ten raw decoder values here:
	// materialization structurally resolves slots 0..7, while retail stores
	// slots 8/9 raw. Seat definitions remain model-owned and are deliberately
	// not inferred from this mask.
	uint8_t spawn_mount_mask = 0;
	std::array<uint16_t, 10> spawn_mount_handles{{
			0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
			0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};
	// Advances for every pool-load record applied to this exact row. Retail
	// reinitializes the complete slot even when (handle,type) is unchanged; the
	// native materializer represents each such generation as a fresh lifetime.
	uint32_t spawn_revision = 0;
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
	// ClientReplicaPipeline captures this row's rigid parent-local pose after the whole
	// batch is present, then recomposes it from each decoded parent sample. This
	// is the retail path for NoNetworkCallback addeweap children.
	uint16_t parent_handle = 0xFFFF;
	int32_t parent_local_x = 0;
	int32_t parent_local_y = 0;
	int32_t parent_local_z = 0;
	// Full wrapped heading delta captured from the absolute 0x0D poses. The
	// persistent no-callback attachment follows the parent's LIVE heading_bam
	// between compact records; yaw_byte remains the coarse presentation mirror.
	int32_t parent_local_heading_bam = 0;
	int32_t parent_local_pitch_bam = 0;
	int32_t parent_local_roll_bam = 0;
	bool parent_pose_valid = false;
	// The 0x0D record's separate TARGET relationship: the STRUCTURAL carrier a
	// pool-1 mounted child rides (retail groundEntity, entity+40) — an occupied
	// boat gun's target is the DRIVING hull while parent_handle above carries
	// the occupant/driver back-ref (+368). Vehicle compacts re-land the same
	// +40 slot per record; this is its 0x0D seed.
	// [orig: NapiNPClientMsg_0x00D @0x432C40 — target → groundEntity stores
	//  @0x432d47/@0x4332d7; parent → occupantEntity (+368) store @0x433289]
	uint16_t target_handle = 0xFFFF;
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

	// --- The client-side between-update mover cluster (net-re §5.38e, D-NET-196).
	// On a JOINER (remote-motion mode), every compact read STAGES the target here
	// and ClientReplicaPipeline::tick_remote_motion chases the live pose (x/y/z above =
	// retail entity+4/+8/+0xC; heading_bam above = entity+0x10) one step per
	// 62.5 Hz tick. On the host/SP roles the fold keeps writing the live pose
	// directly (full-rate loopback; the authority never interpolates, D-NET-89).
	// Staged wire target; the per-class chase overwrites it with the per-step
	// vector at staging time, exactly like retail [orig: entity+0x234/238/23C,
	// step re-store @0x4b9b2e (org1) / @0x4B459F (org2) / watercraft @0x48D480].
	int32_t net_smooth_target[3] = {};
	// Recaptured from the live pose at the top of every mover tick
	// [orig: entity+0x80/84/88 recapture @0x4b9a5f and each family head].
	int32_t net_saved_live_pose[3] = {};
	// Staged heading/pitch targets, mutated into per-step deltas at staging
	// [orig: entity+0x240/+0x244].
	int32_t net_smooth_heading = 0;
	int32_t net_smooth_pitch = 0;
	// org1 only: the PREVIOUS staged heading, promoted on each fold before the
	// new store — the AI-infantry heading chase runs one record behind the wire
	// [orig: entity+0x1A8 promote in the mode-2 read @0x4C0320].
	int32_t net_target_heading_bam = 0;
	// Chase bookkeeping [orig: entity+0x27C / entity+0x27E].
	int16_t net_interp_progress = 0;
	int16_t net_interp_steps = 0;
	// Vehicle speed register mirror (retail vehicleData[177]) — the DECOMPRESSED
	// 16.16 wire value [orig: the mode-2 read decompresses weaponAimY before the
	// store]. Gates the fast-vehicle snap threshold (>= 293 ~ 0.28 m/s ->
	// 0x60000) and decays on starvation [orig: @0x48D480 interp block]; also the
	// prediction leg's commanded-speed source ([136] = [177] mirror).
	int32_t vehicle_speed_reg = 0;
	// Vehicle steer register mirror (retail vehicleData[179], the read-dest of
	// the wire weapon_heading_bam whose write source is the host's steer target
	// [132] — §5.13/D-NET-63). The non-driver machine adopts it as its own
	// steer-command register ([132] = [179]) for the prediction leg
	// [orig: @0x48D480 interp tail mirror].
	int32_t vehicle_steer_bam = 0;
	// Air lateral command mirror (retail vehicleData[178], the read-dest of the
	// wire weapon_aim_z, decompressed) — the CHel/cpln prediction leg's second
	// axis [orig: the three-register air mirror @0x490C9E].
	int32_t vehicle_lat_reg = 0;
	// Armed by the first folded compact for this row: the chase never runs
	// toward a zero-initialized target on rows that only ever saw load-stream
	// spawns (pool-2/3 statics).
	bool net_has_compact = false;
	// Carried rows (carrier_handle set): the latest record's seat-local offset,
	// re-composed against the carrier's CURRENT chased pose every mover tick —
	// the row-level translation of retail rendering mounted riders through the
	// carrier attach each frame (the rider's own mover is bit0-skipped)
	// [orig: Entity_AttachToVehicle bit0 set @0x43C14A; the D-NET-67 lift]. A
	// record with carrier 0xFFFF clears it (per-record consumption, D-NET-195).
	// Set by the embedding sim when a WORLD-side family mover owns this row's
	// motion (the joiner's pool-1 prediction, §5.38e B-facet): the fold live-
	// snaps the wire sample, tick_remote_motion skips the row, and the sim
	// mirrors the predicted world pose back after each tick.
	bool net_world_mover = false;
	// Air-family rows use the AIR chase constant set (snap 0xA0000, deadband
	// 0x2AAA, buckets {8,10,15,20,25,32}) when no world mover predicts them.
	// Stamped by the embedding sim from the resolved vehicle family — netsim
	// itself resolves only the wire class, never the motion family.
	bool net_air_family = false;
	// Bumped once per folded compact record for this row — the sim's staging
	// edge detector (a fresh wire sample arrived since it last staged).
	uint32_t compact_revision = 0;
	// --- The anim-root-motion dead-reckoning cluster (net-re §5.38e; retail's
	// remote body runs the SAME AnimMap primary channel and integrates its root
	// delta — chase early, root late, additive on the same fields [orig: anim
	// update @0x4B41DF; root integration @0x4B7CB4..0x4B7CEF; org1 twin
	// @0x4BF684..0x4BF6A2]. The channel state machine mirrors
	// world::InfantryState's body channel (begin_body_transition /
	// advance_primary_channel) through the shared IRootMotionSource API.
	int16_t rm_adm_id = -2;       // -2 unresolved (embedder stamps), -1 none
	int16_t rm_state = -1;        // playing/target state (the entity+700 mirror)
	int16_t rm_prev_state = -1;   // blend primary [orig: AnimMap prev channel]
	int32_t rm_phase = 0;         // target playhead, half-frame ticks
	int32_t rm_prev_phase = 0;
	float rm_blend_weight = 1.0f; // += 0.1/tick, or 1/15 when the target state
	float rm_blend_step = 0.0f;   // carries flag 0x400 [orig: @0x410640]
	// org2 leg-chain state: the LEGS chase the wire yaw (quarter-step clamp
	// ±0x3000000, twist ±0x30000000, staggered idle re-plant windows) and the
	// body heading is their midpoint — the root delta rotates by THIS, not the
	// view yaw [orig: @0x4b4945..0x4b4ac1; midpoint @0x4b4ab5]. org1 rows
	// rotate by heading_bam (retail pins body == render heading for org1).
	// The witnessed vertical: dz is REPLACED by the capsule-bottom history
	// delta while the prev-bottom slot is live; climbs 32..35 and grenade
	// deaths 176..179 reset the slot every update [orig: AnimMap_UpdateEntity
	// state tests/clear @0x40B607..0x40B637 and bottom delta/store
	// @0x40B88E..0x40B8A0]. The player
	// compact's phase byte seeds ONE transition then dies (retail zeroes
	// entity+0x377 after use [orig: AnimMap_UpdateEntity @0x40B7E4]).
	int32_t rm_prev_bottom = 0;
	bool rm_prev_bottom_live = false;
	bool rm_seed_live = false;
	int32_t rm_leg_yaw[2] = {};
	int32_t rm_leg_target[2] = {};
	int32_t rm_body_heading = 0;
	bool rm_leg_seeded = false;
	int32_t net_seat_local[3] = {};
	uint8_t net_seat_local_yaw_byte = 0;
	// Player/infantry seats compose the carrier yaw; a carrier-local VEHICLE
	// keeps its world-absolute wire euler [orig: @0x4607f5].
	bool net_seat_compose_yaw = false;
	bool net_seat_valid = false;
};

// Latest environment sample carried by the S2C 0x0A header. The revision is an
// ordered-message edge for optional history consumers; the live simulation may
// simply read the latest value.
struct ClientEnvironmentState {
	// Phase-2 fields previously decoded and then discarded. Keep the complete
	// retail snapshot so a joined OpenNova client can project the host's weather.
	uint8_t rain_pct = 0;
	uint8_t env_param = 0;
	bool present = false;
	uint16_t fog_dist = 0;
	uint16_t fog_accel = 0;
	uint16_t tod_fixed = 0;
	uint8_t quake_ticks = 0;
	uint8_t cloud_scroll = 0;
	uint8_t overcast = 0;
	std::uint32_t revision = 0;
};

// Latest complete recipient-scoped phase-8 mounted-ammo snapshot. The World
// adapter consumes revisions once so local weapon actions between phase-8
// samples are not repeatedly reset by the same network value.
struct ClientMountedAmmoState {
	bool present = false;
	uint16_t mount_handle = 0xFFFF;
	bool has_mount = false;
	uint16_t clip = 0;
	uint16_t reserve = 0;
	std::uint32_t revision = 0;
};

// The decoded world the client holds after pumping the loopback. Positions are
// post-compression (lossy, ~|v|>>11 quantization) — exactly what the original
// client renders for its decoded peers. Callers must NOT "correct" them toward the
// authoritative value.
struct ClientState {
	// Monotonic decoded-state edges. topology_revision changes only when the
	// ordered (handle,type) row layout changes; revision also covers field updates.
	// Presenters can therefore invalidate their row plan without coupling that
	// decision to optional history/event journaling.
	std::uint64_t revision = 0;
	std::uint64_t topology_revision = 0;
	// Advances only for accepted 0x0D/0x10/0x20 load records. Unlike revision,
	// per-frame compacts do not touch it; world materializers can therefore fold
	// repeated spawn rows without rebuilding an O(n) handle map every frame.
	std::uint64_t world_stream_revision = 0;
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
	ClientEnvironmentState environment;
	ClientMountedAmmoState mounted_ammo;
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	ClientEntityState &upsert(uint16_t handle);
	void mark_changed() { ++revision; }
	void mark_topology_changed() {
		++topology_revision;
		++revision;
	}
	// Consume-once drain for the per-row anim transition pulses: the presenter
	// calls this after building a snapshot so each pulse dispatches exactly one
	// frame (see ClientEntityState::anim_state_pulse).
	void clear_anim_pulses();
};

} // namespace opennova::netsim
