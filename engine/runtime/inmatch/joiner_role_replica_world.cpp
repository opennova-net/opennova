// The joiner role's replica-world mirror: the streamed world materialized
// into the local registry, L's spawn on the in-match edge, the authoritative
// appliers (the 0x0A tail health, the 0x0F load, the mount and the phase-8
// ammo), and the per-frame write-back of the wire positions onto the mission
// rows and the collision proxies. Moved verbatim out of joiner_role.cpp (the
// file sat at the 2500-line ratchet); see joiner_role.h for the ownership
// split; every phase keeps its witnesses.
#include <runtime/inmatch/joiner_role.h>

#include <runtime/mission/mission_kernel.h>

#include <runtime/replication/client_replica_pipeline.h>

#include <base/gameprofile/game_type.h> // kCaptureTheFlag / kFlagBall (the fresh-pickup lookup)
#include <net/npwire/wire_handle.h>      // pool()/kPoolItem (the wire handle home)


#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity_spawn.h>  // entity_reset_to_spawn_state (redeploy release)
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_view.h>   // the fullscreen damage-feedback words + the shake arm
#include <runtime/world/player_weapon.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <runtime/world/vehicle_mount.h> // resolve_mounted_ammo_slot (phase-8 route)
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

namespace {

// SelfSpawn (mission i32 16.16 + full BAM32 orientation) -> PlayerSpawn for L.
world::PlayerSpawn spawn_from_self(const JoinerConnection::SelfSpawn &s) {
	world::PlayerSpawn spawn;
	// SelfSpawn position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position.set_q16({s.pos_x, s.pos_y, s.pos_z});
	// orientation is ALREADY a full 32-bit BAM (unlike HostJoinerPose.heading, an i16
	// the host shifts << 16): the body keeps the record's full word (D-NET-376,
	// D-NET-NEW-L6-2) [orig: NapiNPClientMsg_0x00C @0x42E967 `mov [edi+10h], ecx`].
	spawn.heading = s.orientation;
	spawn.team = s.team;
	// The named player record carries the host-stamped character selector and
	// packed minimap/character id (a zero animSlot recreated D-NET-146's DBuggy shadow);
	// the spawn then takes the mission start's Player_InitPlayer identity over them
	// (JoinerRole::stamp_mission_start_identity, D-NET-570).
	spawn.anim_slot = s.anim_slot;
	spawn.minimap_net_id = s.net_id;
	// [D-NET-112] The packed id belongs to the wire NetId ONLY. A player carries no SSN, so L
	// takes net_id 0 exactly like the host's own spawn (server_spawn.cpp) — that keeps it out of
	// the WAC/BMS find_by_net_id space, which PlayerSpawn.net_id's 0xFFF0 default would otherwise
	// join, so the zero must be written explicitly. [orig: Server_PlayerAdd @0x51cbc0 slot+440 ->
	// entity+0x15C (the wire NetId); EntityPool_FindByNetId @0x4f0a20 keys entity+0x7C, left 0
	// for players]
	spawn.net_id = 0;
	// L is the client's body from the record: its item init's def word, not the
	// authority's zeroed row (world/player_spawn.h).
	spawn.from_wire_record = true;
	// The record's type word binds the body's row (ItemList_FindIndexByTypeId,
	// the first row for a type no row carries), not the class's camouflage item
	// the authority resolved. [orig: NapiNPClientMsg_0x00C @0x42E730 - the type
	//  read @0x42E83B, ItemList_FindIndexByTypeId @0x42E844, ItemTypeIndex
	//  @0x42E84A]
	spawn.item_type_id = s.item_type_id;
	return spawn;
}

} // namespace

world::Entity *JoinerRole::replica_world_entity(
		world::World &world, world::EntityHandle handle) {
	return materializer_.replica_world_entity(world, handle, kernel_->wire_header_world);
}

void JoinerRole::materialize_world_stream() {
	if (!runtime || kernel_ == nullptr) return;
	materialize_replica_world();
}

void JoinerRole::materialize_replica_world() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	if (!kernel_->wire_header_world) return;

	const replication::ClientState &state = rt.state();
	if (wire_world_topology_revision_seen_ == state.topology_revision &&
			wire_world_stream_revision_seen_ == state.world_stream_revision)
		return;
	materializer_.set_local_player(self_wire_handle(), world.cached.local_player);
	const replication::ClientWorldSyncResult sync =
			materializer_.sync(state, world);
	wire_world_topology_revision_seen_ = state.topology_revision;
	wire_world_stream_revision_seen_ = state.world_stream_revision;
	if (sync.changed()) {
		// The embedder rebuilds its asset-backed caches for the changed rows
		// (retired collision/occlusion instances, item traits, seat specs),
		// then re-folds the retained 0x0D mountHandles image through
		// materializer() once definitions exist.
		on_replica_world_changed(sync);
	}

	// The witnessed initial-state order is pool2 -> pool1 -> pool0 -> pool3.
	// Pool 0 is therefore the first safe fence for the one-shot static-table and
	// portal-weld pass; running it for each pool2 page would weld an incomplete
	// world and mutate shared occlusion records repeatedly.
	if (!wire_world_static_initialized_) {
		bool saw_pool0 = false;
		for (const replication::ClientEntityState &row : state.entities) {
			if (world::EntityHandle{row.handle}.pool() == 0) {
				saw_pool0 = true;
				break;
			}
		}
		if (saw_pool0) {
			// Pools 2 and 1 are complete here: give every streamed static its
			// placed identity so the shell presents it through the same
			// batched placer path as the host (never as a wire-direct node).
			materializer_.assign_placement_origins(world);
			kernel.occlusion_init_mission();
			wire_world_static_initialized_ = true;
		}
	}
}

