// S10a (ADR 0028): the joiner's per-frame world<->net bridge, moved verbatim
// from the Godot binding's simulation_net.cpp. See joiner_world_bridge.h
// for the ownership split; every phase keeps its original witnesses.
#include <net/npruntime/joiner_world_bridge.h>

#include <net/netsim/client_replica_pipeline.h>
#include <net/netsim/entity_wire_bridge.h> // build_player_uplink (the C2S 0x0C body)

#include <net/npwire/ingame_decode.h>    // kRoundEventFlag* (the fire-mode byte)
#include <net/npwire/wire_handle.h>      // pool()/kPoolItem (the wire handle home)

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>   // destruction_notify_item_damage (S2C 0x13 net kill)
#include <runtime/world/entity_spawn.h>  // entity_reset_to_spawn_state (redeploy release)
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <runtime/world/vehicle_mount.h> // resolve_mounted_ammo_slot (phase-8 route)
#include <runtime/world/weapon_table.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::np {

namespace {

constexpr double kFixed16 = 65536.0;

// SelfSpawn (mission i32 16.16 + full BAM32 orientation) -> PlayerSpawn for L.
world::PlayerSpawn spawn_from_self(const JoinerConnection::SelfSpawn &s) {
	world::PlayerSpawn spawn;
	// SelfSpawn position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position = {static_cast<float>(s.pos_x / kFixed16),
	                  static_cast<float>(s.pos_y / kFixed16),
	                  static_cast<float>(s.pos_z / kFixed16)};
	// orientation is ALREADY a full 32-bit BAM (unlike HostJoinerPose.heading, an i16
	// the host shifts << 16) -> pass it straight to the (90 - heading) mission-degree map.
	spawn.yaw = static_cast<int16_t>(
			std::lround(world::mission_yaw_deg_from_bam_heading(s.orientation)));
	spawn.team = s.team;
	// The named player record carries the host-stamped character selector and
	// packed minimap/character id. Preserve both on local L just as retail's
	// Player_InitPlayer does; dropping animSlot recreated the D-NET-146
	// DBuggy-shadow association on the joiner's own presentation.
	spawn.anim_slot = s.anim_slot;
	spawn.minimap_net_id = s.net_id;
	// [D-NET-112] The packed id belongs to the wire NetId ONLY. A player carries no SSN, so L
	// takes net_id 0 exactly like the host's own spawn (server_spawn.cpp) — that keeps it out of
	// the WAC/BMS find_by_net_id space, which PlayerSpawn.net_id's 0xFFF0 default would otherwise
	// join, so the zero must be written explicitly. [orig: Server_PlayerAdd @0x51cbc0 slot+440 ->
	// entity+0x15C (the wire NetId); EntityPool_FindByNetId @0x4f0a20 keys entity+0x7C, left 0
	// for players]
	spawn.net_id = 0;
	return spawn;
}

} // namespace

void JoinerWorldBridge::pump(const PumpContext &ctx, const PumpHooks &hooks) {
	// The provider closures below persist on the pipeline across frames; they
	// read this latched pointer at call time, never a per-call reference.
	world_ = &ctx.world;
	wire_frame_providers(ctx, hooks);
	hooks.resolve_row_adm_ids();
	send_hello_once(ctx.runtime, hooks.send);
	hooks.deposit_inbound();
	const FrameSignals decoded = run_client_net_frame(ctx, hooks);
	// Phase 0 of the authoritative 0x0A is the client's one pre-round
	// predicate. Mirror it onto World before any local entity/system work.
	ctx.world.preround_delay_seconds =
			ctx.runtime.state().preround_delay_seconds;
	materialize_replica_world(ctx, hooks);
	spawn_and_arm_local_player(ctx, hooks);
	if (decoded.health) apply_authoritative_health(ctx, hooks);
	sync_authoritative_mount(ctx, hooks);
	apply_mounted_ammo_update(ctx);
	mirror_mission_entities(ctx);

	// Received projectile/reload gameplay and the decoded remote collision
	// proxies are live inputs to this frame's entity/round/weapon pumps. Applying
	// them here is the retail recv-before-actions boundary, not presentation work.
	refresh_wire_collision_proxies(ctx, hooks);
	apply_gameplay_events(ctx);
	apply_weather_sample(ctx);

	const bool preround_active = ctx.world.preround_delay_seconds != 0;
	hooks.apply_input_pre_tick(); // input latches stay live through the phase
	ctx.world.run_logic_tick(
			/*is_authority=*/false,
			preround_active ? world::TickPhase::PreRound
			                : world::TickPhase::Gameplay);
	if (!preround_active)
		mirror_predicted_vehicles(ctx); // predicted boat poses -> presented rows
	// Vehicle prediction is the final carrier mover on a joiner. Recompose every
	// seat/deck/object attachment from that final pose in this same frame, then
	// publish the refreshed rows back to the local registry consumers. This is
	// the retail second carrier-follow phase; doing it before the world mover
	// leaves children one tick behind their vehicle.
	if (!preround_active) ctx.runtime.refresh_remote_attachments();
	mirror_mission_entities(ctx);
	// The local mounted body was seat-posed earlier in AiSystem::tick, before
	// the joiner-only vehicle prediction pass. Re-pose L against the vehicle's
	// final same-frame transform so the camera/view never trails its seat by one
	// mover tick. Remote riders were recomposed in ClientState just above.
	if (!preround_active && ctx.world.ai != nullptr &&
			ctx.world.cached.local_player.valid()) {
		if (world::AiEntity *local_ai =
				ctx.world.ai->for_handle(ctx.world.cached.local_player)) {
			ctx.world.ai->refresh_mounted_pose(*local_ai, ctx.world);
		}
	}
	// The weather tick follows the entity update on a client exactly as on
	// the host [orig: Game_ProcessMainFrame @ 0x52674b -> @ 0x526774].
	if (hooks.tick_weather) hooks.tick_weather();
	hooks.sync_mounted_input_heading();
	hooks.tick_view();   // retail promotes the per-frame view before weapon actions
	// The equipped-slot FSM pump, after the view promoter. Gated on L: retail
	// pumps weapon actions per-entity, so a joiner whose player has not spawned
	// has no slot to pump — without this gate a click during the join wait
	// discharged the pre-armed FSM with no shooter and the joiner deployed a
	// round short.
	// [orig: WeaponAction_ProcessAllEntities @ 0x526786]
	if (local_spawned_) hooks.tick_weapon();
	++now_tick_;
}

void JoinerWorldBridge::apply_weather_sample(const PumpContext &ctx) {
	// Each newly received phase-2 ENV sub-block lands in the weather home's
	// TARGET globals exactly once; the local currents keep chasing
	// [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c].
	const netsim::ClientEnvironmentState &environment = ctx.runtime.state().environment;
	if (!environment.present || environment.revision == weather_revision_seen_) return;
	weather_revision_seen_ = environment.revision;
	world::WeatherWireSample sample;
	sample.fog_dist = environment.fog_dist;
	sample.fog_accel = environment.fog_accel;
	sample.tod_fixed = environment.tod_fixed;
	sample.quake_ticks = environment.quake_ticks;
	sample.cloud_scroll = environment.cloud_scroll;
	sample.rain_pct = environment.rain_pct;
	sample.overcast = environment.overcast;
	sample.precipitation_kind = environment.env_param;
	ctx.world.weather.apply_wire_sample(sample);
}

