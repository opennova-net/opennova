#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/fire_sound.h>
// The local player's equipped-weapon cluster — moved verbatim from the shell
// binding (S7a, ADR 0028). The pump order, the UseGun borrow, PowerThrow, the
// install bake, and the presentation-event assembly are unchanged; the two
// wire legs (the joiner's fired descriptor, the reload relay) became output
// records the embedder's net layer routes.
#include <runtime/world/player_weapon.h>

#include <runtime/world/vehicle_mount.h>

#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/local_player.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/world/world.h>

#include <base/io/bam.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

using namespace opennova::def;

namespace opennova::world {

namespace {

Vec3 local_player_mission_position(const World &world) {
	const Entity *e = world.registry.get(world.cached.local_player);
	return e != nullptr ? e->position : Vec3{};
}

// The personal slot's zoom seed owner test: the local player, with the same
// permission the inventory fill passes (player_loadout.cpp).
bool local_slot_zoom_sniper_lock(const World &world, int32_t def_category) {
	const Entity *player = world.registry.get(world.cached.local_player);
	return player != nullptr && weapon_slot_zoom_sniper_lock(player->player_class,
			def_category, world.rules.allow_sniper_scope_zoom);
}

} // namespace

WeaponSlotState *active_local_weapon_slot(World &world, LocalPlayerWeapon &w) {
	if (w.usegun_slot_active && w.usegun_mount.valid()) {
		Entity *mount = world.registry.get(w.usegun_mount);
		if (mount != nullptr) {
			if (WeaponSlotState *slot = world.vehicles.resolve_mounted_ammo_slot(*mount))
				return slot;
		}
	}
	return &w.slot;
}

// The local player's Pitch store: the body, its look mirror and the
// input-owned look word move together.
void local_player_level_pitch(World &world) {
	if (AiEntity *body = world.ai.for_handle(world.cached.local_player)) {
		body->pitch = 0;
		body->inf.look_pitch = 0;
	}
	if (world.local_player_state) world.local_player_state->input.look_pitch = 0;
}

const WeaponSlotState *active_local_weapon_slot(const World &world,
		const LocalPlayerWeapon &w) {
	if (w.usegun_slot_active && w.usegun_mount.valid()) {
		const Entity *mount = world.registry.get(w.usegun_mount);
		if (mount != nullptr) {
			if (const WeaponSlotState *slot =
					world.vehicles.resolve_mounted_ammo_slot(*mount))
				return slot;
		}
	}
	return &w.slot;
}

WeaponSwitchGates local_weapon_switch_gates(const World &world,
		const LocalPlayerWeapon &w, const WeaponInventory *inventory) {
	WeaponSwitchGates gates;
	const Entity *e = world.registry.get(world.cached.local_player);
	if (e != nullptr && e->mounted) {
		// [orig: the parentSlot {2,3,5} stance gate @ 0x4e0192 — our SeatType enum
		//  carries the original's raw values: Controller=2, Gunner=3, Driver=5;
		//  passengers (1) keep switching. The equip-commit defer gates {2,3} only
		//  @ 0x4dd6fc.]
		const auto t = e->mount_type;
		gates.seat_blocked = t == SeatType::Controller ||
		                     t == SeatType::Gunner ||
		                     t == SeatType::Driver;
		gates.equip_blocked = t == SeatType::Controller ||
		                      t == SeatType::Gunner;
	}
	gates.equipped_valid = inventory != nullptr &&
			inventory->equipped_combo >= 0 &&
			inventory->slot(inventory->equipped_combo) != nullptr &&
			inventory->slot(inventory->equipped_combo)->adm_index >= 0;
	const WeaponSlotState *active_slot = active_local_weapon_slot(world, w);
	gates.equipped_action =
			w.active ? active_slot->current : weapon_action::kIdle;
	return gates;
}

bool local_usegun_switch_is_instant(const World &world,
		const LocalPlayerWeapon &w) {
	if (w.usegun_switch_action != weapon_action::kSwitchFrom) return false;
	const uint8_t from_adm =
			w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
	const uint8_t to_adm =
			(w.usegun_switch == LocalUseGunSwitch::kAttach ||
			 w.usegun_switch == LocalUseGunSwitch::kSwap)
			? w.usegun_pending_weapon_adm
			: w.usegun_saved_adm;
	const WeaponTableEntry *from = world.tables.weapons.by_index(from_adm);
	const WeaponTableEntry *to = world.tables.weapons.by_index(to_adm);
	const int32_t flags = (from != nullptr ? from->flags : 0) |
			(to != nullptr ? to->flags : 0);
	return (flags & weapon_flag::kEmplaced) != 0;
}

void queue_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w,
		bool same_category) {
	w.usegun_switch_action = same_category ? weapon_action::kSwitchRank
	                                       : weapon_action::kSwitchFrom;
	w.switch_deferred_action = w.usegun_switch_action;
	if (!w.active ||
			(w.usegun_slot_active &&
			 active_local_weapon_slot(world, w) == &w.slot)) {
		commit_local_usegun_weapon_switch(world, w);
		return;
	}
	WeaponSlotState *slot = active_local_weapon_slot(world, w);
	if (w.usegun_switch_action == weapon_action::kSwitchRank)
		weapon_fsm_queue_switch_rank(*slot);
	else
		weapon_fsm_queue_switch_from(*slot);
}

void commit_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w) {
	if (w.usegun_switch == LocalUseGunSwitch::kNone) return;
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) {
		w.usegun_switch = LocalUseGunSwitch::kNone;
		w.usegun_slot_active = false;
		w.usegun_mount = EntityHandle{};
		w.usegun_weapon_adm = 0xFF;
		w.usegun_pending_mount = EntityHandle{};
		w.usegun_pending_weapon_adm = 0xFF;
		w.usegun_saved_adm = 0xFF;
		w.usegun_switch_action = -1;
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
	w.events.push_back(std::move(event));
	if (next_def == nullptr) w.active = false;

	w.usegun_switch = LocalUseGunSwitch::kNone;
	w.usegun_pending_mount = EntityHandle{};
	w.usegun_pending_weapon_adm = 0xFF;
	w.usegun_switch_action = -1;
	w.switch_deferred_action = -1;
	if (!w.usegun_slot_active) {
		w.usegun_mount = EntityHandle{};
		w.usegun_weapon_adm = 0xFF;
		w.usegun_saved_adm = 0xFF;
	}
}

void sync_local_usegun_weapon_transition(World &world, LocalPlayerWeapon &w,
                                         PlayerViewState &view) {
	if (!world.cached.local_player.valid()) return;
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	// UseGun and an EWeap carrier's ctrlx seat both borrow the carrier's
	// persistent slot through the same Player_MountWeaponSlot call.
	// [orig: Entity_AttachToUseGunSlot @0x546C38; Entity_AttachToVehicleSlot
	//  EWeap gate @0x49480B..0x49480F, call @0x494838]
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
	const auto same_category = [&](uint8_t p_from, uint8_t p_to) {
		const WeaponTableEntry *from = world.tables.weapons.by_index(p_from);
		const WeaponTableEntry *to = world.tables.weapons.by_index(p_to);
		return from != nullptr && to != nullptr &&
				from->category == to->category;
	};
	const auto stage_parent = [&](Entity &p_mount, uint8_t target_adm) {
        // The local UseGun attach resets before its mount; a detach does not take
        // this leg. The reset includes the binocular clears, so a wire-echoed
        // attach drops a raised toggle too. A control seat's reset ran at its
        // attach, EWeap or not (vehicle_attach.cpp attach_apply).
        // [orig: Entity_AttachToUseGunSlot @0x546B80 (the
        //  Player_ResetCameraAndMovementState call @0x546ba4)]
		if (player->mount_type == SeatType::Gunner) local_player_camera_reset(&world, w, view);
		if (!w.usegun_slot_active)
			w.usegun_saved_adm = player->pre_use_gun_equipped_adm_index;
		w.usegun_pending_mount = p_mount.handle;
		w.usegun_pending_weapon_adm = target_adm;
		w.usegun_switch = w.usegun_slot_active ? LocalUseGunSwitch::kSwap
		                                       : LocalUseGunSwitch::kAttach;
		const uint8_t from_adm =
				w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
		// The shared helper applied the nonlocal immediate stamp. L retains its
		// outgoing EquippedSlot until the action handler's commit seam.
		player->equipped_adm_index = from_adm;
		queue_local_usegun_weapon_switch(world, w,
				same_category(from_adm, target_adm));
	};
	const auto stage_personal = [&]() {
		const uint8_t from_adm =
				w.usegun_slot_active ? w.usegun_weapon_adm : w.usegun_saved_adm;
		player->equipped_adm_index = from_adm;
		w.usegun_pending_mount = EntityHandle{};
		w.usegun_pending_weapon_adm = 0xFF;
		w.usegun_switch = LocalUseGunSwitch::kDetach;
		// Seats 2 and 3 share the detach restore through Player_MountWeaponSlot
		// (its NVG latch clear ran at the detach, local_weapon_detach_mount).
		// [orig: Entity_DetachFromVehicle @0x43562A..0x43565F]
		queue_local_usegun_weapon_switch(world, w,
				same_category(from_adm, w.usegun_saved_adm));
	};

	if (on_usegun) {
		const bool active_matches = w.usegun_slot_active &&
				w.usegun_mount == mounted_parent->handle &&
				w.usegun_weapon_adm == mounted_adm;
		const bool pending_matches =
				(w.usegun_switch == LocalUseGunSwitch::kAttach ||
				 w.usegun_switch == LocalUseGunSwitch::kSwap) &&
				w.usegun_pending_mount == mounted_parent->handle &&
				w.usegun_pending_weapon_adm == mounted_adm;
		if (w.usegun_switch == LocalUseGunSwitch::kNone) {
			if (!active_matches) stage_parent(*mounted_parent, mounted_adm);
		} else if (!pending_matches) {
			// A later attach overwrites g_PendingWeaponSlot without changing the
			// outgoing slot. This includes direct old-gun -> new-gun swaps.
			stage_parent(*mounted_parent, mounted_adm);
		}
		return;
	}

	if ((w.usegun_slot_active ||
			 w.usegun_switch == LocalUseGunSwitch::kAttach ||
			 w.usegun_switch == LocalUseGunSwitch::kSwap) &&
			w.usegun_switch != LocalUseGunSwitch::kDetach)
		stage_personal();
}

