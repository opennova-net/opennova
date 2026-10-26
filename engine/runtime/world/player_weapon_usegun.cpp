// The local player's UseGun transition leg, split off player_weapon.cpp by responsibility:
// the borrow of a gun's (or its carrier's) persistent slot, the attach / swap / detach
// mounts the transition stages and their commits, the EquippedSlot def reads a borrow
// redirects, and a seat detach's mount of the slot it restores. The mount's own leg
// (mount_queue_switch_out) and the commit's mount (commit_mount) stay player_weapon.cpp's.
#include <runtime/world/player_weapon_internal.h>

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// The local player's g_PendingWeaponSlot as the switch state holds it, whichever mount
// stored it: the UseGun transition's (the gun's slot, or the slot its switch back
// restores), or a personal one, a key away from a gun's slot held on foot included
// (D-NET-420). `from` is the EquippedSlot's def, `to` the pending slot's; false with no
// pending slot.
bool local_pending_switch_defs(const World &world, const LocalPlayerWeapon &w,
		const WeaponInventory *inventory, const WeaponTableEntry *&from,
		const WeaponTableEntry *&to) {
	const WeaponTable &table = world.tables.weapons;
	const auto personal_def = [&](int32_t combo) -> const WeaponTableEntry * {
		const WeaponInventorySlot *slot = inventory != nullptr ? inventory->slot(combo) : nullptr;
		return slot != nullptr && slot->adm_index >= 0
				? table.by_index(static_cast<uint8_t>(slot->adm_index))
				: nullptr;
	};
	from = nullptr;
	to = nullptr;
	if (w.usegun_switch != LocalUseGunSwitch::kNone) {
		from = table.by_index(w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm);
		to = table.by_index(w.usegun_switch == LocalUseGunSwitch::kAttach ||
						w.usegun_switch == LocalUseGunSwitch::kSwap
				? w.usegun_pending_weapon_adm
				: w.usegun_saved_adm);
		return true;
	}
	if (w.switch_in_flight && inventory != nullptr) {
		from = w.usegun_slot_active ? table.by_index(w.usegun_weapon_adm)
		                            : personal_def(inventory->equipped_combo);
		to = personal_def(inventory->pending_combo);
		return true;
	}
	return false;
}

// The bound local player's view, whose words retail's mount writes as process globals;
// null for a weapon no LocalPlayer of this world holds.
PlayerViewState *local_weapon_view(World &world, const LocalPlayerWeapon &w) {
	LocalPlayer *local = world.local_player_state;
	return local != nullptr && &local->weapon == &w ? &local->view : nullptr;
}

// The UseGun transition's category compare: the EquippedSlot's def against the mounted
// slot's [orig: Player_MountWeaponSlot @0x4DFB7B..0x4DFB8B].
bool usegun_same_category(const World &world, uint8_t from_adm, uint8_t to_adm) {
	const WeaponTableEntry *from = world.tables.weapons.by_index(from_adm);
	const WeaponTableEntry *to = world.tables.weapons.by_index(to_adm);
	return from != nullptr && to != nullptr && from->category == to->category;
}

} // namespace

// A draw holds while the pending slot's def is Emplaced (the FSM's pending_emplaced).
// [orig: WeaponAction_SwitchTo @0x5431D0 - g_PendingWeaponSlot @0x543226..0x54322D, its
//  def's Flags 0x80 @0x54322F..0x54323E]
bool local_pending_slot_is_emplaced(const World &world, const LocalPlayerWeapon &w,
		const WeaponInventory *inventory) {
	const WeaponTableEntry *from = nullptr;
	const WeaponTableEntry *to = nullptr;
	return local_pending_switch_defs(world, w, inventory, from, to) && to != nullptr &&
			(to->flags & weapon_flag::kEmplaced) != 0;
}

// With a pending slot, SWITCHFROM is instant when the EquippedSlot's def or the
// pending slot's def is Emplaced (local_pending_switch_defs).
// [orig: WeaponAction_SwitchFrom @0x5433B0 - the local player and g_PendingWeaponSlot
//  @0x543417, the EquippedSlot def's Flags 0x80 @0x54342E, the pending def's
//  @0x543435..0x543448, the sentinel @0x54344A]
bool local_usegun_switch_is_instant(const World &world,
		const LocalPlayerWeapon &w, const WeaponInventory *inventory) {
	const WeaponTableEntry *from = nullptr;
	const WeaponTableEntry *to = nullptr;
	if (!local_pending_switch_defs(world, w, inventory, from, to)) return false;
	const int32_t flags = (from != nullptr ? from->flags : 0) |
			(to != nullptr ? to->flags : 0);
	return (flags & weapon_flag::kEmplaced) != 0;
}

