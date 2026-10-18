// The joiner role's frame: the phase sequence and every witnessed leg of the
// retail client frame, moved verbatim from the joiner world bridge (S10a, ADR
// 0028) with the shell's hook table folded into the role (ADR 0043 slice E8b).
// See joiner_role.h for the ownership split; every phase keeps its witnesses.
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/spectator_session.h>
#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/inmatch/client_weapon_replay.h>
#include <runtime/inmatch/game_config.h>

#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/entity_wire_bridge.h> // build_player_uplink (the C2S 0x0C body)

#include <net/npwire/ingame_decode.h>    // kRoundEventFlag* (the fire-mode byte)
#include <net/npwire/ingame_encode.h>    // the C2S 0x06 fired-round descriptor pose
#include <net/npwire/wire_handle.h>      // pool()/kPoolItem (the wire handle home)
#include <net/title/title_identity.h>    // TitleIdentity::game_id (the rules' title)

#include <base/io/fixed.h>               // kFp16OneD, fp16_16_to_float
#include <base/io/perf_clock.h>          // perf_now_us (the frame's phase clocks)

#include <runtime/mission/collision_resolve.h> // find_item_def (a rider's authored hp)
#include <runtime/mission/seat_spec_extract.h> // refresh_item_seat_spec (a streamed row's seats)
#include <runtime/wac/remote_command.h>        // run_remote_command (the S2C 0x23 handler body)
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>   // destruction_notify_item_damage (S2C 0x13 net kill)
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/local_player_view.h> // local_player_view_refresh (the mount-change edge)
#include <runtime/world/player_view.h>   // the fullscreen damage-feedback words + the shake arm
#include <runtime/world/player_weapon.h>
#include <runtime/world/powerup.h>       // powerup_pickup_by (a remote player's pickup)
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <runtime/world/weapon_table.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

JoinerRole::JoinerRole(KitSeams seams) : kit_seams(std::move(seams)) {}

JoinerRole::~JoinerRole() {
	if (kernel_ != nullptr && kernel_->world.vehicles.occupancy_source == this)
		kernel_->world.vehicles.occupancy_source = nullptr;
}

ClientRuntime &JoinerRole::create_runtime(const std::string &player_name, JoinRole join_role,
		const std::string &spectator_password, const std::string &server_password,
		const std::string &join_password) {
	player_name_ = player_name;
	join_role_ = join_role;
	spectator_password_ = spectator_password;
	server_password_ = server_password;
	join_password_ = join_password;
	runtime = std::make_unique<ClientRuntime>(player_name);
	runtime->set_profile(kernel_ != nullptr ? &kernel_->profile : nullptr);
	runtime->set_join_request(join_role, spectator_password, server_password, join_password);
	// The connect's peer: the 0x41's EIP / EPN on a dial, the 0x42's SIP / SPN.
	runtime->set_host_address(host_addr_);
	runtime->set_title(title_);
	return *runtime;
}

void JoinerRole::bind(mission::MissionKernel &kernel) {
	// The previous kernel may already be gone (a load swaps it before the
	// rebind); only the new one is touched.
	Role::bind(kernel);
	if (runtime) runtime->set_profile(&kernel.profile);
	// The +368 readers (the vehicle sound and part-spin gates) resolve remote
	// claimants through this role's decoded state.
	kernel.world.vehicles.occupancy_source = this;
}

// The boot hook's bring-up. A retail-style menu join has already
// authenticated and learned the map from S2C 0x7B before this local load:
// preserve that exact runtime/socket; rebuilding it here would silently
// reconnect and discard the witnessed pre-load session. Direct-loaded
// callers have not started yet and retain the historical fresh-runtime
// reset (the true return: the embedder re-installs its retained join
// inputs on the fresh runtime).
bool JoinerRole::bring_up() {
	const bool rebuild = !started_ || !runtime;
	if (rebuild) {
		create_runtime(player_name_, join_role_, spectator_password_, server_password_, join_password_);
		reset_for_runtime_rebuild();
	}
	runtime->set_world_ready(true);
	runtime->view().begin_mission();
	reset_for_load(runtime->deployment_release_revision());
	kernel_->local.loadout.pending_player_class = -1; // the embedder re-applies the kit after each load
	return rebuild;
}


// --- the role's own legs ---------------------------------------------------

void JoinerRole::set_socket(opennova::IDatagramSocket *socket, const PeerAddr &host) {
	socket_ = socket;
	host_addr_ = host;
}

// Ship one framed datagram to the dialed host; socketless = dropped.
void JoinerRole::send(const std::vector<uint8_t> &dg) {
	if (socket_ == nullptr || dg.empty()) return;
	socket_->send_to(host_addr_, dg.data(), dg.size());
}

// The install: node = inet_addr(NI) / atol(NP), cookie = atol(BK), relay =
// inet_addr(NK host) / the NK port. A dotted quad that does not parse is
// retail's INADDR_NONE (0) and leaves the proxy disabled.
// [orig: CNapiGameSession_InitTransportConnection @0x4ca051..0x4ca0c7]
void JoinerRole::set_join_proxy(const JoinProxyOptions &options) {
	ProxyRendezvousConfig proxy;
	proxy.node_addr = proxy_inet_addr(options.proxy_node_ip);
	proxy.node_port = options.proxy_node_port;
	proxy.cookie = options.proxy_cookie;
	proxy.relay_addr = proxy_inet_addr(options.proxy_relay_ip);
	proxy.relay_port = options.proxy_relay_port;
	set_join_proxy(proxy);
}

void JoinerRole::set_join_proxy(const ProxyRendezvousConfig &proxy) {
	proxy_ = proxy;
	// PeerAddr packs octets exactly like the inet_addr dword (first octet in the
	// low byte), so the node address is the same word.
	proxy_node_addr_ = PeerAddr{};
	proxy_node_addr_.ip = proxy_.node_addr;
	proxy_node_addr_.port = static_cast<uint16_t>(proxy_.node_port & 0xFFFFu);
	proxy_last_send_ms_ = 0;
}

// The enumerator pump's rendezvous leg: while the announce is outstanding (the
// hello not yet answered) and every proxy field is installed, one 48-byte
// datagram goes to the game node on the 3000 ms announce interval, ahead of
// the announce itself. The first pump sends at once (no last-send tick); the
// enumerator stops once a session answered, so a later phase sends nothing.
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 — the interval
//  @0x6290ed (3000 for a dialing host), the `!last_send || elapsed > interval`
//  arm @0x629119, the all-six gate ending @0x62914f, then
//  CNapiNPConnection_SendPingPacket @0x61F8C0 (the call @0x629159), then
//  NapiNPSession_SendAnnouncePacket @0x61FA00 (the call @0x629170); the
//  enumerator's active flag enum_info+44 is read as "the hello is still being
//  announced" here]
void JoinerRole::pump_proxy_rendezvous() {
	if (socket_ == nullptr || !runtime || !proxy_.enabled()) return;
	if (started_ && runtime->phase() != JoinerConnection::Phase::Hello) return;
	const uint64_t now_ms = io::perf_now_us() / 1000u;
	if (proxy_last_send_ms_ != 0 &&
			now_ms - proxy_last_send_ms_ <= kProxyRendezvousIntervalMs)
		return;
	proxy_last_send_ms_ = now_ms == 0 ? 1 : now_ms;
	const std::array<uint8_t, kProxyRendezvousDatagramSize> packet =
			build_proxy_rendezvous_datagram(proxy_);
	socket_->send_to(proxy_node_addr_, packet.data(), packet.size());
}

// Drain every datagram pending on the socket into the runtime's recv fold
// (the witnessed poll-before-logic order [orig: Game_ProcessMainFrame
// @0x5263f0]).
void JoinerRole::deposit_inbound() {
	if (socket_ == nullptr || !runtime) return;
	uint8_t buf[4096];
	for (;;) {
		PeerAddr from{};
		const int n = socket_->recv_from(buf, sizeof(buf), from);
		if (n <= 0) break;
		runtime->receive(buf, static_cast<std::size_t>(n));
	}
}

void JoinerRole::poll_preload() {
	if (!runtime) return;
	pump_proxy_rendezvous();
	send_hello_once();
	deposit_inbound();
	for (const std::vector<uint8_t> &dg : runtime->Client_ProcessNetworkFrame(now_tick_))
		send(dg);
	++now_tick_;
}

// Retail's seat readers all resolve the same client entity table. Our local
// body L is native and remote pool-0 bodies are decoded rows, so join the two
// only while answering a query (D-NET-157); never copy wire handles into a
// native organic or let a remote handle equal to L mark an own seat.
world::VehicleSeatOccupancy JoinerRole::seat_occupancy(
        const world::Entity &carrier, const world::Seat &seat,
        world::EntityHandle requester) const {
	if (!kernel_ || !runtime)
		return {};
	const world::Entity *local = kernel_->local.player();
	if (local != nullptr && local->mounted && local->mount_target == carrier.handle &&
			local->mount_bone == seat.bone_index) {
		return {true, local->handle == requester, local->health, local->health_max, true, true};
	}
	const auto &state = runtime->state();
	const replication::ClientEntityState *rider = nullptr;
	for (const auto &row : state.entities) {
		if (row.cls != EntityClass::Player && row.cls != EntityClass::Infantry) continue;
		if (runtime->has_self_handle() && row.handle == runtime->self_handle()) continue;
		if (row.state_flags_known && (row.state_flags & world::kEntityFlagDead) != 0) continue;
		if (row.carrier_handle == carrier.handle.packed && row.mount_bone == seat.bone_index) {
			rider = &row;
			break;
		}
	}
	if (rider == nullptr && seat.occupant.valid()) {
		rider = client_entity_for_handle(state, seat.occupant.packed);
		// The retained 0x0D slot seeds occupancy until that occupant's first
		// complete compact. A later dismount/swap supersedes the spawn image.
		if (rider != nullptr &&
				((rider->state_flags_known && (rider->state_flags & world::kEntityFlagDead) != 0) ||
				 (rider->net_has_compact &&
				  (rider->carrier_handle != carrier.handle.packed || rider->mount_bone != seat.bone_index))))
			return {};
		// An occupied raw slot still blocks selection, but an unresolved
		// client rider has no HUD marker [orig: driver rider-state gate @0x5A56F5].
		if (rider == nullptr) return {true, false, 0, 0, false};
		// The retained spawn slot names our wire H until sync_authoritative_mount
		// reconciles it to L; retail reads the one local entity either way.
		if (local != nullptr && runtime->has_self_handle() &&
				rider->handle == runtime->self_handle())
			return {true, local->handle == requester, local->health, local->health_max, true, true};
	}
	if (rider == nullptr) return {};

	world::VehicleSeatOccupancy result{true, false, 1, 1, true, rider->cls == EntityClass::Player};
	const def::DefItemsFile *items = kernel_->items_table();
	const def::DefItemDef *item = items != nullptr
			? mission::find_item_def(*items,
					  static_cast<int>(rider->type_id) + mission::kItemIdOffset)
			: nullptr;
	if (item == nullptr) return result;
	result.health = result.max_health = item->hp;
	if (rider->cls == EntityClass::Player && rider->net_has_compact) {
		// The compact's health/class byte reconstructs a tier midpoint, not
		// an exact health fraction (replication::replica_tier_health).
		result.health = replication::replica_tier_health(rider->health_class_byte, item->hp);
	}
	return result;
}