// On the in-match edge (detected by the recv-fold): learn H + spawn L at the host-advertised
// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
// wire identity the host knows us by — the two stay distinct, reconciled by the owner-ID match (§5.38b).
void JoinerRole::spawn_and_arm_local_player() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	if (rt.in_match() && rt.has_self_handle() && !local_spawned_) {
		const JoinerConnection::SelfSpawn &sp = rt.spawn_pose();
		world::PlayerSpawn spawn = spawn_from_self(sp);
		stamp_mission_start_identity(spawn);
		const world::EntityHandle h = world::spawn_player(world, spawn);
		local_spawned_ = h.valid();
		// A client builds its own body from the host's 0x0C like every
		// pool-0 entity: the slot memset leaves commandGroup (+0x11C) 0 and no
		// client message stores it. The 1 spawn_player stamps is the
		// authority's (Entity_SpawnFromAnimSlotProperty @0x43C50F, reached
		// from Server_PlayerAdd @0x51D056 and
		// Server_InitAllPlayerEntitiesForRound @0x516B67).
		// [orig: NapiNPClientMsg_0x00C @0x42E7F1]
		if (world::Entity *body = world.registry.get(h)) body->group_id = 0;
		// L's +0x154 is its own record's slot byte, whichever spawn built it (the 0x0C's
		// store, the 0x18's). Its DcbId (+0x7C) is the plyr class init's 10000 + that byte
		// (world::entity_dcb_id): the 0x18 runs the init after its store, and a 0x0C body,
		// whose own init ran ahead of the store, takes it again at the mission start's
		// re-init of every pool-0 row, ahead of Player_InitPlayer, where L is born
		// (D-NET-NEW-L5-18).
		// [orig: NapiNPClientMsg_0x00C @0x42E730 - Entity_InitFromItemDef @0x42E84D ahead of
		//  the +0x154 store @0x42EA06; NapiNPClientMsg_FullEntitySpawn @0x433780 - the store
		//  @0x433BBB ahead of the init call @0x433E82; Game_StartMission @0x524360 -
		//  NapiClient_WaitForGameStart @0x524E29, Entity_InitAllFromModels @0x52567F (no
		//  role gate), Player_InitPlayer @0x525BBC; Entity_InitAllFromModels @0x40E460 - the
		//  pool-0 init callbacks @0x40E5DA..0x40E60B; PlayerClass_InitEntity @0x4B1060 -
		//  10000 + [+0x154] @0x4B1149..0x4B1155]
		if (world::Entity *body = world.registry.get(h))
			if (const replication::ClientEntityState *row = rt.state().find(rt.self_handle()))
				body->player_slot_id = row->spawn_byte_154;
		// L is born after the boot's definition sweeps: bind its items.def row
		// now, as the host's L was at boot (without it L kept the "default"
		// sound profile, which authors no slots: no footsteps, foley, landings
		// or death scream).
		if (local_spawned_) kernel.bind_spawned_body(h);
		// Arm L the way the host's own spawn does at Player_InitPlayer time: the
		// shell applied the profile kit/class BEFORE L existed (the pre-spawn
		// apply latched it into the inventory), so stamp the deferred class +
		// damage classes + equipped adm on the fresh entity now. [orig:
		// Player_InitPlayer weapon leg @ 0x4e15f0; equippedAdmIndex stamp @ 0x4dd727]
		if (world::Entity *L = world.registry.get(h)) {
			// Player_InitPlayer ends by clearing the local player's hidden bit, on a
			// joiner as on a host, at every mission start.
			// [orig: Game_StartMission @0x525BBC -> Player_InitPlayer @0x4E15F0,
			//  `and dword ptr [eax+24h], 0FFFFFFFEh` @0x4E19C2]
			L->hidden = false;
			L->flags &= ~world::kEntityFlagCarried;
			// Then the mission start's own block on a non-null local player, a joiner's L
			// as the host's (HostRole::init_local_player_at_mission_start): the indoors bit
			// and the medic-revive latch cleared, the hidden bit cleared, every proximity
			// list rebuilt, the hidden bit set again, so L leaves the mission start hidden;
			// the frame's record fold (fold_local_record, the 0x0A's Flags byte) and the
			// world-state load (apply_world_state_load, the 0x0F's clear) then decide it.
			// [orig: Game_StartMission @0x524360 - the null tests @0x525C70 / @0x525C9A,
			//  `and [eax+24h], 0FF7FFFFFh` @0x525C74, +0x1E0 = 0 @0x525C81,
			//  `and [eax+24h], 0FFFFFFFEh` @0x525C8C, Entity_BuildAllProximityLists
			//  @0x525C90, `or [eax+24h], edi` @0x525C9E (edi = 1 from @0x5255B9)]
			L->flags &= ~world::kEntityFlagIndoors;
			L->medic_reviving = false;
			if (world.collision != nullptr) world.collision->refresh_after_registry_change(world);
			L->flags |= world::kEntityFlagCarried;
			// The mission start's Player_InitPlayer class word: the side L's Team names, as
			// it is (no range test), whatever class the shell's pre-spawn apply latched
			// [orig: Player_InitPlayer @0x4E15F0 - @0x4E1809..0x4E1832; Game_StartMission
			//  @0x525BBC].
			stamp_init_player_class(*L);
			// No damage-class bytes: in a session only the host's accept writes them
			// (D-NET-691) [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515E92;
			//  WeaponLoadout_ApplyFromBuffer's in-session leg sends @0x565D94].
			if (lp.inventory_valid && lp.inventory.equipped_combo >= 0) {
				const world::WeaponInventorySlot *slot =
						lp.inventory.slot(lp.inventory.equipped_combo);
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
					if (lp.weapon.presentation_pending) {
						lp.weapon.presentation_pending = false;
						const world::WeaponTableEntry *def =
								world.tables.weapons.by_index(
										static_cast<uint8_t>(slot->adm_index));
						world::WeaponPresentationEvent event;
						event.tick = world.logic_tick;
						if (const world::Entity *local =
									world.registry.get(
											world.cached.local_player))
							event.world_position = local->position;
						event.switch_to_weapon =
								def != nullptr ? def->name : std::string();
						lp.weapon.events.push_back(std::move(event));
					}
				}
			}
		}
		// Retail polls live keys; a press during the join wait must not cross
		// the spawn edge as a queued shot/reload. Clear the consume-latches too;
		// the embedder clears its device-input latches and seeds the look
		// heading from the spawn facing in the hook.
		lp.weapon.fire_held = false;
		lp.weapon.fire_pressed = false;
		lp.weapon.reload_pressed = false;
		kernel.resolve_new_infantry_adm_ids();
		lp.reset_local_player_input(spawn.heading);
	}
}