void local_weapon_detach_mount(World &world, LocalPlayerWeapon &w,
		const WeaponInventory *inventory) {
	// The equipped slot and its def [orig: @0x4dfa5e..0x4dfa74].
	if (!w.active) return;
	// The detach mounts entity+0x308, else (no def of category < 11 there) the
	// current slot [orig: +0x308 read @0x43563a, the fallback
	// @0x43564e..0x435658]. A borrowed seat's +0x308 is the personal slot its
	// attach saved [orig: @0x494831; @0x546c31 / @0x546c48]. An unborrowed
	// seat's is the last SELECTED slot, stale after a key switch or a cycle,
	// which mount without selecting [orig: Player_SelectWeaponSlot's writes
	// @0x4dd6da / @0x4dd7a8 / @0x4dd81c are its only others]; the port does not
	// model it and clears whenever the weapon is active, which differs only
	// where neither +0x308 nor the current slot has a def (D-WPN-48).
	if (w.usegun_slot_active || w.usegun_switch != LocalUseGunSwitch::kNone) {
		const WeaponTable &table = world.tables.weapons;
		const WeaponTableEntry *target = table.by_index(w.usegun_saved_adm);
		if (target == nullptr || target->category >= 11) {
			const WeaponInventorySlot *current =
					inventory != nullptr ? inventory->slot(inventory->equipped_combo) : nullptr;
			target = current != nullptr && current->adm_index >= 0
					? table.by_index(static_cast<uint8_t>(current->adm_index))
					: nullptr;
		}
		if (target == nullptr) return; // the target's def [orig: @0x4dfa53..0x4dfa58]
	}
	w.nvg_scope_restore = false; // [orig: dword_B76554 = 0 @0x4dfb75]
}

void commit_pending_weapon_switch(World &world, LocalPlayerWeapon &w,
		WeaponInventory *inventory) {
	// The pending -> equipped commit [orig: the switchfrom/switchrank completion
	// consumes g_PendingWeaponSlot; EquippedSlot swap + the equippedAdmIndex stamp
	// @ 0x4dd727; the FP model re-resolve runs shell-side off the event].
	// The completion leaves the NVG restore latch alone: the request's reset /
	// mount / select cleared it [orig: WeaponAction_SwitchFrom @0x5433b0 and
	// WeaponAction_SwitchRank @0x543500 call none of Player_SelectWeaponSlot,
	// Player_ResetCameraAndMovementState, Player_MountWeaponSlot].
	w.switch_in_flight = false;
	w.switch_deferred_action = -1;
	if (inventory == nullptr) return;
	Entity *e = world.registry.get(world.cached.local_player);
	const int32_t combo = inventory->pending_combo;
	const WeaponInventorySlot *slot = inventory->slot(combo);
	if (slot == nullptr || slot->adm_index < 0) return;
	inventory->equipped_combo = combo;
	// The entity stamp is the only entity-DEPENDENT half. Retail's selection has no
	// entity-existence precondition on the PRESENTATION half — the tail of every 0x5A
	// grant runs Player_SelectWeaponSlot against the pool it just rebuilt and the FP
	// viewmodel is re-resolved by a per-frame consumer off EquippedSlot [orig:
	// NapiNPClientMsg_HandleWeaponLoadoutSync tail @0x4296E3; Player_SelectWeaponSlot
	// @0x4DD680 restamps equippedAdmIndex @0x4dd727/@0x4dd7f5;
	// Player_RenderFirstPersonViewModel @0x4DED60]. A joiner applies its grant BEFORE
	// L exists, so dropping the notification here left the viewmodel and the weapon FSM
	// running the submitted weapon while the entity and the wire followed the granted
	// one. Latch it instead and replay exactly one event at the joiner spawn block.
	if (e != nullptr) e->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
	const WeaponTableEntry *def =
			world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	// SWITCHRANK commits in place; only a holster (or an initial mount) queues
	// the target's draw. Treating a fire-mode change as SWITCHFROM inserted a
	// second animation and its draw lockout after the mode-switch action.
	// [orig: WeaponAction_SwitchRank @0x543500 vs SwitchFrom @0x543475]
	w.start_in_switchto = !w.active ||
			active_local_weapon_slot(world, w)->current != weapon_action::kSwitchRank;
	if (e == nullptr) {
		w.presentation_pending = true;
		return;
	}
	// The entity was present: emit directly, exactly as before (the manual-switch and
	// armory paths are unchanged), and drop any latch so it cannot replay a duplicate.
	w.presentation_pending = false;
	WeaponPresentationEvent event;
	event.tick = world.logic_tick;
	event.world_position = local_player_mission_position(world);
	event.switch_to_weapon = def != nullptr ? def->name : std::string();
	w.events.push_back(std::move(event));
}

void handle_weapon_switch_outcome(World &world, LocalPlayerWeapon &w,
		WeaponInventory *inventory, const WeaponSwitchOutcome &out, PlayerViewState &view) {
    if (out.reset_view) {
        // Category requests reset at the admitted walk, before the outgoing
        // action completes. Cycling never sets this flag. [orig: @0x4E0223]
        local_player_camera_reset(&world, w, view);
    }
	switch (out.kind) {
		case WeaponSwitchOutcome::kDeny: {
			// [orig: Sound_PlayInterfaceTriggerSet(dword_24E08C4) @ 0x4e0354]
			WeaponPresentationEvent event;
			event.tick = world.logic_tick;
			event.world_position = local_player_mission_position(world);
			event.switch_denied = true;
			w.events.push_back(std::move(event));
			break;
		}
		case WeaponSwitchOutcome::kMount: {
			// [orig: Player_MountWeaponSlot @ 0x4dfa40 — pending already stamped by
			//  the walk; the OUTGOING slot's FSM plays SWITCHRANK (same category) or
			//  SWITCHFROM (cross category) and its completion commits]
			if (!w.active) {
				commit_pending_weapon_switch(world, w, inventory);
				break;
			}
			// The mount clears the NVG restore latch at the request, past its
			// early-outs (no equipped slot or def) [orig: @0x4dfa4d..0x4dfa74,
			// dword_B76554 = 0 @0x4dfb75].
			w.nvg_scope_restore = false;
			w.switch_in_flight = true;
			WeaponSlotState *active_slot = active_local_weapon_slot(world, w);
			const int32_t action = out.same_category
					? weapon_action::kSwitchRank
					: weapon_action::kSwitchFrom;
			if (active_slot->current == weapon_action::kSwitchTo) {
				// The witnessed writer refuses during SWITCHTO. Unlike the original
				// dispatcher, this shell supplies one press edge, so retain it beside
				// the already-stamped pending combo and keep restoring next after
				// SWITCHTO's delay-start initializer writes its resume action.
				w.switch_deferred_action = action;
				active_slot->next = action;
			} else if (active_slot->next == weapon_action::kSwitchTo) {
				// The draw is queued but has not entered yet. Preserve it; the
				// post-tick latch below attaches the requested outgoing action.
				w.switch_deferred_action = action;
			} else if (out.same_category) {
				w.switch_deferred_action = -1;
				weapon_fsm_queue_switch_rank(*active_slot);
			} else {
				w.switch_deferred_action = -1;
				weapon_fsm_queue_switch_from(*active_slot);
			}
			break;
		}
		default:
			break;
	}
}

void local_player_mount_weapon_slot(World &world, LocalPlayerWeapon &w,
		WeaponInventory &inventory, PlayerViewState &view, int32_t combo) {
	// [orig: Player_MountWeaponSlot @0x4DFA40]
	const WeaponTable &table = world.tables.weapons;
	const WeaponInventorySlot *slot = inventory.slot(combo);
	const WeaponTableEntry *def = slot != nullptr && slot->adm_index >= 0
			? table.by_index(static_cast<uint8_t>(slot->adm_index))
			: nullptr;
	if (def == nullptr) return; // @0x4DFA58
	const WeaponInventorySlot *equipped = inventory.slot(inventory.equipped_combo);
	const WeaponTableEntry *equipped_def = equipped != nullptr && equipped->adm_index >= 0
			? table.by_index(static_cast<uint8_t>(equipped->adm_index))
			: nullptr;
	if (equipped_def == nullptr) return; // @0x4DFA6B..0x4DFA71
	inventory.pending_combo = combo;     // g_PendingWeaponSlot @0x4DFB16
	WeaponSwitchOutcome mount;
	mount.kind = WeaponSwitchOutcome::kMount;
	mount.combo = combo;
	mount.same_category = equipped_def->category == def->category; // @0x4DFB8B
	handle_weapon_switch_outcome(world, w, &inventory, mount, view);
}

bool local_player_equip_weapon_slot(World &world, LocalPlayerWeapon &w,
		WeaponInventory &inventory, PlayerViewState &view, int32_t combo) {
	// [orig: Player_EquipWeaponByEntity @0x4E0370]
	const WeaponInventorySlot *slot = inventory.slot(combo);
	if (slot == nullptr || slot->adm_index < 0 ||
			world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index)) == nullptr) {
		// The deny cue [orig: Sound_PlayInterfaceTriggerSet(dword_24E08C4) @0x4E037E]
		WeaponPresentationEvent event;
		event.tick = world.logic_tick;
		event.world_position = local_player_mission_position(world);
		event.switch_denied = true;
		w.events.push_back(std::move(event));
		return false;
	}
	local_player_mount_weapon_slot(world, w, inventory, view, combo); // @0x4E03B8
	return true;
}