void queue_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w,
		bool same_category) {
	// An at-once commit draws by it; a queued one's completing action decides.
	w.usegun_switch_action = same_category ? weapon_action::kSwitchRank
	                                       : weapon_action::kSwitchFrom;
	// An attach's or swap's mounted slot is a gun's; a detach's switch back mounts a personal
	// slot [orig: Entity_AttachToUseGunSlot @0x546B80 (@0x546C38, the owner @0x546C73);
	// Entity_DetachFromVehicle @0x4355F0 (@0x43565F)].
	const bool gun_slot = w.usegun_switch == LocalUseGunSwitch::kAttach ||
			w.usegun_switch == LocalUseGunSwitch::kSwap;
	// The mounted slot's zoom word, clamped by that slot's own def once the EquippedSlot
	// early-outs pass (an active weapon): an attach or swap mounts the gun's own slot,
	// whatever its route bit says; the switch back mounts the personal slot it stores (the
	// inventory's pending row).
	// [orig: Player_MountWeaponSlot @0x4DFA40 - the EquippedSlot tests @0x4DFA6B / @0x4DFA74,
	//  the clamp @0x4DFAC4..0x4DFB13; Entity_AttachToUseGunSlot `lea edi,[ebx+2B4h]; push
	//  edi` @0x546C27..0x546C37; Entity_AttachToVehicleSlot `lea ecx,[ebp+474h]; push ecx`
	//  @0x494827..0x494837; neither reads Entity_GetWeaponSlots @0x5460E0]
	if (w.active) {
		if (gun_slot) {
			if (Entity *gun = world.registry.get(w.usegun_pending_mount))
				mount_clamp_slot_zoom(world,
						world.tables.weapons.by_index(gun->primary_weapon_slot_adm),
						gun->primary_weapon_slot.scope_zoom);
		} else if (world.local_player_state != nullptr &&
				world.local_player_state->inventory_valid) {
			WeaponInventory &inventory = world.local_player_state->inventory;
			if (WeaponInventorySlot *slot = inventory.slot(inventory.pending_combo))
				if (slot->adm_index >= 0)
					mount_clamp_slot_zoom(world,
							world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index)),
							slot->scope_zoom);
		}
	}
	if (!w.active ||
			(w.usegun_slot_active &&
			 active_local_weapon_slot(world, w) == &w.slot)) {
		commit_local_usegun_weapon_switch(world, w);
		return;
	}
	// The attach's, swap's and detach's mounts are Player_MountWeaponSlot's, the weapon key's
	// leg: a refused or gated word stays stored with nothing retained.
	// [orig: Entity_AttachToUseGunSlot @0x546B80 (the Player_MountWeaponSlot call @0x546C38);
	//  Entity_DetachFromVehicle @0x4355F0 (@0x43565F)]
	mount_queue_switch_out(world, w, local_weapon_view(world, w), same_category,
			world.tables.weapons.by_index(gun_slot ? w.usegun_pending_weapon_adm
			                                       : w.usegun_saved_adm),
			/*personal_slot=*/!gun_slot, /*target_scope_zoom=*/nullptr);
}