// The player record's Flags apply: the record that names the local player folds
// the wire byte into its Flags under 0xE1 (a remote's under 0xFD), so the host's
// deploy-screen bit 0x01, the chute 0x20 and bits 0x40 / 0x80 land on L and ride
// back in the next C2S 0x0C, which writes L's Flags byte raw. Retail runs it in
// the receive, ahead of the frame's send block; L takes the latest record's byte
// once per folded record, as the record fold leaves it.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the byte read @0x4C0EFD..0x4C0EFF,
//  `cmp edi, g_LocalPlayerEntity` @0x4C12F7, `and ebx, 0E1h` @0x4C12FF, the store
//  @0x4C1311; the uplink's raw byte @0x4C1B17; Client_ProcessNetworkFrame @0x42C180,
//  the receive @0x42C228 ahead of the send block @0x42C482]
void JoinerRole::fold_local_record() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	ClientRuntime &rt = *runtime;
	if (!local_spawned_ || !world.cached.local_player.valid() || !rt.has_self_handle()) return;
	const replication::ClientEntityState *self = rt.state().find(rt.self_handle());
	if (self == nullptr || !self->state_flags_known ||
			self->compact_revision == local_flags_revision_seen_)
		return;
	world::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr) return;
	local_flags_revision_seen_ = self->compact_revision;
	// The record's stage first: its world sample becomes L's chase target, its yaw and
	// pitch the staged angles, and the chase restarts; L's own update then walks toward
	// it off the authority (world::player_body_network_chase, D-NET-NEW-L2-14).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - +0x240 @0x4C0FD7, +0x234..+0x23C
	//  @0x4C0FE4..0x4C0FF0, +0x244 @0x4C0FF6, +0x27C = 0 @0x4C0FFC]
	if (world::AiEntity *local_ai = world.ai.for_handle(world.cached.local_player)) {
		local_ai->net_smooth_target[0] = self->record_sample[0];
		local_ai->net_smooth_target[1] = self->record_sample[1];
		local_ai->net_smooth_target[2] = self->record_sample[2];
		local_ai->net_smooth_heading =
				static_cast<int32_t>(static_cast<uint32_t>(self->yaw_byte) << 24);
		local_ai->net_smooth_pitch =
				static_cast<int32_t>(static_cast<uint32_t>(self->pitch_byte) << 24);
		local_ai->net_interp_progress = 0;
	}
	// Every player record, the local one too, re-resolves the body's row by its class:
	// the camouflage item by the mission climate, rebound only when its index differs
	// from the body's (the local record skips the remote stores and lands here)
	// (D-NET-348's joiner half). A climate past the three the format names reads the
	// record's move-order byte as the type there (retail's quirk, no stock mission;
	// ours keeps the desert row: the PERMANENT note in D-NET-348's record).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the local jump @0x4C11D7..0x4C11DD
	//  to @0x4C1248, the climate pick @0x4C1248..0x4C129C (the byte @0x4C0EE1, @0x4C1298),
	//  ItemList_FindIndexByTypeId @0x4C129D, the compare `cmp eax, [edi+1Ch]; jz`
	//  @0x4C12A5..0x4C12A8, the stores @0x4C12B8..0x4C12D6, AnimMap_InitBoneTypes
	//  @0x4C12E0 / @0x4C12EC]
	if (!world.tables.item_rows.empty() && world.tables.class_cammo_installed &&
			world::item_index_by_type_id(
					world, world::player_cammo_type_id(world, local->player_class)) !=
					local->item_type_index)
		world::rebind_player_camouflage_row(world, *local);
	constexpr uint32_t kLocalRecordFlagsMask = 0xE1u;
	const uint32_t wire = self->state_flags;
	local->flags ^= (local->flags ^ wire) & kLocalRecordFlagsMask;
	local->engine_flags ^= (local->engine_flags ^ wire) & kLocalRecordFlagsMask;
	// Then the record's mount relation, inside the receive: the attach-or-detach runs
	// (the mount sync) and returns 1 on an attach attempt (the bone or the carrier
	// changed, or a control seat whose occupant is not this body) or on a detach of a
	// mounted body; for the local player the 1 then snaps Position to the record's world
	// sample the fold staged, so the same frame's 0x0C carries the new relation and the
	// snapped position (D-NET-NEW-L2-13). The frame's later mount sync still takes a
	// carrier this frame materialized.
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the stage +0x234..+0x23C
	//  @0x4C0FE4..0x4C0FF0, Entity_TryAttachOrDetach @0x436610 (the call @0x4C1317; the
	//  attach test @0x4366DD, Entity_GetBoneSlotType @0x434ED0 types 2 / 3 / 5, the detach
	//  test @0x436768), the local snap @0x4C1323..0x4C1343]
	world::Entity *carrier = self->carrier_handle != world::EntityHandle::kInvalid &&
					self->mount_bone != 0
			? replica_world_entity(world, world::EntityHandle{self->carrier_handle})
			: nullptr;
	bool edge = false;
	if (carrier != nullptr) {
		world::SeatType seat_type = world::SeatType::None;
		for (const world::Seat &seat : carrier->seats)
			if (seat.bone_index == self->mount_bone) {
				seat_type = seat.type;
				break;
			}
		const bool claims = world::is_vehicle_control_seat(seat_type) ||
				seat_type == world::SeatType::Gunner;
		// The carrier's occupant names this body as L or, before the mount sync
		// reconciles it, as H (one body in retail).
		const bool occupant_is_self = carrier->primary_occupant == local->handle ||
				carrier->primary_occupant.packed == rt.self_handle();
		edge = !local->mounted ||
				local->mount_target != world::EntityHandle{self->carrier_handle} ||
				local->mount_bone != self->mount_bone || (claims && !occupant_is_self);
	} else {
		edge = local->mounted;
	}
	const int32_t sample[3] = {self->record_sample[0], self->record_sample[1],
			self->record_sample[2]};
	// The record's carrier word whatever its bone: a floor or deck the host echoes with
	// bone 0 is the ground link too, not only a seat's carrier (carrier_handle keeps the
	// seat alone, D-NET-224), so a body standing on a building keeps it between its idle
	// resolves, whose skip band runs no ground probe (D-NET-NEW-L2-37).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the word @0x4C0E05..0x4C0E08, its
	//  resolve into ebp @0x4C0F9C..0x4C106D; Entity_MovementCollisionResolver @0x4B2BD0 -
	//  the skip band's return @0x4B2CBC..0x4B2CF5 ahead of the settle tail]
	const uint16_t wire_carrier = self->record_carrier;
	sync_authoritative_mount();
	if (edge) local->position.set_q16({sample[0], sample[1], sample[2]});
	// The ground link last: a mounted body takes its mount's, any other the record's
	// carrier (a deck or floor the host echoes with bone 0; none for 0xFFFF), so a
	// detach's 0x0C leaves unparented.
	// [orig: @0x4C1346..0x4C1358 - `parentEntity->groundEntity`, else the resolved
	//  record carrier (ebp; zeroed @0x4C0FAB for none)]
	const world::Entity *mount = local->mounted ? world.registry.get(local->mount_target) : nullptr;
	const world::Entity *record_carrier = wire_carrier != world::EntityHandle::kInvalid
			? replica_world_entity(world, world::EntityHandle{wire_carrier})
			: nullptr;
	local->ground_target = mount != nullptr ? mount->ground_target
			: record_carrier != nullptr        ? record_carrier->handle
			                                   : world::EntityHandle{};
}

// The 0x0A tail is the authoritative health source for the recipient's OWN
// player. H belongs to the host's handle space; apply that recipient-local
// scalar to the joiner's distinct motor entity L (the pump gates this phase on
// the frame's decoded health signal). A fresh-frame guard prevents
// ClientState's pre-frame zero default from killing L during the handshake.
// Once L is dead, positive health revives it only after the separate
// deployment release latched by the net-frame folds (the ACK-qualified 0x5A, or
// the self record's respawn edge), and only from a later tail. That edge also snaps L to H's redeployed authoritative
// pose before its next uplink can run.
// [orig: tail health read @0x430428; store to local Health @0x4305df]
void JoinerRole::apply_authoritative_health() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	if (local_spawned_ && world.cached.local_player.valid()) {
		const world::EntityHandle local_h = world.cached.local_player;
		world::Entity *local = world.registry.get(local_h);
		world::AiEntity *local_ai =
				world.ai.for_handle(local_h);
		if (local != nullptr && local_ai != nullptr) {
			// The tail's last word lands on L's +0x120 every frame that carries
			// it, alive or dead: retail's store has no gate past the local
			// entity's null test. [orig: NapiNPClientMsg_0x00A @0x430550 null
			//  test, `g_LocalPlayerEntity->Armor = v109` @0x4305EB]
			local->mana = rt.state().local_armor;

			// ClientRuntime latches authoritative spawn closed on a decoded zero
			// tail, even if a later packet in this recv pump carries stale positive
			// HP. The independent C2S 0x0E gameplay hold never forces health to zero.
			const int16_t health = rt.authoritative_spawn_released()
					? rt.state().local_health : 0;
			// The tail's DECREASE detector, ahead of the store: a signed
			// compare against the recipient's currently stored Health, and only
			// on a DROP does the handler fire its inline cosmetics -- the red
			// damage vignette +0x78 and the camera shake +0x0A, both capped at
			// 0xFF, with NO radar blip (this is not Player_OnDamageReceived).
			// The handshake's pre-release zero default is not a wire drop, so
			// the detector waits for the released tail.
			// [orig: NapiNPClientMsg_0x00A @0x43059a `cmp dx,[eax+0x11E]` /
			//  `jge` @0x4305a1 -> @0x4305a3..0x4305d4, store @0x4305df]
			if (rt.authoritative_spawn_released())
				world::screen_flash_arm_health_drop(lp.view.flash, lp.view.shake,
						health, local->health);
			if (health <= 0) {
				// A zero tail stores Health alone: the dead bit is the body's own
				// death edge, in this frame's entity update after the pump, so a 0x13
				// later in the same pump still finds L alive and its class callback
				// runs past the dead return (the seat leave's C2S 0x27, the re-arm).
				// The record loop after the tail raises the word to 1 when the self
				// record is alive (ClientRuntime::local_entity_health carries it).
				// [orig: NapiNPClientMsg_0x00A @0x42FEC0 (the Health store @0x4305DF, no Flags);
				//  NetPacket_SerializePlayerState @0x4C09C0 - the floor @0x4C11C4..0x4C11CE;
				//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the death edge
				//  @0x4B4BF1..0x4B4C02; Entity_HandleDamageAndTriggerZones @0x407720 (the
				//  dead return @0x40772A..0x40772F)]
				const int16_t stored =
						rt.authoritative_spawn_released() ? rt.local_entity_health() : int16_t{0};
				local->health = stored;
				local_ai->health = stored;
			} else if (redeploy_release_pending_ &&
					rt.state().health_updates_applied >
							redeploy_health_updates_at_release_) {
				// The recipient's own compact is deliberately priority-boosted
				// by the host. Use its redeployed pose, not the corpse pose,
				// before movement/uplink resumes.
				const replication::ClientEntityState *self =
						client_entity_for_handle(
								rt.state(), rt.self_handle());
				if (self != nullptr && self->seen_this_frame) {
					const int32_t heading =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->yaw_byte) << 24);
					const int32_t pitch =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->pitch_byte) << 24);
					// The record's words (D-NET-507): the own row's chase runs in the World's
					// entity pass, after this (D-NET-196).
					local->position.set_q16({self->record_sample[0], self->record_sample[1],
							self->record_sample[2]});
					local->heading = heading;
					local->pitch_bam = pitch;
					local->roll_bam = 0;
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

					local_ai->heading = heading;
					local_ai->pitch = pitch;
					local_ai->roll = 0;
					local_ai->body_pitch = 0;
					local_ai->health = health;
					local_ai->vel_x = 0;
					local_ai->vel_y = 0;
					local_ai->net_smooth_target[0] = self->record_sample[0];
					local_ai->net_smooth_target[1] = self->record_sample[1];
					local_ai->net_smooth_target[2] = self->record_sample[2];
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
					// The reset's tail rebuilds every slice, then L's own list, one
					// 0x0A after retail's record decode runs it (D-NET-687) [orig:
					// @0x4C113C -> Entity_ResetToSpawnState - @0x4B98EB, @0x4B98F1].
					if (world.ai.collision != nullptr)
						world.ai.collision->refresh_entity_proximity(world, *local);

					lp.weapon.fire_held = false;
					lp.weapon.fire_pressed = false;
					lp.weapon.reload_pressed = false;
					// The embedder clears its device-input latches, seeds the
					// look heading and resets its view and map; the kit is the
					// release's 0x5A, already folded (D-NET-378), or after a medic
					// revive, which sends none, the kit L died with (D-NET-379):
					// retail's client runs Player_InitPlayer only at the mission
					// start and a team change [orig: Player_InitPlayer @0x4E15F0,
					// called from Game_StartMission @0x525BBC and
					// NapiNPClientMsg_TeamAssign @0x431B14;
					// NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0].
					lp.reset_local_player_input(heading);
                    lp.reset_for_new_round();
					// Game_InitNewRound's deploy-hold and being-revived clears ran in the
					// receive, in the self record's respawn arm (ClientRuntime): a second
					// clear here would drop a 0x3A the host sent since.
					lp.respawn_init(/*rebuild_kit=*/false);
					redeploy_release_pending_ = false;
					redeploy_health_updates_at_release_ = 0;
				}
			} else if (local->alive && (local->flags & 2u) == 0u) {
				local->health = health;
				local_ai->health = health;
			}
			// The same tail's carried word, after the Health and Armor stores.
			apply_local_carried_object(world, *local);
		}
	}
}