ToSpecialResult local_player_to_special(World &world, LocalPlayerWeapon &w,
		WeaponInventory &inventory, PlayerViewState &view, bool keys_held) {
	// [orig: Input_HandleActionBinding_0 case 0xDC @0x4E115B]
	const Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return ToSpecialResult::kHandled;
	// The seat word is the raw SeatType (entity+0x168): 0 on foot, 1 passenger,
	// 3 gunner [orig: `cmp [eax+168h], ...` @0x4E11A4 / @0x4E123A].
	const int32_t parent_slot = player->mounted ? static_cast<int32_t>(player->mount_type) : 0;
	// EquippedSlot->currentAction (+0x2C) [orig: @0x4E1189 / @0x4E11EB]
	const int32_t action = w.active ? active_local_weapon_slot(world, w)->current
	                                : weapon_action::kIdle;
	if (!keys_held) {
		// The release [orig: @0x4E1183..0x4E11E0]: a reload or a draw in
		// progress defers it @0x4E118C..0x4E1194.
		if (action == weapon_action::kReload || action == weapon_action::kSwitchTo)
			return ToSpecialResult::kDeferred;
		if (inventory.quick_switch_stash < 0 ||
				parent_slot == static_cast<int32_t>(SeatType::Gunner)) // @0x4E1196..0x4E11AB
			return ToSpecialResult::kHandled;
		// A stash whose slot has no weapon would leave retail's fallback
		// Player_SwitchToWeaponByHandle(stash def+0) dereferencing a null def
		// @0x4E11BE..0x4E11C9; the stash is cleared at every refill, so no
		// stock state reaches it, and the port takes the deny cue alone.
		local_player_equip_weapon_slot(world, w, inventory, view,
				inventory.quick_switch_stash);                     // @0x4E11B2
		inventory.quick_switch_stash = -1;                         // @0x4E11D1
		view.binoculars_requested = false;                         // g_BinocularsToggle @0x4E11D7
		return ToSpecialResult::kHandled;
	}
	// The press [orig: @0x4E11E5..0x4E1273]: a reload in progress defers it
	// @0x4E11EB.
	if (action == weapon_action::kReload) return ToSpecialResult::kDeferred;
	// The borrowed UseGun slot is no personal slot this table can stash.
	if (w.usegun_slot_active) return ToSpecialResult::kHandled;
	if (inventory.quick_switch_combo < 0 || inventory.quick_switch_stash >= 0 ||
			(parent_slot != 0 && parent_slot != 1) ||
			inventory.quick_switch_combo == inventory.equipped_combo) // @0x4E1220..0x4E124E
		return ToSpecialResult::kHandled;
	inventory.quick_switch_stash = inventory.equipped_combo;      // @0x4E1255
	if (local_player_equip_weapon_slot(world, w, inventory, view,
			inventory.quick_switch_combo))                         // @0x4E125B
		view.binoculars_requested = false;                         // @0x4E11D7
	else
		inventory.quick_switch_stash = -1;                         // @0x4E126B
	return ToSpecialResult::kHandled;
}

namespace {

// Restart the FP channel's primary half on a served clip: its clock follows
// the clip. [orig: AnimChannel_InitFromData @ 0x410560 via
// AnimMap_PlayAnimBySlot @ 0x40BDD1 (t = 0)]
void fp_start_primary(const anim::AdmRingTable &rings, LocalPlayerWeapon &w, const std::string &key,
		int32_t variant, uint32_t ticks) {
	w.anim_key = key;
	w.anim_variant = variant;
	w.anim_advance_ticks = ticks;
	const anim::AdmClipFacts *clip = rings.clip(w.anim_map, key, variant);
	w.anim_clock = clip != nullptr ? anim::ClipTimeline(clip->fps, clip->frames, clip->loop)
	                               : anim::ClipTimeline();
}

// The FP channel back to nothing playing (a mount, a clear).
void fp_channel_reset(LocalPlayerWeapon &w) {
	w.anim_key.clear();
	w.anim_variant = 0;
	w.anim_advance_ticks = 0;
	w.anim_slot_key.clear();
	w.anim_latched_key.clear();
	w.anim_latched_variant = -1;
	w.anim_blending = false;
	w.anim_blend_key.clear();
	w.anim_blend_variant = 0;
	w.anim_blend_ticks = 0;
	w.anim_fade_countdown = 0;
	w.anim_blend_weight = 0.0f;
	w.anim_clock = anim::ClipTimeline();
}

} // namespace

bool fp_channel_play(anim::AdmRingTable &rings, LocalPlayerWeapon &w, const std::string &key) {
	// A play of slot 0 does nothing: the reset slot is no ring.
	// [orig: AnimMap_PlayAnimBySlot @ 0x40BDA7]
	if (anim::adm_slot_index(key) == 0) return false;
	// The play serves the slot's shared ring (a slot the table does not author
	// holds its reset clip) and latches the served entry. It restarts the
	// primary half at t = 0 and only that half, so a wrap fade in flight runs on
	// and still promotes its clip. [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 — the
	// serve @0x40BDB4..0x40BDC1, the primary re-init @0x40BDD1, the latch
	// S+0x3C / +0x40 / +0x44]
	const anim::AdmServed served = rings.serve(w.anim_map, key);
	const std::string clip_key = served.valid() ? served.key : key;
	const int32_t variant = served.valid() ? served.variant : 0;
	fp_start_primary(rings, w, clip_key, variant, 0);
	w.anim_slot_key = key;
	w.anim_latched_key = clip_key;
	w.anim_latched_variant = variant;
	return true;
}

void fp_channel_advance(anim::AdmRingTable &rings, LocalPlayerWeapon &w) {
	// One gated tick of channel time [orig: AnimChannel_AdvanceDispatch
	// @ 0x40b960, from the action shims].
	if (w.anim_blending) {
		// A wrap fade runs: the incoming half steps, then the outgoing, which
		// wraps on with no ring serve; after eight steps the incoming replaces
		// it. [orig: AnimChannel_AdvanceBlendedPlayback @ 0x40B1E0 — the halves
		//  @0x40B1EB / @0x40B1F2, the countdown and weight, the promotion
		//  @0x40B20E..0x40B224]
		++w.anim_blend_ticks;
		++w.anim_advance_ticks;
		--w.anim_fade_countdown;
		w.anim_blend_weight += anim::kWrapFadeStep;
		if (w.anim_fade_countdown <= 0) {
			fp_start_primary(rings, w, w.anim_blend_key, w.anim_blend_variant, w.anim_blend_ticks);
			w.anim_blending = false;
		}
		return;
	}
	++w.anim_advance_ticks;
	// A looping clip's wrap serves the latched slot's ring: another entry fades
	// in from its start, the same one simply runs on.
	// [orig: AnimChannel_AdvancePlayback @ 0x40B140 — the wrap @0x40B199, the
	//  callback @0x40B1C8 -> AnimMap_AdvanceToNextAnim @ 0x40BDF0: the serve
	//  @0x40BE02..0x40BE07, the latch compare @0x40BE09,
	//  AnimChannel_InitFromParams(ch, clip, 8, 0, 0x1000) @0x40BE24, the latch
	//  @0x40BE29 / @0x40BE33]
	if (!w.anim_clock.wrapped_at(static_cast<int32_t>(w.anim_advance_ticks))) return;
	const anim::AdmServed next = rings.serve(w.anim_map, w.anim_slot_key);
	if (!next.valid() || (next.key == w.anim_latched_key && next.variant == w.anim_latched_variant))
		return;
	w.anim_blending = true;
	w.anim_blend_key = next.key;
	w.anim_blend_variant = next.variant;
	w.anim_blend_ticks = 0;
	w.anim_fade_countdown = anim::kWrapFadeTicks;
	w.anim_blend_weight = 0.0f;
	w.anim_latched_key = next.key;
	w.anim_latched_variant = next.variant;
}