void commit_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w) {
	if (w.usegun_switch == LocalUseGunSwitch::kNone) return;
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) {
		w.usegun_switch = LocalUseGunSwitch::kNone;
		w.usegun_slot_active = false;
		w.usegun_on_foot = false;
		w.usegun_mount = EntityHandle{};
		w.usegun_weapon_adm = 0xFF;
		w.usegun_pending_mount = EntityHandle{};
		w.usegun_pending_weapon_adm = 0xFF;
		w.usegun_saved_adm = 0xFF;
		w.usegun_switch_action = -1;
		w.usegun_pending_on_foot = false;
		w.switch_deferred_action = -1;
		w.active = false;
		w.first_person_model_adm = 0xFF;
		return;
	}

	uint8_t next_adm = 0xFF;
	WeaponSlotState *next_slot = nullptr;
	bool select_parent = w.usegun_switch == LocalUseGunSwitch::kAttach ||
			w.usegun_switch == LocalUseGunSwitch::kSwap;
	if (select_parent) {
		Entity *mount = world.registry.get(w.usegun_pending_mount);
		WeaponSlotState *resolved_slot = mount != nullptr
				? world.vehicles.resolve_mounted_ammo_slot(*mount)
				: nullptr;
		uint8_t resolved_adm =
				mount != nullptr ? mount->primary_weapon_slot_adm : 0xFF;
		if (mount != nullptr && resolved_slot != nullptr &&
				resolved_slot != &mount->primary_weapon_slot) {
			Entity *carrier = world.registry.get(mount->ground_target);
			if (carrier == nullptr ||
					resolved_slot != &carrier->primary_weapon_slot)
				resolved_slot = nullptr;
			else
				resolved_adm = carrier->primary_weapon_slot_adm;
		}
		if (resolved_slot == nullptr ||
				resolved_adm != w.usegun_pending_weapon_adm) {
			select_parent = false;
		} else {
			w.usegun_slot_active = true;
			w.usegun_mount = w.usegun_pending_mount;
			w.usegun_weapon_adm = w.usegun_pending_weapon_adm;
			next_adm = w.usegun_weapon_adm;
			next_slot = resolved_slot;
		}
	}
	if (!select_parent) {
		// A detach's switch back completes its mount: g_PendingWeaponSlot, the slot
		// local_weapon_detach_mount staged (or a later mount's store over it), becomes
		// the EquippedSlot whenever it is set, whatever it holds by then, and the AdmDef
		// byte takes that slot's def as it stands (none: 0xFF).
		// [orig: WeaponAction_SwitchFrom @0x5433B0 (the non-null test
		//  @0x54346C..0x543473, the store @0x543475, the stamp @0x543481..0x543496;
		//  AdmDef_GetIndexFromPtr @0x53FE60 (-1 for a null def)) /
		//  WeaponAction_SwitchRank @0x543500 (the store @0x543539, the stamp @0x54355A)]
		if (w.usegun_switch == LocalUseGunSwitch::kDetach &&
				world.local_player_state != nullptr &&
				world.local_player_state->inventory_valid) {
			WeaponInventory &inventory = world.local_player_state->inventory;
			const WeaponInventorySlot *pending = inventory.slot(inventory.pending_combo);
			if (pending != nullptr) {
				inventory.equipped_combo = inventory.pending_combo;
				w.usegun_saved_adm = pending->adm_index >= 0
						? static_cast<uint8_t>(pending->adm_index)
						: uint8_t{0xFF};
			}
		}
		w.usegun_slot_active = false;
		next_adm = w.usegun_saved_adm;
		next_slot = &w.slot;
	}

	player->equipped_adm_index = next_adm;
	const WeaponTableEntry *next_def = world.tables.weapons.by_index(next_adm);
	// SWITCHFROM draws the committed target through TryQueueSwitchTo. SWITCHRANK
	// swaps directly and must not disturb a persistent target slot's current
	// action/phase/next fields. [orig: commits @0x543475 / @0x543539]
	if (next_slot != nullptr && next_def != nullptr &&
			w.usegun_switch_action == weapon_action::kSwitchFrom)
		weapon_fsm_try_queue_switch_to(*next_slot);
	WeaponPresentationEvent event;
	event.tick = world.logic_tick;
	event.world_position = local_player_mission_position(world);
	event.switch_to_weapon = next_def != nullptr ? next_def->name : std::string();
	event.clear_weapon = next_def == nullptr;
	event.preserve_slot_state = true;
	const size_t event_index = w.events.size();
	w.events.push_back(std::move(event));
	if (next_def == nullptr) w.active = false;

	w.usegun_switch = LocalUseGunSwitch::kNone;
	// A detach's mount of a saved gun's slot leaves it held on foot, as a refused
	// attach's direct store does (D-WPN-48) [orig: Entity_DetachFromVehicle
	// @0x43563A..0x43565F -> Player_MountWeaponSlot @0x4DFA40; the completion
	// WeaponAction_SwitchFrom @0x5433B0 (@0x543475) / WeaponAction_SwitchRank
	// @0x543500 (@0x543539)].
	w.usegun_on_foot = select_parent && w.usegun_pending_on_foot;
	w.usegun_pending_on_foot = false;
	w.usegun_pending_mount = EntityHandle{};
	w.usegun_pending_weapon_adm = 0xFF;
	w.usegun_switch_action = -1;
	w.switch_deferred_action = -1;
	if (!w.usegun_slot_active) {
		w.usegun_mount = EntityHandle{};
		w.usegun_weapon_adm = 0xFF;
		w.usegun_saved_adm = 0xFF;
	}
	// The stored slot's def mounts with the store, the slot as it stands: the gun's, or the
	// personal one a detach's switch back restores. [orig: WeaponAction_SwitchFrom @0x5433B0
	//  (@0x543475) / WeaponAction_SwitchRank @0x543500 (@0x543539); WeaponAction_ProcessFrame
	//  @0x540E71]
	if (next_def != nullptr)
		w.events[event_index].installed = commit_mount(world, w, next_def->name, true);
}

// A seat-2 / seat-3 detach's mount of the slot it restores (local_weapon_detach_mount staged
// it), queued by the mount's own leg inside the detach, ahead of the attach a gun-to-gun move
// runs next [orig: Entity_DetachFromVehicle @0x4355F0 (@0x43562A..0x43565F);
//  Entity_ProcessVehicleAttach @0x435AA0 (@0x435BCE)]
static void stage_local_usegun_switch_back(World &world, LocalPlayerWeapon &w) {
	const uint8_t from_adm = w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
	w.usegun_pending_mount = EntityHandle{};
	w.usegun_pending_weapon_adm = 0xFF;
	w.usegun_pending_on_foot = false;
	w.usegun_switch = LocalUseGunSwitch::kDetach;
	queue_local_usegun_weapon_switch(world, w,
			usegun_same_category(world, from_adm, w.usegun_saved_adm));
}

