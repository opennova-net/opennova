#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/entity_class.h> // EntityClass
#include <net/npwire/ingame_decode.h> // EndRoundStats (the 0x56 board)
#include <runtime/world/guided_missile_flight.h> // the stng pursuit integrator (D-NET-64)

namespace opennova::netsim {

// One connection-slot roster binding, folded from S2C 0x46 player-sync. This
// is the scoreboard's NAME-JOIN table, keyed by CONNECTION SLOT — a different
// key from the entity handle the feed resolves names by; entity_slot is the
// join between the two. The entity binding lands UNCONDITIONALLY on every
// non-removal sync (it is not bitmask-gated) [orig: the slot+36/slot+15
// stores @0x431477/@0x431480]; the named fields land per-bit last-write-wins
// [orig: the bit-gated stores in NapiNPClientMsg_PlayerSync @0x431370].
// A removal DEACTIVATES the slot and wipes it [orig: PlayerSlot_ClearAndUnlink
// @0x434730 zeroes active/team/names/entity; a re-bind re-inits every field
// @0x4346c0, and an unbound slot is never read (its 0x16 rows are dropped),
// so the clan/quality bytes ClearAndUnlink happens to skip are unobservable
// and the fold resets the whole slot].
struct ClientRosterSlot {
	bool bound = false;
	std::string name;         // 0x0001
	std::string clan;         // 0x0002 (the serializer's "team string"; retail ships "")
	uint8_t team = 0;         // 0x0004 [orig: @0x4315f7]; every accepted 0x16 row
	                          // refreshes it too [orig: @0x42fc7c]
	uint8_t downed_revive_seconds = 0; // 0x0008 / S2C 0x54 low seven bits
	bool medic_request_active = false; // 0x0008 / S2C 0x54 bit seven
	uint8_t quality = 0;      // 0x0400, clamped 4 [orig: @0x43170d] — the connection-icon band
	int16_t entity_slot = -1; // pool-0 slot this connection drives; -1 = none
	                          // [orig: the no-entity -1 store @0x431489]
};

// The decoded S2C 0x16 scoreboard. Rows arrive PRE-SORTED by the server (team
// modes by the accumulated points, others by the mode stat [orig:
// Player_ComputeScore @0x500A80 feeding Server_BuildAndBroadcastScoreboard
// @0x50D960]) and the client never re-sorts — the drawer walks the records in
// wire order [orig: HUD_DrawKillList @0x423A30]; slot_id is authoritative, not
// row position. Every well-formed update applies UNCONDITIONALLY: retail
// zeroes its row count before the row loop and parses the team table and
// trailer even for a zero-row list [orig: @0x42fb46], and the drawer shows
// whatever is stored — an empty update yields an empty board. Name/clan are
// copied INTO the row at apply time (retail joins them into its 56-byte
// records inside the parser [orig: @0x42fd4c..0x42fd8f]), so a later roster
// removal does not blank rows already on the board.
struct ClientScoreboardRow {
	uint8_t slot_id = 0;
	uint16_t status_flags = 0;  // the glyph bitfield, NOT a ping [orig: store @0x42fdb4]
	int16_t score1 = 0;         // the mode's primary stat — the only score the
	                            // Tab list draws, SIGN-EXTENDED into the record
	                            // like retail's movsx [orig: @0x42fb9d; drawn
	                            // "%3i" @0x423e76]
	uint16_t score2 = 0;        // accumulated points/EXP (the server's team-mode
	                            // sort key; the retail client never reads it —
	                            // no read width witnessed, stays the raw wire u16)
	uint8_t team = 0;           // flags >> 1
	bool spectator = false;     // flags bit0
	std::string name;           // joined from the roster at apply time
	std::string clan;
};

// One team-table row. The u16 pair carries the SAME two stats as the player
// rows — the mode stat and the accumulated points [orig: the team stores
// @0x50dcb8/@0x50dce4] — and the byte pair is mode-specific: the KOTH hold
// byte (game type 0x10001 [orig: @0x50dc62]) and the CTF flag state (types
// 0x10002/0x90002/0x10004 [orig: @0x50dd30]). The old player_count/alive_count
// names were decode-era guesses.
struct ClientScoreboardTeam {
	uint16_t score1 = 0;
	uint16_t score2 = 0;
	uint8_t koth_hold = 0;
	uint8_t ctf_flag = 0;
};

struct ClientScoreboard {
	bool known = false;
	bool team_mode = false;   // flags bit0 -> g_scoreboard_flags
	bool timed = false;       // flags bit1 — set only for solo KOTH (game type 1)
	                          // [orig: @0x50dd54]
	uint8_t in_game_count = 0;
	uint8_t spectator_count = 0;
	std::vector<ClientScoreboardRow> rows;
	std::vector<ClientScoreboardTeam> teams;  // T0 neutral + one per team
	// Rows skipped because their connection slot has no roster binding yet.
	// Retail drops these too and queues a C2S 0x22 {slot, 0x1CF7} retry
	// [orig: @0x42fc05..0x42fc3a]; the retry send is a D-HUD-24 residual.
	std::uint32_t rows_dropped_unknown_slot = 0;
	std::uint64_t revision = 0;
};

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

// One folded S2C 0x1E game event — the kill-feed lane. The wire record is
// carried through verbatim plus the classification the feed needs; string
// resolution ($A/$B against the roster, the STRCND lookup) happens where the
// name table lives [orig: NetPacket_HandleGameEvent @0x426270 ->
// HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60].
// `kind` is npwire's GameEventKind carried as its underlying byte so this
// header stays free of the decoder include.
struct ClientGameEvent {
	uint8_t event_type = 0;
	uint8_t attacker_index = 0xFF;
	uint8_t victim_index = 0xFF;
	uint8_t aux_index = 0xFF;
	int16_t pos_x = 0;
	int16_t pos_y = 0;
	uint8_t kind = 0;
};

// One folded S2C 0x14 chat line — the player-chat lane. The wire carries
// [channel][sender_slot][cstr text] (D-NET-215); the text is already the
// server-formatted "name(/squad): text" line. Which ring it lands in and which
// colour it takes is the HUD's channel table (hud/feed_format.h
// chat_channel_sink/color); the sender gate (a muted slot, or an unspawned
// spectator while the spawn gate is down) reads the roster the embedder owns
// [orig: NapiNPClientMsg_ChatMessage @0x42f240 -> Chat_DispatchToChannel
//  @0x42b910 — the slot gate @0x42b923..0x42b943, the switch @0x42b95d].
struct ClientChatLine {
	int8_t channel = 0;      // body[0], sign-extended into the switch
	uint8_t sender_slot = 0; // body[1], the roster index
	std::string text;
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
	// byte through the receive arbitration as it decodes [orig: @0x4c1153] —
	// since D-NET-209 that arbitration runs natively per folded record (the
	// net_anim_* pair below) and armed rows present the arbitrated channel;
	// the pulse remains the DISARMED-row fallback and the legacy-publish
	// rollback seam. -1 = none. The presenter consumes it first, then the
	// current state, and clears it (ClientState::clear_anim_pulses).
	int16_t anim_state_pulse = -1;
	uint8_t anim_pulse_ratio = 0;
	// The receive-side body-state ARBITRATION pair — the retail entity fields
	// the per-record apply writes AS IT DECODES [orig: player @0x4c1153,
	// infantry @0x4c0600..0x4c0641]: current (+0x2BC) and the clip-end
	// deferred pending (+0x2B8). A same-as-current record is a pure no-op
	// (the pending SURVIVES); a current in queue class 4, or class 0x20 with
	// a non-idle (flags bit0 clear) arrival, queues the record as pending;
	// everything else commits directly (current = decoded, pending = 0).
	// Retail's own pending sentinel is 0, ambiguity included. -1 current =
	// no record arbitrated yet. The wire-dead park (+0x2C0) needs no field:
	// anim_state_id retains the raw byte and the frozen-row presentation
	// fallback shows it, which is the parked byte's visible outcome.
	int16_t net_anim_current = -1;
	int16_t net_anim_pending = 0;
	// The pending promotion boundary in the growing rm_phase convention,
	// armed by the tick when a pending is present (retail: the deferral ORs
	// 0x40000 into the channel each tick and AnimChannel_AdvancePlayback
	// latches 0x20000 at the next loop wrap / one-shot end [orig:
	// @0x40b7db/@0x40b7ad; @0x40b1ae/@0x40b18f]). <0 = unarmed.
	int32_t net_anim_pending_boundary = -1;
	// The +0x377 one-shot phase seed mirror: retail stores the ratio byte
	// ONLY on the direct-commit leg [orig: @0x4c11a6], so a queued record's
	// phase never seeds the current channel. Player records only.
	uint8_t net_anim_ratio = 0;
	bool net_anim_ratio_live = false;
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
	// Pool-3 S2C 0x20 flag 0x02 carries the raw entity+0 Q16 dword. Marker
	// 6005/6006/2044 use it as their authored waypoint/proximity radius.
	// [orig: NapiNPClientMsg_0x020 @0x425D07..0x425D1B]
	int32_t spawn_bound_radius_q16 = 0;
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
	// ClientReplicaPipeline captures this row's rigid carrier-local pose after the
	// whole batch is present, then recomposes it from the followed carrier's live
	// pose each tick. This is the retail path for NoNetworkCallback addeweap
	// children; the followed carrier is target_handle (groundEntity) when set,
	// else this parent (non-pool-0 only, D-NET-195). The parent_local_* cluster
	// below stores the captured pose for whichever carrier is followed.
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
	// +40 slot per record; this is its 0x0D seed. For a compact-less
	// (NoNetworkCallback) child this slot also drives the LIVE per-tick follow:
	// retail's 'ewep' move function recomposes the child from groundEntity
	// every tick, so the gun tracks a DRIVING carrier between/without records
	// (refresh_carried_entities).
	// [orig: NapiNPClientMsg_0x00D @0x432C40 — target → groundEntity
	//  resolve @0x4332bc, store @0x4332d7; parent → occupantEntity (+368) store @0x433289;
	//  'ewep' move fn Entity_UpdateTransformAndTurret @0x440ca0 via the class
	//  table row @0x82abe0]
	uint16_t target_handle = 0xFFFF;
	// The pure-client stale-carrier sweep's run length: consecutive mover
	// ticks this no-callback child's persistent carrier stayed unresolvable.
	// At 128 the sweep queues C2S 0x0F for the carrier AND the child, then
	// locally destroys the child pending the authority re-spawn — retail keys
	// the same 128-tick cadence on `tick & 0x7F == 127` with the carrier's
	// staleness marker; the per-child run is this transport layer's
	// structural translation of that pair (our unresolvable-carrier state IS
	// the staleness the marker tracks).
	// [orig: Entity_UpdateTransformAndTurret @0x440ca0, the sweep
	//  @0x440d41..0x440e2f]
	int32_t carrier_missing_ticks = 0;
	// Replica-row vertical velocity — the caller-owned gravity channel of the
	// org movers, integrated before the contact resolve and zeroed on landing
	// [orig: org2 vel_z -= 208 then pos += vel @0x4B7CE0..0x4B7CEF; org1
	//  vel_z -= 416 then pos += 2*vel; landing zero in the shared tail].
	int32_t rm_vel_z = 0;
	// The planar velocity pair (retail +0x98/+0x9C) — the momentum channel the
	// org movers maintain beside the anim root: per-tick decay (in-air 63/64
	// with the optional MoveOrder-bit3 air-steer nudge and the root pair
	// ZEROED; grounded/org1 (7v+4)>>3 with the |v|<=8 snap), integrated as
	// pos += vel + root, fed by the org2 ledge 3/4 momentum carry and drained
	// by the water drags [orig: maintenance @0x4b78a8..0x4b79dc /
	// @0x4bf5cb..0x4bf61f; integrate @0x4b7cbf..0x4b7cd2 / @0x4bf684..;
	// carry @0x4b7e43..0x4b7e6d; drags @0x4b8124..0x4b8149].
	int32_t rm_vel_xy[2] = {};
	// The contact resolver's ground-probe hit for this row (wire handle;
	// 0xFFFF = terrain/none) — retail's groundEntity (+0x28) store
	// [orig: Entity_RaycastGroundHeightAndObject @0x414370]. The deck-ride
	// consumes it: the row follows the hit entity's per-tick pose delta.
	uint16_t resolved_ground = 0xFFFF;
	// The row's persistent retail entity-Flags mirror — the caller-owned word
	// the resolver's latch sites and the mover flag channels read and write on
	// a registry entity (InAir 0x2000, Drowning/float 0x8000, LadderContact
	// 0x100000, Submerged 0x200000, Indoors 0x800000). Set/cleared by the
	// resolve, the airborne/landing edges, and the water block; consumed by
	// the root suppressions, the gravity gate, the probe's indoors skip, and
	// the resolver's full-update discriminant.
	uint32_t rm_entity_flags = 0;
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

// Client-retained map-overlay banks. The capacities and routing bits are the
// original fixed tables; keeping them bounded makes refresh, clear, and expiry
// behavior independent from presentation.
// [orig: MapOverlay_UpdateOrCreateSlot @0x5BEA60; MapOverlay_AllocSlot @0x5BE970;
//  update_map_overlay_timers @0x5BFCE0]
inline constexpr std::size_t kMinimapTransientCapacity = 328;
inline constexpr std::size_t kMinimapPersistentCapacity = 328;
inline constexpr std::size_t kMinimapSpecialCapacity = 504;
inline constexpr std::size_t kMinimapLinkedCapacity = 251;
inline constexpr uint16_t kMinimapOverlayLifetimeTicks = 1984;

struct ClientMinimapOverlaySlot {
	bool active = false;
	uint16_t handle = 0xFFFF;
	uint8_t param = 0;      // icon byte (wire +2 on 0x40; 253/24 on 0x6B)
	uint8_t icon_color = 0; // wire color-table index (+3 on 0x40)
	uint8_t flags = 0;      // 0x10 persistent, 0x20 clear, 0x40 special, 0x6B writes 0xC4
	uint8_t source = 0;
	uint32_t argb = 0xFFFFFFFFu;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;          // 0x6B slots: the ring height (16.16)
	int32_t heading_bam = 0;
	uint16_t remaining_ticks = 0;
	// Regular (non-special) markers draw from the live decoded entity; this
	// mirrors retail's entity[538] draw gate and is refreshed each tick.
	// [orig: render_minimap_slot_blip @0x5be4b8]
	bool entity_known = false;
};

// One 0x6B keep-alive link: while it lives it refreshes its special slot's
// lifetime and handle each tick; its expiry clears the slot.
// [orig: linked table @0x28E1B28, update_map_overlay_timers @0x5bfd3a..]
struct ClientMinimapLinkedSlot {
	bool active = false;
	uint16_t handle = 0xFFFF;
	uint32_t remaining_ticks = 0; // wire seconds x62 [orig: @0x4255c9..0x4255d6]
	// The link's STORED special-bank slot (retail keeps a raw slot pointer at
	// link+24 and consults ONLY it — never a handle search — so a coexisting
	// 0x40 special badge for the same handle keeps its own slot). -1 = none
	// (allocation failed while the special bank was full).
	// [orig: overlay_obj = link[6] @0x5bece4; fresh-link alloc @0x5bed39]
	int16_t slot_index = -1;
};

// One client-side flown guided missile — the §5.15/§5.36 S2C 0x44 guidance
// channel folded into typed state. A guided missile sends NO per-tick motion on
// the wire; the non-authority client flies it locally between the periodic
// steer updates [orig: Entity_UpdateGuidedMissile_0 @0x446060 non-authority
// branch; NapiNPClientMsg_0x044 @0x422710 -> NetPacket_DispatchToEntityByNetId
// @0x4D6960]. Group semantics (fork-witnessed on the Karo reference wire):
// 1 = detonate/flight-end (the ONLY terminator — the termination tests are
// authority-side [orig: gate @0x4463cb]), 3/4 = lock + steer point,
// 2 = lock lost, 5 = flare-decoy steer (clears the lock); 6 carries no
// presented surface.
struct ClientGuidedMissile {
	bool active = false;
	uint16_t shooter = 0xFFFF;     // sub-header owner handle (entity+368)
	int16_t net_id = 0;            // the missile net id (sub-header i16)
	uint16_t lock_target = 0xFFFF; // group 3/4 target_slot (0xFFFF = none;
	                               //  cleared by groups 2 and 5)
	int32_t steer[3] = { 0, 0, 0 }; // latest steer point, 16.16 mission
	bool has_steer = false;
	bool terminated = false;       // group 1 (entity+696|=1 detonate/dead bit)
	bool flight_seeded = false;
	world::GuidedFlightState flight;
	std::uint64_t revision = 0;    // bumped on every fold touch
};
inline constexpr std::size_t kGuidedMissileCapacity = 16;

struct ClientMinimapState {
	std::array<ClientMinimapOverlaySlot, kMinimapTransientCapacity> transient{};
	std::array<ClientMinimapOverlaySlot, kMinimapPersistentCapacity> persistent{};
	std::array<ClientMinimapOverlaySlot, kMinimapSpecialCapacity> special{};
	std::array<ClientMinimapLinkedSlot, kMinimapLinkedCapacity> linked{};
	std::uint64_t revision = 0;
};

// The decoded world the client holds after pumping the loopback. Positions are
// post-compression (lossy, ~|v|>>11 quantization) — exactly what the original
// client renders for its decoded peers. Callers must NOT "correct" them toward the
// authoritative value.
// The reassembly stream plus the decoded board. The stream is per-session
// state whose lifetime the decoder does not own, so it lives here -- retail
// keeps the same thing in one global stream [orig: g_scoreReassemblyStream
// @0xA82324, reset only by an offset-0 chunk @0x431D79 and otherwise kept
// across completed decodes].
struct ClientEndRoundStats {
	bool header_known = false;
	EndRoundHeader header;
	// True once a complete board has been decoded at least once. A later
	// partial chunk does not clear it, so the screen keeps showing the last
	// complete board while the next one streams in.
	bool known = false;
	EndRoundStats board;
	// The stream: filled at each chunk's offset, completion tested against
	// each chunk's own declared total, retained after a decode.
	std::vector<uint8_t> buffer;
	// Chunks that arrived since the last offset-0 reset -- diagnostic only.
	uint32_t chunks_seen = 0;
};

// Requester-local S2C 0x81 sample. `updates` is the consume edge for the
// presentation/audio owner; `delta` retains retail's wrapping signed
// current-minus-previous result. The periodic Tab board remains independent.
// [orig: NapiNPClientMsg_ScoreDeltaSound @0x42A0B0]
struct ClientScoreFeedback {
	int32_t score = 0;
	int32_t delta = 0;
	uint32_t updates = 0;
};

// Latest requester-specific deploy-wave panel plus a valid-packet edge. The
// group body is already the strict npwire decode; retaining it here lets both
// loopback and remote clients consume one canonical state.
// [orig: NapiNPClientMsg_HandleSquadRosterSync @0x429880]
struct ClientSpawnWaveStatus {
	bool known = false;
	uint32_t updates = 0;
	SpawnWaveStatus value;
	// The zone whose member list names the local player (retail word_A85BC0:
	// reset to -1 at every fold, set to the group's zone handle when a member
	// equals the local handle @0x429a04..0x429a0b); 0xFFFF = none.
	uint16_t self_zone_handle = 0xFFFF;
};

// Latest victim-local S2C 0x52 camera anchor. Retail stores the three fixed
// coordinates globally and Camera_ComputeThirdPersonPositions consumes them.
// [orig: NapiNPClientMsg_0x052 @0x428A80; consumer @0x438B80]
struct ClientDeathCameraTarget {
	bool known = false;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint32_t updates = 0;
};

struct ClientState {
	// Monotonic decoded-state edges. topology_revision changes only when the
	// ordered (handle,type) row layout changes; revision also covers field updates.
	// Presenters can therefore invalidate their row plan without coupling that
	// decision to optional history/event journaling.
	std::uint64_t revision = 0;
	std::uint64_t topology_revision = 0;
	// Advances only for accepted 0x0D/0x10/0x20 load records and live 0x59/0x12
	// placed-device lifecycle records. Unlike revision, per-frame compacts do not
	// touch it; world materializers can therefore skip ordinary 0x0A traffic.
	std::uint64_t world_stream_revision = 0;
	int32_t anchor_x = 0;                         // latest 0x0A frame anchor
	int32_t anchor_y = 0;
	int32_t anchor_z = 0;
	int16_t local_health = 0;
	// Latest phase-0 0x0A projection of the authority's whole-second
	// pre-round timer. It is the client's Entity_UpdateAllEntities freeze gate;
	// networking and maintenance remain live while nonzero.
	// [orig: reader @0x430064; Game_ProcessMainFrame gate @0x52672C]
	std::uint8_t preround_delay_seconds = 0;
	// The joiner's copy of the round clock, in 62 Hz ticks (-1 = untimed),
	// folded from the 0x0A sub-block-1 timer snapshot: 62 x the wire's whole
	// seconds, or -1 when the wire value is negative. Feeds the end-round
	// ladder's game-time line and timed/untimed arm picks (D-HUD-25).
	// [orig: g_round_time_remaining @0x24C1958 — the store
	//  NapiNPClientMsg_0x00A @0x430219..0x430235; mission-start seed -1
	//  @0x524A89]
	std::int32_t round_time_remaining_ticks = -1;
	// The other three phase-0 0x0A sub-block-0 whole-second timers the DEATH
	// screen reads [orig: NapiNPClientMsg_0x00A stores @0x430084 dword_A85B5C
	// (slot+360, the respawn penalty — STROVER_PENALTYTIMER), @0x43009f
	// dword_A85B60 (slot+368, the local revive window — STROVER_MEDICTIMER /
	// STROVER_CALLMEDIC), @0x4300c3 dword_A85B68 (slot+364, the spawn-target
	// hold — STROVER_PSPRESPAWN); consumer UI_UpdateDeathScreenContent
	// @0x5536a0]. Retained between phase cycles like the client globals.
	// The client-local death screen (retail g_death_screen_active): the 0x0A
	// header's flags1 bit 0 EDGES — a rising edge opens it and zeroes the
	// sub-mode / kill-cam target and arms the enemy-tag grant; a falling edge
	// closes it and clears the grant [orig: NapiNPClientMsg_0x00A
	// @0x42ff88..0x43002b — dword_A860F0/A860F4 = 0 @0x42ffa6, g_enemyTagsVisible
	// @0x42ffb2/@0x430025]. The sub-mode is written by the spectate actions
	// (unported) and stays 0 here.
	bool death_screen_active = false;
	std::uint8_t death_screen_submode = 0;
	bool enemy_tags_visible = false;
	std::uint8_t respawn_penalty_seconds = 0;
	std::uint8_t local_revive_seconds = 0;
	std::uint8_t spawn_hold_seconds = 0;
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
	ClientMinimapState minimap;
	std::array<ClientGuidedMissile, kGuidedMissileCapacity> guided{};
	// The Tab board's two folded lanes: the 0x16 scoreboard and the 0x46
	// connection-slot roster it joins names from.
	// The END-OF-ROUND STAT BOARD, reassembled from the chunked S2C 0x56 lane.
	// Distinct from `scoreboard` above: that is the in-match Tab list refreshed
	// every 311 ticks, this is the post-round board the stat.mnu screen reads
	// [orig: NapiNPClientMsg_0x056 @0x431D10].
	ClientEndRoundStats end_round;
	ClientScoreboard scoreboard;
	ClientScoreFeedback score_feedback;
	// The host VarList's EXP_FANFARE u16 (lo/hi thresholds of the 0x81 tone
	// ladder, hud/score_fanfare.h) [orig: g_sessionvar_exp_fanfare @0x24d5a10].
	uint16_t exp_fanfare = 0;
	ClientSpawnWaveStatus spawn_waves;
	ClientDeathCameraTarget death_camera;
	std::array<ClientRosterSlot, 256> roster{};
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	const ClientEntityState *find(uint16_t handle) const;
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

// The joiner-side map grid-origin resolve: the first decoded pool-3 entity
// of type 2043, the same client pool scan retail's HUD init runs. The host
// resolves the same rule from the mission doc at promotion
// (World::map_grid_origin_*); this is the ONE decoded-view home so the
// selection rule cannot fork per embedder. Returns false when no origin
// entity has decoded yet.
// [orig: HUD_InitOverlaySystem @0x5a4999 pool scan (entity+80 == 2043)]
bool client_minimap_grid_origin(const ClientState &state, int32_t &out_x_q16,
		int32_t &out_y_q16);

// The retail team-byte -> overlay-color resolve for a live regular marker
// row: team 1 -> table[0x0A] blue, team 2 -> table[0x09] red, anything else
// (including undecoded) -> table[0x0C] neutral green. The ONE home for the
// mapping so an embedder restoring a client-local row (the deployed local
// player) cannot fork the palette.
// [orig: the team switch @0x5becb8..0x5bece4 over
//  g_minimap_overlay_color_table @0x840A10]
uint32_t minimap_team_argb(uint8_t team);

} // namespace opennova::netsim
