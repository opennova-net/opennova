#pragma once

#include <net/npruntime/client_runtime.h>

#include <net/netsim/client_state.h>
#include <net/netsim/client_world_materializer.h>

#include <runtime/mission/promote.h> // ItemSeatSpec (the binding-fed per-type seat table)
#include <runtime/simassets/seat_spec_extract.h> // item_seat_spec_for_type (the installed-table probe)
#include <runtime/world/collision.h> // CollisionWorld::ResolveState (per-replica resolver state)
#include <runtime/world/entity.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

// S10a (ADR 0028): the joiner's per-frame world<->net bridge, moved out of the
// Godot binding. The retail client frame is ONE function run on every machine
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180];
// its non-authority half — recv-fold + decoded-consequence application to the
// local world BEFORE this frame's local World tick, then the local motor tick
// and the post-tick attachment recompose — is the sequence pump() owns. The
// netsim ClientReplicaPipeline stays the ONE decoded-entity fold and
// ClientWorldMaterializer the ONE registry materializer (ADR 0026): this class
// sequences and consumes them, it reduces nothing itself.
//
// The embedding shell keeps what a portable bridge cannot own: the socket
// (deposit/send), device input application, the dict-conversion seams, the
// view/weapon pumps it shares with the host path, and the F3 profiling
// clocks. (The S7b disposition originally also kept asset resolution and the
// joiner 0x2F loadout pushes shell-side; ADR 0031 re-opened both — the
// collision/seat sweep lives in simassets/collision_resolve +
// seat_spec_extract, the profile-seed/0x2F composition in
// npruntime/loadout_submit; the shape-C2 round then moved the last live
// residue beside those: the mount-toggle gate/candidate into
// world/vehicle_attach, the mission chunk-tuple stash into mission/promote,
// the 0x5A grant->kit conversion into npruntime/loadout_submit, and the
// CTRL-bus composition + PANM clock into simassets/mounted_pose. The shell
// keeps only the role gates, its own state latches, and the hook wiring.)
// Those legs are injected per pump as PumpHooks — the same shape as
// host_session_pump's resolve-adm callback, the joiner's wider because
// retail runs the local-player legs INSIDE this frame, not around it.
namespace opennova::np {

// The decoded ClientState row for a wire handle, or null. Linear: ClientState
// keys presentation identity by handle and stays small (players + streamed
// movers).
inline const netsim::ClientEntityState *client_entity_for_handle(
		const netsim::ClientState &state, uint16_t handle) {
	for (const netsim::ClientEntityState &entity : state.entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

// (item_seat_spec_for_type moved to simassets/seat_spec_extract.h -- a pure
// specs probe belongs beside the extraction, below the net stack.)

// The mounted shooter's own vehicle joins the projectile trace exclusion exactly like
// retail's mount rule (Controller/Gunner/Driver seats only — passengers keep clipping
// their ride) — resolved from the wire shooter row's carrier + the binding-fed seat table
// instead of live mount pointers. Returns 0xFFFF when unmounted, passenger-seated, or
// the rows aren't streamed yet. Used for BOTH decoded remote rounds and the joiner's
// own predicted rounds: on a joiner the local ignored-mount leg is dead (wire_projected
// skips the local dynamics table), so this carrier gate is the only surviving exclusion.
// [orig: the ignored-mount select feeding Physics_RaycastAgainstBoneCollision @ 0x4e4cb0
//  via ray[18]]
inline uint16_t wire_carrier_exclusion_for(
		const netsim::ClientState &state, uint16_t shooter_handle,
		const std::vector<mission::ItemSeatSpec> &seat_specs) {
	const netsim::ClientEntityState *row =
			client_entity_for_handle(state, shooter_handle);
	if (row == nullptr || row->carrier_handle == world::EntityHandle::kInvalid ||
			row->mount_bone == 0)
		return 0xFFFFu;
	const netsim::ClientEntityState *carrier =
			client_entity_for_handle(state, row->carrier_handle);
	const mission::ItemSeatSpec *spec = carrier != nullptr
			? simassets::item_seat_spec_for_type(seat_specs, carrier->type_id)
			: nullptr;
	if (spec == nullptr) return 0xFFFFu;
	for (const world::Seat &seat : spec->seats) {
		if (seat.bone_index != row->mount_bone) continue;
		if (seat.type == world::SeatType::Controller ||
				seat.type == world::SeatType::Gunner ||
				seat.type == world::SeatType::Driver)
			return row->carrier_handle;
		break;
	}
	return 0xFFFFu;
}

// The joiner pump's phase attribution (microseconds; the F3 Stats board's
// joiner rows). Filled only when the embedder hands pump() a record; each
// pump ADDS onto it so the shell keeps one record per render frame.
struct JoinerPumpPerf {
	uint64_t materialize_us = 0; // stream materialize + decoded-state folds
	uint64_t mirror_us = 0;      // wire pose -> registry mirror (both passes)
	uint64_t proxies_us = 0;     // wire collision proxy rebuild
	uint64_t world_us = 0;       // the local World::run_logic_tick
	uint64_t attach_us = 0;      // remote attachment recompose + local re-pose
	uint64_t player_us = 0;      // weather/heading/view/weapon device pumps
	world::LogicTickPerf world;  // the tick's own phase breakdown (summed)
};

class JoinerWorldBridge {
public:
	// What this frame's client net pump decoded (drives the later phases).
	struct FrameSignals {
		bool health = false;     // authoritative 0x0A health tail applied
		bool objectives = false; // objective sync applied
	};

	// Everything the frame reads by reference from the embedding simulation.
	// World and runtime are per-load rebuilt objects; the aggregates are the
	// shared local-player state the host path pumps through the same types.
	struct PumpContext {
		world::World &world;
		ClientRuntime &runtime;
		world::LocalPlayerWeapon &weapon;
		world::LocalPlayerLoadout &loadout;
		world::WeaponInventory &inventory;
		// A REFERENCE, not a snapshot: apply_authoritative_loadout() flips the
		// embedder's inventory-valid flag mid-pump (inside run_client_net_frame,
		// before spawn_and_arm), and spawn_and_arm must see that live edge to
		// stamp L's equipped_adm_index + replay the deferred SWITCHTO the SAME
		// pump the first S2C 0x5A grant folds in — the normal wire-header join
		// (D-NET-194: no pre-load kit). A by-value copy froze it at pump entry
		// and dropped the stamp until the next respawn (wrong equipped adm on
		// the C2S 0x0C uplink + host third-person; no FP switch). [pinned by
		// joiner_world_bridge_test's mid-pump-grant case]
		bool &inventory_valid;
		const std::vector<mission::ItemSeatSpec> &seat_specs;
		// The per-model .adm registry the authority movers ground on (null =
		// none loaded; the row-side root-motion leg then stays chase-only).
		world::IRootMotionSource *root_motion = nullptr;
		// Phase attribution sink (null = no clocks; the profiling gate).
		JoinerPumpPerf *perf = nullptr;
	};

	// The shell/binding-owned legs of the frame, in the order pump() invokes
	// them. Every hook is required unless noted; the binding builds this on the
	// stack per frame (lambdas over the simulation).
	struct PumpHooks {
		// Ship one framed datagram to the dialed host (the socket send).
		std::function<void(const std::vector<uint8_t> &)> send;
		// Deposit received framed datagrams for this frame's recv pump
		// (socket poll -> runtime.receive).
		std::function<void()> deposit_inbound;
		// Resolve each decoded organic row's adm id once its type is known
		// (render item-db + registration caches stay shell-side).
		std::function<void()> resolve_row_adm_ids;
		// The F3 Stats wire-leg clock stamp: called right after the uplink
		// ships, before the decoded-state folds (may be null = no profiling).
		std::function<void()> on_wire_leg_complete;
		// The S2C 0x5A authoritative-loadout fold (S7b: the binding keeps the
		// dict/profile conversion seam).
		std::function<void()> apply_authoritative_loadout;
		// Re-copy the resident kit page when the side selector moved; true =
		// the resident buffer changed (the pump then re-arms the 0x2F seam).
		std::function<bool()> reseed_kit_on_side_change;
		// Re-arm the joiner 0x2F loadout-submission seam.
		std::function<void()> push_loadout_kit;
		// ~1 Hz tripwire sample landed (state is on the bridge accessors); the
		// binding owns the env-gated diagnostic print channel.
		std::function<void()> on_diagnostic_sample;
		// A streamed topology/world change materialized: rebuild the shell's
		// asset-backed caches (collision/occlusion instances, item traits,
		// seat specs) for the retired/spawned/updated rows.
		std::function<void(const netsim::ClientWorldSyncResult &)>
				on_replica_world_changed;
		// The pool-0 fence passed: run the one-shot static-table/portal-weld
		// pass over the now-complete streamed world.
		std::function<void()> on_replica_world_static_ready;
		// L spawned on the in-match edge: clear the shell's input latches,
		// seed the look heading, resolve fresh adm ids.
		std::function<void(int32_t look_heading_bam)> on_local_player_spawned;
		// L revived on an ACK-qualified redeploy release: clear the shell's
		// input latches, seed the look heading, rebuild the respawn loadout.
		std::function<void(int32_t look_heading_bam)> on_local_player_redeployed;
		// The authoritative mount relation changed: refresh the shell's view
		// effects (binocular latch) + the mounted input heading.
		std::function<void()> on_mount_changed;
		// Wire-side authored-shape resolution for one decoded runtime type id
		// (items.def graphic -> the shell's by-graphic collision model cache).
		// Unresolvable rows return the default shape. This typed value is the
		// cutover API: model, exact effective bound/scale, and bbox center stay
		// inseparable for movement, projectiles, and lighting.
		std::function<world::ResolvedCollisionShape(uint16_t type_id)>
				wire_collision_shape;
		// Device input -> L's body input, before the local World tick
		// (net-before-logic, ADR 0009).
		std::function<void()> apply_input_pre_tick;
		// Mirror an authoritative attach's snapped yaw back into the input
		// heading latch (shared with the host path).
		std::function<void()> sync_mounted_input_heading;
		// The per-frame view promoter (retail promotes the view before weapon
		// actions).
		std::function<void()> tick_view;
		// The equipped-slot FSM pump (the bridge gates it on L's existence).
		std::function<void()> tick_weapon;
		// The weather tick after the local world tick (the kernel's ONE
		// tick_weather: sim legs, thunder/shake events, the render owner's
		// color legs) [orig: Game_ProcessMainFrame @ 0x526774].
		std::function<void()> tick_weather;
	};

	// The joiner's per-frame pump. Retail dispatches received messages before
	// the entity/weapon-action pumps, so decoded consequences are applied to L
	// before this frame's local World tick. In particular, an S2C 0x49 arriving
	// on a reload's DONE boundary must refill the slot before IDLE can observe
	// the stale empty magazine and queue a second C2S 0x25.
	// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180]
	// The pump itself is the phase sequence; each helper carries its witnesses.
	void pump(const PumpContext &ctx, const PumpHooks &hooks);

	// ClientHello once (Idle -> Hello) on the first armed frame. Public because
	// the pre-mission preload pump (no world yet) drives the same latch.
	void send_hello_once(ClientRuntime &runtime,
			const std::function<void(const std::vector<uint8_t> &)> &send);

	// The exact-handle materialized world twin of a decoded wire row (null when
	// the handle has no truthful local carrier). Header-only joins read the
	// materializer's owned rows; full-BMS joins read the promoted registry row
	// (spawn-origin gated).
	world::Entity *replica_world_entity(
			world::World &world, world::EntityHandle handle);

	// The JOINER's per-frame clock (the host uses HostOwner::now_tick). The
	// preload pump shares it, so it advances there too.
	uint32_t now_tick() const { return now_tick_; }
	void advance_tick() { ++now_tick_; }

	bool started() const { return started_; }
	bool local_spawned() const { return local_spawned_; }
	uint16_t self_wire_handle() const { return self_wire_handle_; }
	int flat_seconds() const { return flat_seconds_; }
	bool freeze_suspected() const { return freeze_suspected_; }

	// A true S2C 0x0B mission carries only the 616-byte BMS header. Retail
	// allocates pools 1..3 while consuming 0x0D/0x10/0x20; the materializer
	// gives local world consumers the same exact packed rows. Full-BMS joiners
	// never enter that path and keep ordinary promotion untouched.
	void set_wire_header_world(bool v) { wire_header_world_ = v; }
	bool wire_header_world() const { return wire_header_world_; }
	netsim::ClientWorldMaterializer &materializer() { return materializer_; }

	// enable_join: a fresh join resets every per-session latch (the runtime is
	// rebuilt beside it).
	void reset_for_join();
	// finish_load: a (re)load respawns L and re-baselines the release cursor
	// to the live runtime edge (a retail-style menu join keeps its session);
	// the tripwire samplers deliberately persist across loads.
	void reset_for_load(uint64_t deployment_release_revision_baseline) {
		local_spawned_ = false;
		deployment_release_revision_seen_ = deployment_release_revision_baseline;
		redeploy_release_pending_ = false;
		redeploy_health_updates_at_release_ = 0;
		self_wire_handle_ = 0;
	}
	// finish_load's direct-load branch rebuilt a fresh ClientRuntime: the hello
	// re-arms and the receive-side cursors restart with it.
	void reset_for_runtime_rebuild() {
		started_ = false;
		weather_revision_seen_ = 0;
		mounted_ammo_revision_seen_ = 0;
	}
	// Stop/Start restart with a live wire-header world: force one exact
	// rematerialization fold; the portal tables persist by design (their
	// handles remain identical and the weld records are one-shot mutable).
	void reset_materialization();
	// reset_world: the whole streamed-world image dies with the world.
	void reset_world_stream();

private:
	FrameSignals run_client_net_frame(
			const PumpContext &ctx, const PumpHooks &hooks);
	void wire_frame_providers(const PumpContext &ctx, const PumpHooks &hooks);
	void apply_weather_sample(const PumpContext &ctx);
	void materialize_replica_world(
			const PumpContext &ctx, const PumpHooks &hooks);
	void spawn_and_arm_local_player(
			const PumpContext &ctx, const PumpHooks &hooks);
	void apply_authoritative_health(
			const PumpContext &ctx, const PumpHooks &hooks);
	void sync_authoritative_mount(
			const PumpContext &ctx, const PumpHooks &hooks);
	void apply_mounted_ammo_update(const PumpContext &ctx);
	void mirror_mission_entities(const PumpContext &ctx);
	void mirror_predicted_vehicles(const PumpContext &ctx);
	void refresh_wire_collision_proxies(
			const PumpContext &ctx, const PumpHooks &hooks);
	void apply_gameplay_events(const PumpContext &ctx);

	// The world the current pump runs against, latched at pump entry: the
	// provider closures installed on the runtime's pipeline persist across
	// frames and must never capture a per-call reference (a reload rebuilds
	// the World before it rebuilds the runtime).
	world::World *world_ = nullptr;

	bool started_ = false;       // ClientHello emitted (Idle -> Hello)
	bool local_spawned_ = false; // L spawned at reached_in_match (one-shot guard)
	bool wire_header_world_ = false;
	bool wire_world_static_initialized_ = false;
	uint64_t wire_world_topology_revision_seen_ = ~uint64_t{0};
	uint64_t wire_world_stream_revision_seen_ = ~uint64_t{0};
	netsim::ClientWorldMaterializer materializer_;
	// ClientRuntime raises a monotonic edge for each gameplay/deployment release.
	// The bridge uses it to snap L to the host-selected post-pick pose; health
	// remains separately gated by authoritative_spawn_released(), so C2S 0x0E
	// cannot kill L and a stale positive tail cannot revive a genuinely dead L.
	uint64_t deployment_release_revision_seen_ = 0;
	// S2C 0x50 re-latched OUR OWN team (the second byte_A85B48 writer). The
	// join-time team arrives through the spawn, so only later edges apply here.
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the latch @0x4319db]
	uint64_t self_team_revision_seen_ = 0;
	// Receive-once cursor for the conditional flags2&0x0f==8 mounted-ammo
	// record. A stale net sample must not refill a locally firing gun each tick.
	uint32_t mounted_ammo_revision_seen_ = 0;
	bool redeploy_release_pending_ = false;
	uint32_t redeploy_health_updates_at_release_ = 0;
	// H is stamped in C2S 0x0C and used by the present self-filter. Zero is a
	// valid handle; runtime.has_self_handle() carries validity independently.
	uint16_t self_wire_handle_ = 0;
	uint32_t now_tick_ = 0;
	// The last S2C 0x0A phase-2 ENV revision folded into the weather home.
	uint32_t weather_revision_seen_ = 0;

	// ~1 Hz frozen-session tripwire state.
	std::size_t last_gap_depth_ = 0;
	uint32_t last_frontier_seq_ = 0;
	uint32_t last_records_applied_ = 0;
	uint32_t last_outbound_seq_ = 0;
	bool diagnostic_sampled_ = false;
	int flat_seconds_ = 0;
	bool freeze_suspected_ = false;

	// Per-replica-row resolver state (the movement collision resolver's
	// prev-pose + idle skip counter) keyed by wire handle — the persistent
	// half the ClientState row cannot carry across the netsim seam. Entries
	// for retired rows are benign: a reused handle's stale prev pose triggers
	// one displaced-detect full update and self-corrects.
	std::unordered_map<uint16_t, world::CollisionWorld::ResolveState>
			replica_resolve_states_;
	// Scratch for the replica contact resolver's netsim->world peer copy
	// (cleared per call; grows once to the session's peer cap).
	std::vector<world::CollisionWorld::ReplicaPeer> replica_peer_scratch_;
};

} // namespace opennova::np