void sync_local_usegun_weapon_transition(World &world, LocalPlayerWeapon &w) {
	if (!world.cached.local_player.valid()) return;
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	// UseGun and an EWeap carrier's ctrlx seat both borrow the carrier's
	// persistent slot through the same Player_MountWeaponSlot call.
	// [orig: Entity_AttachToUseGunSlot @0x546B80 (the Player_MountWeaponSlot call
	//  @0x546C38); Entity_AttachToVehicleSlot @0x4946D0 (the EWeap gate
	//  @0x49480B..0x49480F, the call @0x494838)]
	Entity *mounted_parent = player->mounted
			? world.registry.get(player->mount_target) : nullptr;
	if (player->mount_type != SeatType::Gunner &&
			!(player->mount_type == SeatType::Controller && mounted_parent != nullptr &&
			  (mounted_parent->item_attrib & kItemAttribEweap) != 0))
		mounted_parent = nullptr;
	WeaponSlotState *mounted_slot = mounted_parent != nullptr
			? world.vehicles.resolve_mounted_ammo_slot(*mounted_parent)
			: nullptr;
	uint8_t mounted_adm = mounted_parent != nullptr
			? mounted_parent->primary_weapon_slot_adm
			: 0xFF;
	if (mounted_parent != nullptr && mounted_slot != nullptr &&
			mounted_slot != &mounted_parent->primary_weapon_slot) {
		Entity *carrier = world.registry.get(mounted_parent->ground_target);
		if (carrier == nullptr ||
				mounted_slot != &carrier->primary_weapon_slot)
			mounted_slot = nullptr;
		else
			mounted_adm = carrier->primary_weapon_slot_adm;
	}
	const bool on_usegun = mounted_parent != nullptr &&
			player->use_gun_slot_swapped && mounted_slot != nullptr &&
			mounted_adm != 0xFF;
	const auto stage_parent = [&](Entity &p_mount, uint8_t target_adm) {
		// No camera reset here: the UseGun attach's, like a control seat's, ran
		// at the attach (vehicle_attach.cpp attach_apply), and a designated-G
		// route switch resets nothing (D-WPN-49) and never stages here: its route
		// leg stores the EquippedSlot alone (D-WPN-52, below).
		// [orig: Entity_AttachToUseGunSlot @0x546B80 (the
		//  Player_ResetCameraAndMovementState call @0x546ba4); the route leg's
		//  EquippedSlot stores NetPacket_SerializePlayerState @0x4c139d / @0x4c13f8,
		//  NapiNPServerMsg_HandleWeaponToggle @0x511b08 / @0x511b38]
		// An attach while L holds no EquippedSlot (a kit with no selectable slot)
		// takes the direct leg: +0x308 = the null slot and the EquippedSlot = the
		// gun's slot at once, with no mount, no switch and no AdmDef (+0x2B0) stamp;
		// the first-person model follows the slot's def. A later seat detach finds
		// no slot to restore (local_weapon_detach_mount) (D-WPN-52).
		// [orig: Entity_AttachToUseGunSlot @0x546B80 - the null EquippedSlot test
		//  @0x546C23..0x546C25, +0x308 = the null slot @0x546C15 / @0x546C48, the
		//  EquippedSlot @0x546C42..0x546C4E; Entity_AttachToVehicleSlot @0x4946D0 -
		//  @0x49481D..0x494825, @0x494867..0x494898]
		if (!w.usegun_slot_active && world.local_player_state != nullptr &&
				!world.local_player_state->equipped_slot_held()) {
			local_usegun_store_equipped_slot(world, w, p_mount);
			w.usegun_saved_adm = 0xFF;
			return;
		}
		// The personal def the switch-out leaves: the AdmDef byte, which the
		// attach left as the last switch completion stamped it.
		if (!w.usegun_slot_active)
			w.usegun_saved_adm = player->equipped_adm_index;
		// The attach's mount stores g_PendingWeaponSlot over whatever a mount
		// before it staged there: a personal mount still in flight (the previous
		// control seat's detach mount, run by the same process ahead of this
		// attach) no longer commits; the switch-out's completion takes the
		// carrier's slot. [orig: Player_MountWeaponSlot @0x4DFA40 (the
		// g_PendingWeaponSlot store @0x4DFB16), called from
		// Entity_AttachToVehicleSlot @0x4946D0 (the call @0x494838) and
		// Entity_AttachToUseGunSlot @0x546B80 (the call @0x546C38); the earlier
		// detach, Entity_ProcessVehicleAttach @0x435AA0 (the
		// Entity_DetachFromVehicle call @0x435BCE)]
		w.switch_in_flight = false;
		w.usegun_pending_mount = p_mount.handle;
		w.usegun_pending_weapon_adm = target_adm;
		w.usegun_pending_on_foot = false;
		w.usegun_switch = w.usegun_slot_active ? LocalUseGunSwitch::kSwap
		                                       : LocalUseGunSwitch::kAttach;
		// The AdmDef byte stays the outgoing slot's until the switch completion
		// stamps the new one (NEW-L7-3) [orig: WeaponAction_SwitchFrom @0x543496;
		// WeaponAction_SwitchRank @0x54355A].
		const uint8_t from_adm =
				w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
		queue_local_usegun_weapon_switch(world, w,
				usegun_same_category(world, from_adm, target_adm));
	};

	if (on_usegun) {
		w.usegun_on_foot = false; // the seat's borrow names the slot now
		w.usegun_pending_on_foot = false;
		const bool active_matches = w.usegun_slot_active &&
				w.usegun_mount == mounted_parent->handle &&
				w.usegun_weapon_adm == mounted_adm;
		const bool pending_matches =
				(w.usegun_switch == LocalUseGunSwitch::kAttach ||
				 w.usegun_switch == LocalUseGunSwitch::kSwap) &&
				w.usegun_pending_mount == mounted_parent->handle &&
				w.usegun_pending_weapon_adm == mounted_adm;
		if (w.usegun_switch == LocalUseGunSwitch::kNone) {
			if (active_matches) {
			} else if (w.usegun_slot_active && w.usegun_mount == mounted_parent->handle) {
				// A designated-G route switch: the same gun, its slot routed to or
				// from its carrier. The route legs store the route bit and the
				// EquippedSlot and return: no switch action, no AdmDef stamp; the
				// weapon walk runs the new slot as it stands, and the first-person
				// model follows the EquippedSlot's def.
				// [orig: NetPacket_SerializePlayerState @0x4C1390..0x4C139D /
				//  @0x4C13EB..0x4C13F8; NapiNPServerMsg_HandleWeaponToggle @0x511A70
				//  (@0x511AFB..0x511B08, @0x511B2C..0x511B38); WeaponAction_ProcessAllEntities
				//  @0x5426AD (the EquippedSlot read); Player_RenderFirstPersonViewModel
				//  @0x4DED60]
				w.usegun_weapon_adm = mounted_adm;
				if (const WeaponTableEntry *def = world.tables.weapons.by_index(mounted_adm)) {
					WeaponPresentationEvent event;
					event.tick = world.logic_tick;
					event.world_position = local_player_mission_position(world);
					event.switch_to_weapon = def->name;
					event.preserve_slot_state = true;
					// The routed slot's def mounts with the store (commit_mount).
					event.installed = commit_mount(world, w, def->name, true);
					w.events.push_back(std::move(event));
				}
			} else {
				stage_parent(*mounted_parent, mounted_adm);
			}
		} else if (!pending_matches) {
			// A later attach overwrites g_PendingWeaponSlot without changing the
			// outgoing slot. This includes direct old-gun -> new-gun swaps.
			stage_parent(*mounted_parent, mounted_adm);
		}
		return;
	}

	// A borrow held on foot names no seat to leave: only a mount replaces it
	// (D-NET-420) [orig: Entity_AttachToUseGunSlot @0x546C4E]. Nor does the
	// detach's own mount of a saved gun's slot, pending until its completion
	// (D-WPN-48).
	if (w.usegun_on_foot && w.usegun_switch == LocalUseGunSwitch::kNone) return;
	if (w.usegun_pending_on_foot &&
			(w.usegun_switch == LocalUseGunSwitch::kAttach ||
			 w.usegun_switch == LocalUseGunSwitch::kSwap))
		return;
	// A borrow off its seat with no detach that staged its switch back stages it here.
	if ((w.usegun_slot_active ||
			 w.usegun_switch == LocalUseGunSwitch::kAttach ||
			 w.usegun_switch == LocalUseGunSwitch::kSwap) &&
			w.usegun_switch != LocalUseGunSwitch::kDetach)
		stage_local_usegun_switch_back(world, w);
}