void local_weapon_install(World &world, LocalPlayerWeapon &w,
		const WeaponInstallData &data, bool preserve_slot_state,
		bool allow_same_weapon_rebake, WeaponInventory *inventory,
		PlayerViewState &view) {
	// The FP model resolve re-installs the SAME weapon once its viewmodel (and
	// .adm clip lengths) finish loading. That resolve is a render-side consumer
	// in retail with no access to the action slot [orig: the per-frame FP model
	// resolve @ 0x4ded60 vs the mount's slot state in Player_MountWeaponSlot
	// @ 0x4dfa40], so an explicitly requested same-name rebake only refreshes
	// the def and rings.
	// Resetting the slot here instead destroyed a queued/holstering SWITCHFROM
	// whenever the late viewmodel install raced a switch request: the completion
	// never fired, commit_pending_weapon_switch never ran, and the FSM def
	// desynced from equipped_adm_index (the rifle then fired the previous
	// weapon's ammo).
	const bool same_weapon_rebake = allow_same_weapon_rebake && w.active &&
			!w.start_in_switchto && !data.name.empty() &&
			strutil::iequals(data.name, w.def_name);
	// The NVG scope restore latch is the switch request's to clear (the reset,
	// the mount, the select); the install that follows a committed switch, and
	// the render-side rebake, leave it [orig: dword_B76554's writers
	// @0x4dd730 / @0x4dd7fe / @0x4de2b9 / @0x4dfb75; WeaponAction_SwitchFrom
	// @0x5433b0 / SwitchRank @0x543500 write none].
	// A mount is a new presentation epoch: no payload from the previous weapon may
	// cross this seam, even though its strings were copied into the pending records.
	// The same-weapon rebake is NOT an epoch — undrained records (including a
	// racing switch commit or deny) must survive it.
	if (!same_weapon_rebake) w.events.clear();
	// The channel plays from the weapon's ANIMADM rings, one table per file
	// shared by every weapon naming it, held by the weapon table.
	// [orig: AnimMap_LoadAdmFile @ 0x40cc40, the cached entry @ 0x40CD45..0x40CD5C]
	anim::AdmRingTable &rings = world.tables.weapons.rings;
	const int installed = world.tables.weapons.index_of(data.name.c_str());
	if (data.table_baked && installed >= 0) {
		// A mount runs the descriptors the def's END baked as the table loaded;
		// it reads no ring. [orig: Anim_InitActions @ 0x541fa0, run once per
		// def @ 0x5437D0; Player_MountWeaponSlot @ 0x4dfa40 binds that table]
		w.def = world.tables.weapons.entries[static_cast<size_t>(installed)].action_fsm;
	} else {
		// A definition object bakes itself against the same rings. Its own clip
		// lengths stand in for its ANIMADM when no table of that name loaded.
		std::unordered_map<std::string, std::vector<anim::AdmClipFacts>> own;
		for (const std::pair<std::string, std::vector<float>> &kv : data.clip_rings) {
			std::vector<anim::AdmClipFacts> &clips = own[kv.first];
			for (float seconds : kv.second) {
				anim::AdmClipFacts clip;
				clip.seconds = seconds;
				clips.push_back(clip);
			}
		}
		// A table this install adopts is the object's own load, so its bake
		// consumes the heads as the load's would; a table already loaded holds
		// the match's heads, which only the load's bake and the plays move, so
		// a bake against it (a same-weapon re-bake, a def object on a loaded
		// ANIMADM) reads a copy.
		const bool adopts = !rings.loaded(data.animadm);
		rings.adopt(data.animadm, own);
		anim::AdmRingTable copy;
		if (!adopts) copy = rings.copy_of(data.animadm);
		anim::AdmRingTable &bake_rings = adopts ? rings : copy;
		// The bake probes existence as a pure lookup and reads durations
		// ring-wise, one consuming read per 'auto' field, through the table
		// build's own ring callbacks [orig: Anim_InitActions @ 0x541fa0; the
		// lookup @ 0x5421ae, the reads @ 0x5421c5 / @ 0x5421d8].
		WeaponTableRingContext ctx{&bake_rings, data.animadm};
		w.def = WeaponFsmDef{};
		weapon_fsm_bake(data.rows.data(), data.rows.size(), table_clip_resolves, table_clip_seconds,
				&ctx, w.def);
	}
	const int32_t flags = data.flags;
	w.def.auto_fire = (flags & weapon_flag::kAuto) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	w.def.burst3 = (flags & weapon_flag::kBurst) != 0;   // [orig: WeaponAction_Fire @ 0x542c8a]
	w.def.flags = flags;              // raw mask: the scope gate + fov policy read it
	w.def.flags2 = data.flags2;
	w.def.ammo_cost = data.ammo_cost;
	std::snprintf(w.def.soundfireloop, sizeof(w.def.soundfireloop), "%s", data.soundfireloop.c_str());
	std::snprintf(w.def.soundtrailoff, sizeof(w.def.soundtrailoff), "%s", data.soundtrailoff.c_str());
	std::snprintf(w.def.soundhead, sizeof(w.def.soundhead), "%s", data.soundhead.c_str());
	std::snprintf(w.def.soundlockedtone, sizeof(w.def.soundlockedtone), "%s", data.soundlockedtone.c_str());
       // Inset (0x200) picks the 7-step ease
	// The heat model [orig: WeaponDef +0x36C/+0x370/+0x374]. Absent keys leave 0,
	// which disables the model exactly as the original's zero-init does.
	w.def.heat_per_shot = data.heat_per_shot;
	w.def.heat_decay_per_tick = data.heat_decay_per_tick;
	w.def.heat_glow_threshold = data.heat_glow_threshold;
	w.scope_max_mag = data.scope_max_mag;
	// The arms dip's edge reads the category held before this mount (below).
	const bool was_active = w.active;
	const int previous_category = w.hud_category;
    w.hud_category = data.hud_category;
    w.emplaced_stance = data.emplaced_stance;
    w.pitch_min_bam = data.pitch_min_bam;
    w.pitch_max_bam = data.pitch_max_bam;
    if (!same_weapon_rebake) {
        const bool absorb = (flags & DEF_WEAPON_FLAG_ABSORBPITCH) != 0;
        w.pitch_offset_bam = absorb ? io::bam_sub(w.pitch_max_bam, w.pitch_min_bam) : 0;
        // The mount stamp reads the RAW def flag, not the seat-flag query.
        // [orig: Player_MountWeaponSlot @0x4DFA86; Pitch = 0 / offset = max - min
        //  @0x4DFAB7, else 0 @0x4DFABE]
        if (absorb) local_player_level_pitch(world);
    }
    view.weapon_hip_pose = data.view_hip_pose;
    view.weapon_ads_pose = data.view_ads_pose;
    w.def.scope_zero = data.scope_zero;
    if (installed >= 0)
        w.def.scope_zero = world.tables.weapons.entries[installed].action_fsm.scope_zero;
	// The 3P fire attack-stamp kind [orig: weapon.def attack_anim -> the g_AdmDefs record
	// +0xA8; world-wac-ai-re.md §14.8.4]. The sibling special_hold (+0xA4) is NOT cached
	// here: the body updater re-reads it from the ADM table by the posed entity's own
	// equipped index every selection pass [orig: @ 0x4b5dba], which is the single source
	// both the local player and every remote player resolve through.
	w.attack_kind = data.attack_anim;
	// The run-gait class [orig: 'run_anim' -> g_AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	w.run_anim = data.run_anim;
	w.force_crouch = (flags & weapon_flag::kForceCrouch) != 0;
	// A mount binds the table the weapon's load resolved (default.adm for a
	// missing file). [orig: Anim_InitActions @0x541FEF -> AnimMap_LoadAdmFile]
	w.anim_map = data.table_baked && installed >= 0
			? world.tables.weapons.entries[static_cast<size_t>(installed)].animadm
			: data.animadm;
	// A weapon CATEGORY change advances a binding serial; the local InfantryState
	// observes that edge pre-tick and stamps its own 20-tick arms-dip window. The
	// game compares the held and the previously held g_AdmDefs records' +0, the
	// weapon.def category, never their AnimMap: two weapons of one category do not
	// dip, whatever map each names, and two categories over one map do.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4b46cb..0x4b46f5: mov edx,
	// g_AdmDefs[cur*460h] @0x4b46e7, cmp edx, g_AdmDefs[prev*460h] @0x4b46ed, jz,
	// else byte +0x371 = 14h @0x4b46f5; the previous index +0x370 = the held one
	// @0x4b4701]. A fresh mount (no weapon held before) advances too.
	if (!was_active || data.hud_category != previous_category) {
		++w.category_serial;
		if (w.category_serial == 0) ++w.category_serial; // reserve 0 = none
	}
	const int32_t clipsize = data.clipsize;
	w.def.clip_capacity = clipsize > 0 ? clipsize : -1; // no clipsize key = no clip tracking
	if (same_weapon_rebake) {
		// Def + rings rebaked above; the live action slot, serials, input
		// latches, scope state, and charge state all continue untouched.
		w.def_name = data.name;
		return;
	}
	// A queued or in-flight SWITCHTO survives the per-equip reset — the switch flow
	// installs twice (dict-only, then with the rebuilt viewmodel's clip lengths) and
	// the draw-in must reach the second install (D-WPN-6 family artifact).
	const bool carry_switchto = !preserve_slot_state && w.active &&
			(w.slot.next == weapon_action::kSwitchTo ||
			 w.slot.current == weapon_action::kSwitchTo);
	if (!preserve_slot_state) {
		w.slot = WeaponSlotState{};
		// A fresh slot takes WeaponSlot_InitFromDef's zoom seed with the local
		// player as its owner; an inventory-backed install reads back its
		// entry's zoom below [orig: WeaponSlot_InitFromDef @0x53EF2D..0x53EF44].
		w.slot.scope_zoom = weapon_slot_initial_zoom(static_cast<int32_t>(data.scope_max_mag),
				data.scope_initial_mag, data.scope_min_mag,
				local_slot_zoom_sniper_lock(world, data.hud_category));
        w.slot.scope_zero = weapon_scope_zero_initial(w.def.scope_zero);
        if ((flags & 3) != 0)
            w.slot.zero_pitch = weapon_scope_zero_pitch(w.def.scope_zero, w.slot.scope_zero);
        // The zero-yaw term (MountSlot+8) from the seeded step: outside the
        // flags & 3 gate, and never negated at the install [orig:
        // WeaponSlot_InitFromDef @0x53ef4f..0x53ef8b].
        w.slot.zero_yaw = weapon_scope_zero_yaw(w.def.scope_zero, w.slot.scope_zero);
		// Ammo comes from the slot pool when the installed def IS the equipped
		// inventory slot: clip = the slot's loaded rounds, reserve = the def's
		// ammo-class pool [orig: MountSlot+0x10 +
		// Entity_GetScoreValueBySlotType @0x5406e0].
		bool ammo_from_inventory = false;
		if (inventory != nullptr) {
			const WeaponInventorySlot *eq = inventory->slot(inventory->equipped_combo);
			const WeaponTableEntry *def = (eq != nullptr && eq->adm_index >= 0)
					? world.tables.weapons.by_index(static_cast<uint8_t>(eq->adm_index))
					: nullptr;
			if (def != nullptr && strutil::iequals(data.name, def->name)) {
				w.slot.clip = weapon_inventory_loaded_rounds(
                        world.tables.weapons, *inventory, inventory->equipped_combo);
                w.slot.shared_clip = def->ammo_bucket != 0;
                w.slot.scope_zero = eq->scope_zero;
                w.slot.scope_zoom = eq->scope_zoom;
                w.slot.zero_pitch = weapon_scope_zero_pitch(w.def.scope_zero, eq->scope_zero);
                w.slot.zero_yaw = weapon_scope_zero_yaw(w.def.scope_zero, eq->scope_zero);
				w.slot.reserve =
						weapon_pool_get(*inventory, def->ammo_class_id);
				ammo_from_inventory = true;
			}
		}
		if (!ammo_from_inventory) {
			// Fresh slot: full magazine + the def's carried reserve (the interim
			// ammo default for inventory-less installs — D-WPN-7).
			w.slot.clip = clipsize > 0 ? clipsize : 0;
			w.slot.reserve = data.startrounds;
		}
		// A commit-driven inventory install starts in SWITCHTO. A UseGun commit
		// already queued SWITCHTO on the selected parent/personal slot.
		if (w.start_in_switchto || carry_switchto) {
			w.slot.next = weapon_action::kSwitchTo;
			w.start_in_switchto = false;
		}
	}
	// A walk outcome that mounted but has not committed yet (its outgoing
	// SWITCHFROM was displaced by this mount) resumes through the deferred latch
	// once the draw completes, so the pending combo still commits.
	if (w.switch_in_flight)
		w.switch_deferred_action = weapon_action::kSwitchFrom;
	// The mount is the charge epoch [orig: Player_SwitchToWeaponByHandle zeroes
	// g_FireChargeStartTick on the walk, before the mount].
	w.power_throw_start_tick = 0;
	w.pending_throw_charge = 0;
	w.play_serial = 0;
	// Retail's channel halves and latch live in the per-.adm cached record,
	// which a mount never touches, so a fade in flight runs on across a mount
	// onto the same table. Ours is the player's own channel and a mount starts
	// it over: the per-def singleton residual D-NET-184 declares.
	// [orig: AnimMap_LoadAdmFile @0x40CD45..0x40CD5C; Player_MountWeaponSlot
	//  @0x4DFA40]
	fp_channel_reset(w);
	w.fired_serial = w.dry_serial = w.reload_serial = 0;
	w.reload_applied_serial = 0;
	w.reload_received_serial = 0;
	w.reload_received_entity = EntityHandle::kInvalid;
	w.reload_received_param = 0;
	w.unscope_serial = w.rescope_serial = 0;
	w.action_serial = 0;
	w.action_started = -1;
	w.action_end_serial = 0;
	w.action_finished = -1;
	w.fire_held = w.fire_pressed = w.reload_pressed = false;
	// A cross-category non-ForceScoped mount resets the target, not its current.
	// [orig: Player_MountWeaponSlot @0x4DFB44..0x4DFB66]
	const int old_index = world.tables.weapons.index_of(w.def_name.c_str());
	const int new_index = world.tables.weapons.index_of(data.name.c_str());
    const bool category_changed = old_index >= 0 && new_index >= 0 &&
            world.tables.weapons.entries[old_index].category !=
            world.tables.weapons.entries[new_index].category;
    if (category_changed && (data.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0)
        world.weather.core.scalar_channels.camera_fov_target_fp = 80 << 16;
    player_view_weapon_mount(view, data.flags, category_changed);
	w.def_name = data.name;
	w.active = true;
}

void local_weapon_store_scope_zoom(const World &world, const LocalPlayerWeapon &w,
                                   WeaponInventory &inventory) {
	if (!w.active || w.usegun_slot_active) return;
	WeaponInventorySlot *entry = inventory.slot(inventory.equipped_combo);
	if (entry == nullptr || entry->adm_index < 0) return;
	const WeaponTableEntry *def =
			world.tables.weapons.by_index(static_cast<uint8_t>(entry->adm_index));
	if (def == nullptr || !strutil::iequals(def->name, w.def_name)) return;
	entry->scope_zoom = w.slot.scope_zoom;
}

void local_weapon_clear(LocalPlayerWeapon &w, PlayerViewState &view) {
	w.nvg_scope_restore = false;
	w.active = false;
	w.def_name.clear();
	w.power_throw_start_tick = 0;
	w.pending_throw_charge = 0;
	w.first_person_model_adm = 0xFF;
	w.switch_in_flight = false;
	w.switch_deferred_action = -1;
	w.events.clear();
	w.fire_held = false;
	w.fire_pressed = false;
	w.reload_pressed = false;
	fp_channel_reset(w);
	player_view_scope_reset(view);
	w.attack_kind = 0;
	w.run_anim = 0;
	w.force_crouch = false;
	w.anim_map.clear();
}

// [orig: Entity_CanFireWeapon @ 0x4dcb10 — the local branch
//  @ 0x4dcbcf..0x4dcc5d]
bool local_held_weapon_visible(const World &world, const Entity &entity,
		const LocalPlayerWeapon &weapon, const WeaponInventory &inventory,
		bool third_person) {
	if ((entity.flags & 2u) != 0) return false;   // dead [orig: @0x4dcb22]
	if (!weapon.active) return false;             // no EquippedSlot [orig: @0x4dcbcf]
	const WeaponInventorySlot *slot = inventory.slot(inventory.equipped_combo);
	if (slot == nullptr || slot->adm_index < 0) return false;
	const WeaponTableEntry *def =
			world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	if (def == nullptr) return false;             // no Def [orig: @0x4dcbda]
	// The ammo leg, for defs that carry the flag: an empty pool hides the
	// weapon. [orig: @0x4dcbea -> Entity_GetScoreValueBySlotType @0x5406E0,
	// ported as weapon_pool_get]
	if ((def->flags & weapon_flag::kNoClipsNoDraw) != 0 &&
			weapon_pool_get(inventory, def->ammo_class_id) == 0 &&
			weapon_inventory_loaded_rounds(world.tables.weapons, inventory,
                    inventory.equipped_combo) <= 0)
		return false;
	// A weapon with no FIRST-person model is hidden on your OWN body even
	// though every observer still sees it — retail asymmetry, not a bug.
	// [orig: @0x4dcc32]
	if (!def->has_first_person_model_reference) return false;
	if (!entity.mounted) return true;             // [orig: @0x4dcc42]
	// Seat rule, LOCAL flavour: control and driver always hide; the gunner
	// seat hides only while the third-person camera is up. [orig:
	// @0x4dcc44..0x4dcc5d]
	switch (entity.mount_type) {
		case SeatType::Controller:
		case SeatType::Driver:
			return false;
		case SeatType::Gunner:
			return !third_person;
		default:
			return true; // passenger keeps its weapon
	}
}

void local_weapon_set_input(LocalPlayerWeapon &w, const PlayerViewState &view,
		bool fire_held, bool fire_pressed, bool reload_pressed) {
	if (view.binoculars_view_active) {
		w.fire_held = false;
		w.fire_pressed = false;
		w.reload_pressed = false;
		return;
	}
	w.fire_held = fire_held;
	w.fire_pressed = w.fire_pressed || fire_pressed; // latch until consumed
	w.reload_pressed = w.reload_pressed || reload_pressed;
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick. The
// world tick owns the parallel NPC UseGun parent-slot pump; this remains the
// first-person player's input/presentation seam.
// [orig: WeaponAction_ProcessAllEntities @0x542690 pumps every pooled entity]
void weapon_trace_arm(LocalPlayerWeapon &w, bool armed) {
	if (armed == w.trace_armed) return;
	w.trace_armed = armed;
	if (armed) {
		w.trace.assign(kWeaponTraceCapacity, WeaponTraceSample{});
	} else {
		w.trace.clear();
		w.trace.shrink_to_fit();
	}
	w.trace_head = 0;
	w.trace_wrapped = false;
}

void weapon_trace_clear(LocalPlayerWeapon &w) {
	w.trace_head = 0;
	w.trace_wrapped = false;
}

uint32_t weapon_trace_samples_since(const LocalPlayerWeapon &w, uint32_t after_tick,
		bool take_all, std::vector<WeaponTraceSample> &out) {
	if (!w.trace_armed || w.trace.empty()) return 0;
	const size_t cap = w.trace.size();
	const size_t count = w.trace_wrapped ? cap : w.trace_head;
	if (count == 0) return 0;
	// Newest first from just behind the head; stop at the first sample the
	// caller already holds.
	const size_t first_out = out.size();
	for (size_t i = 0; i < count; ++i) {
		const size_t idx = (w.trace_head + cap - 1 - i) % cap;
		const WeaponTraceSample &s = w.trace[idx];
		if (!take_all && s.tick <= after_tick) break;
		out.push_back(s);
	}
	std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first_out), out.end());
	return w.trace[(w.trace_head + cap - 1) % cap].tick;
}