void JoinerWorldBridge::wire_frame_providers(
		const PumpContext &ctx, const PumpHooks &hooks) {
	// The row-side root-motion leg (net-re §5.38e): hand the joiner's view the
	// same per-model .adm registry the authority movers ground on, and resolve
	// each decoded organic row's adm id once its type is known — the netsim
	// twin of resolve_new_infantry_adm_ids (AnimMap_RegisterEntity's spawn
	// half [orig: @0x40bb60]).
	ctx.runtime.view().set_root_motion_source(ctx.root_motion);
	// The same joiner terrain backs the post-root 2u person ground probe.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B7CF4;
	// Entity_UpdateInfantryAI @0x4BF7FA; Entity_MovementCollisionResolver tail
	// @0x4B3D6E..0x4B3DA9]
	ctx.runtime.view().set_remote_motion_terrain(ctx.world.terrain);
	// The replica water/float channel reads the mission water plane
	// [orig: Env_WaterHeightFixed @ 0x26C6454] (EnvState convention: 0 = no
	// water in this world).
	ctx.runtime.view().set_water_z(
			ctx.world.env.water_z, ctx.world.env.water_z != 0);
	// Replica peer/source spheres are initialized from the exact same typed
	// items.def/model result as projectile and lighting projections. Capture the
	// std::function by value: the pipeline retains this callback past the
	// stack-owned PumpHooks instance.
	const auto wire_collision_shape = hooks.wire_collision_shape;
	ctx.runtime.view().set_replica_bound_radius_resolver(
			[wire_collision_shape](uint16_t type_id) -> int32_t {
				return wire_collision_shape(type_id).bound_radius_q16;
			});
	// The FULL replica contact resolver (net-re §5.38e, D-NET-196): with the
	// joiner world's collision tables live, each armed Player/Infantry row's
	// settle runs the ported movement collision resolver — candidate-model
	// contacts + push-out, world-person and replica-peer repulsion, and the
	// ground probe THROUGH candidate models — replacing the terrain-column
	// subset. Retail runs remote organics through the ordinary org movers
	// whose shared tail calls the resolver ungated [orig: calls @0x4B7CF4 /
	// @0x4BF7FA; resolver @0x4B2BD0]. The per-row ResolveState persists here
	// (replica_resolve_states_) across pumps.
	if (ctx.world.ai != nullptr && ctx.world.ai->collision != nullptr) {
		ctx.runtime.view().set_replica_contact_resolver(
				[this](netsim::ClientReplicaPipeline::ReplicaContactQuery
								&q) -> int32_t {
					using world::CollisionWorld;
					CollisionWorld *col = world_->ai->collision;
					CollisionWorld::ResolveState &st =
							replica_resolve_states_[q.row_handle];
					// Bridge-member scratch: this resolver runs per armed
					// replica row per 62.5 Hz pump — a fresh heap vector per
					// call was pure allocator churn for a field-order copy
					// between the two mirrored peer PODs.
					std::vector<CollisionWorld::ReplicaPeer> &peers =
							replica_peer_scratch_;
					peers.clear();
					peers.reserve(static_cast<size_t>(q.peer_count));
					for (int32_t i = 0; i < q.peer_count; ++i) {
						CollisionWorld::ReplicaPeer p;
						p.handle = q.peers[i].handle;
						p.x = q.peers[i].x;
						p.y = q.peers[i].y;
						p.z = q.peers[i].z;
						p.radius = q.peers[i].radius;
						peers.push_back(p);
					}
					world::EntityHandle ground;
					const int32_t clearance = col->resolve_replica(
							*world_, st, q.pos, q.vel_xy, q.vel_z,
							q.capsule_bottom, q.capsule_top,
							q.source_bound_radius_q16, q.is_player_class,
							q.tick, q.anim_state_id, q.anim_state_flags,
							peers.data(), static_cast<int32_t>(peers.size()),
							q.row_handle, &q.entity_flags, &ground);
					q.out_ground = ground.valid() ? ground.packed
					                              : world::EntityHandle::kInvalid;
					return clearance;
				});
		// The deck-ride carrier seam: resolved_ground is the registry handle
		// the ground probe stored; the ride reads that entity's LIVE pose
		// against its mover-entry savedLivePose stamp — the witnessed pair
		// [orig: the org movers read groundEntity's Position vs +0x80../body*
		// +0x8C.. @0x4b530b../@0x4ba47f..]. One shared reader
		// (carrier_pose_fixed) serves both sides so representations can
		// never diverge; a never-stamped entity (static) reads zero delta.
		ctx.runtime.view().set_carrier_pose_provider(
				[this](uint16_t handle,
						netsim::ClientReplicaPipeline::CarrierPose
								&out) -> bool {
					if (world_ == nullptr) return false;
					world::EntityHandle h;
					h.packed = handle;
					const world::Entity *e = world_->registry.get(h);
					if (e == nullptr) return false;
					world::carrier_pose_fixed(
							*e, out.pos, out.yaw, out.pitch, out.roll);
					if (e->saved_live_valid) {
						out.saved_pos[0] = e->saved_live_pos[0];
						out.saved_pos[1] = e->saved_live_pos[1];
						out.saved_pos[2] = e->saved_live_pos[2];
						out.saved_yaw = e->saved_live_yaw;
						out.saved_pitch = e->saved_live_pitch;
						out.saved_roll = e->saved_live_roll;
					} else {
						out.saved_pos[0] = out.pos[0];
						out.saved_pos[1] = out.pos[1];
						out.saved_pos[2] = out.pos[2];
						out.saved_yaw = out.yaw;
						out.saved_pitch = out.pitch;
						out.saved_roll = out.roll;
					}
					out.bound_radius = world::to_fixed(e->bound_radius);
					return true;
				});
	} else {
		ctx.runtime.view().set_replica_contact_resolver(nullptr);
		ctx.runtime.view().set_carrier_pose_provider(nullptr);
	}
}

// ClientHello once (Idle -> Hello) the first armed frame.
void JoinerWorldBridge::send_hello_once(ClientRuntime &runtime,
		const std::function<void(const std::vector<uint8_t> &)> &send) {
	if (!started_) {
		const std::vector<uint8_t> hello = runtime.start();
		if (!hello.empty()) send(hello);
		started_ = true;
	}
}