// The 0x0A tail's carried word onto L's carried link. Retail resolves the word
// through the pool tables (0xFFFF, a pool past 4, a slot past the pool's
// capacity or a slot with no def all read as none); a fresh pickup (L carries
// nothing yet) in CTF or Flagball first aims the HUD's waypoint at the nearest
// enemy capture point; then a word naming another entity than L's +0x268
// drops what L carries and attaches the named one. The attach hides the
// object, links both ways, clears its scars and, for the three flags, puts
// back the def's own update; its pickup event is the authority's alone, and
// its non-flag sound reads dword_24E098C, which nothing writes (its one xref is
// that read @0x43C174).
// [orig: NapiNPClientMsg_0x00A @0x4305F2..0x43069A - the resolve
//  @0x4305F6..0x430644, the fresh-pickup lookup @0x430646..0x430667
//  (SpawnPoint_FindNearestEnemyCapturePoint @0x4DD180), the compare
//  @0x43066C..0x43067A, Entity_DropCarriedObject @0x430681 (@0x439DF0),
//  Entity_AttachCarriedObject @0x430695 (@0x43C130: the def test @0x43C135,
//  +0x268 @0x43C144, Flags |= 1 @0x43C14A, +0x170 @0x43C14F,
//  Scar_ClearEntriesByEntity @0x43C155, the flag ids @0x43C15A..0x43C172, the
//  update @0x43C191; Server_BroadcastFlagPickupEvent @0x517460 gated on
//  is_authority @0x517467)]
void JoinerRole::apply_local_carried_object(world::World &world, world::Entity &local) {
	ClientRuntime &rt = *runtime;
	const uint16_t packed = rt.state().local_carried_handle;
	world::Entity *carried = nullptr;
	if (packed != world::EntityHandle::kInvalid) {
		world::Entity *row = replica_world_entity(world, world::EntityHandle{packed});
		if (row != nullptr && row->has_item_def) carried = row;
	}
	const uint32_t type_word = rt.game_type();
	if (carried != nullptr && !local.mounted_child.valid() &&
			(type_word == game_type::kCaptureTheFlag || type_word == game_type::kFlagBall))
		world.script.waypoints.select_nearest_enemy_capture_point(world.registry, local, type_word);
	const world::EntityHandle held = local.mounted_child;
	if (carried != nullptr ? held == carried->handle : !held.valid()) return;
	if (held.valid()) {
		local.mounted_child = world::EntityHandle{};
		if (world::Entity *object = world.registry.get(held)) {
			object->primary_occupant = world::EntityHandle{};
			world::drop_object_from_carrier(world, *object, world::drop_carrier_pose(world, local));
		}
	}
	if (carried == nullptr) return;
	local.mounted_child = carried->handle;
	carried->flags |= world::kEntityFlagCarried;
	carried->primary_occupant = local.handle;
	world.out.scars.clear_entity(carried->handle);
	if (carried->item_id == 4091 || carried->item_id == 4093 || carried->item_id == 4095)
		carried->drop_motion = world::def_update_drop_motion(*carried);
}

// The S2C 0x0F world-state landing on L, once per decoded 0x0F. Retail writes
// the pose straight onto g_LocalPlayerEntity from the handler — Position,
// Yaw (+ g_LocalPlayerLookYaw), Pitch, Roll — and clears its hidden bit unless
// the death screen is up; the host sends it right after Server_PositionPlayer-
// ForSpawn, so it is the authoritative admission pose (a later 0x0F re-snaps a
// pose the 0x0C spawn record no longer matches). With no local entity the pose
// is skipped, exactly the handler's null test. The same handler's track leg
// (the route, or the map POI list) is apply_replica_track's.
// [orig: NapiNPClientMsg_0x00F @0x42E200 — `if (!g_LocalPlayerEntity)` skip,
//  the pose stores (Pitch @0x42E3E9, Roll @0x42E3F2), `Flags &= ~1` when
//  !g_DeathScreenActive; Server_OnPlayerJoin positions @0x51A786 then
//  serializes 0x0F @0x51A84C]
void JoinerRole::apply_world_state_load() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	const replication::ClientWorldStateLoad &ws = rt.state().world_state;
	if (ws.revision == world_state_revision_seen_) return;
	world_state_revision_seen_ = ws.revision;
	if (local_spawned_ && world.cached.local_player.valid()) {
		world::Entity *local = world.registry.get(world.cached.local_player);
		world::AiEntity *local_ai = world.ai.for_handle(world.cached.local_player);
		if (local != nullptr && local_ai != nullptr) {
			local->position.set_q16({ws.pos_x, ws.pos_y, ws.pos_z}); // the 0x0F words (D-NET-507)
			local->heading = ws.yaw_bam;
			local->pitch_bam = ws.pitch_bam;
			local->roll_bam = ws.roll_bam;
			local_ai->heading = ws.yaw_bam;
			local_ai->pitch = ws.pitch_bam;
			local_ai->roll = ws.roll_bam;
			// The body's look target follows the entity yaw (the next pack
			// mirrors target_heading back onto heading) and the staged
			// reconciliation target moves with the snap so the own-player
			// chase does not drag L back toward the pre-snap pose.
			local_ai->inf.target_heading = ws.yaw_bam;
			local_ai->inf.look_pitch = ws.pitch_bam;
			local_ai->net_smooth_target[0] = ws.pos_x;
			local_ai->net_smooth_target[1] = ws.pos_y;
			local_ai->net_smooth_target[2] = ws.pos_z;
			local_ai->net_smooth_heading = ws.yaw_bam;
			local_ai->net_smooth_pitch = ws.pitch_bam;
			local_ai->net_interp_progress = 0;
			local_ai->net_interp_steps = 0;
			// g_LocalPlayerLookYaw = the wire yaw.
			lp.input.look_heading = ws.yaw_bam;
			lp.input.look_pitch = ws.pitch_bam;
			if (!rt.state().death_screen_active) {
				local->hidden = false;
				local->flags &= ~world::kEntityFlagCarried;
			}
		}
	}
}