// The +368 claimant a retail client attaches from the decoded records: the remote
// rider on the carrier's control seat. The mover's per-tick validation keeps only a
// ctrlx/drvrx occupant on the control bone, so that seat relation alone reproduces
// the claim; the retained 0x0D slot seeds it until the rider's first compact, as in
// seat_occupancy. The row projects into a transient body for the +368 readers; the
// decoded row carries no eye sample (the roster tags' record residue).
// [orig: NetPacket_SerializePlayerState @0x4C1317 /
//  NetPacket_SerializeInfantryEntityState @0x4C0678 -> Entity_TryAttachOrDetach ->
//  Entity_ProcessVehicleAttach @0x435AA0 -> Entity_AttachToVehicleSlot stores
//  @0x4947D2 / @0x4948D8; validation Entity_UpdateTankVehiclePhysics
//  @0x489484..0x4894BE]
bool JoinerRole::remote_claimant(const world::Entity &carrier, world::Entity &out) const {
	if (!runtime) return false;
	const world::Seat *control = nullptr;
	for (const world::Seat &seat : carrier.seats) {
		if (world::is_vehicle_control_seat(seat.type)) {
			control = &seat;
			break;
		}
	}
	if (control == nullptr) return false;
	const auto &state = runtime->state();
	const auto is_self = [&](uint16_t handle) {
		return runtime->has_self_handle() && handle == runtime->self_handle();
	};
	const auto dead = [](const replication::ClientEntityState &row) {
		return row.state_flags_known && (row.state_flags & world::kEntityFlagDead) != 0;
	};
	const replication::ClientEntityState *rider = nullptr;
	for (const auto &row : state.entities) {
		if (row.cls != EntityClass::Player && row.cls != EntityClass::Infantry) continue;
		if (is_self(row.handle) || dead(row)) continue;
		if (row.carrier_handle == carrier.handle.packed &&
				row.mount_bone == control->bone_index) {
			rider = &row;
			break;
		}
	}
	if (rider == nullptr && control->occupant.valid() && !is_self(control->occupant.packed)) {
		const replication::ClientEntityState *seeded =
				client_entity_for_handle(state, control->occupant.packed);
		if (seeded != nullptr &&
				(seeded->cls == EntityClass::Player || seeded->cls == EntityClass::Infantry) &&
				!dead(*seeded) &&
				(!seeded->net_has_compact ||
						(seeded->carrier_handle == carrier.handle.packed &&
								seeded->mount_bone == control->bone_index)))
			rider = seeded;
	}
	if (rider == nullptr) return false;
	out = world::Entity{};
	out.handle = world::EntityHandle{rider->handle};
	out.item_id = rider->type_id;
	out.position = {opennova::io::fp16_16_to_float(rider->x),
			opennova::io::fp16_16_to_float(rider->y), opennova::io::fp16_16_to_float(rider->z)};
	out.team = rider->team_known ? rider->team : 0;
	out.flags = rider->state_flags;
	if (rider->cls == EntityClass::Player) {
		out.engine_flags |= world::kEntityFlagPlayer;
		out.player_class = rider->spawn_player_class;
	}
	out.mounted = true;
	out.mount_target = carrier.handle;
	out.mount_type = control->type;
	out.mount_bone = control->bone_index;
	return true;
}

void JoinerRole::collect_hostile_mounts(const world::Entity &requester,
        std::vector<world::EntityHandle> &out) const {
	if (!runtime) return;
	// The same live enemy parent-link walk as the native pool-0 scan.
	// [orig: Vehicle_HasEnemyOccupant @0x4359F0..0x435A5F]
	for (const auto &row : runtime->state().entities) {
		if (row.cls != EntityClass::Player && row.cls != EntityClass::Infantry) continue;
		if (runtime->has_self_handle() && row.handle == runtime->self_handle()) continue;
		if (!row.team_known || row.team == requester.team || row.mount_bone == 0 ||
				row.carrier_handle == world::EntityHandle::kInvalid ||
				(row.state_flags_known && (row.state_flags & world::kEntityFlagDead) != 0)) continue;
		out.push_back(world::EntityHandle{row.carrier_handle});
	}
}

// The non-authority use-item action queues the request and waits for the
// compact relationship echo before changing the local body's relation; an
// attach request turns the body to the seat first.
// [orig: Entity_ToggleVehicleMount @0x436950;
// Entity_RequestVehicleAttach @0x4364A0; Entity_SendDetachPacket @0x435510]
bool JoinerRole::queue_mount_toggle() {
	if (!kernel_ || !runtime) return false;
	world::World &world = kernel_->world;
	const world::Entity *player = kernel_->local.player();
	if (player == nullptr || !player->alive || player->health <= 0) return false;
	if (player->mounted && world.script.wac_values.seatbelt != 0)
		return false;
	world::VehicleSeatSelection hit;
	// Mounted Use first scans for an alternative seat, then falls back to
	// detach. Both relations wait for the host echo; only the attach request's
	// yaw leg edits L here. [orig: Entity_ToggleVehicleMount @0x4369AC..0x4369C7]
	if (!world.vehicles.find_mount_toggle_candidate(*player, hit, this))
		return player->mounted &&
				runtime->queue_vehicle_detach(player->mount_target.packed);
	const world::Entity *vehicle = world.registry.get(hit.vehicle);
	if (vehicle == nullptr || hit.seat_index < 0 ||
			hit.seat_index >= static_cast<int>(vehicle->seats.size())) return false;
	const uint8_t bone = vehicle->seats[static_cast<size_t>(hit.seat_index)].bone_index;
	// The request turns L to the seat before it queues the 0x26; the host's
	// confirm writes no yaw. [orig: Entity_RequestVehicleAttach @0x43655F..0x4365CE
	//  ahead of the queue @0x436602]
	world.vehicles.request_attach_heading(player->handle, hit);
	return runtime->queue_vehicle_attach(hit.vehicle.packed, bone);
}

// [orig: Entity_FindAvailableSeat @0x436790 -> Entity_RequestVehicleAttach @0x436602]
bool JoinerRole::queue_numbered_seat(int index) {
	if (!kernel_ || !runtime) return false;
	world::VehicleSeatSelection selected;
	if (!kernel_->local.find_numbered_seat(index, selected, this)) return false;
	const world::Entity *carrier = kernel_->world.registry.get(selected.vehicle);
	if (carrier == nullptr) return false;
	const uint8_t bone = carrier->seats[static_cast<size_t>(selected.seat_index)].bone_index;
	kernel_->world.vehicles.request_attach_heading(kernel_->world.cached.local_player, selected);
	return runtime->queue_vehicle_attach(selected.vehicle.packed, bone);
}

bool JoinerRole::queue_stance_change(uint16_t action_id) {
	return runtime && runtime->queue_stance_change(action_id);
}

bool JoinerRole::request_stance(int stance) {
	if (!kernel_ || stance < 0 || stance > 2) return false;
	if (!kernel_->local.stance_request_allowed()) return false;
	return queue_stance_change(world::LocalPlayer::kStanceActionIds[stance]);
}

// Stamp each decoded Player/Infantry row's .adm registry id from its wire
// type once per row; -1 = no adm (the row stays chase-only, truthful). The
// kernel resolves and caches per type (adm_id_for_runtime_type).
// The row-side root-motion leg shares the authority's per-model registry
// (net-re section 5.38e), the AnimMap_RegisterEntity spawn half [orig: @0x40bb60].
void JoinerRole::resolve_row_adm_ids() {
	mission::MissionKernel &kernel = *kernel_;
	if (!runtime) return;
	const bool rearm = infantry_adm_revision_seen_ != kernel.infantry_adm_revision();
	for (replication::ClientEntityState &es : runtime->state().entities) {
		if (rearm) {
			es.rm_adm_id = -2;
			es.rm_state = -1;
			es.rm_leg_seeded = false;
			es.wpn_state = es.wpn_playing = -1;
			es.wpn_deferred = 0;
		}
		if (es.rm_adm_id != -2) continue;
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry) continue;
		if (es.type_id == 0) continue;
		const int adm_id = kernel.adm_id_for_runtime_type(es.type_id);
		if (adm_id < -1) continue; // the sources are not armed yet: leave the row pending
		es.rm_adm_id = static_cast<int16_t>(adm_id);
	}
	infantry_adm_revision_seen_ = kernel.infantry_adm_revision();
	// The first decoded row may have registered the first usable model map.
	runtime->view().set_root_motion_source(kernel.root_motion.empty() ? nullptr : &kernel.root_motion);
	// The rows' secondary selection reads the held record's hold kind.
	runtime->view().set_weapon_table(&kernel.world.tables.weapons);
}