// Recv-fold + connect-drive + the gated C2S 0x0C uplink, then the decoded-state
// folds (loadout/kit, side assignment, deployment-release latch, the ~1 Hz
// freeze tripwire, objective sync into world subgoals). The wire-leg-complete
// hook fires right after the uplink ship, before the folds, so the embedder's
// F3 Stats board measures exactly the wire leg. Returns what this frame's pump
// decoded (drives the later phases).
JoinerWorldBridge::FrameSignals JoinerWorldBridge::run_client_net_frame(
		const PumpContext &ctx, const PumpHooks &hooks) {
	const uint32_t now = now_tick_;
	// Run the client net frame first: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C
	// uplink (gated InMatch && deployed inside the runtime). The uplink describes L's pose as left by
	// the previous entity update; this frame's raw-input/motor pass follows all inbound application.
	const uint32_t health_updates_before =
			ctx.runtime.state().health_updates_applied;
	const uint32_t objective_updates_before =
			ctx.runtime.state().objective_updates_applied;
	const bool local_existed_before_net = local_spawned_;
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = local_spawned_ && ctx.world.ai &&
			ctx.world.cached.local_player.valid();
	const world::Entity *e =
			have_L ? ctx.world.registry.get(ctx.world.cached.local_player) : nullptr;
	const world::AiEntity *ae =
			have_L ? ctx.world.ai->for_handle(ctx.world.cached.local_player) : nullptr;
	// A release can arrive during this recv pump. Do not let that newly-opened
	// runtime gate transmit the corpse/stale pre-deploy pose in the same frame;
	// L resumes uplinking only after the simulation has consumed the release and
	// a later positive authoritative health tail.
	const bool can_offer_uplink =
			ctx.runtime.is_deployed() && e != nullptr && ae != nullptr &&
			e->alive && e->health > 0 && (e->flags & 2u) == 0u &&
			!redeploy_release_pending_;
	if (can_offer_uplink) {
		const PlayerExtendedUplink up =
				netsim::build_player_uplink(ctx.world, *e, *ae);
		outs = ctx.runtime.Client_ProcessNetworkFrame(up, now);
	} else {
		outs = ctx.runtime.Client_ProcessNetworkFrame(now);
	}
	for (const std::vector<uint8_t> &dg : outs) hooks.send(dg);
	if (hooks.on_wire_leg_complete) hooks.on_wire_leg_complete();
	hooks.apply_authoritative_loadout();
	// The team selector may only just have become known (the S2C 0x04 latch landing
	// after the catalog) or may have moved us across the line (S2C 0x50). Either way the
	// resident kit buffer follows the side, exactly as retail re-copies restrictionData
	// for the newly assigned side [orig: NapiNPClientMsg_TeamAssign @0x431a9a].
	if (hooks.reseed_kit_on_side_change()) hooks.push_loadout_kit();
	// S2C 0x50 re-latched OUR OWN team. L's team is seeded once by spawn_from_self
	// (the join-time 0x0C record's team byte); a later assignment must move both the
	// entity's Team and the round sim's presenting-client team, or friend/foe styling
	// keeps rendering the old side. Retail does the same two writes: byte_A85B48 for
	// the latch and entity->Team for the entity itself.
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — @0x4319db / @0x4319ee; the round-spawn
	//  style select reads the local player's Team @0x4ec740]
	{
		const uint64_t self_team_revision = ctx.runtime.self_team_revision();
		if (self_team_revision > self_team_revision_seen_) {
			self_team_revision_seen_ = self_team_revision;
			const uint8_t assigned = ctx.runtime.assigned_team();
			if (world::Entity *L =
					ctx.world.registry.get(ctx.world.cached.local_player)) {
				L->team = assigned;
			}
			ctx.world.round_sim.local_team = assigned;
			// The 0x50 handler also RE-SELECTS the newly assigned side's profile page
			// and re-submits it: profileData = (team==1||team==3) ? blue : red, class
			// = *profileData, page = profileData + {6,2054,4102,6150,8198}
			// [orig: NapiNPClientMsg_TeamAssign @0x431a35..@0x431a9e — the submit
			// passes slot 195 RAW]. The re-submission itself lives in the joiner
			// runtime; re-arm the seam from the NEW side so its content is the page
			// retail would have copied. The resident buffer itself moves with the side
			// in reseed_kit_on_side_change above (retail's qmemcpy replaces
			// restrictionData wholesale @0x431a9a) — a reassignment WITHIN one side
			// reaches only this re-arm, which is what retail's unconditional re-submit
			// does too.
			hooks.push_loadout_kit();
		}
	}
	const uint64_t deployment_release_revision =
			ctx.runtime.deployment_release_revision();
	if (deployment_release_revision > deployment_release_revision_seen_) {
		deployment_release_revision_seen_ = deployment_release_revision;
		// The first release creates L below. A later release must revive that
		// existing identity, but only after a positive 0x0A tail observed after
		// this release frame (not a stale positive already queued ahead of it).
		if (local_existed_before_net) {
			redeploy_release_pending_ = true;
			redeploy_health_updates_at_release_ =
					ctx.runtime.state().health_updates_applied;
		}
	}
	// Frozen-session tripwire (live-diagnosis aid, ~1 Hz): sample the signature
	// continuously; the embedder's diagnostic hook owns the (env-gated) print.
	// An unrecovered S2C sequence gap stalls the ordered frontier, the remote world
	// freezes, retained records stop retiring, and the host eventually reaps us.
	if ((now % 62u) == 0u) {
		const std::size_t gap_depth = ctx.runtime.inbound_gap_depth();
		const uint32_t frontier = ctx.runtime.inbound_frontier_seq();
		const uint32_t out_seq = ctx.runtime.outbound_seq();
		const uint32_t records = ctx.runtime.state().compact_records_applied;
		// A true ordered-replication stall requires an unresolved gap. Flat records
		// alone are normal before deployment and whenever the remote world is idle.
		// Continuing outbound sequence movement proves the local socket/frame pump
		// is still alive rather than paused.
		const bool frontier_flat =
				diagnostic_sampled_ && frontier == last_frontier_seq_;
		const bool records_flat =
				diagnostic_sampled_ && records == last_records_applied_;
		const bool outbound_advanced =
				diagnostic_sampled_ && out_seq > last_outbound_seq_;
		const bool stalled_sample =
				ctx.runtime.in_match() && ctx.runtime.is_deployed() &&
				gap_depth > 0 && frontier_flat && records_flat &&
				outbound_advanced;
		if (stalled_sample) ++flat_seconds_;
		else flat_seconds_ = 0;
		freeze_suspected_ = flat_seconds_ >= 3;
		if (hooks.on_diagnostic_sample) hooks.on_diagnostic_sample();
		last_frontier_seq_ = frontier;
		last_records_applied_ = records;
		last_outbound_seq_ = out_seq;
		last_gap_depth_ = gap_depth;
		diagnostic_sampled_ = true;
	}
	FrameSignals decoded;
	decoded.health =
			ctx.runtime.state().health_updates_applied != health_updates_before;
	decoded.objectives =
			ctx.runtime.state().objective_updates_applied != objective_updates_before;
	if (decoded.objectives) {
		const netsim::ClientState &client = ctx.runtime.state();
		ctx.world.subgoals.won = client.objective_won;
		ctx.world.subgoals.lost = client.objective_lost;
		ctx.world.subgoals.show_win = client.objective_show_win;
		ctx.world.subgoals.show_lose = client.objective_show_lose;
	}
	return decoded;
}

world::Entity *JoinerWorldBridge::replica_world_entity(
		world::World &world, world::EntityHandle handle) {
	if (!handle.valid()) return nullptr;
	if (wire_header_world_)
		return materializer_.owned(world, handle);
	world::Entity *entity = world.registry.get(handle);
	return entity != nullptr &&
			entity->spawn_origin != world::kSpawnOriginNone
			? entity
			: nullptr;
}

void JoinerWorldBridge::materialize_replica_world(
		const PumpContext &ctx, const PumpHooks &hooks) {
	if (!wire_header_world_) return;

	const netsim::ClientState &state = ctx.runtime.state();
	if (wire_world_topology_revision_seen_ == state.topology_revision &&
			wire_world_stream_revision_seen_ == state.world_stream_revision)
		return;
	const netsim::ClientWorldSyncResult sync =
			materializer_.sync(state, ctx.world);
	wire_world_topology_revision_seen_ = state.topology_revision;
	wire_world_stream_revision_seen_ = state.world_stream_revision;
	if (sync.changed()) {
		// The embedder rebuilds its asset-backed caches for the changed rows
		// (retired collision/occlusion instances, item traits, seat specs),
		// then re-folds the retained 0x0D mountHandles image through
		// materializer() once definitions exist.
		hooks.on_replica_world_changed(sync);
	}

	// The witnessed initial-state order is pool2 -> pool1 -> pool0 -> pool3.
	// Pool 0 is therefore the first safe fence for the one-shot static-table and
	// portal-weld pass; running it for each pool2 page would weld an incomplete
	// world and mutate shared occlusion records repeatedly.
	if (!wire_world_static_initialized_) {
		bool saw_pool0 = false;
		for (const netsim::ClientEntityState &row : state.entities) {
			if (world::EntityHandle{row.handle}.pool() == 0) {
				saw_pool0 = true;
				break;
			}
		}
		if (saw_pool0) {
			hooks.on_replica_world_static_ready();
			wire_world_static_initialized_ = true;
		}
	}
}