// The def of the slot a borrow of `gun` holds: its own, or its carrier's when the
// route sends it there; 0xFF with no slot.
static uint8_t borrowed_slot_adm(const World &world, const Entity &gun) {
	const WeaponSlotState *slot = world.vehicles.resolve_mounted_ammo_slot(gun);
	if (slot == nullptr) return 0xFF;
	if (slot == &gun.primary_weapon_slot) return gun.primary_weapon_slot_adm;
	const Entity *carrier = world.registry.get(gun.ground_target);
	return carrier != nullptr ? carrier->primary_weapon_slot_adm : uint8_t{0xFF};
}

// The gun whose slot L's borrow holds as the EquippedSlot (active_local_weapon_slot's
// borrowed leg); null when no borrow holds a resolved slot and the EquippedSlot is the
// personal slot.
static const Entity *local_borrowed_gun(const World &world, const LocalPlayerWeapon &w) {
	if (!w.usegun_slot_active || !w.usegun_mount.valid()) return nullptr;
	const Entity *gun = world.registry.get(w.usegun_mount);
	return gun != nullptr && world.vehicles.resolve_mounted_ammo_slot(*gun) != nullptr
			? gun : nullptr;
}

// The def of L's EquippedSlot, the slot the weapon walk pumps (active_local_weapon_slot):
// a borrow's resolved slot (the gun's own, or its carrier's on a designated-G route, whose
// def neither a route switch nor the route leg's store stamps into the AdmDef byte,
// D-WPN-52), else the personal slot, which holds the installed def. `has_def` is false
// for a slot with no def; the adm is the def's index by name (0xFF for one the table
// lacks).
uint8_t local_equipped_slot_def_adm(const World &world, const LocalPlayerWeapon &w,
		bool &has_def) {
	if (const Entity *gun = local_borrowed_gun(world, w)) {
		const uint8_t slot_adm = borrowed_slot_adm(world, *gun);
		has_def = slot_adm != 0xFF;
		return slot_adm;
	}
	has_def = !w.def_name.empty();
	return has_def ? static_cast<uint8_t>(world.tables.weapons.index_of(w.def_name.c_str()))
	               : uint8_t{0xFF};
}