// A streamed topology/world change landed in the registry: retire the
// changed rows' collision/occlusion instances and caches, refresh traits +
// seat specs, then re-fold the retained 0x0D mountHandles image through the
// materializer.
void JoinerRole::on_replica_world_changed(const replication::ClientWorldSyncResult &sync) {
	mission::MissionKernel &kernel = *kernel_;
	for (const world::EntityLifetime lifetime : sync.retired)
		kernel.retire_replica_entity(lifetime);
	// Zone/deploy lookup is a registry-derived cache. Any streamed topology
	// change can add/remove an eligible zone and must invalidate it before UI
	// or a C2S 0x0E pick consults the world.
	++world_sync_serial_;
	kernel.resweep_item_traits();
	auto refresh_seats = [&](const std::vector<world::EntityLifetime> &rows) {
		for (const world::EntityLifetime lifetime : rows) {
			if (!mission::pool_takes_item_seat_spec(lifetime.handle.pool())) continue;
			if (world::Entity *entity = kernel.world.registry.get(lifetime))
				mission::refresh_item_seat_spec(kernel.world, kernel.seat_specs, *entity,
						kernel.wire_header_world);
		}
	};
	refresh_seats(sync.spawned);
	refresh_seats(sync.updated);
	// The wire materializer deliberately does not fabricate seat
	// definitions. Item/model resolution above installs them, then this
	// idempotent fold projects the retained 0x0D mountHandles image by each
	// seat's fixed retail_slot.
	materializer_.set_local_player(self_wire_handle(), kernel.world.cached.local_player);
	(void)materializer_.sync(runtime->state(), kernel.world);
	kernel.refresh_collision_instances();
}

// The authoritative mount relation changed: the view effects refresh and the
// mounted input heading follows the relation. The binoculars toggle and its
// sway ride through; the attach's own camera reset clears the toggle where
// retail's does (attach_apply) [orig: Player_ResetCameraAndMovementState
// @0x4DE2B3; the sway's writer Binoculars_RandomizeSwayOffsets @0x4dd830].
void JoinerRole::on_mount_changed() {
	mission::MissionKernel &kernel = *kernel_;
	world::LocalPlayer &lp = kernel.local;
	world::local_player_view_refresh(&kernel.world, lp.view);
	lp.sync_local_mounted_input_heading();
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick —
// the world-side pump body with the joiner's two wire request records.
// [orig: WeaponAction_ProcessAllEntities @0x542690 pumps every pooled entity]
void JoinerRole::tick_local_weapon() {
	mission::MissionKernel &kernel = *kernel_;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	world::LocalWeaponPumpIO io;
	io.view = &lp.view;
	io.inventory = lp.inventory_valid ? &lp.inventory : nullptr;
	io.is_authority = false;
	io.self_wire_handle = self_wire_handle();
	// The joiner's OWN predicted round runs the wire-proxy walk with the
	// local mount exclusion dead, so resolve the carrier gate from the self
	// wire row like any decoded remote round — otherwise a mounted joiner's
	// fire stops on its own vehicle's proxy.
	io.carrier_exclusion = [this]() {
		return wire_carrier_exclusion_for(runtime->state(), self_wire_handle(),
				kernel_->seat_specs);
	};
	world::local_weapon_pump_tick(kernel.world, lp.weapon, io);
	lp.hud_map_control.weapon_command(io.map_command);
	std::vector<world::LocalWeaponFiredWire> fires;
	if (io.fired.valid)
		fires.push_back(io.fired);
	// Countermeasures use the same sequence and descriptor path as the local
	// weapon. Their launch effects already ran at the vehicle input boundary.
	// The SHOOTER is the vehicle's occupantEntity — the pilot — not the
	// vehicle: Weapon_FireProcess passes (aim, occupant, aiRuntime[3], 1,
	// ammoDefIndex, 0) [orig: Weapon_FireProcess @0x53f6d6 -> @0x53f70a ->
	// Entity_FireWeaponAndSendPacket @0x42BD80].
	for (auto &params : kernel.world.out.source_fires) {
		const world::AiEntity *pilot = kernel.world.ai.for_handle(params.owner);
		if (pilot == nullptr)
			continue;
		params.shot_seq = ++lp.weapon.round_sequence;
		params.shooter_handle = self_wire_handle();
		params.shooter_carrier_handle = io.carrier_exclusion();
		kernel.world.round_sim.spawn(kernel.world, params, world::RoundConsequenceMode::VisualOnly);
		world::LocalWeaponFiredWire fired;
		fired.valid = true;
		fired.shot_seq = params.shot_seq;
		// The descriptor's off32 is the PILOT's entity+0x160 — his handheld
		// ammo-def index, the same source the on-foot pump reads (the equipped
		// adm's ammo row) — NOT the flare's: the writer's flags byte is
		// *(WORD*)(shooterEntity+352) [orig: @0x42c01a..0x42c052 -> off32
		// @0x42a7da]. The flare's ammo-def index travels only as the adm byte
		// (weaponFlags -> off7 @0x42a69b) under targetId 1 (off6 @0x42a68a,
		// the ammo-def arm).
		const world::Entity *shooter = kernel.world.registry.get(params.owner);
		const world::WeaponTableEntry *handheld = shooter != nullptr
				? kernel.world.tables.weapons.by_index(shooter->equipped_adm_index)
				: nullptr;
		fired.ammo_index = handheld != nullptr ? handheld->ammo_index : -1;
		fired.adm_index = params.adm_index;
		fired.round.origin_x = world::to_fixed(params.origin.x);
		fired.round.origin_y = world::to_fixed(params.origin.y);
		fired.round.origin_z = world::to_fixed(params.origin.z);
		fired.round.dir_yaw = params.dir_yaw_bam;
		fired.round.dir_pitch = params.dir_pitch_bam;
		fired.round.mode_flags = 1;
		// off33 is fire_flags = Weapon_GetScopeZoomLevel(can_fire, 12) |
		// (can_fire ? 0x80 : 0) for the local shooter [orig: @0x42bdd6..
		// 0x42bdf9]; a seated pilot (parentSlot 2/5) fails Player_IsOpticalViewVisible
		// [orig: @0x5cf7a8..0x5cf7b6], and with weaponActive 0 the zoom helper
		// returns its default 12 [orig: Weapon_GetScopeZoomLevel @0x422fd1/
		// @0x422fd5]. The on-foot pump stamps the same 12 on its own witnessed leg.
		fired.round.subtype = 12;
		for (int axis = 0; axis < 3; ++axis)
			fired.shooter_pose[axis] = pilot->pos[axis];
		fired.shooter_pose[3] = pilot->heading;
		fired.shooter_pose[4] = pilot->pitch;
		fires.push_back(fired);
	}
	kernel.world.out.source_fires.clear();
	for (const auto &fired : fires) {
		// The client-side half of Entity_FireWeaponAndSendPacket: the pump
		// predicted the round; queue the fixed C2S 0x06 descriptor. The pose
		// helper writes full XYZ, rounded Yaw/Pitch high words, and retail's
		// five low-word deltas against the live shooter pose. The runtime
		// stamps its own currentTick when accepting it.
		// [orig: @0x42A62F/@0x42A6A1..0x42A890]
		opennova::ClientFiredRound fire;
		fire.shooter_handle = self_wire_handle();
		fire.fire_flags = fired.round.mode_flags;
		fire.adm_index = fired.adm_index;
		fire.target_handle = fired.target_handle;
		// hit_part is NOT a bare sequence: it is
		// (own roster slot << 9) | (shot_seq & 0x1FF). The host copies the
		// raw word straight into the GLOBAL word_B7C670 on the network arm
		// [orig: Server_ClientFiredRound @0x50BAA0 @0x50c2ba / @0x50c774],
		// and its own composition of the same word packs the shooter's
		// per-player record slot+20 into bits 9.. exactly this way
		// [orig: @0x50bda5 `(*((WORD*)v91 + 10) << 9) | (packet & 0x1FF)`].
		// Sending a bare sequence leaves those bits ZERO, i.e. roster slot
		// 0, so every round we fired claimed the same owner. Witnessed on
		// the wire: a retail joiner at mySlot=1 sends
		// 0x0201/0x0202/0x0203 where we sent 0x0001/0x0002/0x0003
		// (.scratch/golden/retail-coop-playerinfo-join.pcapng vs
		// opennova-joiner-profile-kit-verified.pcapng).
		// SCOPE, corrected 2026-07-26: the packing above is ROUND ATTRIBUTION
		// only. word_B7C670's entire causal reach is the round record's net id
		// [orig: CEntityManager_AllocateSlot @0x4EAAE6 adopt-or-mint, @0x4EABD5
		// stores it at the round record's +120]. An earlier revision of this
		// comment ALSO blamed it for a retail host's own first-person weapon
		// reacting to our shots; that was WRONG, and the symptom survived this
		// fix. The actual mechanism is D-NET-184 and is not packet-driven.
		fire.hit_part = opennova::pack_fired_round_hit_part(rt.local_player_slot(), fired.shot_seq);
		// entity+0x160 — the shooter's current AMMO-DEFINITION index, a u16 index
		// into g_AmmoDefTable (stride 276). The host stores it onto the remote
		// shooter's entity [orig: the send-side read Entity_FireWeaponAndSendPacket
		// @0x42C01A; the equip-time source WeaponSlot_InitFromEntityDef @0x54673B
		// copies admEntry[1]'s low word; retail seeds 3 beside the WPN_M4AUTO
		// default in PlayerClass_InitEntity @0x4B1105, which is why a retail
		// client was captured sending 0x03]. Only the LOW BYTE crosses the wire —
		// the writer's parameter is a char @0x42a7da and the receiver reads one
		// byte @0x51347d — so indices >= 256 are untransmittable by design.
		// We shipped 0 here until 2026-07-26 (D-WPN-8).
		fire.extra_byte1 =
				fired.ammo_index >= 0 ? static_cast<uint8_t>(fired.ammo_index) : uint8_t(0);
		fire.extra_byte2 = fired.round.subtype;
		fire.misc_byte = fired.charge;
		const std::array<int32_t, 5> fire_pose = {
			fired.round.origin_x,
			fired.round.origin_y,
			fired.round.origin_z,
			fired.round.dir_yaw,
			fired.round.dir_pitch,
		};
		const std::array<int32_t, 5> shooter_pose = {
			fired.shooter_pose[0],
			fired.shooter_pose[1],
			fired.shooter_pose[2],
			fired.shooter_pose[3],
			fired.shooter_pose[4],
		};
		opennova::set_client_fired_round_pose(fire, fire_pose, shooter_pose);
		rt.queue_fired_round(fire);
	}
	if (io.reload.valid) {
		opennova::WeaponReload reload;
		reload.entity_handle = io.reload.entity_handle;
		reload.reload_param = io.reload.reload_param;
		rt.queue_reload_request(reload);
	}
}

// An S2C 0x41 or 0x42 applied inside the frame mutated the live charattr
// table; the World's per-class ATTRIBUTES words follow it the same frame
// [orig: the HUD reads g_CharAttr directly, CharAttr_ClassHasAttribute
// @0x4125e0, its ATTRIBUTES latch among its gates, so the change is visible
// on the next draw; see docs/interface/hud-re.md]. A failed/missing
// charattr.def leaves the all-zero table -- no class carries an attribute,
// retail's failed-load state [orig: CharAttr_LoadFromDef @0x412140 memsets
// 0x7C0 bytes first].
void JoinerRole::sync_class_attribute_flags() {
	const CharAttrTable *live = runtime ? runtime->charattr_table() : nullptr;
	static const CharAttrTable kNoTable{};
	kernel_->world.tables.class_attribute_flags =
			charattr_class_attribute_rows(live != nullptr ? *live : kNoTable);
}

// The joiner frame [orig: Game_ProcessMainFrame @0x5263f0 -- the client-side
// branch of the one frame function the host frame also rides]: the view
// session sample, the pump (provider wiring, hello, recv-fold + uplink +
// folds, the decoded-consequence application, the local World tick and the
// post-tick recompose), the charattr words, then the new-soldier .adm ground.
void JoinerRole::run_tick(const TickInput &) {
	mission::MissionKernel &kernel = *kernel_;
	if (!runtime) {
		last_net_us_ = 0;
		return;
	}
	kernel.local.view_session_inputs = view_session_inputs_for(
			runtime.get(), /*joiner=*/true, runtime->local_player_dead(),
			kernel.world.rules.mp_session);
	wire_leg_start_us_ = static_cast<int64_t>(io::perf_now_us());
	pump();
	sync_class_attribute_flags();
	// The pump's phases, its wire leg and the local world tick's own breakdown
	// all land on the kernel's profile, once per tick.
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_NET, last_net_us_);
	const int64_t adm_start = static_cast<int64_t>(io::perf_now_us());
	kernel.resolve_new_infantry_adm_ids();
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_ADM_RESOLVE,
				static_cast<int64_t>(io::perf_now_us()) - adm_start);
}