// On the in-match edge (detected by the recv-fold): learn H + spawn L at the host-advertised
// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
// wire identity the host knows us by — the two stay distinct, reconciled by the name-match (§5.38b).
void JoinerWorldBridge::spawn_and_arm_local_player(
		const PumpContext &ctx, const PumpHooks &hooks) {
	if (ctx.runtime.in_match() && !local_spawned_ && ctx.world.ai) {
		self_wire_handle_ = ctx.runtime.self_handle();
		const JoinerConnection::SelfSpawn &sp = ctx.runtime.spawn_pose();
		const world::PlayerSpawn spawn = spawn_from_self(sp);
		const world::EntityHandle h = world::spawn_player(ctx.world, spawn);
		local_spawned_ = h.valid();
		// Arm L the way the host's own spawn does at Player_InitPlayer time: the
		// shell applied the profile kit/class BEFORE L existed (the pre-spawn
		// apply latched it into the inventory), so stamp the deferred class +
		// damage classes + equipped adm on the fresh entity now. [orig:
		// Player_InitPlayer weapon leg @ 0x4e15f0; equippedAdmIndex stamp @ 0x4dd727]
		if (world::Entity *L = ctx.world.registry.get(h)) {
			if (ctx.loadout.pending_player_class >= 5 &&
					ctx.loadout.pending_player_class <= 9)
				L->player_class =
						static_cast<uint8_t>(ctx.loadout.pending_player_class);
			world::local_loadout_sync_damage_classes(ctx.world, ctx.loadout);
			if (ctx.inventory_valid && ctx.inventory.equipped_combo >= 0) {
				const world::WeaponInventorySlot *slot =
						ctx.inventory.slot(ctx.inventory.equipped_combo);
				if (slot != nullptr && slot->adm_index >= 0) {
					L->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
					// Replay the deferred PRESENTATION half of every selection that
					// committed while L did not exist (the 0x5A grant applies before
					// the spawn). Retail has no entity precondition there: the
					// selection restamps equippedAdmIndex and the FP viewmodel is
					// re-resolved per frame off EquippedSlot [orig: the 0x5A tail
					// @0x4296E3 -> Player_SelectWeaponSlot @0x4DD680 @0x4dd727;
					// Player_RenderFirstPersonViewModel @0x4DED60]. Exactly ONE event
					// is queued, naming the weapon the inventory actually selected —
					// without it the viewmodel and the weapon FSM kept running the
					// SUBMITTED weapon while the entity and the wire followed the
					// granted one.
					// weapon.start_in_switchto is deliberately left as the last
					// rebuild settled it — the spawn mount plays no switch actions
					// (D-WPN-21), and this replay only restores the notification.
					if (ctx.weapon.presentation_pending) {
						ctx.weapon.presentation_pending = false;
						const world::WeaponTableEntry *def =
								ctx.world.weapons.by_index(
										static_cast<uint8_t>(slot->adm_index));
						world::WeaponPresentationEvent event;
						event.tick = ctx.world.logic_tick;
						if (const world::Entity *local =
									ctx.world.registry.get(
											ctx.world.cached.local_player))
							event.world_position = local->position;
						event.switch_to_weapon =
								def != nullptr ? def->name : std::string();
						ctx.weapon.events.push_back(std::move(event));
					}
				}
			}
		}
		// Retail polls live keys; a press during the join wait must not cross
		// the spawn edge as a queued shot/reload. Clear the consume-latches too;
		// the embedder clears its device-input latches and seeds the look
		// heading from the spawn facing in the hook.
		ctx.weapon.fire_held = false;
		ctx.weapon.fire_pressed = false;
		ctx.weapon.reload_pressed = false;
		hooks.on_local_player_spawned(
				world::bam_heading_from_mission_yaw_deg(spawn.yaw));
	}
}

// The 0x0A tail is the authoritative health source for the recipient's OWN
// player. H belongs to the host's handle space; apply that recipient-local
// scalar to the joiner's distinct motor entity L (the pump gates this phase on
// the frame's decoded health signal). A fresh-frame guard prevents
// ClientState's pre-frame zero default from killing L during the handshake.
// Once L is dead, positive health revives it only after the separate
// ACK-qualified deployment release latched by the net-frame folds, and only
// from a later tail. That edge also snaps L to H's redeployed authoritative
// pose before its next uplink can run.
// [orig: tail health read @0x430428; store to local Health @0x4305df]
void JoinerWorldBridge::apply_authoritative_health(
		const PumpContext &ctx, const PumpHooks &hooks) {
	if (local_spawned_ && ctx.world.cached.local_player.valid()) {
		const world::EntityHandle local_h = ctx.world.cached.local_player;
		world::Entity *local = ctx.world.registry.get(local_h);
		world::AiEntity *local_ai =
				ctx.world.ai ? ctx.world.ai->for_handle(local_h) : nullptr;
		if (local != nullptr && local_ai != nullptr) {
			// ClientRuntime latches authoritative spawn closed on a decoded zero
			// tail, even if a later packet in this recv pump carries stale positive
			// HP. The independent C2S 0x0E gameplay hold never forces health to zero.
			const int16_t health = ctx.runtime.authoritative_spawn_released()
					? ctx.runtime.state().local_health : 0;
			if (health <= 0) {
				local->health = health;
				local_ai->health = health;
				local->alive = false;
				local->flags |= 2u;
			} else if (redeploy_release_pending_ &&
					ctx.runtime.state().health_updates_applied >
							redeploy_health_updates_at_release_) {
				// The recipient's own compact is deliberately priority-boosted
				// by the host. Use its redeployed pose, not the corpse pose,
				// before movement/uplink resumes.
				const netsim::ClientEntityState *self =
						client_entity_for_handle(
								ctx.runtime.state(), ctx.runtime.self_handle());
				if (self != nullptr && self->seen_this_frame) {
					const int32_t heading =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->yaw_byte) << 24);
					const int32_t pitch =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->pitch_byte) << 24);
					local->position = {
							static_cast<float>(
									static_cast<double>(self->x) / kFixed16),
							static_cast<float>(
									static_cast<double>(self->y) / kFixed16),
							static_cast<float>(
									static_cast<double>(self->z) / kFixed16),
					};
					local->yaw = static_cast<int16_t>(std::lround(
							world::mission_yaw_deg_from_bam_heading(heading)));
					local->pitch = static_cast<int16_t>(std::lround(
							static_cast<double>(pitch) * world::kDegreesPerBam));
					local->roll = 0;
					local->health = health;
					local->alive = true;
					local->hidden = false;
					local->flags &= ~1u;
					local->death_anim_state = 0;
					local->corpse_timer = 0;
					local->net_move_input = 0;
					local->net_analog_x = 0;
					local->net_analog_y = 0;
					local->net_analog_z = 0;
					world::entity_reset_to_spawn_state(*local);

					local_ai->pos[0] = self->x;
					local_ai->pos[1] = self->y;
					local_ai->pos[2] = self->z;
					local_ai->heading = heading;
					local_ai->pitch = pitch;
					local_ai->roll = 0;
					local_ai->body_pitch = 0;
					local_ai->health = health;
					local_ai->vel_x = 0;
					local_ai->vel_z = 0;
					local_ai->net_smooth_target[0] = self->x;
					local_ai->net_smooth_target[1] = self->y;
					local_ai->net_smooth_target[2] = self->z;
					local_ai->net_smooth_heading = heading;
					local_ai->net_smooth_pitch = pitch;
					local_ai->net_interp_progress = 0;
					local_ai->net_interp_steps = 0;
					local_ai->collide_state = {};

					world::InfantryState &inf = local_ai->inf;
					inf.active = true;
					inf.is_local_player = true;
					// The shared spawn/revive reset (InfantryState::
					// reset_for_spawn): one field list with the host spawn
					// seeding, so a NEW motor field re-defaults here without
					// a second hand-list to forget.
					inf.reset_for_spawn(heading);

					ctx.weapon.fire_held = false;
					ctx.weapon.fire_pressed = false;
					ctx.weapon.reload_pressed = false;
					// The embedder clears its device-input latches, seeds the
					// look heading, and rebuilds the respawn loadout.
					hooks.on_local_player_redeployed(heading);
					redeploy_release_pending_ = false;
					redeploy_health_updates_at_release_ = 0;
				}
			} else if (local->alive && (local->flags & 2u) == 0u) {
				local->health = health;
				local_ai->health = health;
			}
		}
	}
}