std::vector<WeaponTraceSample> weapon_trace_samples(const LocalPlayerWeapon &w) {
	std::vector<WeaponTraceSample> out;
	if (!w.trace_armed || w.trace.empty()) return out;
	const size_t count = w.trace_wrapped ? w.trace.size() : w.trace_head;
	out.reserve(count);
	const size_t first = w.trace_wrapped ? w.trace_head : 0;
	for (size_t i = 0; i < count; ++i) {
		out.push_back(w.trace[(first + i) % w.trace.size()]);
	}
	return out;
}

LocalWeaponInputBlock local_weapon_input_block(const World &world,
		const LocalPlayerWeapon &w) {
	if (!w.active) return LocalWeaponInputBlock::kInactive;
	const Entity *player = world.registry.get(world.cached.local_player);
	const bool alive = player != nullptr && player->alive && player->health > 0;
	if (!alive) return LocalWeaponInputBlock::kDead;
	if (w.usegun_switch != LocalUseGunSwitch::kNone) return LocalWeaponInputBlock::kUseGunSwitch;
	if (mount_blocks_firing(*player, world.registry.get(player->mount_target)))
		return LocalWeaponInputBlock::kSeat;
	return LocalWeaponInputBlock::kNone;
}

const char *local_weapon_input_block_name(LocalWeaponInputBlock block) {
	switch (block) {
		case LocalWeaponInputBlock::kNone: return "";
		case LocalWeaponInputBlock::kInactive: return "no weapon is installed";
		case LocalWeaponInputBlock::kDead: return "the local player is not alive";
		case LocalWeaponInputBlock::kUseGunSwitch: return "a UseGun switch is in flight";
		case LocalWeaponInputBlock::kSeat: return "the occupied seat blocks firing (controller/driver)";
	}
	return "";
}

namespace {

// One recorded tick. Reads the slot AFTER the pump's ammo mirroring so clip and
// reserve are the values the frame actually ends on.
void weapon_trace_record(LocalPlayerWeapon &w, const WeaponSlotState &slot,
		const WeaponFsmEvents &ev, uint32_t tick) {
	if (!w.trace_armed || w.trace.empty()) return;
	WeaponTraceSample &out = w.trace[w.trace_head];
	out = WeaponTraceSample{};
	out.tick = tick;
	out.current = slot.current;
	out.next = slot.next;
	out.prev = slot.prev;
	out.phase = slot.phase;
	out.counter = slot.counter;
	out.clip = slot.clip;
	out.reserve = slot.reserve;
	out.heat = weapon_slot_accumulated_heat(w.def, slot, static_cast<int32_t>(tick));
	out.action_started = ev.action_started;
	out.action_finished = ev.action_finished;
	out.action_effect = ev.action_effect;
	out.fired = ev.fired;
	out.dry_fired = ev.dry_fired;
	out.reload_requested = ev.reload_requested;
	out.reload_applied = ev.reload_applied;
	out.advance_anim = ev.advance_anim;
	std::snprintf(out.anim_key, sizeof(out.anim_key), "%s", w.anim_key.c_str());
	out.anim_variant = w.anim_variant;
	w.trace_head = (w.trace_head + 1) % w.trace.size();
	if (w.trace_head == 0) w.trace_wrapped = true;
}

// The local player's scope as the weapon FSM's legs reach it, inline: the
// whole toggle, or the bare promoted-byte clear (weapon_fsm.h WeaponFsmScope).
class LocalWeaponScope final : public WeaponFsmScope {
public:
	LocalWeaponScope(World &world, const LocalPlayerWeapon &w, PlayerViewState &view)
			: world_(world), w_(w), view_(view) {}
	void toggle(WeaponSlotState &slot) override {
		local_player_toggle_weapon_scope(world_, w_, view_, slot);
	}
	void clear_promoted() override { player_view_clear_scope_promoted(view_); }

private:
	World &world_;
	const LocalPlayerWeapon &w_;
	PlayerViewState &view_;
};

} // namespace