// Project the requester-local decoded 0x0A mount relationship onto L only
// after the host confirms C2S 0x26/0x27.
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180]
void JoinerRole::sync_authoritative_mount() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	if (!rt.has_self_handle() || !local_spawned_ ||
			!world.cached.local_player.valid())
		return;
	const replication::ClientEntityState *self =
			client_entity_for_handle(rt.state(), rt.self_handle());
	if (self == nullptr) return;
	world::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr) return;

	const bool wire_mounted =
			self->carrier_handle != world::EntityHandle::kInvalid &&
			self->mount_bone != 0 &&
			replica_world_entity(world, world::EntityHandle{
					self->carrier_handle}) != nullptr;
	if (wire_mounted) {
		world::Entity *carrier = replica_world_entity(
				world, world::EntityHandle{self->carrier_handle});
		// The retained spawn occupancy names H, while the client motor uses
		// L (section 5.38b). Reconcile only our confirmed seat's identity
		// before the world-side receive path resolves its previous occupant.
		for (world::Seat &seat : carrier->seats) {
			if (seat.bone_index == self->mount_bone &&
					seat.occupant.packed == rt.self_handle())
				seat.occupant = local->handle;
		}
		if (carrier->primary_occupant.packed == rt.self_handle())
			carrier->primary_occupant = local->handle;
	}
	// Retail repeats the claim check even for an unchanged carrier+bone.
	// A client consumes the confirmed assignment through the replacement
	// arm, not the authority's occupied/enemy-seat rejection path.
	// [orig: Entity_TryAttachOrDetach @0x4366DD; client attach @0x435BBA]
	const bool changed = world.vehicles.apply_confirmed_mount(local->handle,
			wire_mounted ? world::EntityHandle{self->carrier_handle}
			             : world::EntityHandle{},
			wire_mounted ? self->mount_bone : 0);

	// A designated-G EWeap uses the compact player's seat_type as an
	// authoritative selected-MountSlot echo: 1 = child slot, 2 = the groundEntity
	// vehicle slot. Apply this even when the carrier+bone relation is unchanged;
	// action 6 commonly changes only this byte. The legs read L's own parent as the
	// record's attach above left it (an L on foot, or one whose attach did not take,
	// runs them on what it rides, none on foot), with retail's gates alone: the
	// parent a mountable gun with the G capability, and for byte 2 its groundEntity
	// a type-1 EWeap (D-NET-NEW-L2-34). The slot init (prepare_weapon_slot, our
	// stand-in for the spawn-time MountSlot init) runs but gates nothing. Each leg
	// stores the mount's route bit and L's EquippedSlot alone, never L's AdmDef byte
	// (+0x2B0), which the next C2S 0x0C echoes (D-NET-NEW-L3-15's joiner leg).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - Entity_TryAttachOrDetach called
	//  @0x4C1317, L's parent read @0x4C1346, the byte @0x4C135B, the parent's null test
	//  @0x4C1367, Entity_IsMountableGun @0x4C1371, +0x326 & 2 @0x4C137E; byte 1
	//  `and byte [esi+312h], 0F7h` @0x4C1390 and +0x118 @0x4C139D; byte 2's groundEntity
	//  gates @0x4C13C1..0x4C13E5, `or byte [esi+312h], 8` @0x4C13EB and +0x118 @0x4C13F8;
	//  WeaponSlot_InitFromEntityDef @0x5466C0]
	bool route_changed = false;
	world::Entity *route_mount = nullptr;
	if (self->seat_type == 1u || self->seat_type == 2u) {
		world::Entity *mount = local->mounted && local->mount_target.valid()
				? world.registry.get(local->mount_target)
				: nullptr;
		if (mount != nullptr && world::mount_is_designated_g_gun(*mount)) {
			const bool select_parent = self->seat_type == 2u;
			world::Entity *parent =
					select_parent ? world.registry.get(mount->ground_target) : nullptr;
			if (!select_parent || world::designated_g_route_carrier(parent)) {
				(void)world.vehicles.prepare_weapon_slot(*mount);
				if (parent != nullptr) (void)world.vehicles.prepare_weapon_slot(*parent);
				route_changed =
						mount->primary_weapon_slot.redirect_to_parent_slot != select_parent;
				mount->primary_weapon_slot.redirect_to_parent_slot =
						select_parent;
				route_mount = mount;
			}
		}
	}
	if (changed || route_changed) {
		if (changed) {
			// The embedder refreshes its view effects and the mounted input
			// heading for the new relation.
			on_mount_changed();
		}
		world::sync_local_usegun_weapon_transition(world, lp.weapon);
	}
	// The leg's EquippedSlot store, L sitting on the gun (route_mount is L's own
	// parent): after the record's attach staged its switch-out above, L's slot becomes
	// the route's at once, so that switch-out never completes (the weapon walk pumps the
	// EquippedSlot alone) and the AdmDef byte keeps its def. Each frame stands in for
	// each record: the store is idempotent once the borrow holds the slot, and every
	// staging of a switch follows a record that moves L's seat (D-WPN-52 follow).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - Entity_TryAttachOrDetach called
	//  @0x4C1317, L's parent read @0x4C1346, the stores @0x4C139D / @0x4C13F8;
	//  WeaponAction_ProcessAllEntities @0x542690 (@0x5426AD)]
	if (route_mount != nullptr)
		world::local_usegun_store_equipped_slot(world, lp.weapon, *route_mount);
}