// Project the requester-local decoded 0x0A mount relationship onto L only
// after the host confirms C2S 0x26/0x27.
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180]
void JoinerWorldBridge::sync_authoritative_mount(
		const PumpContext &ctx, const PumpHooks &hooks) {
	if (!ctx.runtime.has_self_handle() || !local_spawned_ ||
			!ctx.world.cached.local_player.valid())
		return;
	const netsim::ClientEntityState *self =
			client_entity_for_handle(ctx.runtime.state(), ctx.runtime.self_handle());
	if (self == nullptr) return;
	world::Entity *local = ctx.world.registry.get(ctx.world.cached.local_player);
	if (local == nullptr) return;

	const bool wire_mounted =
			self->carrier_handle != world::EntityHandle::kInvalid &&
			self->mount_bone != 0 &&
			replica_world_entity(ctx.world, world::EntityHandle{
					self->carrier_handle}) != nullptr;
	bool changed = false;
	if (!wire_mounted) {
		if (local->mounted)
			changed = world::entity_detach_from_vehicle(ctx.world, local->handle);
	} else if (!local->mounted ||
			local->mount_target.packed != self->carrier_handle ||
			local->mount_bone != self->mount_bone) {
		// Wire-materialized mission entities retain the host's exact packed
		// pool/slot identity in the joiner's native world. The
		// server has already validated this exact carrier+bone pair.
		changed = world::entity_process_vehicle_attach(
				ctx.world, local->handle,
				world::EntityHandle{self->carrier_handle},
				self->mount_bone);
	}

	// A designated-G EWeap uses the compact player's seat_type as an
	// authoritative selected-MountSlot echo: 1 = child slot, 2 = the validated
	// groundEntity vehicle slot. Apply this even when the carrier+bone relation
	// is unchanged; action 6 commonly changes only this byte.
	// [orig: client player compact apply @0x4c1353 route legs]
	bool route_changed = false;
	if (wire_mounted && (self->seat_type == 1u || self->seat_type == 2u)) {
		world::Entity *mount = replica_world_entity(
				ctx.world, world::EntityHandle{self->carrier_handle});
		if (mount != nullptr) {
			// Retail does not blindly trust the authority byte: both route legs first
			// require Entity_IsMountableGun (non-type-1 EWeap) and the authored G
			// capability. Only the type-2 leg adds the groundEntity parent check.
			// [orig: compact apply @0x4c1353; Entity_IsMountableGun @0x434240]
			bool route_valid = mount->has_item_def && mount->item_type != 1u &&
					(mount->item_attrib & world::kItemAttribEweap) != 0u &&
					(mount->emplacement_attachment_flags & 0x02u) != 0u &&
					world::vehicle_prepare_weapon_slot(ctx.world, *mount);
			uint8_t equipped_adm = mount->primary_weapon_slot_adm;
			if (route_valid && self->seat_type == 2u && mount->ground_target.valid() &&
					mount->emplacement_parent == mount->ground_target &&
					mount->emplacement_parent_spawn_id != 0) {
				world::Entity *parent =
						replica_world_entity(ctx.world, mount->ground_target);
				route_valid = parent != nullptr &&
						parent->registry_spawn_id ==
								mount->emplacement_parent_spawn_id &&
						parent->has_item_def && parent->item_type == 1u &&
						(parent->item_attrib & world::kItemAttribEweap) != 0u &&
						world::vehicle_prepare_weapon_slot(ctx.world, *parent);
				if (route_valid)
					equipped_adm = parent->primary_weapon_slot_adm;
			} else if (self->seat_type == 2u) {
				route_valid = false;
			}
			if (route_valid) {
				const bool select_parent = self->seat_type == 2u;
				route_changed =
						mount->primary_weapon_slot.redirect_to_parent_slot !=
								select_parent ||
						local->equipped_adm_index != equipped_adm;
				mount->primary_weapon_slot.redirect_to_parent_slot =
						select_parent;
				local->equipped_adm_index = equipped_adm;
			}
		}
	}
	if (!changed && !route_changed) return;
	if (changed) {
		// The embedder refreshes its view effects (binocular latch) and the
		// mounted input heading for the new relation.
		hooks.on_mount_changed();
	}
	world::sync_local_usegun_weapon_transition(ctx.world, ctx.weapon);
}

// Fold the latest complete phase-8 mounted-ammo sample into the exact
// MountSlot selected by retail's live route bit. Missing wire rows remain
// pending until materialization; invalid classes are consumed and ignored.
void JoinerWorldBridge::apply_mounted_ammo_update(const PumpContext &ctx) {
	const netsim::ClientMountedAmmoState &ammo =
			ctx.runtime.state().mounted_ammo;
	if (!ammo.present || ammo.revision == mounted_ammo_revision_seen_)
		return;

	// The on-foot sentinel is a complete phase-8 observation, but it has no
	// MountSlot to mutate. Consume it so repeated render frames cannot replay it.
	if (!ammo.has_mount ||
			ammo.mount_handle == world::EntityHandle::kInvalid) {
		mounted_ammo_revision_seen_ = ammo.revision;
		return;
	}

	const world::EntityHandle handle{ammo.mount_handle};
	world::Entity *mount = wire_header_world_
			? materializer_.owned(ctx.world, handle)
			: ctx.world.registry.get(handle);
	// A streamed row can arrive in a later initial-state page. Unlike an invalid
	// item class, absence is not a verdict: retain this revision and retry after
	// the materializer has populated the exact packed slot.
	if (mount == nullptr) return;

	world::WeaponSlotState *slot =
			world::resolve_mounted_ammo_slot(ctx.world, *mount);
	if (slot != nullptr) {
		// Retail stores both words in signed 16-bit MountSlot fields. Preserve the
		// 0xffff sentinel as -1 rather than widening it to 65535.
		slot->clip = world::retail_signed_i16(ammo.clip);
		slot->reserve = world::retail_signed_i16(ammo.reserve);
	}
	// A live row with the wrong ItemDef/type/route is the retail
	// consume-and-ignore branch. Do not let one malformed class block later
	// phase-8 revisions forever.
	mounted_ammo_revision_seen_ = ammo.revision;
}