// The C2S 0x25 a local reload request ships. A UseGun seat (parentSlot 3)
// addresses the entity the actor sits on, its parentEntity, whichever slot
// that gun routes to (the host follows the gun's own redirect); every other
// seat, the armed ctrlx included, addresses the actor.
// [orig: WeaponAction_Reload `cmp [edi+168h], 3` / `mov ecx, [edi+16Ch]`
//  @0x5430EA..0x543103 -> NetPacket_SendEntityDeathNotification @0x432930]
LocalWeaponReloadWire local_reload_request_wire(const World &world,
		uint16_t actor_wire_handle, uint16_t param) {
	const Entity *actor = world.registry.get(world.cached.local_player);
	const Entity *seat_parent = actor != nullptr && actor->mounted &&
			actor->mount_type == SeatType::Gunner
			? world.registry.get(actor->mount_target) : nullptr;
	LocalWeaponReloadWire out;
	out.valid = true;
	out.entity_handle = seat_parent != nullptr ? seat_parent->handle.packed : actor_wire_handle;
	// The producer replaces the parameter word with 0xFFFF when the addressed
	// entity is an EWeap; the host refills that entity's own slot route and
	// never reads the word. [orig: NetPacket_SendEntityDeathNotification
	//  `test byte ptr [eax+54h], 20h` @0x43296C -> 0xFFFF @0x432974, else the
	//  caller's parameter @0x43299B; WeaponSlot_ReloadAmmo @0x54172D]
	const Entity *addressed = seat_parent != nullptr ? seat_parent : actor;
	out.reload_param = addressed != nullptr && addressed->has_item_def &&
			(addressed->item_attrib & kItemAttribEweap) != 0 ? uint16_t(0xFFFF) : param;
	return out;
}