const WeaponFsmDef *local_equipped_slot_def(const World &world, const LocalPlayerWeapon &w) {
	// [orig: WeaponAction_SwitchFrom @0x5433B0 / WeaponAction_SwitchRank @0x543500 - the
	//  commits' EquippedSlot stores @0x543475 / @0x543539; the readers take the slot's def
	//  +0x20: HUD_DrawPowerThrowChargeBar @0x59986C, WeaponSlot_HasAmmoAvailable @0x541B39]
	// The personal slot's def is the installed one, which the commit that stores the slot
	// mounts (commit_mount): a detach's switch back reads its own def from the commit on.
	if (const Entity *gun = local_borrowed_gun(world, w)) {
		const WeaponTableEntry *row = world.tables.weapons.by_index(borrowed_slot_adm(world, *gun));
		return row != nullptr ? &row->action_fsm : nullptr;
	}
	return &w.def;
}

// The adm byte a local fire names: the firing slot's def, looked up by its name; only a
// slot with no def takes its owner's AdmDef byte (+0x2B0).
// [orig: WeaponAction_Fire @0x542B10 - the slot's def test @0x542C1C..0x542C21,
//  AdmDef_FindIndexByName(def + 0x14) @0x542C23..0x542C2F, the +0x2B0 fallback
//  @0x542C35..0x542C3F, the byte pushed as weaponFlags @0x542C55 to
//  Entity_FireWeaponAndSendPacket @0x542C5E]
uint8_t local_weapon_fire_adm(World &world, const LocalPlayerWeapon &w, const Entity &shooter) {
	bool has_def = false;
	const uint8_t slot_adm = local_equipped_slot_def_adm(world, w, has_def);
	return has_def ? slot_adm : shooter.equipped_adm_index;
}

// The first-person model is the EquippedSlot's def's: the borrowed slot's while a borrow
// holds it (its def adm the borrow keeps), else the personal slot's, whose def the
// AdmDef byte names.
// [orig: Player_RenderFirstPersonViewModel @0x4DED60 - the EquippedSlot @0x4DEDAA, its
//  Def @0x4DEDBC and the Def's fpModel test]
uint8_t local_first_person_model_def_adm(const LocalPlayerWeapon &w, const Entity &player) {
	return w.usegun_slot_active ? w.usegun_weapon_adm : player.equipped_adm_index;
}

// A designated-G route leg's EquippedSlot store for the local player, on each record that
// seats L on the gun: the gun's own slot (byte 1) or its carrier's (byte 2). The record's
// attach ran first in the same apply, and its mount queued the switch-out on the slot L
// held; the weapon walk pumps only the EquippedSlot, so that switch-out never completes.
// The borrow holds the stored slot at once, nothing stays staged, the AdmDef byte keeps
// the def it had, and the first-person model follows the slot's def.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - Entity_TryAttachOrDetach called
//  @0x4C1317 ahead of the stores @0x4C139D / @0x4C13F8; Entity_AttachToUseGunSlot
//  @0x546B80 -> Player_MountWeaponSlot (called @0x546C38; g_PendingWeaponSlot @0x4DFB16,
//  the switch-out queued on the EquippedSlot @0x4DFB96 / @0x4DFB9E);
//  WeaponAction_ProcessAllEntities @0x542690 (the EquippedSlot read @0x5426AD, the
//  WeaponAction_ProcessFrame call @0x5426C1)]
void local_usegun_store_equipped_slot(World &world, LocalPlayerWeapon &w, Entity &gun) {
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	const uint8_t slot_adm = borrowed_slot_adm(world, gun);
	if (slot_adm == 0xFF) return;
	if (w.usegun_slot_active && w.usegun_mount == gun.handle &&
			w.usegun_weapon_adm == slot_adm && w.usegun_switch == LocalUseGunSwitch::kNone)
		return;
	// The personal def a later detach mounts again: the byte the attach left unstamped.
	if (!w.usegun_slot_active && w.usegun_switch == LocalUseGunSwitch::kNone)
		w.usegun_saved_adm = player->equipped_adm_index;
	w.usegun_switch = LocalUseGunSwitch::kNone;
	w.usegun_switch_action = -1;
	w.switch_deferred_action = -1;
	w.usegun_pending_mount = EntityHandle{};
	w.usegun_pending_weapon_adm = 0xFF;
	w.usegun_pending_on_foot = false;
	w.usegun_slot_active = true;
	w.usegun_on_foot = false;
	w.usegun_mount = gun.handle;
	w.usegun_weapon_adm = slot_adm;
	if (const WeaponTableEntry *def = world.tables.weapons.by_index(slot_adm)) {
		WeaponPresentationEvent event;
		event.tick = world.logic_tick;
		event.world_position = local_player_mission_position(world);
		event.switch_to_weapon = def->name;
		event.preserve_slot_state = true;
		// The stored slot's def mounts with the store (commit_mount).
		event.installed = commit_mount(world, w, def->name, true);
		w.events.push_back(std::move(event));
	}
}