// Mirror the decoded wire positions of mission entities (pools 1-3) back onto
// the joiner's exact-handle materialized registry rows. The local sim is NOT authoritative
// for any of them — this write-back exists so position CONSUMERS of the local
// world stay truthful on a joiner: the occlusion frame evaluates entity
// visibility from registry positions (a driven-off vehicle must occlude at its
// live position, not its spawn point), and the collision tick tables pick up the
// same fix. Type-guarded against the row materialized at that exact streamed
// identity; synthetic children (spawn_origin sentinel) are skipped except
// for rows deliberately materialized from a true wire-header mission. Those rows
// also mirror orientation/team because collision and seat consumers observe them.
void JoinerWorldBridge::mirror_mission_entities(const PumpContext &ctx) {
	for (netsim::ClientEntityState &es : ctx.runtime.state().entities) {
		const world::EntityHandle h{es.handle};
		const int pool = h.pool();
		if (pool < 1 || pool > 3) continue;
		world::Entity *local = replica_world_entity(ctx.world, h);
		if (local == nullptr ||
				static_cast<uint16_t>(local->item_id) != es.type_id)
			continue;
		// Pools 1..3 are live client rows, not just presentation records. Keep
		// every world-side collision/seat consumer on the same full wire pose.
		const world::Entity *attachment_parent =
				local->emplacement_parent.valid()
				? ctx.world.registry.get(local->emplacement_parent)
				: nullptr;
		const bool unresolved_persistent_attachment =
				es.parent_pose_valid && es.parent_handle != 0xFFFFu &&
				!local->emplacement_pose_metadata_resolved &&
				local->emplacement_parent.packed == es.parent_handle &&
				attachment_parent != nullptr &&
				attachment_parent->registry_spawn_id ==
						local->emplacement_parent_spawn_id;
		// A 0x0D zero heading is omitted on the wire after retail memset, so
		// heading_known remains false. Persistent parent recomposition still owns
		// an exact heading_bam; mirror it for unresolved children whose rigid wire
		// pose is the world-consumer source of truth.
		if (es.heading_known || unresolved_persistent_attachment)
			local->yaw = static_cast<int16_t>(std::lround(
					world::mission_yaw_deg_from_bam_heading(es.heading_bam)));
		local->pitch = static_cast<int16_t>(std::lround(
				double(es.pitch_bam) * world::kDegreesPerBam));
		local->roll = static_cast<int16_t>(std::lround(
				double(es.roll_bam) * world::kDegreesPerBam));
		if (es.team_known) local->team = es.team;
		// Watercraft prediction (§5.38e B-facet, D-NET-196): a placed cbot
		// vehicle's motion is owned by the WORLD-side client mover — stage the
		// latest wire sample + registers on a fresh compact and let
		// watercraft_client_tick chase + predict; the row is mirrored BACK
		// after the tick (mirror_predicted_vehicles).
		if (pool == 1 && es.cls == EntityClass::Vehicle) {
			const world::VehicleTraits *traits =
					ctx.world.vehicle_traits.get(local->item_id);
			if (traits != nullptr) {
				es.net_world_mover = true;
				// The row-side fallback chase selects its constant set by
				// family; only the sim resolves families (client_state.h note).
				es.net_air_family = traits->family ==
								world::VehicleFamily::Helicopter ||
						traits->family == world::VehicleFamily::Plane;
				world::Entity::VehicleMotorState &m = local->veh;
				// The witnessed mover freezes: wire bit0 (not-ready/attached),
				// the dead-pose/wreck bit, and a carried row riding a deck
				// carrier all stop the prediction motor — the row keeps its
				// snapped wire pose (wreck eulers included) / its per-tick
				// seat-follow, and the mirror-back below yields via
				// net_predicted [orig: the Flags&1 early return @0x4b9a03; the
				// dead-pose short form's frozen live stores @0x460930..0x460A50].
				// D-NET-66: death stays a snap.
				const bool wire_frozen =
						(es.state_flags_known &&
								(es.state_flags &
										(0x01u | netsim::kVehicleFlagDeadPose)) !=
										0u) ||
						(es.net_seat_valid && es.carrier_handle != 0xFFFFu);
				if (wire_frozen) {
					// The row holds its snapped/followed pose; the registry
					// entity adopts it below like any un-predicted row so
					// occlusion/collision see the wreck where it rests. The
					// platform/motor transients die with the freeze so a
					// respawned or re-staged hull never inherits stale bob
					// phase, accumulators, or vertical velocity.
					m.net_predicted = false;
					m.net_seen_revision = es.compact_revision;
					// The motor's speed registers are transients too: the
					// client pass still runs the movement-sound tail for a
					// frozen row [world/ai_system.cpp fallback branch ->
					// update_ground_vehicle_sound], which reads veh.speed
					// directly, so a row frozen mid-motion would keep playing
					// its moving engine lane (and hold a reverse latch) on
					// state nothing advances any more.
					m.speed = 0;
					m.speed_accel = 0;
					m.cmd_speed = 0;
					m.slide_z = 0;
					m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] =
							m.plat_acc[3] = 0;
					m.plat_at_rest = false;
					m.plat_porpoise = false;
					m.plat_planing = false;
					m.plat_bob_phase = 0.0f;
					local->position.x = static_cast<float>(es.x) / 65536.0f;
					local->position.y = static_cast<float>(es.y) / 65536.0f;
					local->position.z = static_cast<float>(es.z) / 65536.0f;
					continue;
				}
				if (es.compact_revision != m.net_seen_revision) {
					const bool prediction_arming = !m.net_predicted;
					m.net_seen_revision = es.compact_revision;
					// The fold live-snapped the row to the wire sample (rows
					// whose first compact landed before this flag flipped stage
					// their pre-compact pose for one record — self-corrected by
					// the next fold's live snap); that sample is the staged
					// target [orig: the mode-2 staging].
					m.net_smooth_target[0] = es.x;
					m.net_smooth_target[1] = es.y;
					m.net_smooth_target[2] = es.z;
					m.net_smooth_heading = es.heading_bam;
					m.net_recv_speed = es.vehicle_speed_reg;
					m.net_recv_steer_bam = es.vehicle_steer_bam;
					m.net_recv_lat = es.vehicle_lat_reg;
					// The replicated engine/collective bit [orig: Flags 0x80,
					// air families].
					m.net_engine_on = (es.state_flags & 0x80u) != 0u;
					m.net_interp_progress = 0;
					// Live vehicle compacts omit Euler X/Y. Seed the client mover's
					// attitude exactly once when prediction arms from the retained
					// spawn/dead-pose row; re-seeding on every live compact would
					// erase the platform/aero solve performed between records.
					if (prediction_arming) {
						m.air_pitch_bam = es.pitch_bam;
						m.air_roll_bam = es.roll_bam;
						m.plat_solve_valid = false;
						// Promotion seam (no retail equivalent — retail's contact
						// solve runs from entity existence, so Flags 0x2000 is
						// always current): seed an arming Air-family row airborne
						// so its first predicted tick runs the aero block, not
						// the grounded sheds. The tick-tail solve reproduces the
						// true state one tick later either way.
						if (traits->family == world::VehicleFamily::Helicopter ||
								traits->family == world::VehicleFamily::Plane)
							local->flags |= world::kEntityFlagInAir;
					}
					m.net_predicted = true;
				}
				continue; // the world mover owns the registry position now
			}
		}
		local->position.x = static_cast<float>(es.x) / 65536.0f;
		local->position.y = static_cast<float>(es.y) / 65536.0f;
		local->position.z = static_cast<float>(es.z) / 65536.0f;
	}
}

// The world->view half of the watercraft prediction: after the tick, the
// predicted registry pose becomes the presented row pose (present reads the
// client replicas, ADR 0011). Heading gains the mover's full BAM precision.
void JoinerWorldBridge::mirror_predicted_vehicles(const PumpContext &ctx) {
	for (netsim::ClientEntityState &es : ctx.runtime.state().entities) {
		if (!es.net_world_mover) continue;
		const world::EntityHandle h{es.handle};
		world::Entity *local = replica_world_entity(ctx.world, h);
		if (local == nullptr || !local->veh.net_predicted) continue;
		es.x = world::to_fixed(local->position.x);
		es.y = world::to_fixed(local->position.y);
		es.z = world::to_fixed(local->position.z);
		es.heading_bam = local->veh.yaw_bam;
		local->yaw = static_cast<int16_t>(std::lround(
				world::mission_yaw_deg_from_bam_heading(local->veh.yaw_bam)));
		// Every family mover owns live attitude now: air/water solves plus the
		// ground/bike wheeled contact solve (Entity_ProcessTrackedVehiclePhysics
		// @0x47C1C0 client subset). Publish zero too: level is a real solved
		// pose, not a validity sentinel. A boxless ground row's motor never
		// advances the seeded wire attitude, so the mirror is stable there.
		const world::VehicleTraits *traits =
				ctx.world.vehicle_traits.get(local->item_id);
		const bool owns_attitude = traits != nullptr;
		if (owns_attitude) {
			es.pitch_bam = local->veh.air_pitch_bam;
			es.roll_bam = local->veh.air_roll_bam;
			// Mounted-pose and bone consumers read Entity pitch/roll, not the
			// motor registers. Keep the exact-handle materialized carrier coherent before
			// the post-mover attachment phase below.
			local->pitch = static_cast<int16_t>(std::lround(
					double(local->veh.air_pitch_bam) * world::kDegreesPerBam));
			local->roll = static_cast<int16_t>(std::lround(
					double(local->veh.air_roll_bam) * world::kDegreesPerBam));
		}
	}
}