void local_weapon_pump_tick(World &world, LocalPlayerWeapon &w,
		LocalWeaponPumpIO &io) {
	io.map_command = 0;
	io.fired = LocalWeaponFiredWire{};
	io.reload = LocalWeaponReloadWire{};
	if (io.view == nullptr || !world.cached.local_player.valid()) return;
	PlayerViewState &view = *io.view;
	sync_local_usegun_weapon_transition(world, w, view);
	if (!w.active) return;
	WeaponSlotState &active_slot = *active_local_weapon_slot(world, w);
	const Entity *player = world.registry.get(world.cached.local_player);
	const bool player_alive =
			player != nullptr && player->alive && player->health > 0;
	const bool usegun_switch_pending =
			w.usegun_switch != LocalUseGunSwitch::kNone;
	if (!player_alive && !usegun_switch_pending) {
		active_slot.refire_queued = false;
		w.power_throw_start_tick = 0;
		w.pending_throw_charge = 0;
		w.fire_pressed = false;
		w.reload_pressed = false;
		return;
	}
	// Capture the outgoing slot identity. A switch completion below changes the
	// active selection, but this tick's ammo bridge still belongs to the slot the
	// FSM actually pumped.
	const bool borrowed_usegun_slot = w.usegun_slot_active;
    if (io.inventory != nullptr && !borrowed_usegun_slot) {
        const auto *eq = io.inventory->slot(io.inventory->equipped_combo);
        const auto *def = eq != nullptr && eq->adm_index >= 0
                ? world.tables.weapons.by_index(static_cast<uint8_t>(eq->adm_index)) : nullptr;
        active_slot.shared_clip = def != nullptr && def->ammo_bucket != 0;
        if (active_slot.shared_clip) {
            active_slot.clip = weapon_inventory_loaded_rounds(world.tables.weapons,
                    *io.inventory, io.inventory->equipped_combo);
            active_slot.reserve = weapon_pool_get(*io.inventory, def->ammo_class_id);
        }
    }
	WeaponFsmInputs in;
	// The action binding permits an armed controller's borrowed carrier slot.
	// [orig: Input_HandleActionBinding_0 @0x4E09CB..0x4E09FF]
	const bool accept_weapon_input =
			local_weapon_input_block(world, w) == LocalWeaponInputBlock::kNone;
	in.fire_held = accept_weapon_input && w.fire_held;
	in.fire_pressed = accept_weapon_input && w.fire_pressed;
	// PowerThrow: the press never fires — it starts the windup; the release
	// converts the held time into the charge byte and fires. [orig: press gate
	// @ 0x4e08fd (def Flags sign bit 0x80000000, fireable + ammo ->
	// g_FireChargeStartTick = tick), release @ 0x4e07e9 -> WeaponSlot_RequestFire
	// with the computed charge; world-wac-ai-re §27.]
	bool power_throw_release = false;
	if ((w.def.flags & weapon_flag::kPowerThrow) != 0) {
		if (!accept_weapon_input) {
			w.power_throw_start_tick = 0;
			w.pending_throw_charge = 0;
		} else if (w.fire_held || w.fire_pressed) {
			// The windup refuses while a switch action runs OR is queued — the
			// press gate's fireable term, not just the current action [orig: the
			// fireable check @ 0x4e08fd]. Without the queued/deferred legs a
			// press landing inside the draw-in latched a windup whose release
			// the FSM then refused, leaking the charge byte onto a later shot.
			const bool fireable =
					active_slot.current == weapon_action::kIdle &&
					active_slot.next == weapon_action::kIdle &&
					!w.switch_in_flight && w.switch_deferred_action < 0;
			const bool has_ammo =
					active_slot.clip > 0 || w.def.clip_capacity < 0;
			if (w.power_throw_start_tick == 0 && fireable && has_ammo)
				w.power_throw_start_tick = world.logic_tick;
			in.fire_held = false;
			in.fire_pressed = false;
		} else if (w.power_throw_start_tick != 0) {
			const int32_t held = static_cast<int32_t>(
					world.logic_tick - w.power_throw_start_tick);
			w.pending_throw_charge = power_throw_charge_from_hold(held);
			w.power_throw_start_tick = 0;
			in.fire_pressed = true;
			in.fire_held = false;
			power_throw_release = true;
		}
	} else {
		w.power_throw_start_tick = 0;
		// Ordinary presses and deferred auto-fire requests pass through WAC's
		// category gate before the seat/fireability rejection. PowerThrow uses
		// its separate press/release path above. Do not cancel an accepted FIRE:
		// only refuse the new binding request, including a deferred one.
		// [orig: Input_HandleActionBinding_0 @0x4E0968..0x4E097B]
		if (player_alive && (w.fire_pressed || active_slot.refire_queued)) {
			const WeaponTableEntry *def =
					world.tables.weapons.by_index(player->equipped_adm_index);
			if (def != nullptr && !world.script.weapon_input.record_fire_request(def->category)) {
				in.fire_pressed = false;
				active_slot.refire_queued = false;
			}
		}
	}
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = accept_weapon_input && w.reload_pressed &&
			weapon_fsm_reload_allowed(w.def, active_slot);
	in.is_local = true;
	in.is_authority = io.is_authority; // the joiner defers the refill to the §5.58 round-trip
	in.auto_reload = world.rules.auto_reload; // [orig: g_AutoReloadEnabled @ 0x24D2118]
	// The weapon FSM consumes the promoted/settled scope bit, not the raw
	// requested-engagement bit. Player_UpdatePerFrame runs before the weapon
	// pump in retail and only promotes g_WeaponScopeActive after the ease has
	// completed [orig: promoter @ 0x4de4f7; weapon pump @ 0x526786].
	in.scope_active = player_view_scope_settled(view);
	// The FSM's scope legs run the toggle, or clear the promoted byte, where
	// its handlers do [orig: Player_ToggleWeaponScope @0x4df0c0 called
	// @0x543136 / @0x54305d / @0x5413a6; g_WeaponScopeActive = 0 @0x5429f0 /
	// @0x542ad3 / @0x5413a0].
	LocalWeaponScope scope(world, w, view);
	in.scope = &scope;
	in.instant_emplaced_switch = local_usegun_switch_is_instant(world, w);
	// The heat window is a deadline against the logic tick, not a stored level.
	// [orig: current_tick @ 0x24C1968]
	in.current_tick = static_cast<int32_t>(world.logic_tick);
	// The window's water gate is the owner's BODY Z against the global water
	// plane, not the drowning bit and not the eye height: `Position.Z >
	// g_EnvWaterHeightFixed` keeps the window, at-or-below (with no Underwater def
	// flag) clears it. No authored water (env.water_z == 0) never submerges.
	// [orig: WeaponAction_ProcessFrame @ 0x540e60, the gate @ 0x54101c]
	in.submerged = player != nullptr && world.env.water_z != 0 &&
			to_fixed(player->position.z) <= world.env.water_z;
	if (player != nullptr) weapon_fire_environment_inputs(world, *player, in);
	if (!accept_weapon_input) active_slot.refire_queued = false;
	WeaponFsmEvents ev;
	weapon_fsm_tick(w.def, active_slot, in, ev);
	io.map_command = ev.map_command;
	if (player != nullptr) weapon_sound_publish(world, *player, w.def, ev);
	// A release whose fire request the FSM refused must not leave the charge
	// latched for a later unrelated shot — the charge byte is consumed by the
	// very fire it triggers [orig: descriptor +20 consume @ 0x4ec5bb].
	if (power_throw_release && !ev.fired &&
			active_slot.current != weapon_action::kFire &&
			active_slot.next != weapon_action::kFire)
		w.pending_throw_charge = 0;
	// SWITCHTO seeds next=prev at the end of its delay-start phase. Reapply the
	// retained one-shot after every draw tick so its eventual transition performs
	// the pending inventory handoff without requiring another key press.
	if (w.switch_deferred_action >= 0) {
		if (active_slot.current == w.switch_deferred_action) {
			w.switch_deferred_action = -1;
		} else {
			active_slot.next = w.switch_deferred_action;
		}
	}
	w.fire_pressed = false; // edges consume on the first tick of the frame
	w.reload_pressed = false;
	WeaponPresentationEvent pending;
	pending.tick = world.logic_tick;
	bool has_presentation_event = false;
	if (ev.play_anim && fp_channel_play(world.tables.weapons.rings, w, ev.anim_key)) {
		++w.play_serial;
		pending.anim_key = w.anim_key;
		pending.anim_variant = w.anim_variant;
		has_presentation_event = true;
	}
	if (ev.advance_anim) fp_channel_advance(world.tables.weapons.rings, w);
	if (ev.action_started >= 0) {
		// Copy the begin leg while this def is mounted; a later weapon switch cannot
		// change the queued sound/effect payload.
		// [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860].
		++w.action_serial;
		w.action_started = ev.action_started;
		pending.action_started = ev.action_started;
		if (ev.action_started < weapon_action::kCount) {
			const WeaponFsmAction &act = w.def.actions[ev.action_started];
			pending.action_soundset = act.soundset;
			pending.action_particle = act.particle;
			pending.action_particle_userpoint = act.particle_userpoint;
		}
		has_presentation_event = true;
	}
	if (ev.action_finished >= 0) {
		// The END leg: the finished action's soundsetend — fire rows carry the gunshot
		// here, reload rows the completion sound
		// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100].
		++w.action_end_serial;
		w.action_finished = ev.action_finished;
		pending.action_finished = ev.action_finished;
		if (ev.action_finished < weapon_action::kCount) {
			pending.action_end_soundset =
					w.def.actions[ev.action_finished].soundsetend;
		}
		has_presentation_event = true;
	}
	if (ev.action_effect >= 0 && ev.action_effect < weapon_action::kCount) {
		// The recoil-row DIRECT effect leg — casing eject / bolt smoke at the arbiter
		// tick. Copied like the begin leg so a weapon switch cannot swap the payload.
		// [orig: WeaponAction_Recoil @ 0x542dd0 spawn @ 0x542f64]
		const WeaponFsmAction &act = w.def.actions[ev.action_effect];
		pending.action_effect = ev.action_effect;
		pending.effect_particle = act.particle;
		pending.effect_particle_userpoint = act.particle_userpoint;
		has_presentation_event = true;
	}
	// Preserve the retail call order within one pump: clip start, begin leg, then
	// finish leg. Records themselves stay in logic-tick order until the shell drains.
	if (has_presentation_event) {
		pending.world_position = local_player_mission_position(world);
		pending.scope_settled = player_view_scope_settled(view);
		pending.third_person = view.third_person;
		const Entity *local = world.registry.get(world.cached.local_player);
		pending.vehicle_attack_context =
				local != nullptr && mount_blocks_weapon_channel(*local);
		w.events.push_back(std::move(pending));
	}
	if (ev.fired) {
		++w.fired_serial;
		// The 3P body attack stamp — knife/grenade kinds only; rifle fire stamps NO body
		// state (the FP clip plays on the weapon adm channel, and the fire path's only
		// other anim side effect drives the .3di control registers)
		// [orig: WeaponAction_Fire @ 0x542bbc..0x542bea; ActionSlot_TryAllocCtrlRegAnim
		//  @ 0x401f00 -> dword_83FCE8].
		AiEntity *p = world.ai.for_handle(world.cached.local_player);
		if (p != nullptr && p->inf.active) {
			infantry_weapon_attack_stamp(p->inf, w.attack_kind);
		}
		// Local/SP fire already passed the same FSM/ammo authority that the remote
		// C2S 0x06 handler validates. Append the host's round-ring record and spawn
		// the authoritative projectile here; the loopback server handler correctly
		// ignores this player because retail's local action has already done both.
		// [orig: WeaponAction_Fire @ 0x542c5e ->
		// Entity_FireWeaponAndSendPacket @ 0x42bd80 local re-entry ->
		// RoundData_AddRound @ 0x4fdb40 inline RoundData_SpawnRound @ 0x4ec0d0]
		Entity *shooter = world.registry.get(world.cached.local_player);
		if (shooter != nullptr && p != nullptr) {
			const uint8_t adm_index = shooter->equipped_adm_index;
			const WeaponTableEntry *adm = world.tables.weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
                int32_t pose[6];
                local_weapon_fire_pose(world, w, ev.fired_clip_before_consume,
                        player_view_scope_settled(view), pose);
                const Vec3 origin{float(pose[0])/65536.0f, float(pose[1])/65536.0f, float(pose[2])/65536.0f};
                const FixedVec3 fire_origin{pose[0], pose[1], pose[2]};
				// The authority arm first passes its own-slot gate, then the
				// round validation. [orig: Entity_FireWeaponAndSendPacket
				//  @0x42be3a ahead of Server_ClientFiredRound @0x42bf34]
				const bool accepted = !io.is_authority ||
						(io.authority_fire_admitted &&
						 weapon_fire_owner_status(world, *shooter, adm, false) == 0 &&
						 weapon_fire_origin_status(world, *shooter, *adm, fire_origin, false) == 0);
				if (accepted) {
				// The round bearing frame IS the engine heading frame: RoundSim's
				// (cos, sin) mission-axis mapping is wire-validated on the 0x06 yaw
				// BAM (round_sim.cpp spawn, D-NET-153), and the retail spawner runs
				// raw descriptor angles through sin/cos [orig: RoundData_SpawnRound
				// trig @ 0x4ec511..0x4ec5fb]. Applying the (90 - heading)
				// mission-yaw flip here mirrored every local shot across the NE
				// diagonal (impacts landed 90 deg off the aim ray - the
				// fp_impact_probe pin; the same mistake D-NET-153 records for the
				// wire leg).
				const int32_t dir_yaw = pose[3];
				// Fire position/direction sees the undoubled recoil accumulator;
				// the camera is the separate 2*R consumer. Spread below still
				// samples R>>8 before this shot adds its own impulse.
				// [orig: Entity_CalcWeaponFirePosition @0x4DC847]
				const int32_t dir_pitch = pose[4];
                const EntityHandle fire_target{p->slot.f[3] != 0
                    ? uint16_t(p->slot.f[3] - 1) : EntityHandle::kInvalid};
                shooter->last_fire_target = fire_target;
				w.round_sequence = static_cast<uint16_t>(w.round_sequence + 1u);
				const uint16_t shot_seq = w.round_sequence;
				// The fire tail reads the gun's point for the slot as the FSM
				// leaves it: next = RECOIL (the mflash column), the clip spent.
				// [orig: WeaponAction_Fire tail @0x542D1F..0x542D5B]
				world.vehicles.weapon_recoil(*shooter,
						w.def.actions[weapon_action::kFire].action_value, adm,
						active_local_weapon_slot(world, w)->clip, 1);

				RoundEvent round_event;
				round_event.shooter_handle = !io.is_authority
						? io.self_wire_handle
						: world.cached.local_player.packed;
				round_event.origin_x = pose[0];
				round_event.origin_y = pose[1];
				round_event.origin_z = pose[2];
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
				round_event.shot_seq = shot_seq;
				const uint32_t clip_before_consume = static_cast<uint32_t>(
						std::max(0, ev.fired_clip_before_consume));
				round_event.mode_flags = static_cast<uint8_t>(
						((clip_before_consume & 0x3u) << 4u) | 0x02u);
				// The local shooter's fire-context composite: 0x80 while the
				// optic view can fire, plus the zero step (the default 12
				// otherwise). Bit 7 selects the round's aimed ERROR row and
				// the low six bits its zero elevation (RoundSim::spawn); the
				// server's bit-6-clearing composite preserves both.
				// [orig: Entity_FireWeaponAndSendPacket @0x42bdcb..0x42bdfb --
				//  Player_IsOpticalViewVisible @0x42bdd8, Weapon_GetScopeZoomLevel(can,
				//  12) @0x42bde2, `neg; sbb; and 80h; add` @0x42bdee..0x42bdfb]
				const bool can_fire = local_player_scope_view_visible(world, w, view);
				const WeaponSlotState *fire_zero_slot = active_local_weapon_slot(world, w);
				round_event.subtype = static_cast<uint8_t>((can_fire ? 0x80 : 0) +
						weapon_scope_zoom_step(w.def.scope_zero, w.def.flags,
								fire_zero_slot->scope_zero, w.aim_range_q16, can_fire, 12));
				round_event.adm_index = adm_index;
				// The PowerThrow charge rides the ring/wire slot_byte (ring+32,
				// wire flags|0x80 leg) and scales the spawned round's launch
				// speed [orig: WeaponAction_Fire arg 6 <- MountSlot+0x5C ->
				// descriptor +20 @ 0x4ec5bb; deserializer restore @ 0x42f769].
				round_event.slot_byte = w.pending_throw_charge;
				// The validated fire ends the shooter's spawn protection. The listen
				// host's own fire takes the C2S 0x06 case's clear through
				// Entity_FireWeaponAndSendPacket's authority leg: in an MP session a
				// non-spectator slot's nonzero entity+292 goes to 0 ahead of the ring
				// append (the roster row mirrors the slot's spectator latch).
				// [orig: Entity_FireWeaponAndSendPacket @0x42BD80 authority leg
				//  @0x42be03..0x42bf34 -> Server_ClientFiredRound @0x50c736..0x50c75d]
				// The authority's own local round scores the shot (scorer
				// event 1) ahead of that clear. [orig: Server_ClientFiredRound
				//  @0x50BAA0 (the event-1 call @0x50C727)]
				if (io.is_authority)
					world.match.record_shot(world, world.cached.local_player);
				if (io.is_authority && world.rules.mp_session &&
						shooter->damage_state != 0) {
					const MatchPlayer *row = world.match.player(world.cached.local_player);
					if (row == nullptr || !row->spectator) shooter->damage_state = 0;
				}
				if (io.is_authority) world.out.rounds.add(round_event);

				RoundSpawnParams round;
				round.owner = world.cached.local_player;
				round.shooter_handle = round_event.shooter_handle;
				round.origin = origin;
				round.dir_yaw_bam = dir_yaw;
				round.dir_pitch_bam = dir_pitch;
				round.ammo_index = adm->ammo_index;
				round.adm_index = adm_index;
				round.shot_seq = shot_seq;
				round.subtype = round_event.subtype;
				round.charge = w.pending_throw_charge;
				// The firing slot's tracer cadence byte (MountSlot+0x80) — the
				// resolved ACTIVE slot, so a mounted redirect advances the
				// parent slot's phase exactly like retail's passed weaponSlot
				// [orig: RoundData_SpawnRound @ 0x4ec199 on the fire
				//  descriptor's slot].
				if (WeaponSlotState *fire_slot =
								active_local_weapon_slot(world, w))
					round.tracer_counter = &fire_slot->tracer_shot_counter;
				if (!io.is_authority && io.carrier_exclusion)
					// The joiner's OWN predicted round runs the wire-proxy walk with
					// the local mount exclusion dead, so resolve the carrier gate from
					// the self wire row like any decoded remote round — otherwise a
					// mounted joiner's fire stops on its own vehicle's proxy.
					round.shooter_carrier_handle = io.carrier_exclusion();
				world.round_sim.spawn(world, round,
						!io.is_authority ? RoundConsequenceMode::VisualOnly
						                 : RoundConsequenceMode::Authoritative);

				// The wire half of Entity_FireWeaponAndSendPacket: the embedder's
				// net layer builds the fixed C2S 0x06 descriptor from this record.
				// [orig: @0x42A62F/@0x42A6A1..0x42A890]
				if (!io.is_authority) {
					io.fired.valid = true;
					io.fired.round = round_event;
					io.fired.shot_seq = shot_seq;
					io.fired.adm_index = adm_index;
					io.fired.ammo_index = adm->ammo_index;
					io.fired.charge = w.pending_throw_charge;
                    io.fired.target_handle = fire_target.packed;
					io.fired.shooter_pose[0] = p->pos[0];
					io.fired.shooter_pose[1] = p->pos[1];
					io.fired.shooter_pose[2] = p->pos[2];
					io.fired.shooter_pose[3] = p->heading;
					io.fired.shooter_pose[4] = p->pitch;
				}
				}
				w.pending_throw_charge = 0;
			}
		}
	}
	if (ev.dry_fired) ++w.dry_serial;
	if (ev.reload_requested) {
		++w.reload_serial;
		const uint16_t actor_handle = !io.is_authority
				? io.self_wire_handle : world.cached.local_player.packed;
		if (!borrowed_usegun_slot && io.inventory != nullptr &&
				io.inventory->equipped_combo >= 0) {
			io.reload = local_reload_request_wire(world, actor_handle,
					static_cast<uint16_t>(io.inventory->equipped_combo));
		} else if (borrowed_usegun_slot && w.usegun_mount.valid()) {
			// The parameter is the borrowed slot's category * 65 + rank; the
			// addressed entity is local_reload_request_wire's. Wire-header
			// materialization preserves the host's packed handles, so this
			// path is identical for host and joiner.
			// [orig: WeaponAction_Reload @0x5430DB..0x5430E7]
			Entity *mount = world.registry.get(w.usegun_mount);
			WeaponSlotState *mounted_slot = mount != nullptr
					? world.vehicles.resolve_mounted_ammo_slot(*mount)
					: nullptr;
			Entity *slot_owner = mount;
			uint8_t mounted_adm =
					mount != nullptr ? mount->primary_weapon_slot_adm : 0xFF;
			if (mount != nullptr && mounted_slot != nullptr &&
					mounted_slot != &mount->primary_weapon_slot) {
				slot_owner = world.registry.get(mount->ground_target);
				if (slot_owner == nullptr ||
						mounted_slot != &slot_owner->primary_weapon_slot) {
					mounted_slot = nullptr;
				} else {
					mounted_adm = slot_owner->primary_weapon_slot_adm;
				}
			}
			const WeaponTableEntry *mounted_def =
					world.tables.weapons.by_index(mounted_adm);
			if (mounted_slot != nullptr && slot_owner != nullptr &&
					mounted_def != nullptr) {
				io.reload = local_reload_request_wire(world, actor_handle,
						static_cast<uint16_t>(
								static_cast<uint16_t>(mounted_def->category) * 65u +
								static_cast<uint16_t>(mounted_def->rank)));
			}
		}
	}
	if (ev.reload_applied) {
		++w.reload_applied_serial;
		// The refill stamps the 3P body reload-anim window on the entity — 80 ticks; the
		// infantry weapon channel then wants state 65 reload until it expires (and the
		// locked clip plays to its end). In the original the stamp lives inside the
		// refill itself; the SP/listen-host loopback applies it at reload start.
		// [orig: WeaponSlot_ReloadAmmo @ 0x54173c; world-wac-ai-re.md §14.8.5]
		AiEntity *p = world.ai.for_handle(world.cached.local_player);
		if (p != nullptr && p->inf.active) p->inf.reload_anim_ticks = 80;
	}
	// --- the slot-pool bridge: the pool model is authoritative for ammo -------------
	// [orig: the FSM's clip lives on the MountSlot (+0x10) and the reserve is the
	//  per-ammo-class pool — one storage, two views; this port mirrors between the
	//  single-slot FSM state and the inventory]
	if (w.usegun_switch != LocalUseGunSwitch::kNone && ev.switch_completed &&
			active_slot.current == w.usegun_switch_action)
		commit_local_usegun_weapon_switch(world, w);
	if (io.inventory != nullptr && !borrowed_usegun_slot) {
		WeaponInventory &inventory = *io.inventory;
		WeaponInventorySlot *eq = inventory.slot(inventory.equipped_combo);
		const WeaponTableEntry *eq_def = (eq != nullptr && eq->adm_index >= 0)
				? world.tables.weapons.by_index(static_cast<uint8_t>(eq->adm_index))
				: nullptr;
		if (eq != nullptr && eq_def != nullptr) {
			if (ev.reload_applied) {
				// The witnessed refill: refund the remaining clip to the pool, draw a
				// full clip clamped by it [orig: WeaponSlot_ReloadAmmo @ 0x541720,
				// §5.58] — overriding the FSM's single-class transfer (D-WPN-2).
				// eq->clip still holds the pre-reload remaining rounds (mirrored on
				// the previous tick); the refund below consumes it.
				weapon_inventory_reload_slot(world.tables.weapons, inventory,
						inventory.equipped_combo);
				active_slot.clip = weapon_inventory_loaded_rounds(
                        world.tables.weapons, inventory, inventory.equipped_combo);
			} else {
				weapon_inventory_set_loaded_rounds(world.tables.weapons, inventory,
                        inventory.equipped_combo, active_slot.clip);
			}
			active_slot.reserve =
					weapon_pool_get(inventory, eq_def->ammo_class_id);
			// The post-recoil auto-switch [orig: WeaponAction_Recoil tail @ 0x543062:
			// def+0x168 -> Player_SwitchToWeaponByHandle(def[+0x164]*65) — the
			// grenade/LAW switchback, unconditional per throw].
			if (ev.action_finished == weapon_action::kRecoil &&
					eq_def->has_switchcategory) {
				handle_weapon_switch_outcome(world, w, io.inventory,
						weapon_switch_to_handle(world.tables.weapons, inventory,
								eq_def->switchcategory *
										weapon_combo::kRanksPerCategory,
								local_weapon_switch_gates(world, w,
										io.inventory)), view);
			}
		}
		// A queued manual switch commits at the outgoing SWITCHFROM/SWITCHRANK
		// swap seam [orig: the completion consumes g_PendingWeaponSlot].
		if (w.switch_in_flight && ev.switch_completed) {
			commit_pending_weapon_switch(world, w, io.inventory);
		}
	}
	// The FSM's scope legs already ran inside the tick (LocalWeaponScope);
	// the serials count the calls for the dev tools.
	if (ev.unscope) ++w.unscope_serial;
	if (ev.rescope) ++w.rescope_serial;
	// Devtools instrumentation, last: the slot has finished mirroring, so the
	// sample is the state this tick actually ends on.
	weapon_trace_record(w, active_slot, ev, world.logic_tick);
}

