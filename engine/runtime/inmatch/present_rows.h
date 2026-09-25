#pragma once

// The packed present rows (world::PF_*, runtime/world/present_rows.h) the
// shell's present passes read: ONE row per entity the witnessed collectors
// visit, built here for both roles (ADR 0043 slice G3; ex the GDExtension's
// Simulation::present_snapshot_from_* pair).
//
//   * the authoritative listen host / SP local client presents from its OWN
//     pools -- retail's local client reads process memory and its loopback
//     0x0A is header-only [orig: serialize_entity_states_to_packet @0x50f07e;
//     collect_visible_entities_for_terrain @0x5c8c60] (D-NET-140);
//   * a joiner renders the host's stream wire-direct from the state its
//     ClientReplicaPipeline decoded (ClientState), enriched by the local
//     player's own mount state and by the registry rows an explicit complete-
//     BMS join promoted.
//
// Every row starts from initialize_client_replica_present_row; the joiner path
// runs project_client_replica_present_row first (the canonical decoded-client
// projection) and enriches after it.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/person_overlays.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::inmatch {

struct NapiNPServerCtx;

// What both collectors read. The kernel is the world + the local player's
// view/weapon state + the asset-derived tables (seat specs, mounted graphics,
// the model cache); the runtime is the decoded replica view (its ClientState,
// the joiner's self handle, the LFP camp percents) and may be null on the
// world path of a bare host. The server context is the listen host's own (null
// elsewhere): its player slots keep the per-player state retail reads off a
// remote player's entity there (the EquippedSlot the held-weapon gate tests).
struct PresentRowsContext {
	mission::MissionKernel &kernel;
	ClientRuntime *runtime = nullptr;
	bool joiner = false;
	const NapiNPServerCtx *server = nullptr;
};

// The decoded fold's dead->alive respawn revision, mirrored per pool row on the
// world path so WirePresentPass sees the same PF_RESPAWN_REVISION edges on
// every role. Owned by the embedder across frames (cleared with the world).
struct PoolPresentLifecycle {
	uint64_t registry_spawn_id = 0;
	uint32_t respawn_revision = 0;
	bool dead_known = false;
	bool dead = false;
};
using PoolPresentLifecycleMap = std::unordered_map<uint16_t, PoolPresentLifecycle>;

// The presented mission yaw of one pool row. Retail draws entity+0x10 (the
// 32-bit engine-frame heading); the port keeps that mirror on the vehicle motor
// (VehicleMotorState::yaw_bam) and on the infantry AI row (AiEntity::heading),
// while Entity::yaw is the whole-degree mission mirror every other row carries.
double pool_present_yaw_deg(const world::Entity &e, const world::AiEntity *ae,
		EntityClass cls);

// The presented pitch / roll of one pool row. Retail draws entity+0x14 / +0x18,
// 32-bit angles like the heading; once the vehicle motor has run the port keeps
// that pair on VehicleMotorState (air_pitch_bam / air_roll_bam) and Entity::pitch
// / roll are its whole-degree mirrors. Publishing the mirrors would tilt the drawn
// hull up to half a degree off entity_placement_matrix, the frame every
// simulation read (collision, userpoints, the carrier-owned camera) uses.
double pool_present_pitch_deg(const world::Entity &e);
double pool_present_roll_deg(const world::Entity &e);

// The door phases of the rows that publish any: a flat int32 side table of
// (row index, count, phase[count]) entries in row order, rebuilt beside the
// rows on every build, so only door-bearing entities carry retail's ordinal
// DOOR_xx bus [orig: build_bone_transforms @0x4E3070 loop @0x4e312a..0x4e3145;
//  BoneCallback_AnimatedBones_World @0x4E3180 loop @0x4e3201..0x4e3218 write
//  exactly num_doors slots per model]. A row's PF_DOOR_COUNT is its entry's
// count (0 = no entry); the signed phase dwords ride exactly.
using DoorPhaseTable = std::vector<int32_t>;

// The joiner's rows: one per decoded ClientState entity (PF_STRIDE floats
// each), the self echo left at its zero defaults, then one appended pool row
// per locally allocated item fragment (item_section_piece without a decoded
// row), written by the same per-entity writer the world path runs; only those
// fragment rows touch `lifecycle`. A decoded row whose materialized local
// entity carries door motion publishes that entity's own DoorSystem phases
// on `door_phases` (retail runs the door records and their CTRL publisher on
// every peer). Does NOT consume the animation pulses: the
// caller clears them (ClientState::clear_anim_pulses) once the rows are handed
// on, so each transition pulse dispatches exactly one presented frame.
void build_client_replica_present_rows(const PresentRowsContext &context,
        PoolPresentLifecycleMap &lifecycle, std::vector<float> &out, DoorPhaseTable &door_phases);

// The host/SP rows: one per live registry slot, in registry order.
void build_world_present_rows(const PresentRowsContext &context,
		PoolPresentLifecycleMap &lifecycle, std::vector<float> &out,
		DoorPhaseTable &door_phases);

// The local player's own item overlays (the canopy, the goggles, the
// binoculars, the carried object — world/person_overlays.h) for the local
// avatar presenter, from the same inputs a pool row publishes. False (and a
// cleared `out`) without a local infantry body.
bool local_player_person_overlays(const PresentRowsContext &context,
		world::PersonOverlays &out);

// A joiner's decoded row's own entity Flags dword, as its retail client entity
// holds it: the load-stream dword (0x0C/0x0D/0x10/0x20) with the live compact
// low byte, the client's runtime latch bits, and the Player class bit the wire
// class names (kEntityFlagPlayer). The render-slot march start reads it
// (world::PF_SLOT_MARCH_OFFSET_X).
uint32_t replica_entity_flags_dword(const replication::ClientEntityState &es);

} // namespace opennova::inmatch
