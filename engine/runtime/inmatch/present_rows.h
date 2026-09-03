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

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::inmatch {

// What both collectors read. The kernel is the world + the local player's
// view/weapon state + the asset-derived tables (seat specs, mounted graphics,
// the model cache); the runtime is the decoded replica view (its ClientState,
// the joiner's self handle, the LFP camp percents) and may be null on the
// world path of a bare host.
struct PresentRowsContext {
	mission::MissionKernel &kernel;
	ClientRuntime *runtime = nullptr;
	bool joiner = false;
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

// The joiner's rows: one per decoded ClientState entity (PF_STRIDE floats
// each), the self echo left at its zero defaults. Does NOT consume the
// animation pulses: the caller clears them (ClientState::clear_anim_pulses)
// once the rows are handed on, so each transition pulse dispatches exactly
// one presented frame.
void build_client_replica_present_rows(const PresentRowsContext &context,
		std::vector<float> &out);

// The host/SP rows: one per live registry slot, in registry order.
void build_world_present_rows(const PresentRowsContext &context,
		PoolPresentLifecycleMap &lifecycle, std::vector<float> &out);

} // namespace opennova::inmatch