// Fold the latest complete phase-8 mounted-ammo sample into the exact
// MountSlot selected by retail's live route bit. Missing wire rows remain
// pending until materialization; invalid classes are consumed and ignored.
void JoinerRole::apply_mounted_ammo_update() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	const replication::ClientMountedAmmoState &ammo =
			rt.state().mounted_ammo;
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
	world::Entity *mount = kernel_->wire_header_world
			? materializer_.owned(world, handle)
			: world.registry.get(handle);
	// A streamed row can arrive in a later initial-state page. Unlike an invalid
	// item class, absence is not a verdict: retain this revision and retry after
	// the materializer has populated the exact packed slot.
	if (mount == nullptr) return;

	// The reader's inline route repeats the setter's ground checks; a failed one
	// stores nothing [orig: NapiNPClientMsg_0x00A @0x42FEC0 - @0x4304FD,
	//  @0x43050C..0x430524, the stores @0x43053D / @0x430541].
	world::WeaponSlotState *slot =
			world.vehicles.resolve_phase8_ammo_slot(*mount);
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
// same fix. Type-guarded against the row the replica rule gives that exact
// streamed identity: a wire-header mission's materialized row, or a full-BMS
// join's promoted one, the records and the promote's addeweap children (no
// spawn origin, riding their carrier's attachment row) alike; the joiner's own
// spawns (the spawn-origin sentinel, no attachment) are skipped. Those rows
// also mirror orientation/team because collision and seat consumers observe them.
namespace {

// The class update the joiner's world runs for a row no compact writes: an
// addeweap child whose world row resolved its anchor on the carrier it
// follows (the ewep update composes it), or a genx row (its ride and fall).
// Any other row keeps the replica's follow.
// [orig: the ewep row @0x82ABE0 -> Entity_UpdateTransformAndTurret @0x440CA0;
//  the genx row @0x82AC10 -> Entity_UpdateParentTransform @0x4A88B0]
replication::ClientEntityState::WorldClassUpdate world_class_update_for(
		const replication::ClientEntityState &es, const world::Entity &local) {
	using Update = replication::ClientEntityState::WorldClassUpdate;
	if (es.cls != EntityClass::NoNetworkCallback) return Update::None;
	const uint16_t carrier = replication::persistent_carrier_handle(es);
	if (carrier == world::EntityHandle::kInvalid) return Update::None;
	if (local.emplaced_update)
		return local.emplacement_pose_metadata_resolved &&
						local.emplacement_parent.packed == carrier
				? Update::Ewep
				: Update::None;
	return local.parent_follow_update ? Update::Genx : Update::None;
}

} // namespace

void JoinerRole::mirror_mission_entities() {
	for (replication::ClientEntityState &es : runtime->state().entities) mirror_mission_entity(es);
}

void JoinerRole::mirror_mission_entity(replication::ClientEntityState &es) {
	world::World &world = kernel_->world;
	const world::EntityHandle h{es.handle};
	const int pool = h.pool();
	if (pool < 1 || pool > 3) return;
	world::Entity *local = replica_world_entity(world, h);
	if (local == nullptr ||
			static_cast<uint16_t>(local->item_id) != es.type_id)
		return;
	// A flag's pose is the client's own between S2C 0x2F states: the
	// materializer lands each state on the registry row and the drop's
	// fall and ride move it after, so the presented row follows the
	// registry row here, not the other way round.
	// [orig: NapiNPClientMsg_0x02F @0x430E10, the one client writer of a
	//  flag's pose]
	if (kernel_->wire_header_world && replication::is_carry_objective(es.type_id)) {
		es.x = local->position.x.q16;
		es.y = local->position.y.q16;
		es.z = local->position.z.q16;
		if (local->veh.yaw_seeded) {
			es.heading_bam = local->veh.yaw_bam;
			es.pitch_bam = local->veh.air_pitch_bam;
			es.roll_bam = local->veh.air_roll_bam;
		}
		return;
	}
	// A row no compact writes runs the def's move function as its update,
	// with no role gate, and nothing on the wire writes its pose after the
	// 0x0D: where the world runs that update (the ewep compose at the
	// subType's anchor, the genx ride and fall), the 0x0D's pose seeds the
	// world row once per spawn and the row reads the world's pose back
	// from then on (D-NET-690).
	// [orig: NapiNPClientMsg_0x00D @0x432C40 -- Position @0x432D8F..0x432DB6,
	//  the euler words @0x432DCA..0x432DF8, def+0x158 -> +0x1C4 @0x43331A;
	//  Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53]
	es.world_class_update = world_class_update_for(es, *local);
	if (es.world_class_update !=
			replication::ClientEntityState::WorldClassUpdate::None) {
		if (es.world_seeded_spawn_revision != es.spawn_revision) {
			es.world_seeded_spawn_revision = es.spawn_revision;
			local->position.set_q16({es.x, es.y, es.z});
			local->veh.yaw_seeded = true;
			local->veh.yaw_bam = es.heading_bam;
			local->veh.air_pitch_bam = es.pitch_bam;
			local->veh.air_roll_bam = es.roll_bam;
			local->heading = es.heading_bam; // the wire's Yaw word (D-NET-NEW-L6-2)
			local->pitch_bam = es.pitch_bam;
			local->roll_bam = es.roll_bam;
			if (es.team_known) local->team = es.team;
			return;
		}
		es.x = local->position.x.q16;
		es.y = local->position.y.q16;
		es.z = local->position.z.q16;
		if (local->veh.yaw_seeded) {
			es.heading_bam = local->veh.yaw_bam;
			es.pitch_bam = local->veh.air_pitch_bam;
			es.roll_bam = local->veh.air_roll_bam;
			es.yaw_byte = static_cast<uint8_t>(static_cast<uint32_t>(es.heading_bam) >> 24);
		}
		// The class update's Flags bits: the ewep hide of a dead hull's
		// children and its clear on a live vehicle carrier.
		// [orig: Entity_UpdateTransformAndTurret @0x440CDD, @0x440EB3]
		constexpr uint8_t kClassBits = static_cast<uint8_t>(world::kEntityFlagCarried |
				world::kEntityFlagDead | world::kEntityFlagHusk);
		es.state_flags = static_cast<uint8_t>((es.state_flags & ~kClassBits) |
				(local->flags & kClassBits));
		return;
	}
	// Pools 1..3 are live client rows, not just presentation records. Keep
	// every world-side collision/seat consumer on the same full wire pose.
	const world::Entity *attachment_parent =
			local->emplacement_parent.valid()
			? world.registry.get(local->emplacement_parent)
			: nullptr;
	const uint16_t followed_carrier = replication::persistent_carrier_handle(es);
	const bool unresolved_persistent_attachment =
			es.parent_pose_valid && followed_carrier != 0xFFFFu &&
			!local->emplacement_pose_metadata_resolved &&
			local->emplacement_parent.packed == followed_carrier &&
			attachment_parent != nullptr &&
			attachment_parent->registry_spawn_id ==
					local->emplacement_parent_spawn_id;
	// A 0x0D zero heading is omitted on the wire after retail memset, so
	// heading_known remains false. Persistent parent recomposition still owns
	// an exact heading_bam; mirror it for unresolved children whose rigid wire
	// pose is the world-consumer source of truth.
	if (es.heading_known || unresolved_persistent_attachment)
		local->heading = es.heading_bam;
	local->pitch_bam = es.pitch_bam;
	local->roll_bam = es.roll_bam;
	if (es.team_known) local->team = es.team;
	// Watercraft prediction (§5.38e B-facet, D-NET-196): a placed cbot
	// vehicle's motion is owned by the WORLD-side client mover — stage the
	// latest wire sample + registers on a fresh compact and let
	// watercraft_client_tick chase + predict; the row is mirrored BACK
	// after the tick (mirror_predicted_vehicles).
	if (pool == 1 && es.cls == EntityClass::Vehicle) {
		const world::VehicleTraits *traits =
				world.vehicles.traits.get(local->item_id);
		if (traits != nullptr) {
			es.net_world_mover = true;
			// The row-side fallback chase selects its constant set by
			// family; only the sim resolves families (client_state.h note).
			es.net_air_family = traits->family ==
							world::VehicleFamily::Helicopter ||
					traits->family == world::VehicleFamily::Plane;
			world::Entity::VehicleMotorState &m = local->veh;
			// The reader's tail on each accepted record: the destroyed bit
			// snaps a client row that is not dead yet and kills it through
			// Entity_KillBySlotId(slot, 0, 0) while its Health word is
			// nonzero; Flags bits 0/3/4/5/7 then follow the wire (1/2/6 stay
			// the client's own) and the record's health word lands, zero for
			// the dead-pose form or a kill.
			// [orig: Entity_SerializeVehicleState @0x460560 — kill edge
			//  @0x460A1A..0x460AE2, Flags @0x460AEA..0x460AFC, Health store
			//  @0x460AF1..0x460AFF; the short form's zero @0x460688]
			if (es.state_flags_known && es.compact_revision != m.net_seen_revision) {
				int16_t health_word = static_cast<int16_t>(es.health_word);
				if ((es.state_flags & world::kEntityFlagDead) != 0u &&
						((local->flags | local->engine_flags) &
								world::kEntityFlagDead) == 0u) {
					local->position.x.q16 = es.x;
					local->position.y.q16 = es.y;
					local->position.z.q16 = es.z;
					if (m.yaw_seeded) m.yaw_bam = es.heading_bam;
					local->heading = es.heading_bam;
					if (local->health != 0) {
						health_word = 0;
						world::apply_item_state_event(world, *local, 0);
					}
				}
				const uint32_t wire_flags = es.state_flags;
				local->flags ^= (local->flags ^ wire_flags) & 0xB9u;
				local->engine_flags ^= (local->engine_flags ^ wire_flags) & 0xB9u;
				local->health = health_word;
			}
			// The mover freezes on the dead-pose/wreck form alone: the row
			// keeps its snapped wire pose (wreck eulers included) and the
			// mirror-back below yields via net_predicted [orig: the dead-pose
			// short form's frozen live stores @0x460930..0x460A50]. D-NET-66:
			// death stays a snap. Wire bit0 is not a vehicle freeze: no
			// vehicle mover tests it and the pool-1 update calls the mover
			// ungated [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53].
			// A carried record is not a freeze either, whatever its carrier
			// (a pool-1 deck, or the static a bridge, roof or ramp
			// resolves to): the reader composes it into a world sample,
			// stages that and lands the carrier as groundEntity, and the
			// family mover predicts from the sample while its carrier block
			// rides the ground link [orig: Entity_TransformLocalToWorld
			// @0x4608ce -> the staging @0x4607cd..0x4607fb, groundEntity
			// @0x460802; the not-driven leg @0x48B7F0; the carrier blocks
			// (vehicle_follow_carrier)].
			const bool wire_frozen = es.state_flags_known &&
					(es.state_flags & replication::kVehicleFlagDeadPose) != 0u;
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
				// The yaw rate and the tank's pivot latch as well: the frozen
				// row's fold would otherwise keep re-registering the fourth
				// loop from a latch the traction tail can no longer clear.
				m.wheel_rate_bam = 0;
				m.pivot_sound_latched = false;
				m.pivot_sound_prev_rate = 0;
				es.vehicle_vertical_velocity_pending = false;
				m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] =
						m.plat_acc[3] = 0;
				m.plat_at_rest = false;
				m.plat_porpoise = false;
				m.plat_planing = false;
				m.plat_bob_phase = 0.0f;
				local->position.x.q16 = es.x;
				local->position.y.q16 = es.y;
				local->position.z.q16 = es.z;
				return;
			}
			if (es.compact_revision != m.net_seen_revision) {
				const bool prediction_arming = !m.net_predicted;
				m.net_seen_revision = es.compact_revision;
				// Every live record lands its carrier as the ground link, or
				// null for the 0xFFFF form; the mover's own ground refresh
				// rewrites it every eighth tick [orig: Entity_SerializeVehicleState
				// @0x460802 (esi = the carrier, 0 @0x4607AF)].
				const world::Entity *record_carrier =
						es.carrier_handle != 0xFFFFu
								? replica_world_entity(world,
										  world::EntityHandle{es.carrier_handle})
								: nullptr;
				local->ground_target = record_carrier != nullptr
						? record_carrier->handle
						: world::EntityHandle{};
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
				// entity+0xA0 vertical velocity: every live record the
				// wire-dead gate admitted re-lands the family prediction's
				// slide_z [orig: the mode-2 store @0x46091e].
				if (es.vehicle_vertical_velocity_pending) {
					m.slide_z = es.vehicle_vertical_velocity;
					es.vehicle_vertical_velocity_pending = false;
				}
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
			return; // the world mover owns the registry position now
		}
	}
	local->position.x.q16 = es.x;
	local->position.y.q16 = es.y;
	local->position.z.q16 = es.z;
}