void local_weapon_direct_borrow(World &world, LocalPlayerWeapon &w, Entity &gun) {
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	const uint8_t adm = borrowed_slot_adm(world, gun);
	const WeaponTableEntry *def = world.tables.weapons.by_index(adm);
	if (def == nullptr) return;
	// The personal def the detach restore and the stage's outgoing slot read.
	if (!w.usegun_slot_active) w.usegun_saved_adm = player->equipped_adm_index;
	w.usegun_slot_active = true;
	w.usegun_on_foot = true;
	w.usegun_mount = gun.handle;
	w.usegun_weapon_adm = adm;
	w.usegun_switch = LocalUseGunSwitch::kNone;
	w.usegun_pending_mount = EntityHandle{};
	w.usegun_pending_weapon_adm = 0xFF;
	w.usegun_pending_on_foot = false;
	w.usegun_switch_action = -1;
	// The first-person model follows the EquippedSlot's def each frame
	// [orig: Player_RenderFirstPersonViewModel @0x4DED60], and the walk pumps the stored
	// slot with that def from the store on (commit_mount).
	WeaponPresentationEvent event;
	event.tick = world.logic_tick;
	event.world_position = local_player_mission_position(world);
	event.switch_to_weapon = def->name;
	event.preserve_slot_state = true;
	event.installed = commit_mount(world, w, def->name, true);
	w.events.push_back(std::move(event));
}

// The detach's mount of a gun's slot entity+0x308 saved: the borrow the
// transition stages for an attach, g_PendingWeaponSlot replaced, whose completion
// leaves the slot held on foot. [orig: Entity_DetachFromVehicle @0x43563A..0x43565F
// -> Player_MountWeaponSlot @0x4DFA40 (the store @0x4DFB16, the category compare
// @0x4DFB8B, SWITCHRANK @0x4DFB96 / SWITCHFROM @0x4DFB9E)]
static void stage_local_saved_gun_mount(World &world, LocalPlayerWeapon &w, Entity &gun,
		uint8_t adm) {
	const Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	if (!w.usegun_slot_active) w.usegun_saved_adm = player->equipped_adm_index;
	const uint8_t from_adm = w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
	w.switch_in_flight = false;
	w.usegun_pending_mount = gun.handle;
	w.usegun_pending_weapon_adm = adm;
	w.usegun_pending_on_foot = true;
	w.usegun_switch = w.usegun_slot_active ? LocalUseGunSwitch::kSwap : LocalUseGunSwitch::kAttach;
	const WeaponTableEntry *from = world.tables.weapons.by_index(from_adm);
	const WeaponTableEntry *to = world.tables.weapons.by_index(adm);
	queue_local_usegun_weapon_switch(world, w,
			from != nullptr && to != nullptr && from->category == to->category);
}