// Project persistent decoded remote poses into collision-only visual
// proxies: Player/Infantry rows join the person walk, pool-1 movers carry
// their authored collision geometry at the decoded pose. Wire H remains
// presentation identity; local World authority never receives a cloned
// entity or an H->L owner mapping.
void JoinerWorldBridge::refresh_wire_collision_proxies(
		const PumpContext &ctx, const PumpHooks &hooks) {
	if (ctx.world.ai == nullptr || ctx.world.ai->collision == nullptr) return;
	std::vector<world::WirePersonCollisionProxy> person_proxies;
	std::vector<world::WireDynamicCollisionProxy> dynamic_proxies;
	uint16_t self_wire_handle = world::EntityHandle::kInvalid;
	if (ctx.runtime.has_self_handle())
		self_wire_handle = ctx.runtime.self_handle();
	if (ctx.runtime.in_match()) {
		for (const netsim::ClientEntityState &entity :
				ctx.runtime.state().entities) {
			if (entity.handle == world::EntityHandle::kInvalid ||
					(self_wire_handle != world::EntityHandle::kInvalid &&
					 entity.handle == self_wire_handle) ||
					(entity.state_flags_known &&
					 (entity.state_flags & 0x01u) != 0))
				continue;

			// Pool-0 organics (players AND non-player infantry) join the person
			// walk at the decoded position; retail's client walks its wire-built
			// pool 0 the same way [orig: Physics_RaycastAgainstProximityList
			// @ 0x4e4a30 over the client-built person table].
			if (entity.cls == EntityClass::Player ||
					entity.cls == EntityClass::Infantry) {
				const world::ResolvedCollisionShape shape =
						hooks.wire_collision_shape(entity.type_id);
				world::WirePersonCollisionProxy proxy;
				proxy.wire_handle = entity.handle;
				proxy.position_q16 = world::FixedVec3{
						entity.x, entity.y, entity.z};
				proxy.bound_radius_q16 = shape.bound_radius_q16;
				proxy.uniform_scale_q16 = shape.uniform_scale_q16;
				proxy.bbox_center_q16 = shape.bbox_center_q16;
				person_proxies.push_back(proxy);
				continue;
			}

			// Pool-1 movers (vehicles, emplacements, runtime items) project
			// their authored collision geometry at the decoded pose. Pool-2
			// statics keep colliding through the locally loaded mission set.
			if (wire_handle::pool(entity.handle) != wire_handle::kPoolItem)
				continue;
			const world::ResolvedCollisionShape shape =
					hooks.wire_collision_shape(entity.type_id);
			if (shape.model_id < 0 && shape.bound_radius_q16 == 0 &&
					!shape.pool1_candidate_source_eligible)
				continue;
			world::WireDynamicCollisionProxy proxy;
			proxy.wire_handle = entity.handle;
			proxy.model_id = shape.model_id;
			proxy.position_q16 = world::FixedVec3{
					entity.x, entity.y, entity.z};
			// The decoded pose mirrors the retail client entity fields: the compact
			// heading sample plus locally integrated sub-byte body motion, and retained
			// spawn/dead pitch/roll samples (entity+20/+24, live compacts omit
			// both for vehicles).
			proxy.heading_bam = entity.heading_bam;
			proxy.pitch_bam = entity.pitch_bam;
			proxy.roll_bam = entity.roll_bam;
			proxy.bound_radius_q16 = shape.bound_radius_q16;
			proxy.uniform_scale_q16 = shape.uniform_scale_q16;
			proxy.bbox_center_q16 = shape.bbox_center_q16;
			proxy.candidate_source_eligible =
					shape.pool1_candidate_source_eligible;
			const world::EntityHandle possible_twin{entity.handle};
			const world::Entity *twin = ctx.world.registry.get(possible_twin);
			if (twin != nullptr &&
					static_cast<uint16_t>(twin->item_id) == entity.type_id)
				proxy.registry_twin = possible_twin;
			dynamic_proxies.push_back(proxy);
		}
	}
	// ClientState is persistent and frame-budgeted; omission from one 0x0A is
	// not a despawn signal, so this intentionally does not read seen_this_frame.
	// Known-dead bit 1 is retained too: retail dead bodies remain person blockers
	// and a destroyed vehicle's shell keeps blocking (husk-model substitution for
	// wire proxies is a tracked residual).
	ctx.world.ai->collision->replace_wire_collision_proxies(
			std::move(person_proxies), std::move(dynamic_proxies),
			self_wire_handle);
}