// The world->view half of the watercraft prediction: after the tick, the
// predicted registry pose becomes the presented row pose (present reads the
// client replicas, ADR 0011). Heading gains the mover's full BAM precision.
void JoinerRole::mirror_predicted_vehicles() {
	for (replication::ClientEntityState &es : runtime->state().entities)
		if (es.net_world_mover) mirror_predicted_vehicle(es);
}

void JoinerRole::mirror_predicted_vehicle(replication::ClientEntityState &es) {
	world::World &world = kernel_->world;
	const world::EntityHandle h{es.handle};
	world::Entity *local = replica_world_entity(world, h);
	if (local == nullptr || !local->veh.net_predicted) return;
	es.x = local->position.x.q16;
	es.y = local->position.y.q16;
	es.z = local->position.z.q16;
	es.heading_bam = local->veh.yaw_bam;
	local->heading = local->veh.yaw_bam;
	// Every family mover owns live attitude now: air/water solves plus the
	// ground/bike wheeled contact solve (Entity_ProcessTrackedVehiclePhysics
	// @0x47C1C0 client subset). Publish zero too: level is a real solved
	// pose, not a validity sentinel. A boxless ground row's motor never
	// advances the seeded wire attitude, so the mirror is stable there.
	const world::VehicleTraits *traits =
			world.vehicles.traits.get(local->item_id);
	const bool owns_attitude = traits != nullptr;
	if (owns_attitude) {
		es.pitch_bam = local->veh.air_pitch_bam;
		es.roll_bam = local->veh.air_roll_bam;
		// Mounted-pose and bone consumers read Entity pitch/roll, not the
		// motor registers. Keep the exact-handle materialized carrier coherent before
		// the post-mover attachment phase below.
		local->pitch_bam = local->veh.air_pitch_bam;
		local->roll_bam = local->veh.air_roll_bam;
	}
}

