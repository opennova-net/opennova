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

#include <net/npwire/wire_handle.h>      // pool()/kPoolItem (the wire handle home)

#include <base/io/fixed.h>               // kFp16OneD, fp16_16_to_float

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

constexpr double kFixed16 = io::kFp16OneD;

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
	// The motor keeps the record's full word, never its whole-degree mirror
	// (D-NET-376) [orig: NapiNPClientMsg_0x00C @0x42E967 `mov [edi+10h], ecx`].
	spawn.heading_bam = s.orientation;
	spawn.team = s.team;
	// The named player record carries the host-stamped character selector and
	// packed minimap/character id; L keeps both (a zero animSlot recreated D-NET-146's
	// DBuggy shadow), where retail's Player_InitPlayer keeps the NetId but restamps animSlot
	// by Team (D-NET-570) [orig: Player_InitPlayer @0x4E15F0 - @0x4E1817..0x4E1843].
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
	return spawn;
}

} // namespace

world::Entity *JoinerRole::replica_world_entity(
		world::World &world, world::EntityHandle handle) {
	return materializer_.replica_world_entity(world, handle, kernel_->wire_header_world);
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
		const world::PlayerSpawn spawn = spawn_from_self(sp);
		const world::EntityHandle h = world::spawn_player(world, spawn);
		local_spawned_ = h.valid();
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
			if (lp.loadout.pending_player_class >= 5 &&
					lp.loadout.pending_player_class <= 9)
				L->player_class =
						static_cast<uint8_t>(lp.loadout.pending_player_class);
			world::local_loadout_sync_damage_classes(world, lp.loadout);
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
		lp.reset_local_player_input(world::player_spawn_heading(spawn));
	}
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
            if (const auto *self = client_entity_for_handle(rt.state(), rt.self_handle());
                    self != nullptr && self->state_flags_known) {
                const uint32_t chute = self->state_flags & world::kEntityFlagParachute;
                local->flags = (local->flags & ~world::kEntityFlagParachute) | chute;
                local->engine_flags = (local->engine_flags & ~world::kEntityFlagParachute) | chute;
            }
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
				local->health = health;
				local_ai->health = health;
				local->alive = false;
				local->flags |= 2u;
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
					local_ai->vel_y = 0;
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
					// revive, which sends none, the kit L died with (D-NET-379).
					lp.reset_local_player_input(heading);
                    lp.reset_for_new_round();
                    rt.reset_local_round_state();
					if (kit_seams.respawn) kit_seams.respawn();
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
//  serializes 0x0F @0x51A864]
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
			local->position = {
					static_cast<float>(static_cast<double>(ws.pos_x) / kFixed16),
					static_cast<float>(static_cast<double>(ws.pos_y) / kFixed16),
					static_cast<float>(static_cast<double>(ws.pos_z) / kFixed16),
			};
			local->yaw = static_cast<int16_t>(std::lround(
					world::mission_yaw_deg_from_bam_heading(ws.yaw_bam)));
			local->pitch = static_cast<int16_t>(std::lround(
					static_cast<double>(ws.pitch_bam) * world::kDegreesPerBam));
			local->roll = static_cast<int16_t>(std::lround(
					static_cast<double>(ws.roll_bam) * world::kDegreesPerBam));
			local_ai->pos[0] = ws.pos_x;
			local_ai->pos[1] = ws.pos_y;
			local_ai->pos[2] = ws.pos_z;
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
	// authoritative selected-MountSlot echo: 1 = child slot, 2 = the validated
	// groundEntity vehicle slot. Apply this even when the carrier+bone relation
	// is unchanged; action 6 commonly changes only this byte.
	// [orig: client player compact apply @0x4c1353 route legs]
	bool route_changed = false;
	if (wire_mounted && (self->seat_type == 1u || self->seat_type == 2u)) {
		world::Entity *mount = replica_world_entity(
				world, world::EntityHandle{self->carrier_handle});
		if (mount != nullptr) {
			// Retail does not blindly trust the authority byte: both route legs first
			// require Entity_IsMountableGun (non-type-1 EWeap) and the authored G
			// capability. Only the type-2 leg adds the groundEntity parent check.
			// [orig: compact apply @0x4c1353; Entity_IsMountableGun @0x434240]
			bool route_valid = mount->has_item_def && mount->item_type != 1u &&
					(mount->item_attrib & world::kItemAttribEweap) != 0u &&
					(mount->emplacement_attachment_flags & 0x02u) != 0u &&
					world.vehicles.prepare_weapon_slot(*mount);
			uint8_t equipped_adm = mount->primary_weapon_slot_adm;
			if (route_valid && self->seat_type == 2u && mount->ground_target.valid() &&
					mount->emplacement_parent == mount->ground_target &&
					mount->emplacement_parent_spawn_id != 0) {
				world::Entity *parent =
						replica_world_entity(world, mount->ground_target);
				route_valid = parent != nullptr &&
						parent->registry_spawn_id ==
								mount->emplacement_parent_spawn_id &&
						parent->has_item_def && parent->item_type == 1u &&
						(parent->item_attrib & world::kItemAttribEweap) != 0u &&
						world.vehicles.prepare_weapon_slot(*parent);
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
		// The embedder refreshes its view effects and the mounted input
		// heading for the new relation.
		on_mount_changed();
	}
	world::sync_local_usegun_weapon_transition(world, lp.weapon);
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

	world::WeaponSlotState *slot =
			world.vehicles.resolve_mounted_ammo_slot(*mount);
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
void JoinerRole::mirror_mission_entities() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	for (replication::ClientEntityState &es : rt.state().entities) {
		const world::EntityHandle h{es.handle};
		const int pool = h.pool();
		if (pool < 1 || pool > 3) continue;
		world::Entity *local = replica_world_entity(world, h);
		if (local == nullptr ||
				static_cast<uint16_t>(local->item_id) != es.type_id)
			continue;
		// A flag's pose is the client's own between S2C 0x2F states: the
		// materializer lands each state on the registry row and the drop's
		// fall and ride move it after, so the presented row follows the
		// registry row here, not the other way round.
		// [orig: NapiNPClientMsg_0x02F @0x430E10, the one client writer of a
		//  flag's pose]
		if (kernel_->wire_header_world && replication::is_carry_objective(es.type_id)) {
			es.x = world::to_fixed(local->position.x);
			es.y = world::to_fixed(local->position.y);
			es.z = world::to_fixed(local->position.z);
			if (local->veh.yaw_seeded) {
				es.heading_bam = local->veh.yaw_bam;
				es.pitch_bam = local->veh.air_pitch_bam;
				es.roll_bam = local->veh.air_roll_bam;
			}
			continue;
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
						local->position.x = opennova::io::fp16_16_to_float(es.x);
						local->position.y = opennova::io::fp16_16_to_float(es.y);
						local->position.z = opennova::io::fp16_16_to_float(es.z);
						if (m.yaw_seeded) m.yaw_bam = es.heading_bam;
						local->yaw = static_cast<int16_t>(std::lround(
								world::mission_yaw_deg_from_bam_heading(es.heading_bam)));
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
					local->position.x = opennova::io::fp16_16_to_float(es.x);
					local->position.y = opennova::io::fp16_16_to_float(es.y);
					local->position.z = opennova::io::fp16_16_to_float(es.z);
					continue;
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
				continue; // the world mover owns the registry position now
			}
		}
		local->position.x = opennova::io::fp16_16_to_float(es.x);
		local->position.y = opennova::io::fp16_16_to_float(es.y);
		local->position.z = opennova::io::fp16_16_to_float(es.z);
	}
}

// The world->view half of the watercraft prediction: after the tick, the
// predicted registry pose becomes the presented row pose (present reads the
// client replicas, ADR 0011). Heading gains the mover's full BAM precision.
void JoinerRole::mirror_predicted_vehicles() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	for (replication::ClientEntityState &es : rt.state().entities) {
		if (!es.net_world_mover) continue;
		const world::EntityHandle h{es.handle};
		world::Entity *local = replica_world_entity(world, h);
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
				world.vehicles.traits.get(local->item_id);
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
	if (rt.in_match()) {
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

} // namespace opennova::inmatch