void local_weapon_detach_mount(World &world, LocalPlayerWeapon &w,
		WeaponInventory *inventory, PlayerViewState &view) {
	// The equipped slot and its def [orig: @0x4dfa5e..0x4dfa74].
	if (!w.active) return;
	// The detach mounts entity+0x308, else (no def of category < 11 there) the
	// current slot [orig: +0x308 read @0x43563a, the fallback
	// @0x43564e..0x435658]. A borrowed seat's +0x308 is the slot its attach saved:
	// the personal one, or a gun's the EquippedSlot was [orig: @0x494831;
	// @0x546c31 / @0x546c48]. An unborrowed seat's is the last SELECTED slot,
	// stale after a key switch or a cycle, which mount without selecting [orig:
	// Player_SelectWeaponSlot's writes @0x4dd6da / @0x4dd7a8 / @0x4dd81c are its
	// only others], and the player leaves holding it: the whole mount runs, a
	// same-category one queueing SWITCHRANK (D-WPN-48).
	// A saved gun's slot, borrowed seat or not: its mount stages a borrow the
	// player then holds on foot.
	Entity *saved_gun = inventory != nullptr && inventory->selected_mount != EntityHandle::kInvalid
			? world.registry.get(EntityHandle{inventory->selected_mount})
			: nullptr;
	const uint8_t saved_gun_adm = saved_gun != nullptr ? borrowed_slot_adm(world, *saved_gun) : 0xFF;
	const WeaponTableEntry *saved_gun_def = world.tables.weapons.by_index(saved_gun_adm);
	if (saved_gun_def != nullptr && saved_gun_def->category < 11) {
		stage_local_saved_gun_mount(world, w, *saved_gun, saved_gun_adm);
		w.nvg_scope_restore = false; // [orig: dword_B76554 = 0 @0x4dfb75]
		return;
	}
	// A borrow held on foot is no seat's: an unborrowed seat's detach mounts +0x308
	// over it as over a personal slot (D-NET-420).
	if ((!w.usegun_slot_active || w.usegun_on_foot) &&
			w.usegun_switch == LocalUseGunSwitch::kNone) {
		if (inventory == nullptr) return;
		const WeaponTable &table = world.tables.weapons;
		// A saved gun's slot whose def is of category 11 or more takes the fallback.
		int32_t target = inventory->selected_mount != EntityHandle::kInvalid
				? -1 : inventory->selected_combo;
		const WeaponInventorySlot *saved = inventory->slot(target);
		const WeaponTableEntry *saved_def = saved != nullptr && saved->adm_index >= 0
				? table.by_index(static_cast<uint8_t>(saved->adm_index))
				: nullptr;
		if (saved_def == nullptr || saved_def->category >= 11)
			target = inventory->current_slot; // [orig: Entity_DetachFromVehicle @0x4355F0 (@0x435650)]
		local_player_mount_weapon_slot(world, w, *inventory, view, target);
		return;
	}
	{
		// A borrowed seat's +0x308: the saved gun's slot (reaching here, its def is
		// of category 11 or more, or none), else the personal slot its attach saved,
		// else none. With no def of category < 11 there the detach mounts the current
		// slot word's slot instead, whichever slot the attach saved.
		// [orig: Entity_DetachFromVehicle @0x4355F0 - +0x308 @0x43563A, its def
		//  @0x435644..0x435649, the category test @0x43564B..0x43564E, the fallback
		//  slot[g_CurrentWeaponSlot] @0x435650..0x435658, the Player_MountWeaponSlot
		//  call @0x43565F]
		const WeaponTable &table = world.tables.weapons;
		const auto personal_def = [&](int32_t combo) -> const WeaponTableEntry * {
			const WeaponInventorySlot *slot = inventory->slot(combo);
			return slot != nullptr && slot->adm_index >= 0
					? table.by_index(static_cast<uint8_t>(slot->adm_index))
					: nullptr;
		};
		int32_t target_combo = -1;
		const WeaponTableEntry *target = nullptr;
		if (inventory == nullptr) {
			target = table.by_index(w.usegun_saved_adm);
		} else if (inventory->selected_mount != EntityHandle::kInvalid) {
			target = saved_gun_def;
		} else {
			target_combo = inventory->selected_combo;
			target = personal_def(target_combo);
		}
		if (target == nullptr || target->category >= 11) {
			target_combo = inventory != nullptr ? inventory->current_slot : -1;
			target = inventory != nullptr ? personal_def(target_combo) : nullptr;
		}
		if (target != nullptr && inventory != nullptr) {
			// The mount stores the target in g_PendingWeaponSlot, whose completion
			// makes it the EquippedSlot: the switch back staged below
			// (stage_local_usegun_switch_back) restores its def, and its commit the slot.
			// [orig: Player_MountWeaponSlot @0x4DFA40 (the store @0x4DFB16);
			//  WeaponAction_SwitchFrom @0x5433B0 (@0x543475) / WeaponAction_SwitchRank
			//  @0x543500 (@0x543539)]
			inventory->pending_combo = target_combo;
			w.usegun_saved_adm = static_cast<uint8_t>(inventory->slot(target_combo)->adm_index);
		}
		if (target == nullptr) {
			// The mount returns at once on the target's def [orig: @0x4dfa53..0x4dfa58]
			// and the detach writes no local player's EquippedSlot, so a borrow whose
			// attach saved no slot (the direct leg, D-WPN-52) stays the EquippedSlot:
			// the player leaves holding the gun's slot on foot, as after a refused
			// attach (D-NET-420). [orig: Entity_DetachFromVehicle @0x4355F0 - the
			// non-local restore @0x435669..0x435687 skipped for the local player]
			if (w.usegun_slot_active && w.usegun_switch == LocalUseGunSwitch::kNone)
				w.usegun_on_foot = true;
			return;
		}
	}
	w.nvg_scope_restore = false; // [orig: dword_B76554 = 0 @0x4dfb75]
	// The mount runs inside the detach (its queue on the gun's slot, its lower), ahead of the
	// attach a gun-to-gun move runs next; a staged switch back or a pending borrow held on
	// foot stages nothing, as the transition's own staging skips them.
	// [orig: Entity_DetachFromVehicle @0x4355F0 (Player_MountWeaponSlot @0x43565F);
	//  Entity_ProcessVehicleAttach @0x435AA0 (@0x435BCE)]
	if (w.usegun_switch != LocalUseGunSwitch::kDetach &&
			!(w.usegun_pending_on_foot && (w.usegun_switch == LocalUseGunSwitch::kAttach ||
					w.usegun_switch == LocalUseGunSwitch::kSwap)))
		stage_local_usegun_switch_back(world, w);
}

} // namespace opennova::world