namespace {

constexpr double kFixed16 = io::kFp16OneD;

} // namespace

void JoinerRole::pump() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	// The provider closures below persist on the pipeline across frames; they
	// read this latched pointer at call time, never a per-call reference.
	world_ = &world;
	// A joiner is never the logic authority (run_logic_tick restamps it; events applied
	// before the first tick read it; the default is true), and runs its title's rules.
	world.rules.logic_authority = false;
	world.rules.title = title_ != nullptr ? title_->game_id : gameprofile::GAME_JO;
	// The phase clocks (F3 Stats): each span below laps onto the SIM_CLIENT_*
	// rows of the world's profile; an inactive profile reads no clock at all.
	// The wire leg (Client_ProcessNetworkFrame) has its own rows.
	rt.set_profile(world.profile);
	devtools::ProfileLap lap(world.profile);
	wire_frame_providers();
	resolve_row_adm_ids();
	pump_proxy_rendezvous();
	send_hello_once();
	deposit_inbound();
	// The frame input reaches L's body BEFORE the client net frame builds
	// this frame's C2S 0x0C from that body: retail's Player_PackInputStateToEntity
	// immediately precedes Player_BuildTag0CInputBody in the send block, the
	// same order the host frame keeps (host_role.cpp: pack, then pump). Packing
	// after the net frame shipped the PREVIOUS frame's move byte, stance/scope
	// flags, analog triplet and heading one tick late. The input latches stay
	// live through the pre-round phase. The pack itself runs only when this
	// frame's send block opens, so under a dictated holdoff the keys held on
	// every frame of the window accumulate into the one boundary pack -- the
	// MoveOrder the local motor drives from until the next pack and the byte
	// the boundary 0x0C carries; a key tapped between boundaries still reaches
	// the host.
	// [orig: Client_ProcessNetworkFrame @0x42C180 — the send-holdoff gate
	//  @0x42C3DD, Player_PackInputStateToEntity @0x42C3E9, then
	//  Player_BuildTag0CInputBody @0x42C46F..0x42C4A3]
	lap.restart();
	lp.apply_player_input_pre_tick(rt.send_block_opens_this_frame());
	lap.mark(devtools::Slot::SIM_CLIENT_PLAYER);
	const FrameSignals decoded = run_client_net_frame();
	// Phase 0 of the authoritative 0x0A is the client's one pre-round
	// predicate. Mirror it onto World before any local entity/system work.
	world.preround_delay_seconds =
			rt.state().preround_delay_seconds;
	lap.restart(); // the wire leg has its own rows
	materialize_replica_world();
	const bool had_local = local_spawned_;
	spawn_and_arm_local_player();
	apply_world_state_load();
	apply_replica_track(world, rt.state(), rt.game_type(), true, track_seen_);
	if (decoded.health) apply_authoritative_health();
	// The S2C 0x3A revive tint. Retail arms the word inside the message handler;
	// our replica state keeps the recipient's retained +0x1E0 "a medic is
	// reviving me" word instead, so the arm rides that latch's rising edge here
	// (the latch drops on the local respawn, and the respawn's Game_InitNewRound
	// clears the tint outright).
	// [orig: NapiNPClientMsg_0x03A @0x422685 -> dword_B764BC = 255;
	//  NapiNPClientMsg_GameReset @0x422843; Game_InitNewRound @0x422790]
	world::screen_flash_track_revive(kernel.local.view.flash,
			rt.state().local_medic_reviving);
	// Every 0x0A header carrying the recipient tail re-latches L's stance from
	// the authority's copy, ahead of that frame's records (the mount echo
	// below). Before L exists there is nothing to latch. [orig:
	// NapiNPClientMsg_0x00A @0x430549 `test g_LocalPlayerEntity` ->
	// @0x430562..0x43058f, ahead of the record loop]
	if (rt.state().health_updates_applied != stance_echo_seen_) {
		stance_echo_seen_ = rt.state().health_updates_applied;
		if (local_spawned_ && world.cached.local_player.valid())
			kernel.local.latch_stance(rt.state().local_stance_bits);
	}
	sync_authoritative_mount();
	apply_mounted_ammo_update();
	lap.mark(devtools::Slot::SIM_CLIENT_MATERIALIZE);
	mirror_mission_entities();
	// The Attack & Defend latch the boot ran with no L, on L's spawn frame
	// once the mirror applied the frame's teams, as retail's joiner latches
	// after its mission start pumped every message, the last drain after
	// Player_InitPlayer [orig: Game_StartMission @0x524360 -- the
	//  NapiClient_WaitForGameStart call @0x524E29, the Player_InitPlayer call
	//  @0x525BBC, the receive drain @0x525BD9 (gated @0x525BC4 / @0x525BCC),
	//  the non-authority jz @0x525E29 / @0x525E58 to @0x52609A, the Game_LatchAttackDefendRole
	//  call @0x5260C1].
	if (!had_local && local_spawned_) lp.latch_attack_defend_role(rt.game_type());
	lap.mark(devtools::Slot::SIM_CLIENT_MIRROR);

	// Received projectile/reload gameplay and the decoded remote collision
	// proxies are live inputs to this frame's entity/round/weapon pumps. Applying
	// them here is the retail recv-before-actions boundary, not presentation work.
	refresh_wire_collision_proxies();
	lap.mark(devtools::Slot::SIM_CLIENT_PROXIES);
	sync_replica_weapon_slots(rt.state(), world, self_wire_handle());
	apply_gameplay_events();
	apply_weather_sample();
	// The client's folded round-over latch (a 0x1D header or a 0x25) raises
	// the world's gate before the frame's entity update, which the gate then
	// holds; a mission start lowers both (ClientReplicaPipeline::begin_mission
	// beside the load's fresh Match).
	// [orig: g_SpawnSuccessGate — NapiNPClientMsg_0x01D @0x430858,
	//  NapiNPClientMsg_GameReset @0x422849; Game_ProcessMainFrame -- the
	//  is_in_session / g_SpawnSuccessGate tests @0x526734..0x526742]
	if (rt.state().spawn_success_gate) world.match.latch_round_over();
	lap.mark(devtools::Slot::SIM_CLIENT_MATERIALIZE);

	const bool preround_active = world.preround_delay_seconds != 0;
    world.rules.cease_fire = rt.state().cease_fire;
    // The joiner's rules word is the S2C 0x64 +44 mpattrib dword (g_RulesFlags
    // @0x24D1E34); its 0x10000 bit gates the scope-zero -1 floor in session
    // [orig: Player_AdjustWeaponZoomLevel @0x4dbd0c..0x4dbd2e].
	world.rules.mpattrib = rt.view().mp_attributes();
    world.rules.auto_scope_zero =
            (rt.view().mp_attributes() & GameConfig::kMpAttribAutoScopeZero) != 0;
    world.rules.no_friendly_fire =
            (rt.view().mp_attributes() & GameConfig::kMpAttribNoFriendlyFire) != 0;
    materializer_.fill_minefield_actors(rt.state(), self_wire_handle(), world.minefields.remote_actors);
	stamp_spectator_motor(kernel, runtime.get(), /*joiner=*/true); // this frame's receive folded
	// The tick's own phases land on the SIM_WORLD_* / SIM_UPDATE_* rows inside run_logic_tick.
	world.run_logic_tick(
			/*is_authority=*/false,
			preround_active ? world::TickPhase::PreRound
			                : world::TickPhase::Gameplay);
	lap.mark(devtools::Slot::SIM_CLIENT_WORLD);
	// The door callback's C2S 0x1A requests this tick raised, queued in raise
	// order for the next send boundary (world/doors.cpp carries the witness).
	for (const world::DoorRowEvent &request : world.out.door_requests)
		rt.queue_door_request(request.handle, request.state, request.number);
	world.out.door_requests.clear();
	if (!preround_active)
		mirror_predicted_vehicles(); // predicted boat poses -> presented rows
	lap.mark(devtools::Slot::SIM_CLIENT_MIRROR);
	// Vehicle prediction is the final carrier mover on a joiner. Recompose every
	// seat/deck/object attachment from that final pose in this same frame, then
	// publish the refreshed rows back to the local registry consumers. This is
	// the retail second carrier-follow phase; doing it before the world mover
	// leaves children one tick behind their vehicle.
	if (!preround_active) rt.refresh_remote_attachments();
	lap.mark(devtools::Slot::SIM_CLIENT_ATTACH);
	mirror_mission_entities();
	lap.mark(devtools::Slot::SIM_CLIENT_MIRROR);
	// The local mounted body was seat-posed earlier in the entity update's
	// pool-0 walk, before the joiner-only vehicle prediction pass. Re-pose L
	// against the vehicle's final same-frame transform so the camera/view never
	// trails its seat by one mover tick. Remote riders were recomposed in
	// ClientState just above.
	if (!preround_active && world.cached.local_player.valid()) {
		if (world::AiEntity *local_ai =
				world.ai.for_handle(world.cached.local_player)) {
			world.ai.refresh_mounted_pose(*local_ai, world);
		}
	}
	lap.mark(devtools::Slot::SIM_CLIENT_ATTACH);
	// The weather tick follows the entity update on a client exactly as on
	// the host [orig: Game_ProcessMainFrame @ 0x52674b -> @ 0x526774].
	kernel.tick_weather();
	if (!preround_active) {
		rt.tick_remote_stance_sounds(world);
		// The replica dead tail's 186-tick decay: the def's particledeath
		// spawned at the corpse origin, oriented (0, 0, -0.5) and tagged with
		// the corpse, its group held in the corpse's +0x1CC slot (a group the
		// corpse owns there is released first) -- the victim-hit emitter's
		// descriptor shape. [orig: Entity_UpdateInfantryAI @0x4b9ea7..0x4b9f3e
		//  (release @0x4b9ec9, tag @0x4b9f0f, orientation z 0xFFFF8000
		//  @0x4b9f24, CEffectWorld_SpawnEmitterAtPosition @0x4b9f36, the slot
		//  store @0x4b9f3e); the org2 twin @0x4b4e39]
		const def::DefItemsFile *items = kernel_->items_table();
		for (const auto &decay : rt.view().drain_corpse_decays()) {
			if (decay.release) {
				world::DestructionEffectEvent release;
				release.family = 1;
				release.attach_wire_handle = decay.handle;
				release.attach_spawn_origin = world::kSpawnOriginNone;
				release.release = true;
				world.out.destruction.effects.push_back(std::move(release));
				continue;
			}
			const def::DefItemDef *item = items != nullptr
					? mission::find_item_def(*items,
							  static_cast<int>(decay.type_id) + mission::kItemIdOffset)
					: nullptr;
			if (item == nullptr || item->particledeath[0] == '\0') continue;
			world::DestructionEffectEvent effect;
			effect.effect = item->particledeath;
			effect.pos = {opennova::io::fp16_16_to_float(decay.pos[0]),
					opennova::io::fp16_16_to_float(decay.pos[1]),
					opennova::io::fp16_16_to_float(decay.pos[2])};
			effect.dir = {0.0f, 0.0f, -0.5f};
			effect.family = 1;
			effect.attach_wire_handle = decay.handle;
			effect.attach_spawn_origin = world::kSpawnOriginNone;
			effect.section_tagged = true;
			effect.positioned = true;
			world.out.destruction.effects.push_back(std::move(effect));
		}
		tick_replica_emplaced_channels(rt.state(), kernel.seat_specs, world, self_wire_handle(),
				&materializer_, kernel.wire_header_world);
		rt.tick_remote_recoil();
	}
	lp.sync_local_mounted_input_heading();
	// What the view arbiter reads from the session (death screen, end round,
	// the death camera): the joiner samples it AFTER this frame's recv fold,
	// exactly the value its view tick consumed at this point in the frame.
	lp.view_session_inputs = view_session_inputs_for(runtime.get(), /*joiner=*/true,
			rt.local_player_dead(), world.rules.mp_session);
	lp.tick_view();   // retail promotes the per-frame view before weapon actions
	// The equipped-slot FSM pump, after the view promoter. Gated on L: retail
	// pumps weapon actions per-entity, so a joiner whose player has not spawned
	// has no slot to pump — without this gate a click during the join wait
	// discharged the pre-armed FSM with no shooter and the joiner deployed a
	// round short.
	// [orig: WeaponAction_ProcessAllEntities @ 0x526786]
	if (local_spawned_) {
		tick_replica_weapon_slots(rt.state(), world);
		tick_local_weapon();
		lp.tick_medic_cooldown(rt.local_player_dead()); // Player_UpdatePerFrame's cooldown leg
	}
	lap.mark(devtools::Slot::SIM_CLIENT_PLAYER);
	++now_tick_;
}