WeaponInstallData weapon_install_data_from_def(const DefWeaponDef &row) {
	WeaponInstallData data;
	data.name = row.weapon_name;
	data.animadm = row.animadm;
	data.flags = row.flags;
	data.flags2 = row.flags2;
	data.ammo_cost = row.ammo_class_count;
	data.soundfireloop = row.soundfireloop;
	data.soundtrailoff = row.soundtrailoff;
	data.soundhead = row.soundhead;
	data.soundlockedtone = row.soundlockedtone;

	data.heat_per_shot = row.heat_per_shot;
	data.heat_decay_per_tick = row.heat_decay_per_tick;
	data.heat_glow_threshold = row.heat_glow_threshold;
	data.scope_max_mag = row.scope_max_mag;
	data.scope_initial_mag = row.scope_max_mag_arg2;
	data.scope_min_mag = row.scope_min_mag;
    data.hud_category = row.category;
    data.emplaced_stance = row.emplacedstance;
    // The parser negates the minimum and uses truncated BAM/degree.
    // [orig: WeaponDefs_ParseLineCallback @0x5443D1..0x544440]
    data.pitch_min_bam = int32_t(0u - uint32_t(row.targetpitchmin) * 11930464u);
    data.pitch_max_bam = int32_t(uint32_t(row.targetpitchmax) * 11930464u);
    for (int i = 0; i < 3; ++i) {
        // The parser's float position is authored units * 256; rotation is
        // Q16 degrees * 0x0B60B60, rounded by add/adc 0x8000 before SHRD.
        // [orig: WeaponDefs_ParseLineCallback @0x543680, pos @0x544614..0x5446D8 /
        //  tpos @0x54475B..0x544825]
        data.view_hip_pose.position_q16[i] = row.pos[i] * 256.0f;
        data.view_ads_pose.position_q16[i] = row.tpos[i] * 256.0f;
        const auto rotation_bam = [](int32_t degrees_q16) {
            const int64_t product = static_cast<int64_t>(degrees_q16) * 0x0B60B60 + 0x8000;
            // Logical shift followed by a low-word store reproduces SHRD,
            // including negative products, without a signed right shift.
            return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 16);
        };
        data.view_hip_pose.rotation_bam[i] = rotation_bam(row.pos_rotation_deg_q16[i]);
        data.view_ads_pose.rotation_bam[i] = rotation_bam(row.tpos_rotation_deg_q16[i]);
    }
    data.scope_zero.max_steps = row.scope_max_zero_steps;
    data.scope_zero.min_steps = row.scope_zero_extra;
    data.scope_zero.step_metres = row.scope_zero_step;
    data.scope_zero.default_metres = row.scope_zero_default;
    data.scope_zero.paralax_distance_q16 = row.scope_paralax_distance_fp16;
	data.attack_anim = row.attack_anim;
	data.run_anim = row.run_anim;
	data.clipsize = row.clipsize;
	data.startrounds = row.startrounds;
	// The ACTION rows, verbatim, for the weapon-FSM bake plus the per-ACTION
	// audio/effect hooks [orig: ActionDef_ParseScriptLine @ 0x4023c0 rows;
	// bound by Anim_InitActions @ 0x541fa0].
	data.rows.reserve(row.actions_count);
	for (size_t a = 0; a < row.actions_count; ++a) {
		const DefWeaponAction &act = row.actions[a];
		WeaponFsmActionRow r;
		std::snprintf(r.name, sizeof(r.name), "%s", act.name);
		std::snprintf(r.anim, sizeof(r.anim), "%s", act.anim);
		std::snprintf(r.function, sizeof(r.function), "%s", act.function);
		r.action_value = act.action_value;
		r.delaystart = act.delaystart;
		r.delayend = act.delayend;
		std::snprintf(r.soundset, sizeof(r.soundset), "%s", act.soundset);
		std::snprintf(r.soundsetend, sizeof(r.soundsetend), "%s", act.soundsetend);
		std::snprintf(r.particle, sizeof(r.particle), "%s", act.particle);
		std::snprintf(r.particleuserpoint, sizeof(r.particleuserpoint), "%s",
				act.particleuserpoint);
		data.rows.push_back(r);
	}
	return data;
}

} // namespace opennova::world