// Project persistent decoded remote poses into collision-only visual
// proxies: Player/Infantry rows join the person walk, pool-1 movers carry
// their authored collision geometry at the decoded pose. Wire H remains
// presentation identity; local World authority never receives a cloned
// entity or an H->L owner mapping. The World's entity update calls it at its
// proximity build (World::before_proximity_build, installed by the pump), so
// the pool-1 walk and the projectile pass ahead of it read the table the
// previous update built, and a row the receive spawned since joins no trace
// until then (D-NET-196).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9,
//  the Weapon_UpdateAllProjectiles call @0x4C223A, the Entity_BuildProximityLists_Pool01
//  call @0x4C240A; the build's other callers run at the load and the teleports only
//  (@0x43D4FD, @0x4C20F5, @0x4F7F43), and Game_StartMission's @0x525E63 sits behind
//  the is_authority jz @0x525E58]
void JoinerRole::refresh_wire_collision_proxies() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	if (world.ai.collision == nullptr) return;
	std::vector<world::WirePersonCollisionProxy> person_proxies;
	std::vector<world::WireDynamicCollisionProxy> dynamic_proxies;
	uint16_t self_wire_handle = world::EntityHandle::kInvalid;
	if (rt.has_self_handle())
		self_wire_handle = rt.self_handle();
	// The game loop's entity update rebuilds the person and dynamic proximity lists every
	// frame, the local player alive or dead: death sends the connection back to Driving,
	// not out of the game loop.
	// [orig: Entity_UpdateAllEntities @0x4c240a -> Entity_BuildProximityLists_Pool01
	//  @0x4B9340 (g_PersonProxCount @0x4b935a, no local-death test); its readers
	//  Physics_RaycastAgainstProximityList @0x4e4a30, Projectile_RaycastProximitySlots
	//  @0x4e57c4, Entity_MovementCollisionResolver @0x4b3ac6]
	if (rt.in_game_loop()) {
		for (const replication::ClientEntityState &entity :
				rt.state().entities) {
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
						kernel.wire_collision_shape_for_type(entity.type_id);
				world::WirePersonCollisionProxy proxy;
				proxy.wire_handle = entity.handle;
				proxy.position_q16 = world::FixedVec3{
						entity.x, entity.y, entity.z};
				proxy.bound_radius_q16 = shape.bound_radius_q16;
				proxy.uniform_scale_q16 = shape.uniform_scale_q16;
				proxy.bbox_center_q16 = shape.bbox_center_q16;
				// The posed bone walk's model and person_extra_radius's
				// inputs: the Flags word's Player bit, a seat mount and the
				// movement byte's moving bit.
				proxy.model_id = shape.model_id;
				proxy.player = (entity.rm_entity_flags & world::kEntityFlagPlayer) != 0;
				proxy.mounted = entity.carrier_handle != world::EntityHandle::kInvalid &&
						entity.mount_bone != 0;
				proxy.moving = (entity.move_input & world::Entity::kMoveOrderMoving) != 0;
				// The row's Flags dead bit as it stands at the build, which the
				// person repulsion's live re-read skips (D-NET-NEW-L4-2); a row
				// updated after the build carries its own
				// (sync_wire_collision_proxies_live).
				proxy.dead = (entity.rm_entity_flags & world::kEntityFlagDead) != 0;
				proxy.live_flags = wire_proxy_live_flags(entity);
				person_proxies.push_back(proxy);
				continue;
			}

			// Pool-1 movers (vehicles, emplacements, runtime items) project
			// their authored collision geometry at the decoded pose. Pool-2
			// statics keep colliding through the locally loaded mission set.
			if (wire_handle::pool(entity.handle) != wire_handle::kPoolItem)
				continue;
			const world::ResolvedCollisionShape shape =
					kernel.wire_collision_shape_for_type(entity.type_id);
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
			// The twin is the decoded row's own native row by the replica rule,
			// never a later same-type occupant of the handle (D-NET-676)
			// [orig: NapiNPClientMsg_EntityDeath @0x42EBC6, the slot address].
			if (const world::Entity *twin = replication::replica_row_entity(&materializer_,
						world, world::EntityHandle{entity.handle}, entity.type_id,
						kernel.wire_header_world))
				proxy.registry_twin = twin->handle;
			proxy.live_flags = wire_proxy_live_flags(entity);
			dynamic_proxies.push_back(proxy);
		}
	}
	// ClientState is persistent and frame-budgeted; omission from one 0x0A is
	// not a despawn signal, so this intentionally does not read seen_this_frame.
	// Known-dead bit 1 is retained too: retail dead bodies remain person blockers
	// and a destroyed vehicle's shell keeps blocking (husk-model substitution for
	// wire proxies is a tracked residual).
	world.ai.collision->replace_wire_collision_proxies(
			std::move(person_proxies), std::move(dynamic_proxies),
			self_wire_handle);
}

// Each decoded row's proxy takes the row's live words, which a row moved since the
// table was built carries (the receive's snaps, ClientRuntime's pool-1 visits, the
// pool-0 visits around the own body): a person its Position and Flags dead bit, a
// pool-1 mover its Position, heading and pitch / roll. The table's copies stay as
// built, and a row the table does not hold gets no proxy (D-NET-196).
// [orig: Entity_MovementCollisionResolver @0x4B2BD0 (g_PersonProxEntity @0x4B3B7A, the
//  live reads @0x4B3B89..0x4B3B99); Physics_RaycastAgainstProximityList @0x4E4A30 (the
//  Physics_RaycastAgainstBoneSections call @0x4E4C59); Projectile_RaycastProximitySlots
//  @0x4E5340 (g_DynProxEntity @0x4E5760, the Physics_RaycastAgainstBoneCollision call
//  @0x4E578D)]
void JoinerRole::sync_wire_collision_proxies_live() {
	if (kernel_->world.ai.collision == nullptr) return;
	for (const replication::ClientEntityState &row : runtime->state().entities)
		sync_wire_collision_proxy_live(row);
}

void JoinerRole::sync_wire_collision_proxy_live(const replication::ClientEntityState &row) {
	world::CollisionWorld *collision = kernel_->world.ai.collision;
	if (collision == nullptr) return;
	const world::FixedVec3 position{row.x, row.y, row.z};
	if (row.cls == EntityClass::Player || row.cls == EntityClass::Infantry) {
		collision->set_wire_person_proxy_live(row.handle, position,
				(row.rm_entity_flags & world::kEntityFlagDead) != 0, wire_proxy_live_flags(row));
	} else if (wire_handle::pool(row.handle) == wire_handle::kPoolItem) {
		collision->set_wire_dynamic_proxy_live(row.handle, position, row.heading_bam,
				row.pitch_bam, row.roll_bam, wire_proxy_live_flags(row));
	}
}

// The live Flags bits both proximity traces pass a row over on (0x2000001): the row's
// Flags bit 0 (the wire's low byte, the bit the build itself skips), and a pool-1 row's
// minefield mark, which its twin's lndm class init stamps (world::MinefieldSystem::
// initialize) (D-NET-196).
// [orig: Physics_RaycastAgainstProximityList @0x4E4A30 (the test @0x4E4C33);
//  Projectile_RaycastProximitySlots @0x4E5340 (the test @0x4E5767);
//  Entity_BuildProximityLists_Pool01 @0x4B9340 (the bit-0 skips @0x4B9382 / @0x4B93E4);
//  Entity_InitHardpoints @0x4417D0 (the mark @0x441A23)]
uint32_t JoinerRole::wire_proxy_live_flags(const replication::ClientEntityState &row) {
	uint32_t flags = row.state_flags_known ? (row.state_flags & world::kEntityFlagCarried) : 0u;
	if (wire_handle::pool(row.handle) == wire_handle::kPoolItem)
		if (const world::Entity *twin = replication::replica_row_entity(&materializer_,
					kernel_->world, world::EntityHandle{row.handle}, row.type_id,
					kernel_->wire_header_world))
			flags |= twin->engine_flags & 0x02000000u;
	return flags;
}

// The World's pool-1 slot leg on a joiner (World::pool1_slot_leg, installed by the pump):
// the slot's decoded row takes its visit at its own slot, right after the World's mover
// ran there, interleaved with the World's movers in slot order. A row the World's mover
// predicts takes its twin's pose (mirror_predicted_vehicle), any other row's twin takes
// the row's pose (mirror_mission_entity: a chased hull's chase, a deck rider's follow);
// then its proxy's live words, which the projectile pass after the walk reads past its
// slot gate on the previous build's copy (D-NET-196).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
//  Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 / @0x4C21E0 / @0x4C21E9, the
//  Weapon_UpdateAllProjectiles call @0x4C223A; Entity_UpdatePool1Slot @0x4B8DD0 - the
//  +0x1C4 mover @0x4B8E41..0x4B8E53; Projectile_RaycastProximitySlots @0x4E5340
//  (g_DynProxEntity @0x4E5760)]
void JoinerRole::visit_replica_pool1_slot(world::EntityHandle handle) {
	replication::ClientEntityState *row = runtime->view().visit_pool1_row(handle.packed);
	if (row == nullptr) return;
	if (row->net_world_mover)
		mirror_predicted_vehicle(*row);
	else
		mirror_mission_entity(*row);
	sync_wire_collision_proxy_live(*row);
}

} // namespace opennova::inmatch