void JoinerRole::apply_weather_sample() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	// Each newly received phase-2 ENV sub-block lands in the weather home's
	// TARGET globals exactly once; the local currents keep chasing
	// [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c].
	const replication::ClientEnvironmentState &environment = rt.state().environment;
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
	world.weather.apply_wire_sample(sample);
}

void JoinerRole::wire_frame_providers() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	// The same joiner terrain backs the post-root 2u person ground probe.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B7CF4;
	// Entity_UpdateInfantryAI @0x4BF7FA; Entity_MovementCollisionResolver tail
	// @0x4B3D6E..0x4B3DA9]
    rt.view().set_round_receiver([this](const replication::ClientRoundEvent &ev) { apply_round_event(ev); });
    rt.view().set_guided_round_resolver([this](int16_t id) { return kernel_->world.round_sim.find_guided(id); });
    world.round_sim.guided_inputs_provider = [this](const world::LiveRound &round, world::GuidedInputs &in) {
        const auto *owner = runtime->state().find(round.shooter_handle);
        in.owner = in.owner || owner != nullptr;
        // Local fire has current input; a delayed replica must not replace
        // its lock when the predicted missile launches.
        if (owner && round.owner != kernel_->world.cached.local_player) {
            in.owner_ai = true;
            in.owner_target = owner->fire_target_handle;
            if (const auto *aim = runtime->state().find(in.owner_target)) {
                in.owner_aim = true;
                in.aim[0] = aim->x; in.aim[1] = aim->y; in.aim[2] = aim->z;
            }
        }
        const uint16_t target_id = round.guided.target == 0xFFFF && round.guided.phase == 0
                ? in.owner_target : round.guided.target;
        const auto *target = runtime->state().find(target_id);
        if (target) {
            in.target_present = true;
            const uint32_t dead_bit = target->cls == EntityClass::Vehicle
                    ? replication::kVehicleFlagDeadPose : world::kEntityFlagDead;
            in.target_alive = (target->state_flags & dead_bit) == 0;
            in.target_origin[0] = target->x; in.target_origin[1] = target->y; in.target_origin[2] = target->z;
        }
    };
    // The rounds' wire actors: a remote player is a decoded replica row here,
    // where retail's client resolves the wire handle to its own pool slot,
    // whose team (the 0x0C spawn @0x42e979), playerClass (the spawn
    // @0x42e9d5, then the player compact's difficulty byte & 0xF @0x4ad5a2),
    // equipped adm (+0x2B0, the compact @0x4c11f2) and chased Position the
    // whiz blip reads; the local player's own wire handle is the missile
    // note's target-is-local arm. [orig: NetPacket_DeserializeRoundEvent
    //  @0x42f491; Entity_SerializeGuidedMissileState @0x447ece]
    world.round_sim.wire_actor_provider = [this](uint16_t handle, world::RoundSim::WireActor &out) {
        out = world::RoundSim::WireActor{};
        out.is_local = runtime->has_self_handle() && handle == runtime->self_handle();
        const auto *row = runtime->state().find(handle);
        if (row == nullptr) return out.is_local;
        out.team = row->team_known ? row->team : 0;
        out.player_class = row->net_has_compact
                ? static_cast<int32_t>(row->health_class_byte & 0x0Fu)
                : static_cast<int32_t>(row->spawn_player_class);
        out.equipped_adm_index = row->equipped_adm_index;
        out.pos[0] = row->x;
        out.pos[1] = row->y;
        out.pos[2] = row->z;
        return true;
    };
	rt.view().set_remote_motion_terrain(world.tables.terrain);
	// The replica water/float channel reads the mission water plane
	// [orig: g_EnvWaterHeightFixed @ 0x26C6454] (EnvState convention: 0 = no
	// water in this world).
	rt.view().set_water_z(
			world.env.water_z, world.env.water_z != 0);
	// Replica peer/source spheres are initialized from the exact same typed
	// items.def/model result as projectile and lighting projections. The
	// pipeline retains this callback across frames; it reads the kernel the
	// role is bound to at call time.
	rt.view().set_replica_bound_radius_resolver(
			[this](uint16_t type_id) -> int32_t {
				return kernel_->wire_collision_shape_for_type(type_id).bound_radius_q16;
			});
	// The replica dead tail's def fields, from the same items.def the spawn
	// stream typed the row from: deathtime (the edge's moveTimer seed),
	// LeaveCorpse, and whether the def names a decay effect.
	// [orig: Entity_UpdateInfantryAI @0x4b9c97 (seed), @0x4b9e54 (attrib
	//  0x400000), @0x4b9e9a (the def+0x412 decay word)]
	rt.view().set_replica_death_traits_resolver(
			[this](uint16_t type_id,
					replication::ClientReplicaPipeline::ReplicaDeathTraits &out) {
				const def::DefItemsFile *items = kernel_->items_table();
				const def::DefItemDef *item = items != nullptr
						? mission::find_item_def(*items,
								  static_cast<int>(type_id) + mission::kItemIdOffset)
						: nullptr;
				if (item == nullptr) return false;
				out.deathtime_ticks = item->deathtime_ticks;
				out.leave_corpse = (item->attrib & def::DEF_ITEM_ATTRIB_LEAVECORPSE) != 0;
				out.decay_effect = item->particledeath[0] != '\0';
				return true;
			});
	// The 0x13's class callback reads the joiner world's hit record, which
	// this peer's own rounds write [orig: NapiNPClientMsg_EntityDeath
	// @0x42EBF5 -> OrganicClass_HandleEvent @0x40734C].
	rt.view().set_replica_hit_record_resolver(
			[this]() -> const world::HitRecord * {
				return &kernel_->world.round_sim.hit_record;
			});
	// The 0x26 route's section store, at the fold, under the gate
	// apply_item_state_event applies to the same entity (it stores the same
	// word again when the drain runs the callback).
	// [orig: Entity_KillBySlotId @0x42BD29..0x42BD47]
	rt.view().set_replica_kill_section_store(
			[this](uint16_t handle, int16_t section) {
				world::World &world = kernel_->world;
				const world::Entity *e = world.registry.get(world::EntityHandle{handle});
				if (e != nullptr && e->item_type_index != 0 &&
						(e->engine_flags & world::kEntityFlagDead) == 0)
					world.round_sim.hit_record.section = section;
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
	if (world.ai.collision != nullptr) {
		rt.view().set_replica_contact_resolver(
				[this](replication::ClientReplicaPipeline::ReplicaContactQuery
								&q) -> int32_t {
					using world::CollisionWorld;
					CollisionWorld *col = world_->ai.collision;
					CollisionWorld::ResolveState &st =
							replica_resolve_states_[q.row_handle];
					// Role-member scratch: this resolver runs per armed
					// replica row per 62.5 Hz pump, and every row of one pump
					// shares the same staged peer table (q.tick is the pump
					// key), so the field-order copy between the two mirrored
					// peer PODs happens once per tick, not once per row.
					std::vector<CollisionWorld::ReplicaPeer> &peers =
							replica_peer_scratch_;
					if (replica_peer_scratch_src_ != q.peers ||
							replica_peer_scratch_count_ != q.peer_count ||
							replica_peer_scratch_tick_ != q.tick) {
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
						replica_peer_scratch_src_ = q.peers;
						replica_peer_scratch_count_ = q.peer_count;
						replica_peer_scratch_tick_ = q.tick;
					}
					world::EntityHandle ground;
					const world::Vec3 resolve_pos{opennova::io::fp16_16_to_float(q.pos[0]),
							opennova::io::fp16_16_to_float(q.pos[1]),
							opennova::io::fp16_16_to_float(q.pos[2])};
					const int32_t clearance = col->resolve_replica(
							*world_, st, q.pos, q.vel_xy, q.vel_z,
							q.capsule_bottom, q.capsule_top,
							q.source_bound_radius_q16, q.is_player_class,
							q.tick, q.anim_state_id, q.anim_state_flags,
							peers.data(), static_cast<int32_t>(peers.size()),
							q.row_handle, &q.entity_flags, &ground);
					// A remote player's powerup contact runs the pickup on this
					// peer's copy of the row, for a transient copy of the body at
					// the pose the resolve read (its integrated position): the
					// def hp until the first compact, the class byte's tier after,
					// zero once dead. The pickup's sound plays there and the row
					// hides or is destroyed here, as on every peer.
					// [orig: Entity_MovementCollisionResolver @0x4B2FB8..0x4B2FCC
					//  (no authority gate) -> PowerupAction_Pickup @0x4428A0;
					//  Entity_InitFromItemDef @0x49E550; the dead zero @0x4C10FB]
					const auto pickups = col->take_replica_powerup_contacts();
					if (!pickups.empty()) {
						const replication::ClientEntityState *row =
								runtime->state().find(q.row_handle);
						world::Entity picker;
						picker.has_item_def = true;
						picker.item_id = q.type_id;
						picker.position = resolve_pos;
						picker.health_max = world_->tables.player.item_hp.value_or(0);
						picker.health = picker.health_max;
						if (row != nullptr && row->net_health_zero) picker.health = 0;
						else if (row != nullptr && row->net_has_compact)
							picker.health = replication::replica_tier_health(
									row->health_class_byte, picker.health_max);
						world::TickContext ctx;
						ctx.world = world_;
						ctx.logic_tick = world_->logic_tick;
						ctx.is_authority = false;
						for (const auto &contact : pickups)
							world::powerup_pickup_by(*world_, contact.target, picker, ctx);
					}
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
		rt.view().set_carrier_pose_provider(
				[this](uint16_t handle,
						replication::ClientReplicaPipeline::CarrierPose
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
		rt.view().set_replica_contact_resolver(nullptr);
		rt.view().set_carrier_pose_provider(nullptr);
	}
}

// ClientHello once (Idle -> Hello) the first armed frame.
void JoinerRole::send_hello_once() {
	if (!started_) {
		const std::vector<uint8_t> hello = runtime->start();
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
JoinerRole::FrameSignals JoinerRole::run_client_net_frame() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	const uint32_t now = now_tick_;
	// Run the client net frame first: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C
	// uplink (gated InMatch && deployed inside the runtime). The uplink describes L's pose as left by
	// the previous entity update; this frame's raw-input/motor pass follows all inbound application.
	const uint32_t health_updates_before =
			rt.state().health_updates_applied;
	const uint32_t objective_updates_before =
			rt.state().objective_updates_applied;
	const bool local_existed_before_net = local_spawned_;
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = local_spawned_ && world.cached.local_player.valid();
	const world::Entity *e =
			have_L ? world.registry.get(world.cached.local_player) : nullptr;
	const world::AiEntity *ae =
			have_L ? world.ai.for_handle(world.cached.local_player) : nullptr;
	// A release can arrive during this recv pump. Do not let that newly-opened
	// runtime gate transmit the corpse/stale pre-deploy pose in the same frame;
	// L resumes uplinking only after the simulation has consumed the release and
	// a later positive authoritative health tail.
	const bool can_offer_uplink =
			rt.is_deployed() && e != nullptr && ae != nullptr &&
			e->alive && e->health > 0 && (e->flags & 2u) == 0u &&
			!redeploy_release_pending_;
	rt.set_local_player_position(world); // L, the receive handlers' local player
	if (can_offer_uplink) {
		// The interest list scores the decoded entities against L; the HUD
		// target cursor is the aim acquisition's combat target, else its
		// aim-ray entity, named by its wire handle (a materialized pool-1..3
		// row sits at its own; L answers to its self handle).
		// [orig: HUD_BuildEntityInfo @0x4B87DA..0x4B882D -> g_HUDTargetCursorEntity
		//  (HUD+0x168), read @0x50E1C6 / @0x50E42C]
		// It is built at the send block, after this frame's receive, whose fold
		// of L's own record (its compact revision moved) re-points L's ground
		// link first [orig: @0x42C228 ahead of @0x42C482; case 2 @0x4C1346].
		const uint16_t self = rt.has_self_handle() ? rt.self_handle() : 0xFFFF;
		const replication::ClientEntityState *self_row = rt.state().find(self);
		const uint32_t self_revision = self_row != nullptr ? self_row->compact_revision : 0;
		outs = rt.Client_ProcessNetworkFrame([&](PlayerExtendedUplink &up) {
			replication::UplinkClientInputs interest;
			interest.replica = &rt.state();
			interest.self_wire_handle = self;
			interest.hud_target_wire_handle = replication::uplink_hud_target_wire_handle(
					world, *ae, self, kernel.local.aim_wire_person());
			interest.avg_fps = uplink_avg_fps_;
			interest.cpu_percent = uplink_cpu_percent_;
			self_row = rt.state().find(self);
			if (self_row != nullptr && self_row->compact_revision != self_revision)
				interest.self_echo = self_row;
			up = replication::build_player_uplink(world, *e, *ae, interest);
			return true;
		}, now);
	} else {
		outs = rt.Client_ProcessNetworkFrame(now);
	}
	for (const std::vector<uint8_t> &dg : outs) send(dg);
	// The wire leg ends here (the stats board's net split).
	last_net_us_ = static_cast<int64_t>(io::perf_now_us()) - wire_leg_start_us_;
	// The host policy is installed before either the granted kit or side
	// reseed can consult availability. A pre-load packet remains retained in
	// ClientState until the first world frame reaches this boundary.
	// [orig: NapiNPClientMsg_HandleWeaponRestrictions @0x42D4C0]
	if (weapon_availability_revision_seen_ != rt.state().weapon_availability_revision) {
		lp.loadout.availability.values = rt.state().weapon_availability;
		weapon_availability_revision_seen_ = rt.state().weapon_availability_revision;
	}
	if (kit_seams.apply_authoritative) kit_seams.apply_authoritative();
	// The team selector may only just have become known (the S2C 0x04 latch landing
	// after the catalog) or may have moved us across the line (S2C 0x50). Either way the
	// resident kit buffer follows the side, exactly as retail re-copies restrictionData
	// for the newly assigned side [orig: NapiNPClientMsg_TeamAssign @0x431a9a].
	if (kit_seams.reseed_on_side_change && kit_seams.reseed_on_side_change())
		if (kit_seams.push) kit_seams.push();
	// S2C 0x50 re-latched OUR OWN team. L's team is seeded once by spawn_from_self
	// (the self record's team byte, or a pre-spawn 0x50's); a later one must move both the
	// entity's Team and the round sim's presenting-client team, or friend/foe styling
	// keeps rendering the old side. Retail does the same two writes: g_LocalTeam for
	// the latch and entity->Team for the entity itself.
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — @0x4319db / @0x4319ee; the round-spawn
	//  style select reads the local player's Team @0x4ec740]
	{
		const uint64_t self_team_revision = rt.self_team_revision();
		if (self_team_revision > self_team_revision_seen_) {
			self_team_revision_seen_ = self_team_revision;
			const uint8_t assigned = rt.assigned_team();
			if (world::Entity *L =
					world.registry.get(world.cached.local_player)) {
				L->team = assigned;
			}
			world.round_sim.local_team = assigned;
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
			if (kit_seams.push) kit_seams.push();
		}
	}
	const uint64_t deployment_release_revision =
			rt.deployment_release_revision();
	if (deployment_release_revision > deployment_release_revision_seen_) {
		deployment_release_revision_seen_ = deployment_release_revision;
		// The first release creates L below. A later release must revive that
		// existing identity, but only after a positive 0x0A tail observed after
		// this release frame (not a stale positive already queued ahead of it).
		if (local_existed_before_net) {
			redeploy_release_pending_ = true;
			redeploy_health_updates_at_release_ =
					rt.state().health_updates_applied;
		}
	}
	// Frozen-session tripwire (live-diagnosis aid, ~1 Hz): sample the signature
	// continuously; the embedder's diagnostic hook owns the (env-gated) print.
	// An unrecovered S2C sequence gap stalls the ordered frontier, the remote world
	// freezes, retained records stop retiring, and the host eventually reaps us.
	if ((now % 62u) == 0u) {
		const std::size_t gap_depth = rt.inbound_gap_depth();
		const uint32_t frontier = rt.inbound_frontier_seq();
		const uint32_t out_seq = rt.outbound_seq();
		const uint32_t records = rt.state().compact_records_applied;
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
				rt.in_match() && rt.is_deployed() &&
				gap_depth > 0 && frontier_flat && records_flat &&
				outbound_advanced;
		if (stalled_sample) ++flat_seconds_;
		else flat_seconds_ = 0;
		freeze_suspected_ = flat_seconds_ >= 3;
		diagnostic_sample_pending_ = true;
		last_frontier_seq_ = frontier;
		last_records_applied_ = records;
		last_outbound_seq_ = out_seq;
		diagnostic_sampled_ = true;
	}
	FrameSignals decoded;
	decoded.health =
			rt.state().health_updates_applied != health_updates_before;
	decoded.objectives =
			rt.state().objective_updates_applied != objective_updates_before;
	if (decoded.objectives) {
		const replication::ClientState &client = rt.state();
		world.script.subgoals.won = client.objective_won;
		world.script.subgoals.lost = client.objective_lost;
		world.script.subgoals.show_win = client.objective_show_win;
		world.script.subgoals.show_lose = client.objective_show_lose;
	}
	return decoded;
}

// Drain typed S2C gameplay events after the client recv pump: tag-2 fires
// spawn visual-only rounds; the requester's 0x49 echo performs its refill.
void JoinerRole::apply_gameplay_events() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	rt.apply_received_effects(world);

	// S2C 0x23 replicated WAC commands: run the registry row's handler here
	// with the operands the wire carried — the same body the host VM ran for a
	// broadcast row, or ran only here for a targeted ptext/pwave/pconsol. Fx
	// and SoundSet operands index this peer's mounted catalogs.
	// [orig: GameMode_DispatchRemoteCommand @0x4F81E0 — the call-convention
	//  switch @0x4f8438 into the row's handler]
	const std::vector<ScriptRemoteCommand> remote_commands =
			rt.drain_script_remote_commands();
	if (!remote_commands.empty()) {
		const std::vector<std::string> effect_names =
				kernel.script_effect_catalog.interned_names();
		const wac::RemoteCommandNames names{
				&effect_names, &kernel.script_sound_catalog.names()};
		std::vector<world::ScriptRemoteArg> args;
		for (const ScriptRemoteCommand &command : remote_commands) {
			args.clear();
			args.reserve(command.args.size());
			for (const ScriptRemoteCommandArg &arg : command.args)
				args.push_back({static_cast<int32_t>(arg.value), arg.text});
			const wac::RemoteCommandResult result =
					wac::run_remote_command(world, command.command_index, args, names);
			// A wire row with no handler body here is the joiner's dispatch gap;
			// the host VM records the same kind at its instruction site.
			if (!result.handled)
				world.diagnostics.record({world::RuntimeGapKind::WacCommand,
						command.command_index, 0, -1, -1}, world.logic_tick);
		}
	}

	// S2C 0x3F: kind 0 re-runs the objective notification here with relay
	// flag 0 (off the authority it posts the chat lines and relays nothing),
	// then NEW_GOAL at the local player for flag 1. Kind 1, and the kind-0
	// arm's fall-through into the same reads, is the mission-text chat relay:
	// the key resolves in this peer's mission text and a non-empty line posts
	// (a kind-0 body has no tail, so its empty key posts nothing).
	// [orig: NapiNPClientMsg_0x03F @0x42BB20 — HUD_ShowObjectiveNotification
	//  @0x42bbc2, NEW_GOAL @0x42bbc7..0x42bbe4, MissionText_GetStringByKey
	//  @0x42bc26 -> GameMsg_AddChatLineAndRelay @0x42bc39 (its line gate
	//  @0x5ba170..0x5ba18d)]
	for (const ObjectiveNotification &notice : rt.drain_objective_notifications()) {
		if (notice.kind == 0) {
			world.show_objective_notification(notice.slot, notice.is_win, notice.is_active, 0);
			if (notice.flag == 1) {
				if (const world::Entity *player = world.registry.get(world.cached.local_player))
					world.out.fire_sounds.play_immediate("NEW_GOAL", player->position, 0);
			}
		}
		if ((notice.kind == 0 || notice.kind == 1) && !notice.key.empty() &&
				(world.rules.mp_session_peer || !world.rules.mp_session))
			world.out.effects.push({"mission_text_chat", notice.team, 0, 0, 0, notice.key});
	}

	// Retail's S2C 0x0A tag-2 record is a fired-round descriptor. Re-run the
	// normal round spawner so tracers and physical impacts are produced locally;
	// World::run_logic_tick admits this pool only under the explicit
	// mp_session && !projectile_authority visual-client gate. Every descriptor
	// is spawned VisualOnly below, and that mode gates every gameplay consequence.
	for (const auto &ev : rt.drain_round_events()) apply_round_event(ev);

	for (const WeaponReload &reload :
			rt.drain_reload_notifications()) {
		++lp.weapon.reload_received_serial;
		lp.weapon.reload_received_entity = reload.entity_handle;
		lp.weapon.reload_received_param = reload.reload_param;
		// ClientRuntime already applied the remote-Person receive-handler stamp
		// before that frame's body tick. This drain retains diagnostics plus the
		// self-only WeaponSlot_ReloadAmmo branch below.
		if (!lp.inventory_valid || !rt.has_self_handle() ||
				reload.entity_handle != rt.self_handle())
			continue;

		const int32_t combo = reload.reload_param;
		world::WeaponInventorySlot *reloaded = lp.inventory.slot(combo);
		const world::WeaponTableEntry *def =
				(reloaded != nullptr && reloaded->adm_index >= 0)
						? world.tables.weapons.by_index(
								static_cast<uint8_t>(reloaded->adm_index))
						: nullptr;
		if (reloaded == nullptr || def == nullptr) continue;

		// WeaponSlot_ReloadAmmo is run only on the echoed notification: refund
		// the payload-addressed slot's remaining clip to its ammo-class pool and
		// draw a full clip. A late echo still reloads that exact slot after a switch;
		// only the currently active personal slot has an FSM mirror to update here.
		world::weapon_inventory_reload_slot(
				world.tables.weapons, lp.inventory, combo);
		if (lp.weapon.active && !lp.weapon.usegun_slot_active &&
				lp.inventory.equipped_combo >= 0) {
			world::WeaponSlotState &active_slot =
					*world::active_local_weapon_slot(world, lp.weapon);
			const world::WeaponInventorySlot *equipped =
					lp.inventory.slot(lp.inventory.equipped_combo);
			const world::WeaponTableEntry *equipped_def =
					(equipped != nullptr && equipped->adm_index >= 0)
							? world.tables.weapons.by_index(
									static_cast<uint8_t>(equipped->adm_index))
							: nullptr;
			if (equipped_def != nullptr) {
				active_slot.reserve = world::weapon_pool_get(
						lp.inventory, equipped_def->ammo_class_id);
			}
			if (combo == lp.inventory.equipped_combo) {
				active_slot.clip = world::weapon_inventory_loaded_rounds(
                        world.tables.weapons, lp.inventory, combo);
				active_slot.phase = static_cast<uint8_t>(
						active_slot.phase &
						~world::weapon_phase::kReloadPendingBit);
			}
		}
		++lp.weapon.reload_applied_serial;
		world::AiEntity *player = world.ai.for_handle(world.cached.local_player);
		if (player != nullptr && player->inf.active)
			player->inf.reload_anim_ticks = 80;
	}
	apply_weapon_pickups(*this, rt, world, lp);

	// The frame decays remote recoil after the emplacement channels consume it.
}

void JoinerRole::reset_for_join() {
	// A fresh ClientRuntime restarts the environment revision at 0: the
	// phase-2 cursor must not swallow the first sample of a rejoin.
	weather_revision_seen_ = 0;
	weapon_availability_revision_seen_ = 0;
	world_state_revision_seen_ = 0;
	track_seen_ = ReplicaTrackSeen{};
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
	last_frontier_seq_ = 0;
	last_records_applied_ = 0;
	last_outbound_seq_ = 0;
	diagnostic_sampled_ = false;
	flat_seconds_ = 0;
	freeze_suspected_ = false;
	replica_peer_scratch_src_ = nullptr;
	replica_peer_scratch_count_ = 0;
	replica_peer_scratch_tick_ = 0;
}

void JoinerRole::reset_materialization() {
	materializer_.clear();
	wire_world_topology_revision_seen_ = ~uint64_t{0};
	wire_world_stream_revision_seen_ = ~uint64_t{0};
}

void JoinerRole::reset_world_stream() {
	wire_world_static_initialized_ = false;
	reset_materialization();
}

bool JoinerRole::session_lost(SessionError &error) const {
	if (!runtime) return false;
	if (runtime->session_lost()) {
		error = {SessionErrorCode::SessionLost, runtime->session_loss_reason()};
		return true;
	}
	// A stored exit reason (the NovaWorld session's end) exits the mission like
	// a lost connection; the shell's post-mission route reads the reason itself.
	if (runtime->mission_exit_reason() != 0) {
		error = {SessionErrorCode::SessionLost,
				"mission exit " + std::to_string(runtime->mission_exit_reason())};
		return true;
	}
	return false;
}

// The editor Stop/Start rewind on a joiner: the kernel's restore, then one
// exact rematerialization fold of the retained ClientState (the body-empty
// baseline removed its registry carriers; the portal tables stay, their
// handles are identical and the weld records one-shot mutable).
bool JoinerRole::reset_to_baseline(SessionError &error) {
	if (!kernel_->restore_baseline()) {
		error = {SessionErrorCode::TickFailed, "mission baseline is unavailable"};
		return false;
	}
	if (kernel_->wire_header_world) reset_materialization();
	return true;
}

// Leaving: the disconnect datagrams ride the shell's send leg.
void JoinerRole::close() {
	if (kernel_) {
		kernel_->world.round_sim.guided_inputs_provider = {};
		kernel_->world.round_sim.wire_actor_provider = {};
	}
	if (!runtime) return;
	for (const std::vector<uint8_t> &dg : runtime->disconnect()) send(dg);
}

void JoinerRole::apply_round_event(const replication::ClientRoundEvent &ev) {
    auto &world = kernel_->world;
    auto &rt = *runtime;
    auto &kernel = *kernel_;

	// The retail deserializer dispatches only the alt/projectile bit or the
	// standard adm-indexed bit [orig: @0x42f2a8]. Other flag shapes do not
	// enter RoundData_SpawnRound.
	if ((ev.flags & (kRoundEventFlagAltFire | kRoundEventFlagAdmIndexed)) == 0)
		return;
	// The event names its shooter, which must resolve to a live entity with an
	// item def on this client, or the whole event is dropped: no 0xFFFF, no pool
	// past 4, no slot past its pool, no empty slot or def. Here a shooter is a
	// decoded row with its type or a native twin with its def.
	// [orig: NetPacket_DeserializeRoundEvent - the 0xFFFF test @0x42F451, the pool
	//  @0x42F465, the slot @0x42F47C..0x42F482, the entity and its +0x20 def
	//  @0x42F497..0x42F4A1]
	{
		if (wire_handle::is_batch_end_sentinel(ev.shooter_handle)) return;
		const replication::ClientEntityState *row = rt.state().find(ev.shooter_handle);
		const world::Entity *twin = world.registry.get(world::EntityHandle{ev.shooter_handle});
		if ((row == nullptr || row->type_id == 0) && (twin == nullptr || !twin->has_item_def))
			return;
	}
	// Bit 0 is tested first: the ammo-def arm reads the byte as the round's ammo
	// index and spawns with no weapon. Only with bit 0 clear does the adm-indexed
	// arm resolve the byte as a weapon and fly that weapon's round.
	// [orig: NetPacket_DeserializeRoundEvent - the arm test @0x42F521, the ammo
	//  bound @0x42F52C..0x42F53E, the null weapon @0x42F582 into
	//  RoundData_SpawnRound @0x42F5BA; the adm arm's AdmDef_GetEntryByIndex
	//  @0x42F6D9 and its ammo byte @0x42F6F9]
	const bool ammo_arm = (ev.flags & kRoundEventFlagAltFire) != 0;
	int32_t round_ammo = -1;
	if (ammo_arm) {
		if (world.tables.ammo.by_index(ev.adm_index) == nullptr) return;
		round_ammo = ev.adm_index;
	} else {
		const world::WeaponTableEntry *adm =
				world.tables.weapons.by_index(ev.adm_index);
		if (adm == nullptr || adm->ammo_index < 0) return;
		round_ammo = adm->ammo_index;
	}
	world::RoundSpawnParams round;
	world::RoundSourceState source;
	round.owner = world::EntityHandle{};
	round.shooter_handle = ev.shooter_handle;
	// The mounted shooter's own vehicle joins the trace exclusion exactly
	// like retail's mount rule — see wire_carrier_exclusion_for.
	round.shooter_carrier_handle = wire_carrier_exclusion_for(
			rt.state(), ev.shooter_handle, kernel.seat_specs);
	// Retail resolves the wire shooter entity and copies its TEAM into the
	// spawned round — the friend/enemy throwable item and tracer styling key
	// on it. Without this every remote grenade wore the enemy variant (and a
	// variant with no motor row froze mid-air). [orig: @0x4ec705]
	if (replication::ClientEntityState *shooter_row =
				rt.state().find(ev.shooter_handle)) {
		round.shooter_team = shooter_row->team;
		const int anim = shooter_row->anim_state_id;
		// The category follows the retail animation-flags table, not a
		// hand-maintained list of familiar locomotion clips. In particular,
		// 170/171 remain crouched, 172 is prone, and idle_mortar (46) is
		// neither. [orig: g_AnimStateFlagsTable @0x8139E8, mapped onto +0x12C by
		// NetPacket_SerializePlayerState @0x4C11FD..0x4C1242; category read in
		// RoundData_SpawnRound @0x4EC129..0x4EC14C, the row @0x4EC2A6..0x4EC2FD]
		const uint32_t anim_flags = world::infantry_anim_flags(anim);
		const bool prone = (anim_flags & world::kAnimStanceFlagProne) != 0;
		// The spread's up/down limit reads the raw prone bit, not the row
		// [orig: RoundData_SpawnRound @0x4EC137, read @0x4EC454].
		source.prone = prone;
		const bool crouched =
				(anim_flags & world::kAnimStanceFlagCrouched) != 0;
		const bool mounted =
				round.shooter_carrier_handle != wire_handle::kInvalid;
		// The shooter's Flags dword as a retail client holds a decoded peer's:
		// the spawn word and the client motor's bits (the row's mirror), with the
		// latest compact record's byte folded in under 0xFD, as the 0x0A fold
		// does (`Flags ^= (Flags ^ wire) & 0xFD`): Parachute 0x20 rides the
		// record, Drowning 0x8000 and LadderContact 0x100000 are the motor's.
		// [orig: NetPacket_SerializePlayerState @0x4C1307..0x4C1311 (a player);
		//  @0x4C065D..0x4C0662 (an infantry)]
		const uint32_t mirror = shooter_row->rm_entity_flags;
		const uint32_t shooter_flags = shooter_row->state_flags_known
				? (mirror & ~0xFDu) | (shooter_row->state_flags & 0xFDu)
				: mirror;
		// Retail tests eye Z (Position.Z + CameraOffset.Z) against the fixed
		// water plane [orig: RoundData_SpawnRound @0x4EC2DE..0x4EC2EA]. The
		// decoded row has no CameraOffset carrier, so raw fixed position Z is
		// the bounded projection. Do not invent a standing-eye constant here.
		const bool below_water = world.env.water_z != 0 &&
				shooter_row->z < world.env.water_z;
		source.person_with_item_def =
				shooter_row->cls == EntityClass::Player ||
				shooter_row->cls == EntityClass::Infantry;
		source.player = shooter_row->cls == EntityClass::Player;
		// The recoil's x4 leg: Drowning, or the eye below the water
		// [orig: RoundData_SpawnRound `test ecx, 8000h` @0x4EC342 (the shotgun
		//  leg) / @0x4EC86D (the single round), the eye @0x4EC34E..0x4EC35A /
		//  @0x4EC879..0x4EC885].
		source.underwater =
				(shooter_flags & world::kEntityFlagDrowning) != 0u || below_water;
		// The stance row: Parachute, Drowning or LadderContact, or the eye below
		// the water, forces the standing row 2; a mount after it wins (row 1).
		// [orig: RoundData_SpawnRound `test dword ptr [ebp+24h], 108020h`
		//  @0x4EC2D5, the eye @0x4EC2DE..0x4EC2EA, row 2 @0x4EC2EC, the mount
		//  @0x4EC2F4..0x4EC2FD]
		const bool standing_override =
				(shooter_flags & (world::kEntityFlagParachute | world::kEntityFlagDrowning |
				                  world::kEntityFlagLadderContact)) != 0u ||
				below_water;
		source.stance_category = mounted ? 1 : (standing_override ? 2 :
				(prone ? 0 : (crouched ? 1 : 2)));
		source.scope_raised =
				(shooter_row->state_flags &
				 world::kEntityFlagScopeRaised) != 0;
		// The spawn's Indoors stamp reads the resolved shooter's live Flags
		// (client_weapon_replay.cpp) [orig: RoundData_SpawnRound @0x4EC830].
		source.indoors = replica_shooter_indoors(kernel, *shooter_row);
		source.recoil_pitch = &shooter_row->recoil_pitch;
		// The zero-elevation row keys on the shooter's equipped AdmDef, the
		// replica's 0x0A off-16 echo [orig: RoundData_SpawnRound entity+0x2B0
		// @0x4ec16f].
		source.equipped_adm_index = shooter_row->equipped_adm_index;
		replica_weapon_action_source(*shooter_row, world, ev.flags, source);
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
	round.ammo_index = round_ammo;
	round.adm_index = ev.adm_index;
	round.no_weapon = ammo_arm;
	round.shot_seq = ev.shot_seq;
	round.subtype = ev.subtype;
	round.charge = ev.slot_byte;
	// Carry the arm through so presentation can honour retail's split: the
	// adm-indexed arm spawns no ammo-def effect at the wire position (which is
	// the shooter's EYE — Position + CameraOffset), it executes the addressed
	// def's action rows at the weapon's own userpoint instead.
	// [orig: @0x42f521 / @0x42f6ce]
	round.wire_round_flags = ev.flags;
	world.round_sim.spawn(
			world, round, world::RoundConsequenceMode::VisualOnly);

}
} // namespace opennova::inmatch