// Drain typed S2C gameplay events after the client recv pump: tag-2 fires
// spawn visual-only rounds; the requester's 0x49 echo performs its refill.
void JoinerWorldBridge::apply_gameplay_events(const PumpContext &ctx) {
	// S2C 0x13 entity-death notifies: run the class death callback on the world
	// twin — retail's client zeroes Health and invokes deathCallback(entity, 4, 0),
	// which for a destructible item IS the local husk-swap + death-explosion
	// chain (the visual client's only live channel for another peer destroying a
	// static; the 0x10/0x20 load batches never re-stream after load). Pool-0
	// organics have no materialized world twin here — their death presentation
	// rides the compact dead bit — and destruction_notify_item_damage's own
	// gates keep AI-driven vehicles on their state-machine death path, exactly
	// like the authority side.
	// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — Health = 0 @0x42ebd6,
	//  cb(entity, 4, 0) @0x42ebf5; cb == Entity_HandleDestructibleDeathEvent
	//  @0x440210 for destructibles]
	for (const EntityDeathRecord &death : ctx.runtime.drain_entity_deaths()) {
		const world::EntityHandle handle{death.entity_handle};
		if (handle.pool() < 1 || handle.pool() > 3) continue;
		world::Entity *victim = materializer_.owned(ctx.world, handle);
		if (victim == nullptr) continue;
		victim->health = 0;
		victim->alive = false;
		victim->last_attacker = world::EntityHandle{};
		world::destruction_notify_item_damage(ctx.world, *victim, 4);
	}

	// Retail's S2C 0x0A tag-2 record is a fired-round descriptor. Re-run the
	// normal round spawner so tracers and physical impacts are produced locally;
	// World::run_logic_tick admits this pool only under the explicit
	// mp_session && !projectile_authority visual-client gate. Every descriptor
	// is spawned VisualOnly below, and that mode gates every gameplay consequence.
	for (const netsim::ClientRoundEvent &ev : ctx.runtime.drain_round_events()) {
		// The retail deserializer dispatches only the alt/projectile bit or the
		// standard adm-indexed bit [orig: @0x42f2a8]. Other flag shapes do not
		// enter RoundData_SpawnRound.
		if ((ev.flags & (kRoundEventFlagAltFire | kRoundEventFlagAdmIndexed)) == 0)
			continue;
		const world::WeaponTableEntry *adm =
				ctx.world.weapons.by_index(ev.adm_index);
		if (adm == nullptr || adm->ammo_index < 0) continue;
		world::RoundSpawnParams round;
		world::RoundSourceState source;
		round.owner = world::EntityHandle{};
		round.shooter_handle = ev.shooter_handle;
		// The mounted shooter's own vehicle joins the trace exclusion exactly
		// like retail's mount rule — see wire_carrier_exclusion_for.
		round.shooter_carrier_handle = wire_carrier_exclusion_for(
				ctx.runtime.state(), ev.shooter_handle, ctx.seat_specs);
		// Retail resolves the wire shooter entity and copies its TEAM into the
		// spawned round — the friend/enemy throwable item and tracer styling key
		// on it. Without this every remote grenade wore the enemy variant (and a
		// variant with no motor row froze mid-air). [orig: @0x4ec705]
		if (netsim::ClientEntityState *shooter_row =
					ctx.runtime.state().find(ev.shooter_handle)) {
			round.shooter_team = shooter_row->team;
			const int anim = shooter_row->anim_state_id;
			// The category follows the retail animation-flags table, not a
			// hand-maintained list of familiar locomotion clips. In particular,
			// 170/171 remain crouched, 172 is prone, and idle_mortar (46) is
			// neither. [orig: g_animStateFlagsTable @0x8139E8; category read in
			// RoundData_SpawnRound @0x4EC252..0x4EC27A]
			const uint32_t anim_flags = world::infantry_anim_flags(anim);
			const bool prone = (anim_flags & world::kAnimStanceFlagProne) != 0;
			const bool crouched =
					(anim_flags & world::kAnimStanceFlagCrouched) != 0;
			const bool swimming = anim == 36 || anim == 37 || anim == 154;
			const bool mounted =
					round.shooter_carrier_handle != wire_handle::kInvalid;
			// Retail tests eye Z (Position.Z + CameraOffset.Z) against the fixed
			// water plane [orig: RoundData_SpawnRound @0x4EC2DE..0x4EC2EA]. The
			// decoded row has no CameraOffset carrier, so raw fixed position Z is
			// the bounded projection; the swimming anim remains an independent
			// positive witness. Do not invent a standing-eye constant here.
			const bool below_water = ctx.world.env.water_z != 0 &&
					shooter_row->z < ctx.world.env.water_z;
			source.person_with_item_def =
					shooter_row->cls == EntityClass::Player ||
					shooter_row->cls == EntityClass::Infantry;
			source.player = shooter_row->cls == EntityClass::Player;
			source.underwater = swimming || below_water;
			// Mounted is the later retail override and therefore wins even while
			// below water; otherwise the underwater predicate forces standing row 2.
			source.stance_category = mounted ? 1 : (source.underwater ? 2 :
					(prone ? 0 : (crouched ? 1 : 2)));
			source.scope_raised =
					(shooter_row->state_flags &
					 world::kEntityFlagScopeRaised) != 0;
			source.recoil_pitch = &shooter_row->recoil_pitch;
			round.source_state = &source;
			// The adm-arm action sounds play at the SHOOTER's position, and a
			// decoded remote shooter has no local entity — supply its row
			// position for the sim's fire-sound leg (world/fire_sound.h).
			// [orig: entity+4 @ 0x4020ef; the pool resolve @ 0x42f491]
			round.shooter_pos = world::Vec3{
					static_cast<float>(shooter_row->x / kFixed16),
					static_cast<float>(shooter_row->y / kFixed16),
					static_cast<float>(shooter_row->z / kFixed16)};
			round.shooter_pos_valid = true;
		}
		round.origin.x = static_cast<float>(ev.origin_x) / kFixed16;
		round.origin.y = static_cast<float>(ev.origin_y) / kFixed16;
		round.origin.z = static_cast<float>(ev.origin_z) / kFixed16;
		round.dir_yaw_bam = ev.dir_yaw_bam;
		round.dir_pitch_bam = ev.dir_pitch_bam;
		round.ammo_index = adm->ammo_index;
		round.adm_index = ev.adm_index;
		round.shot_seq = ev.shot_seq;
		round.subtype = ev.subtype;
		round.charge = ev.slot_byte;
		// Carry the arm through so presentation can honour retail's split: the
		// adm-indexed arm spawns no ammo-def effect at the wire position (which is
		// the shooter's EYE — Position + CameraOffset), it executes the addressed
		// def's action rows at the weapon's own userpoint instead.
		// [orig: @0x42f521 / @0x42f6ce]
		round.wire_round_flags = static_cast<uint8_t>(ev.flags &
				(kRoundEventFlagAltFire | kRoundEventFlagAdmIndexed));
		ctx.world.round_sim.spawn(
				ctx.world, round, world::RoundConsequenceMode::VisualOnly);
	}

	for (const WeaponReload &reload :
			ctx.runtime.drain_reload_notifications()) {
		++ctx.weapon.reload_received_serial;
		ctx.weapon.reload_received_entity = reload.entity_handle;
		ctx.weapon.reload_received_param = reload.reload_param;
		// ClientRuntime already applied the remote-Person receive-handler stamp
		// before that frame's body tick. This drain retains diagnostics plus the
		// self-only WeaponSlot_ReloadAmmo branch below.
		if (!ctx.inventory_valid || !ctx.runtime.has_self_handle() ||
				reload.entity_handle != ctx.runtime.self_handle())
			continue;

		const int32_t combo = reload.reload_param;
		world::WeaponInventorySlot *reloaded = ctx.inventory.slot(combo);
		const world::WeaponTableEntry *def =
				(reloaded != nullptr && reloaded->adm_index >= 0)
						? ctx.world.weapons.by_index(
								static_cast<uint8_t>(reloaded->adm_index))
						: nullptr;
		if (reloaded == nullptr || def == nullptr) continue;

		// WeaponSlot_ReloadAmmo is run only on the echoed notification: refund
		// the payload-addressed slot's remaining clip to its ammo-class pool and
		// draw a full clip. A late echo still reloads that exact slot after a switch;
		// only the currently active personal slot has an FSM mirror to update here.
		world::weapon_inventory_reload_slot(
				ctx.world.weapons, ctx.inventory, combo);
		if (ctx.weapon.active && !ctx.weapon.usegun_slot_active &&
				ctx.inventory.equipped_combo >= 0) {
			world::WeaponSlotState &active_slot =
					*world::active_local_weapon_slot(ctx.world, ctx.weapon);
			const world::WeaponInventorySlot *equipped =
					ctx.inventory.slot(ctx.inventory.equipped_combo);
			const world::WeaponTableEntry *equipped_def =
					(equipped != nullptr && equipped->adm_index >= 0)
							? ctx.world.weapons.by_index(
									static_cast<uint8_t>(equipped->adm_index))
							: nullptr;
			if (equipped_def != nullptr) {
				active_slot.reserve = world::weapon_pool_get(
						ctx.inventory, equipped_def->ammo_class_id);
			}
			if (combo == ctx.inventory.equipped_combo) {
				active_slot.clip = reloaded->clip;
				active_slot.phase = static_cast<uint8_t>(
						active_slot.phase &
						~world::weapon_phase::kReloadPendingBit);
			}
		}
		++ctx.weapon.reload_applied_serial;
		world::AiEntity *player = ctx.world.ai
				? ctx.world.ai->for_handle(ctx.world.cached.local_player)
				: nullptr;
		if (player != nullptr && player->inf.active)
			player->inf.reload_anim_ticks = 80;
	}

	// Decoded shots above stamp the peer accumulator first; retail's client body
	// update then decays it in this same frame. Locally predicted fire is pumped
	// later, after the local World body tick, so it begins decaying next frame.
	ctx.runtime.tick_remote_recoil();
}

void JoinerWorldBridge::reset_for_join() {
	// A fresh ClientRuntime restarts the environment revision at 0: the
	// phase-2 cursor must not swallow the first sample of a rejoin.
	weather_revision_seen_ = 0;
	// The verbatim enable_join latch reset. self_team_revision_seen_ is
	// deliberately absent — the shipped binding never reset it on a fresh
	// join, and this move preserves behavior exactly (S10b owns any
	// correction).
	started_ = false;
	local_spawned_ = false;
	deployment_release_revision_seen_ = 0;
	mounted_ammo_revision_seen_ = 0;
	redeploy_release_pending_ = false;
	redeploy_health_updates_at_release_ = 0;
	self_wire_handle_ = 0;
	last_gap_depth_ = 0;
	last_frontier_seq_ = 0;
	last_records_applied_ = 0;
	last_outbound_seq_ = 0;
	diagnostic_sampled_ = false;
	flat_seconds_ = 0;
	freeze_suspected_ = false;
}

void JoinerWorldBridge::reset_materialization() {
	materializer_.clear();
	wire_world_topology_revision_seen_ = ~uint64_t{0};
	wire_world_stream_revision_seen_ = ~uint64_t{0};
}

void JoinerWorldBridge::reset_world_stream() {
	wire_header_world_ = false;
	wire_world_static_initialized_ = false;
	reset_materialization();
}

} // namespace opennova::np
