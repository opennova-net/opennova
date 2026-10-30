#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/fire_sound.h>
// The local player's equipped-weapon cluster — moved verbatim from the shell
// binding (S7a, ADR 0028). The pump order, the UseGun borrow, PowerThrow, the
// install bake, and the presentation-event assembly are unchanged; the two
// wire legs (the joiner's fired descriptor, the reload relay) became output
// records the embedder's net layer routes.
#include <runtime/world/player_weapon.h>
#include <runtime/world/player_weapon_internal.h>

#include <runtime/world/vehicle_mount.h>

#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/local_player.h>
#include <runtime/world/mount_controls.h> // the IsTurret gun's carrier and its occupied seats
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

Vec3 local_player_mission_position(const World &world) {
	const Entity *e = world.registry.get(world.cached.local_player);
	return e != nullptr ? e->position : Vec3{};
}

namespace {

// The personal slot's zoom seed owner test: the local player, with the same
// permission the inventory fill passes (player_loadout.cpp).
bool local_slot_zoom_sniper_lock(const World &world, int32_t def_category) {
	const Entity *player = world.registry.get(world.cached.local_player);
	return player != nullptr && weapon_slot_zoom_sniper_lock(player->player_class,
			def_category, world.rules.sniper_zoom_fill);
}

// The install's body past its presentation-epoch clear (local_weapon_install). `view_writes`:
// the install writes the mount request's pitch and view words itself (no request runs around
// it: an embedder's install); a commit (commit_mount: a switch's, a select's, a store no mount
// runs) writes none (mount_queue_switch_out writes them at the request).
void mount_install(World &world, LocalPlayerWeapon &w, const WeaponInstallData &data,
		bool preserve_slot_state, bool allow_same_weapon_rebake, WeaponInventory *inventory,
		PlayerViewState &view, bool view_writes);

// The install payload of the weapon.def row a name's last block holds
// (MissionTables::weapon_defs); false with no parse or no such row.
bool install_data_by_name(const World &world, const std::string &name,
		bool allow_same_weapon_rebake, WeaponInstallData &out) {
	const DefWeaponsFile *defs = world.tables.weapon_defs;
	if (defs == nullptr || name.empty()) return false;
	// The entry the weapon table holds for the name: a name's last block
	// (def_weapon_index_by_name), the one whose descriptors the table baked.
	const int index = def_weapon_index_by_name(defs->entries, defs->count, name.c_str());
	if (index < 0) return false;
	out = weapon_install_data_from_def(defs->entries[index]);
	// The mount runs the descriptors the weapon table baked as it loaded, and
	// its channel plays from the table's shared ANIMADM rings. A same-weapon
	// re-bake (the dev tools' live ACTION edits) bakes the retained row as
	// edited instead. [orig: Player_MountWeaponSlot @ 0x4dfa40; Anim_InitActions
	// @ 0x541fa0 at the def's END]
	out.table_baked = !allow_same_weapon_rebake;
	return true;
}

} // namespace

// The commit's own mount: a switch's commit and an EquippedSlot store mount the
// EquippedSlot's def at once, so the walk's FSM, the PowerThrow branch and the press and
// release legs read the committed slot's def from the commit on, the ticks after it in the
// same frame included, where the slot table's entries carry their own def. The frame's
// pending presentation records stay (in tick order, the switch's own among them): the
// presenter's leg for the switch is device work alone. False when nothing mounts (no
// weapon.def parse or row, no bound local player): the record then leaves the mount to
// the presenter.
// [orig: WeaponAction_ProcessAllEntities @0x542690 - the EquippedSlot read @0x5426AD;
//  WeaponAction_ProcessFrame @0x540E60 - weaponSlot->Def @0x540E71;
//  Input_HandleActionBinding_0 @0x4E0420 - the EquippedSlot @0x4E08E2, its def @0x4E08F2,
//  the PowerThrow sign bit @0x4E08FD]
bool commit_mount(World &world, LocalPlayerWeapon &w, const std::string &name,
		bool preserve_slot_state) {
	LocalPlayer *local = world.local_player_state;
	if (local == nullptr || &local->weapon != &w) return false;
	WeaponInstallData data;
	if (!install_data_by_name(world, name, false, data)) return false;
	mount_install(world, w, data, preserve_slot_state, false,
			local->inventory_valid ? &local->inventory : nullptr, local->view,
			/*view_writes=*/false);
	return true;
}

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
	// A borrowed gun's slot is the EquippedSlot, its def the walks' (D-NET-420).
	if (w.usegun_slot_active) {
		gates.borrowed_def = world.tables.weapons.by_index(w.usegun_weapon_adm);
		gates.equipped_valid = gates.borrowed_def != nullptr;
	}
	const WeaponSlotState *active_slot = active_local_weapon_slot(world, w);
	gates.equipped_action =
			w.active ? active_slot->current : weapon_action::kIdle;
	return gates;
}

// The mounted slot's zoom into its own def's span: the floor its scope_min_mag (the
// sniper lock's max for a class-6 player on a Primary def), the cap its scope_max_mag
// [orig: Player_MountWeaponSlot @0x4DFAC4..0x4DFB13].
void mount_clamp_slot_zoom(const World &world, const WeaponTableEntry *def,
		int32_t &scope_zoom) {
	if (def == nullptr) return;
	ScopeZoomLimits limits;
	limits.scope_min_mag = def->scope_min_mag;
	limits.category = def->category;
	local_player_scope_zoom_mount_clamp(world, limits, def->flags, def->scope_max_mag, scope_zoom);
}

// Player_MountWeaponSlot's request, for every local mount (a weapon key's, a loadout apply's, a
// seat detach's switch back, a UseGun attach's or swap's) on `target`, the mounted slot's def
// (null: none to read). An AbsorbPitch def levels the body's Pitch and takes its elevation span
// as the offset, any other zeroes the offset. Past the g_PendingWeaponSlot store (the caller's) a
// ForceScoped def sets the promoted scope byte and seeds the rangefinder word with its +0xA0;
// another category's def clears the byte and resets the fov target. The NVG latch clears; the
// EquippedSlot queues at once, nothing retained: same category, SWITCHRANK (preempting a draw
// through the pump's counter-0 transition) unless the lower / raise interp runs; cross
// category, a forced SWITCHFROM, refused over a running SWITCHTO / SWITCHFROM, whose queue
// lowers the view (the saved-game load word is zero in a session); then the six view-bias
// words clear, and the first-person camera interp binds the mounted slot, engaging an optic
// def's scope while the byte is set. Ahead of the bind a ForceCrouch def sends the stance
// key's crouch, a reliable C2S 0x1D of action id 0xA9, through the seat-flag query: an
// OnlyScoped def's only while the mounted slot's owner is not the local player (a gun's
// slot, whose owner its attach writes past the mount; `personal_slot` false) or the
// promoted byte, as written above, is set. All of it at the request, a refused or gated
// queue's included: the next SWITCHFROM / SWITCHRANK completion commits the stored word and
// writes none of it (commit_mount's view_writes) but a SWITCHRANK's clear of the scope byte
// for a def with no optic (clear_scope_at_local_switchrank).
// [orig: Player_MountWeaponSlot @0x4DFA40 - AbsorbPitch @0x4DFA7A..0x4DFA86, Pitch = 0
//  @0x4DFAA9, the offset @0x4DFAB3..0x4DFAB7, else 0 @0x4DFABE; @0x4DFB16; ForceScoped
//  @0x4DFB23..0x4DFB2F, g_WeaponScopeActive = 1 @0x4DFB31, dword_B76808 = Def+0xA0
//  @0x4DFB3B..0x4DFB44, the category compare @0x4DFB4C..0x4DFB5E, g_WeaponScopeActive = 0
//  @0x4DFB60, g_CameraFovTargetQ16 @0x4DFB66; dword_B76554 @0x4DFB75, the compare
//  @0x4DFB7B..0x4DFB8B, the interp test @0x4DFB8D..0x4DFB93, WeaponSlot_TryQueueSwitchRank
//  @0x4DFB96, WeaponSlot_ForceQueueSwitchFrom @0x4DFB9E, its return @0x4DFBA6, nextAction == 7
//  @0x4DFBAA..0x4DFBBA, dword_A892C8 @0x4DFBBC, the lower @0x4DFBC5, the bias clear
//  @0x4DFBCF..0x4DFBE8, Entity_CheckWeaponSeatFlags(slot, 0x40000) @0x4DFBF2..0x4DFC02 and the
//  0x1D @0x4DFC04..0x4DFC30, the bind and engage @0x4DFC35..0x4DFC9A; Entity_CheckWeaponSeatFlags
//  @0x540D00 (the slot's def @0x540D09, OnlyScoped @0x540D19..0x540D22, the owner +0x24
//  @0x540D24..0x540D2D, g_WeaponScopeActive @0x540D2F); Entity_AttachToUseGunSlot @0x546B80
//  (the gun slot's owner @0x546C73 past the mount @0x546C38); WeaponAction_ProcessFrame
//  @0x540E60 (@0x5413B1..0x5413EC); callers Entity_AttachToUseGunSlot @0x546B80 (@0x546C38),
//  Entity_AttachToVehicleSlot @0x4946D0 (@0x494838), Entity_DetachFromVehicle @0x4355F0
//  (@0x43565F), NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (@0x4296F4)]
void mount_queue_switch_out(World &world, LocalPlayerWeapon &w, PlayerViewState *view,
		bool same_category, const WeaponTableEntry *target, bool personal_slot,
		int32_t *target_scope_zoom) {
	WeaponInstallData data;
	const bool has_data =
			target != nullptr && install_data_by_name(world, target->name, false, data);
	if (has_data) {
		if ((data.flags & DEF_WEAPON_FLAG_ABSORBPITCH) != 0) {
			local_player_level_pitch(world);
			w.pitch_offset_bam = io::bam_sub(data.pitch_max_bam, data.pitch_min_bam);
		} else {
			w.pitch_offset_bam = 0;
		}
	}
	if (target_scope_zoom != nullptr) mount_clamp_slot_zoom(world, target, *target_scope_zoom);
	if (target != nullptr) {
		if ((target->flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0) {
			if (view != nullptr) view->scope_settled = true;
			w.aim_range_q16 = target->action_fsm.scope_zero.default_metres;
		} else if (!same_category) {
			if (view != nullptr) view->scope_settled = false;
			world.weather.core.scalar_channels.camera_fov_target_fp = 80 << 16;
		}
	}
	w.nvg_scope_restore = false;
	WeaponSlotState *slot = active_local_weapon_slot(world, w);
	if (same_category) {
		if (view == nullptr || !player_view_weapon_lower_raise_active(*view))
			weapon_fsm_queue_switch_rank(*slot);
	} else if (weapon_fsm_queue_switch_from(*slot) && slot->next == weapon_action::kSwitchFrom &&
			view != nullptr) {
		player_view_start_weapon_lower(*view);
	}
	if (view != nullptr) player_view_mount_clear_bias(*view);
	if (target != nullptr && (target->flags & DEF_WEAPON_FLAG_FORCECROUCH) != 0 &&
			((target->flags & DEF_WEAPON_FLAG_ONLYSCOPED) == 0 || !personal_slot ||
			 (view != nullptr && view->scope_settled)))
		world.out.stance_requests.push_back(LocalPlayer::kStanceActionIds[1]);
	if (view != nullptr && has_data)
		player_view_weapon_mount_bind(*view, data.flags, data.view_hip_pose, data.view_ads_pose);
}

bool commit_pending_weapon_switch(World &world, LocalPlayerWeapon &w,
		WeaponInventory *inventory, PendingCommit kind) {
	const bool select = kind != PendingCommit::kSwitch;
	// The pending -> equipped commit [orig: the switchfrom/switchrank completion
	// consumes g_PendingWeaponSlot; EquippedSlot swap + the equippedAdmIndex stamp
	// @ 0x4dd727; the FP model re-resolve runs shell-side off the event].
	// The completion leaves the NVG restore latch alone: the request's reset /
	// mount / select cleared it [orig: WeaponAction_SwitchFrom @0x5433b0 and
	// WeaponAction_SwitchRank @0x543500 call none of Player_SelectWeaponSlot,
	// Player_ResetCameraAndMovementState, Player_MountWeaponSlot].
	w.switch_in_flight = false;
	w.switch_deferred_action = -1;
	if (inventory == nullptr) return false;
	// The completing action is the EquippedSlot's, a borrowed slot held on foot's
	// included, read before the commit replaces it.
	const bool completed_rank = w.active &&
			active_local_weapon_slot(world, w)->current == weapon_action::kSwitchRank;
	Entity *e = world.registry.get(world.cached.local_player);
	const int32_t combo = inventory->pending_combo;
	const WeaponInventorySlot *slot = inventory->slot(combo);
	if (slot == nullptr || slot->adm_index < 0) return false;
	// A SWITCHRANK completion storing the slot the EquippedSlot already is (a loadout apply's
	// mount of the slot its select stored, same category with itself) changes nothing past the
	// stamp: the slot runs on through its SWITCHRANK's tail, no mount, no draw.
	// [orig: WeaponAction_SwitchRank @0x543500 (the store @0x543539, the stamp @0x54355A)]
	if (!select && completed_rank && combo == inventory->equipped_combo &&
			!w.usegun_slot_active && !w.usegun_on_foot && e != nullptr) {
		const WeaponTableEntry *held = world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
		if (held != nullptr && strutil::iequals(held->name, w.def_name)) {
			e->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
			w.start_in_switchto = false;
			w.presentation_pending = false;
			return true;
		}
	}
	inventory->equipped_combo = combo;
	// The personal slot is the EquippedSlot now: a borrow held on foot ends (D-NET-420), and
	// one off its seat whose switch back was refused (the respawn's select, its staged switch
	// back with it) [orig: WeaponAction_SwitchFrom @0x5433B0 (@0x543475); WeaponAction_SwitchRank
	// @0x543500 (@0x543539); Player_SelectWeaponSlot @0x4DD680 (@0x4DD6E0..0x4DD704)].
	const bool seated = e != nullptr && e->mounted &&
			(e->mount_type == SeatType::Controller || e->mount_type == SeatType::Gunner);
	if (w.usegun_on_foot || (w.usegun_slot_active && !seated)) {
		if (!w.usegun_on_foot) w.usegun_switch = LocalUseGunSwitch::kNone;
		w.usegun_on_foot = false;
		w.usegun_slot_active = false;
		w.usegun_mount = EntityHandle{};
		w.usegun_weapon_adm = 0xFF;
		w.usegun_saved_adm = 0xFF;
	}
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
	// second animation and its draw lockout after the mode-switch action. A select
	// stores the slot with no switch action at all (the spawn mount plays none,
	// D-WPN-21). [orig: WeaponAction_SwitchRank @0x543500 vs SwitchFrom @0x543475;
	//  Player_SelectWeaponSlot @0x4DD680 (the store @0x4DD704)]
	w.start_in_switchto = !select && !completed_rank;
	if (e == nullptr) {
		w.presentation_pending = true;
		return true;
	}
	// The entity was present: emit directly, exactly as before (the manual-switch and
	// armory paths are unchanged), and drop any latch so it cannot replay a duplicate.
	w.presentation_pending = false;
	WeaponPresentationEvent event;
	event.tick = world.logic_tick;
	event.world_position = local_player_mission_position(world);
	event.switch_to_weapon = def != nullptr ? def->name : std::string();
	// The committed slot and its def mount with the store (commit_mount): a fresh slot
	// whose draw the switch action's completion queued (start_in_switchto).
	// [orig: WeaponAction_SwitchFrom @0x5433B0 - WeaponSlot_TryQueueSwitchTo @0x5434A3]
	// A switch's commit writes none of the mount's view words (its request did) but a
	// SWITCHRANK's scope byte clear, which the pump runs past this store
	// (clear_scope_at_local_switchrank); a select writes none (PendingCommit).
	event.installed = def != nullptr && commit_mount(world, w, def->name, false);
	w.events.push_back(std::move(event));
	return true;
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
				commit_pending_weapon_switch(world, w, inventory, PendingCommit::kSwitch);
				break;
			}
			// The word is stored (pending_combo); the mount's queue, its NVG latch clear
			// past its early-outs (no equipped slot or def) and its bias clear are the
			// shared leg's (mount_queue_switch_out) [orig: @0x4dfa4d..0x4dfa74].
			w.switch_in_flight = true;
			w.switch_deferred_action = -1;
			WeaponInventorySlot *target =
					inventory != nullptr ? inventory->slot(out.combo) : nullptr;
			mount_queue_switch_out(world, w, &view, out.same_category,
					target != nullptr && target->adm_index >= 0
							? world.tables.weapons.by_index(static_cast<uint8_t>(target->adm_index))
							: nullptr,
					/*personal_slot=*/true, target != nullptr ? &target->scope_zoom : nullptr);
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
	// The EquippedSlot's def: a borrowed gun's slot names the gun's (D-NET-420)
	// [orig: g_LocalPlayerEntity->EquippedSlot @0x4DFA63, its def @0x4DFB7B..0x4DFB8B].
	const WeaponInventorySlot *equipped = inventory.slot(inventory.equipped_combo);
	const WeaponTableEntry *equipped_def = w.usegun_slot_active
			? table.by_index(w.usegun_weapon_adm)
			: equipped != nullptr && equipped->adm_index >= 0
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
	// The current slot word takes the def's own combo, rank + 65 * category
	// [orig: Player_EquipWeaponByEntity @0x4E0370 (@0x4E03A1..0x4E03B2)].
	const WeaponTableEntry *def = world.tables.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	inventory.current_slot = def->category * weapon_combo::kRanksPerCategory + def->rank;
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

namespace {

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
bool install_is_same_weapon_rebake(const LocalPlayerWeapon &w, const WeaponInstallData &data,
		bool allow_same_weapon_rebake) {
	return allow_same_weapon_rebake && w.active && !w.start_in_switchto && !data.name.empty() &&
			strutil::iequals(data.name, w.def_name);
}

} // namespace

void local_weapon_install(World &world, LocalPlayerWeapon &w,
		const WeaponInstallData &data, bool preserve_slot_state,
		bool allow_same_weapon_rebake, WeaponInventory *inventory,
		PlayerViewState &view) {
	// A mount the embedder asks for is a new presentation epoch: no payload from the
	// previous weapon may cross this seam, even though its strings were copied into the
	// pending records. The same-weapon rebake is NOT an epoch: undrained records
	// (including a racing switch commit or deny) must survive it. A switch's own commit
	// mounts with no clear (commit_mount): its records stay in tick order.
	if (!install_is_same_weapon_rebake(w, data, allow_same_weapon_rebake)) w.events.clear();
	mount_install(world, w, data, preserve_slot_state, allow_same_weapon_rebake, inventory,
			view, /*view_writes=*/true);
}

bool local_weapon_install_by_name(World &world, LocalPlayerWeapon &w, const std::string &name,
		bool preserve_slot_state, bool allow_same_weapon_rebake, WeaponInventory *inventory,
		PlayerViewState &view, bool tool_seed) {
	// [orig: Player_MountWeaponSlot @0x4DFA40; the declaration carries the rule]
	WeaponInstallData data;
	if (!install_data_by_name(world, name, allow_same_weapon_rebake, data)) return false;
	data.tool_seed = tool_seed;
	local_weapon_install(world, w, data, preserve_slot_state, allow_same_weapon_rebake,
			inventory, view);
	return true;
}

bool local_weapon_install_spawn_by_name(World &world, LocalPlayerWeapon &w,
		const std::string &name, WeaponInventory *inventory, PlayerViewState &view) {
	// [orig: Player_InitPlayer @0x4E15F0; the declaration carries the rule]
	WeaponInstallData data;
	if (!install_data_by_name(world, name, false, data)) return false;
	w.events.clear(); // a new presentation epoch, as local_weapon_install's
	mount_install(world, w, data, /*preserve_slot_state=*/false,
			/*allow_same_weapon_rebake=*/false, inventory, view, /*view_writes=*/false);
	return true;
}

namespace {

void mount_install(World &world, LocalPlayerWeapon &w, const WeaponInstallData &data,
		bool preserve_slot_state, bool allow_same_weapon_rebake, WeaponInventory *inventory,
		PlayerViewState &view, bool view_writes) {
	const bool same_weapon_rebake =
			install_is_same_weapon_rebake(w, data, allow_same_weapon_rebake);
	// The NVG scope restore latch is the switch request's to clear (the reset,
	// the mount, the select); the install that follows a committed switch, and
	// the render-side rebake, leave it [orig: dword_B76554's writers
	// @0x4dd730 / @0x4dd7fe / @0x4de2b9 / @0x4dfb75; WeaponAction_SwitchFrom
	// @0x5433b0 / SwitchRank @0x543500 write none].
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
    if (!same_weapon_rebake && view_writes) {
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
		// The mount reads the def's own slot of the slot table, at its combo (rank +
		// 65 * category): clip = the slot's loaded rounds, reserve = the def's
		// ammo-class pool, the scope words the slot's. Retail mounts nothing but a
		// slot of that table, so a def the loadout never landed reads its slot's
		// cleared words: no rounds, the pool's count, the init's scope seeds above.
		// [orig: MountSlot+0x10 + Entity_GetScoreValueBySlotType @0x5406e0; the slot
		//  table + 100 * (rank + 65 * category) @0x5415D3; WeaponSlot_InitFromDef
		//  @0x53EE70 (the memset @0x53EE86), whose callers each set the rounds they
		//  load (WeaponSlot_SetSharedLoadedRounds from PlayerSlot_ReinitListedSlots
		//  @0x4FE2C3)]
		bool ammo_from_inventory = false;
		const WeaponTableEntry *own_def = installed >= 0
				? world.tables.weapons.by_index(static_cast<uint8_t>(installed))
				: nullptr;
		if (inventory != nullptr && own_def != nullptr) {
			const int32_t combo = own_def->rank +
					weapon_combo::kRanksPerCategory * own_def->category;
			const WeaponInventorySlot *own = inventory->slot(combo);
			const bool landed = own != nullptr && own->adm_index == installed;
			if (landed) {
				w.slot.clip = weapon_inventory_loaded_rounds(world.tables.weapons, *inventory, combo);
                w.slot.scope_zero = own->scope_zero;
                w.slot.scope_zoom = own->scope_zoom;
                w.slot.zero_pitch = weapon_scope_zero_pitch(w.def.scope_zero, own->scope_zero);
                w.slot.zero_yaw = weapon_scope_zero_yaw(w.def.scope_zero, own->scope_zero);
			} else {
				w.slot.clip = 0;
			}
			w.slot.shared_clip = own_def->ammo_bucket != 0;
			w.slot.reserve = weapon_pool_get(*inventory, own_def->ammo_class_id);
			// A tool's mount of a def the loadout never landed seeds below.
			ammo_from_inventory = landed || !data.tool_seed;
		}
		if (!ammo_from_inventory) {
			// No slot table to read (a harness or the editor's weapon preview with no
			// inventory, a def the weapon table lacks), or a tool's mount of a def the
			// loadout never landed (WeaponInstallData::tool_seed). Every game mount
			// passes the local player's inventory and no tool seed (the spawn and the
			// switch commit), so this seed, a full magazine and the def's carried
			// reserve, is the tools' alone.
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
	// SWITCHFROM was displaced by this mount's fresh slot) resumes through the
	// deferred latch once the draw completes, so the pending combo still commits.
	// A slot-preserving mount (an EquippedSlot store) displaces nothing: a word the
	// switch-out left stored (a gated same-category mount's, a refused SWITCHFROM's)
	// stays stored with nothing queued, as retail's g_PendingWeaponSlot does until
	// a completion [orig: Player_MountWeaponSlot @0x4DFB8D..0x4DFB9E;
	// g_PendingWeaponSlot's only commits WeaponAction_SwitchFrom @0x543475 /
	// WeaponAction_SwitchRank @0x543539].
	if (w.switch_in_flight && !preserve_slot_state)
		w.switch_deferred_action = weapon_action::kSwitchFrom;
	// A mount leaves a running PowerThrow windup alone: the walk clears it ahead of the mount
	// (local_weapon_walk_clears_windup, past the walk's own gates), and a mount no walk runs (a
	// UseGun's, a ToSpecial switch's, a loadout apply's) carries it onto the new slot, whose
	// release requests fire there (local_weapon_pump_tick). [orig: Player_MountWeaponSlot
	//  @0x4DFA40 writes no g_FireChargeStartTick (@0xB76800); Player_SwitchToWeaponByHandle
	//  @0x4E019B;
	//  Entity_AttachToUseGunSlot @0x546B80 (the mount @0x546C38); Input_HandleActionBinding_0
	//  case 0xDC's Player_EquipWeaponByEntity @0x4E11B2 / @0x4E125B]
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
	w.power_throw_released = false;
	w.fire_press_dispatched = w.fire_press_dry = false;
	// A cross-category non-ForceScoped mount resets the target, not its current.
	// [orig: Player_MountWeaponSlot @0x4DFB44..0x4DFB66]
	const int old_index = world.tables.weapons.index_of(w.def_name.c_str());
	const int new_index = world.tables.weapons.index_of(data.name.c_str());
    const bool category_changed = old_index >= 0 && new_index >= 0 &&
            world.tables.weapons.entries[old_index].category !=
            world.tables.weapons.entries[new_index].category;
    if (view_writes) {
        if (category_changed && (data.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0)
            world.weather.core.scalar_channels.camera_fov_target_fp = 80 << 16;
        // [orig: dword_B76808 = Def+0xA0 @0x4DFB3B..0x4DFB44]
        if ((data.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0)
            w.aim_range_q16 = data.scope_zero.default_metres;
        player_view_weapon_mount(view, data.flags, category_changed);
    }
	w.def_name = data.name;
	w.active = true;
}

} // namespace

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
	w.power_throw_released = false;
	w.fire_press_dispatched = w.fire_press_dry = false;
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
	// The ammo leg, for defs that carry the flag: the pool, plus the loaded rounds
	// only for a one-round clip, both read through the entity's seat; a zero total
	// hides the weapon. [orig: @0x4dcbea -> Entity_GetScoreValueBySlotType
	// @0x4dcbf5; the clip legs clipsize != -1 @0x4dcc08 and == 1 @0x4dcc0d, the bucket
	// WeaponSlot_GetSharedLoadedRounds @0x4dcc1b or slot+0x10 @0x4dcc25; the zero test @0x4dcc2b]
	if ((def->flags & weapon_flag::kNoClipsNoDraw) != 0) {
		const WeaponAmmoSeat seat = entity_ammo_seat(world, entity);
		int32_t total = weapon_seat_pool(seat, def->ammo_class_id,
				weapon_pool_get(inventory, def->ammo_class_id));
		if (def->clipsize == 1) {
			const int32_t own = weapon_inventory_loaded_rounds(world.tables.weapons, inventory,
					inventory.equipped_combo);
			total += def->ammo_bucket != 0 ? weapon_seat_bucket(seat, own) : own;
		}
		if (total == 0) return false;
	}
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
		w.fire_press_dispatched = w.fire_press_dry = false;
		return;
	}
	w.fire_held = fire_held;
	w.fire_pressed = w.fire_pressed || fire_pressed; // latch until consumed
	w.reload_pressed = w.reload_pressed || reload_pressed;
}

uint32_t local_weapon_input_tick(const World &world) {
	// [orig: Game_ProcessMainFrame @0x5265B4 ahead of @0x526608 / @0x52661D; the declaration
	//  carries the derivation]
	return world.logic_tick + 1;
}

namespace {

// The PowerThrow windup test on the equipped slot: WeaponSlot_CanFireInCurrentState (the
// current action alone: IDLE, and on an automatic def FIRE, RECOIL, SCOPEUP and SCOPEDOWN too)
// and WeaponSlot_HasAmmoAvailable (rounds loaded or in the pool; a clipless def's pool) pass:
// the windup's start, the input phase's tick; else 0. Neither reads the queued action or a
// switch, so a reload record ahead of a key's press in the same drain, or a walk whose
// SWITCHFROM the pump has not entered yet, leaves the press its windup (a draw in progress
// is SWITCHTO's, which refuses it). [orig: Input_ProcessBindings @0x4DDACB / @0x4DDAE4;
//  Input_HandleActionBinding_0 case 0x95's press leg @0x4E0922 / @0x4E0935;
//  WeaponSlot_CanFireInCurrentState @0x53F0B0; WeaponSlot_HasAmmoAvailable @0x541B30]
uint32_t power_throw_windup_start(World &world, LocalPlayerWeapon &w) {
	const WeaponSlotState &slot = *active_local_weapon_slot(world, w);
	return weapon_fsm_can_fire_in_current_state(w.def, slot) &&
					weapon_fsm_has_ammo_available(w.def, slot)
			? local_weapon_input_tick(world)
			: 0u;
}

// The fire face: after case 0x95's fire request returns nonzero, the local player's facial
// slot (none: nothing) takes expression 4 for 15 transitions and its gaze straight ahead, (0,
// 0), for 15 steps. The legs read the request's return at their record, as retail's call
// returns it there: the pump requests the latched press or throw on the same slot, whose
// current action only the walk moves between them, so the face lands ahead of the world
// tick's facial step as retail's does ahead of its entity update.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - the press leg @0x4E0A25..0x4E0A78, the
//  release leg @0x4E087F..0x4E08D2: the slot word entity+0x1B8 (0: none @0x4E0A33 /
//  @0x4E088D), its row unk_26C77A0 + 100 * (word & 0x7FFF): +0x2C = 4, +0x30 = 15,
//  +0x50 = 15, +0x48 / +0x4C = 0.0; the fields as FaceAnim_UpdateSlot @0x57FA50 reads them
//  (+0x2C the override @0x57FACB, +0x30 its timer @0x57FAD2, +0x48 / +0x4C the directed gaze
//  @0x57FB2C / @0x57FB48, +0x50 its timer @0x57FB25..0x57FB35); Game_ProcessMainFrame
//  @0x5263F0 - Input_ProcessFrame @0x52661D ahead of Entity_UpdateAllEntities @0x52674B
//  (FaceAnim_UpdateAllSlots @0x580000, the call @0x4C21FB)]
void local_player_fire_face(World &world) {
	const Entity *player = world.registry.get(world.cached.local_player);
	FacialSlot *face = player != nullptr ? world.facials.for_entity(*player) : nullptr;
	if (face == nullptr) return;
	face->expression_override = 4; // +0x2C
	face->override_timer = 15;     // +0x30
	face->directed_timer = 15;     // +0x50
	face->directed_eyes = {};      // +0x48 / +0x4C
}

} // namespace

void local_weapon_left_button_down(World &world, LocalPlayerWeapon &w,
		const PlayerViewState &view) {
	// [orig: Input_ProcessBindings @0x4DDA88..0x4DDAFE; the declaration carries the gates]
	if (view.binoculars_view_active || !w.active) return;
	const Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	if (player->mounted && is_vehicle_control_seat(player->mount_type)) return;
	w.power_throw_start_tick = (w.def.flags & weapon_flag::kPowerThrow) != 0
			? power_throw_windup_start(world, w) : 0u; // @0x4DDAF6 / @0x4DDAFE
}

bool local_weapon_power_throw_press(World &world, LocalPlayerWeapon &w) {
	// [orig: Input_HandleActionBinding_0 case 0x95 @0x4E08DC..0x4E095C; the declaration
	//  carries the order]
	if (!w.active || w.power_throw_start_tick != 0) return false; // @0x4E08EC / @0x4E0915
	w.power_throw_start_tick = power_throw_windup_start(world, w); // @0x4E0947 / @0x4E0954
	return w.power_throw_start_tick != 0;
}

void local_weapon_walk_clears_windup(const World &world, LocalPlayerWeapon &w) {
	// [orig: Player_SwitchToWeaponByHandle @0x4E0170; the declaration carries the gate]
	const Entity *player = world.registry.get(world.cached.local_player);
	const SeatType seat = player != nullptr && player->mounted ? player->mount_type : SeatType::None;
	if (seat == SeatType::Gunner || seat == SeatType::Controller || seat == SeatType::Driver)
		return; // @0x4E017D / @0x4E0186 / @0x4E018F
	w.power_throw_start_tick = 0; // @0x4E019B
}

bool local_weapon_release_fire(World &world, LocalPlayerWeapon &w) {
	// [orig: Input_HandleActionBinding_0 case 0x95's release leg @0x4E07E9..0x4E0879; the
	//  declaration carries the order]
	if (w.power_throw_start_tick == 0) return false; // @0x4E07EF
	const int32_t held = static_cast<int32_t>(local_weapon_input_tick(world) -
			w.power_throw_start_tick); // @0x4E07F7..0x4E07FC
	w.power_throw_start_tick = 0; // @0x4E0801
	// A lone gunner's release ends the windup and requests nothing (DFX2-37; the declaration
	// carries the witness).
	if (local_solo_turret_holds_fire(world)) return false;
	w.pending_throw_charge = power_throw_charge_from_hold(held);
	w.power_throw_released = true; // the request @0x4E086F, the pump's
	if (w.active && weapon_fsm_fire_request_taken(w.def, *active_local_weapon_slot(world, w)))
		local_player_fire_face(world); // @0x4E0877..0x4E08D2
	return true;
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

bool local_solo_turret_holds_fire(const World &world) {
	const Entity *player = world.registry.get(world.cached.local_player);
	const Entity *mount = player != nullptr && player->mounted
			? world.registry.get(player->mount_target) : nullptr;
	if (mount == nullptr || !mount->has_item_def ||
			(mount->item_attrib2 & opennova::def::DEF_ITEM_ATTRIB2_ISTURRET) == 0)
		return false;
	if (!world.rules.title().emplaced_solo_holds_fire) return false;
	const Entity *carrier = entity_ground_word(world, *mount);
	return carrier != nullptr && emplaced_carrier_occupied_seats(world, *carrier) <= 1;
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
//  @0x5430EA..0x543103 -> NetPacket_SendReloadRequest @0x432930]
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
	// never reads the word. [orig: NetPacket_SendReloadRequest
	//  `test byte ptr [eax+54h], 20h` @0x43296C -> 0xFFFF @0x432974, else the
	//  caller's parameter @0x43299B; WeaponSlot_ReloadAmmo @0x54172D]
	const Entity *addressed = seat_parent != nullptr ? seat_parent : actor;
	out.reload_param = addressed != nullptr && addressed->has_item_def &&
			(addressed->item_attrib & kItemAttribEweap) != 0 ? uint16_t(0xFFFF) : param;
	return out;
}

namespace {

// The personal slot's shared-bucket mirror: a bucket weapon's loaded rounds and its
// pool read through the player's seat as it stands when they are read, on a
// controller seat 0 and on a gunner seat the carrier's clip. [orig: WeaponSlot_GetSharedLoadedRounds from
// WeaponSlot_CanFire @0x541C79, the reload gate @0x4E12A7 / @0x4E12C6,
// WeaponAction_Idle @0x542990, WeaponAction_EmptyIdle @0x542A8C, WeaponAction_Recoil
// @0x542E07, HUD_BuildEntityInfo @0x4B85D3; the pool from WeaponSlot_CanFire
// @0x541C9B; Entity_GetScoreValueBySlotType @0x5406E0]
void read_personal_bucket(const World &world, WeaponSlotState &slot,
		const WeaponInventory &inventory, const WeaponAmmoSeat &seat) {
	const auto *eq = inventory.slot(inventory.equipped_combo);
	const auto *def = eq != nullptr && eq->adm_index >= 0
			? world.tables.weapons.by_index(static_cast<uint8_t>(eq->adm_index)) : nullptr;
	slot.shared_clip = def != nullptr && def->ammo_bucket != 0;
	if (!slot.shared_clip) return;
	slot.clip = weapon_seat_bucket(seat, weapon_inventory_loaded_rounds(world.tables.weapons,
			inventory, inventory.equipped_combo));
	slot.reserve = weapon_seat_pool(seat, def->ammo_class_id,
			weapon_pool_get(inventory, def->ammo_class_id));
}

} // namespace

bool local_weapon_request_reload(World &world, LocalPlayerWeapon &w,
		const PlayerViewState &view, WeaponInventory *inventory) {
	// [orig: Input_HandleActionBinding_0 case 0xD3 @0x4E1278..0x4E12F3; the declaration
	//  carries the gates]
	if (view.binoculars_view_active || !w.active) return false;
	// The case has no seat test: a Driver's EquippedSlot, or an unarmed Controller's, stays
	// its own weapon's, which the weapon walk pumps whatever the seat, so the reload runs
	// there too. The pump's other blocks (a dead body, a UseGun switch in flight) stand.
	// [orig: Entity_AttachToVehicleSlot @0x4946D0 - the Driver (5) leg @0x4948A6..0x494936
	//  leaves the EquippedSlot, the carrier's slot replaces it only on an EWeap
	//  @0x49480F..0x494875; WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1]
	const LocalWeaponInputBlock block = local_weapon_input_block(world, w);
	if (block != LocalWeaponInputBlock::kNone && block != LocalWeaponInputBlock::kSeat)
		return false;
	WeaponSlotState &slot = *active_local_weapon_slot(world, w);
	const Entity *player = world.registry.get(world.cached.local_player);
	if (inventory != nullptr && !w.usegun_slot_active)
		read_personal_bucket(world, slot, *inventory,
				player != nullptr ? entity_ammo_seat(world, *player) : WeaponAmmoSeat{});
	if (!weapon_fsm_reload_allowed(w.def, slot)) return false;
	weapon_fsm_request_reload(slot);
	return true;
}

bool local_weapon_press_fire(World &world, LocalPlayerWeapon &w, const PlayerViewState &view) {
	// [orig: Input_HandleActionBinding_0 case 0x95's press leg @0x4E08DC..0x4E0A1E; the
	//  declaration carries the order]
	if (view.binoculars_view_active || !w.active) return false; // no EquippedSlot @0x4E08EC
	bool slot_has_def = false;
	const uint8_t slot_adm = local_equipped_slot_def_adm(world, w, slot_has_def);
	const WeaponTableEntry *def = slot_has_def ? world.tables.weapons.by_index(slot_adm) : nullptr;
	if (def != nullptr && !world.script.weapon_input.record_fire_request(def->category))
		return false; // @0x4E0968 / @0x4E0976
	// OnlyFireScoped and OnlyFireLocked are the EquippedSlot def's flags (a borrowed carrier
	// slot's on a D-WPN-52 route); a slot with no def skips both [orig: EquippedSlot->Def+8
	// @0x4E097C / @0x4E09AC; no def @0x4E08F7 -> @0x4E09CB].
	const int32_t slot_flags = def != nullptr ? def->flags : 0;
	if ((slot_flags & weapon_flag::kOnlyFireScoped) != 0 &&
			(!local_player_scope_view_visible(world, w, view) || view.camera_mode != 0))
		return false; // @0x4E098D / @0x4E09A0
	bool dry = false;
	if ((slot_flags & weapon_flag::kOnlyFireLocked) != 0) {
		const Entity *player = world.registry.get(world.cached.local_player);
		const AiEntity *body = player != nullptr ? world.ai.for_handle(player->handle) : nullptr;
		dry = body == nullptr || (body->slot.f[2] & 1) == 0; // @0x4E09BD..0x4E09C9
	}
	if (local_weapon_input_block(world, w) != LocalWeaponInputBlock::kNone) return false;
	// From a seat other than the Controller's (the Driver's refused above), a lone gunner
	// latches no press (DFX2-37; local_solo_turret_holds_fire carries the witness).
	const Entity *seated = world.registry.get(world.cached.local_player);
	if (seated != nullptr && seated->mount_type != SeatType::Controller &&
			local_solo_turret_holds_fire(world))
		return false;
	local_weapon_set_input(w, view, true, true, false);
	w.fire_press_dispatched = true;
	w.fire_press_dry = dry;
	if (weapon_fsm_fire_request_taken(w.def, *active_local_weapon_slot(world, w)))
		local_player_fire_face(world); // @0x4E0A0E..0x4E0A78
	return true;
}

namespace {

// A local SWITCHFROM completion's raise, past its commit and the draw it queued: the
// committed EquippedSlot's def, as the commit mounted it (commit_mount), one step for an
// Emplaced def. A SWITCHRANK's commit raises nothing.
// [orig: WeaponAction_SwitchFrom @0x5433B0 - the store @0x543475,
//  WeaponSlot_TryQueueSwitchTo @0x5434A3, Player_StartWeaponLowerRaiseInterp(1) @0x5434AA;
//  WeaponAction_SwitchRank @0x543500 calls none]
void start_local_weapon_raise(const World &world, const LocalPlayerWeapon &w,
		PlayerViewState &view) {
	const WeaponFsmDef *def = local_equipped_slot_def(world, w);
	player_view_start_weapon_raise(view,
			def != nullptr && (def->flags & weapon_flag::kEmplaced) != 0);
}

// A local SWITCHRANK completion's scope write, past its store and its AdmDef stamp: the
// committed EquippedSlot's def, as the commit mounted it, with neither Scoped nor Sighted
// (Flags & 3) clears the promoted scope byte, the bare write; an optic def keeps it. The
// store of the slot the EquippedSlot already is (a loadout apply's mount) clears it the same.
// A SWITCHFROM's commit writes no view word.
// [orig: WeaponAction_SwitchRank @0x543500 - the store @0x543539, the stamp @0x54355A, the
//  EquippedSlot def's Flags & 3 @0x543564..0x54357A, g_WeaponScopeActive = 0 @0x54357C;
//  WeaponAction_SwitchFrom @0x5433B0 (@0x543464..0x5434AA) writes none]
void clear_scope_at_local_switchrank(const World &world, const LocalPlayerWeapon &w,
		PlayerViewState &view) {
	const WeaponFsmDef *def = local_equipped_slot_def(world, w);
	if (def != nullptr && (def->flags & (DEF_WEAPON_FLAG_SCOPED | DEF_WEAPON_FLAG_SIGHTED)) == 0)
		player_view_clear_scope_promoted(view);
}

} // namespace

void local_weapon_pump_tick(World &world, LocalPlayerWeapon &w,
		LocalWeaponPumpIO &io) {
	io.map_command = 0;
	io.fired = LocalWeaponFiredWire{};
	io.reload = LocalWeaponReloadWire{};
	io.fire_deferred = false;
	if (io.view == nullptr || !world.cached.local_player.valid()) return;
	PlayerViewState &view = *io.view;
	sync_local_usegun_weapon_transition(world, w);
	if (!w.active) return;
	WeaponSlotState &active_slot = *active_local_weapon_slot(world, w);
	const Entity *player = world.registry.get(world.cached.local_player);
	const bool player_alive =
			player != nullptr && player->alive && player->health > 0;
	const bool usegun_switch_pending =
			w.usegun_switch != LocalUseGunSwitch::kNone;
	// A running PowerThrow windup survives the dead body (below), as it does a UseGun switch in
	// flight and a def with no PowerThrow flag: the walk never writes g_FireChargeStartTick.
	// Its writers are the input legs (the left button's down, case 0x95's press and release
	// legs), the walk Player_SwitchToWeaponByHandle (Player_InitPlayer's at a respawn,
	// LocalPlayer::respawn_init) and the round's init.
	// [orig: g_FireChargeStartTick @0xB76800 - Input_ProcessBindings @0x4DDAF6 / @0x4DDAFE;
	//  Input_HandleActionBinding_0 @0x4E0801 / @0x4E0947 / @0x4E0954;
	//  Player_SwitchToWeaponByHandle @0x4E019B; Game_InitNewRound @0x42277E;
	//  DialogSystem_Init @0x52776D]
	if (!player_alive && !usegun_switch_pending) {
		active_slot.refire_queued = false;
		w.pending_throw_charge = 0;
		w.fire_pressed = false;
		w.reload_pressed = false;
		w.power_throw_released = false;
		w.fire_press_dispatched = w.fire_press_dry = false;
		return;
	}
	// Capture the outgoing slot identity. A switch completion below changes the
	// active selection, but this tick's ammo bridge still belongs to the slot the
	// FSM actually pumped.
	const bool borrowed_usegun_slot = w.usegun_slot_active;
	// The slot's ammo reads go through the player's seat: on a controller seat a
	// shared bucket reads 0 and a pool 0 unless the carrier is EWeap, and the
	// bucket is never written there. [orig: WeaponSlot_SetSharedLoadedRounds @0x540684..0x540692]
	const WeaponAmmoSeat ammo_seat =
			player != nullptr ? entity_ammo_seat(world, *player) : WeaponAmmoSeat{};
	if (io.inventory != nullptr && !borrowed_usegun_slot)
		read_personal_bucket(world, active_slot, *io.inventory, ammo_seat);
	WeaponFsmInputs in;
	// The action binding permits an armed controller's borrowed carrier slot.
	// [orig: Input_HandleActionBinding_0 @0x4E09CB..0x4E09FF]
	const LocalWeaponInputBlock input_block = local_weapon_input_block(world, w);
	const bool accept_weapon_input = input_block == LocalWeaponInputBlock::kNone;
	in.fire_held = accept_weapon_input && w.fire_held;
	in.fire_pressed = accept_weapon_input && w.fire_pressed;
	// PowerThrow: the press never fires; the windup's start and its end are the input
	// phase's (the left button's down, the fire row's press and release legs at their records:
	// local_weapon_left_button_down, local_weapon_power_throw_press, local_weapon_release_fire),
	// and the pump requests the throw the release leg took, with its charge byte.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0x95's release leg
	//  WeaponSlot_RequestFire(EquippedSlot, 0x96, charge) @0x4E086F; the press leg's
	//  PowerThrow branch @0x4E08FD..0x4E095C requests none; world-wac-ai-re §27]
	// The dead body and a UseGun switch in flight drop a release's throw, and the windup runs
	// on (above). The seat drops neither: the PowerThrow branch sits ahead of the press leg's
	// seat test and the release leg has none, a Driver's EquippedSlot (an unarmed Controller's too) stays its own weapon's,
	// and the walk, the fire action and the send test no seat, so a key's windup throws from
	// those seats. [orig: Input_HandleActionBinding_0 - the PowerThrow sign bit @0x4E08FD ahead
	//  of the seat test @0x4E09CB..0x4E09DE; Entity_AttachToVehicleSlot @0x4946D0 - the Driver
	//  leg @0x4948A6..0x494936; WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1;
	//  WeaponAction_Fire @0x542B10; Entity_FireWeaponAndSendPacket @0x42BD80]
	bool power_throw_release = false;
	// The branch reads the EquippedSlot's def, which its commit mounted (commit_mount)
	// [orig: Input_HandleActionBinding_0 @0x4E08E2 / @0x4E08F2 / @0x4E08FD].
	if ((w.def.flags & weapon_flag::kPowerThrow) != 0) {
		in.fire_held = false;
		in.fire_pressed = false;
		if (!accept_weapon_input && input_block != LocalWeaponInputBlock::kSeat) {
			w.pending_throw_charge = 0;
		} else if (w.power_throw_released) {
			in.fire_pressed = true;
			power_throw_release = true;
		}
	} else {
		// A sourceless press meets WAC's category gate here, before the seat and
		// fireability rejection; a press dispatched through case 0x95's press leg at its
		// record (a deferred refire's among them) passed it there (local_weapon_press_fire).
		// PowerThrow uses its separate press/release path above. Do not cancel an accepted
		// FIRE: only refuse the new binding request.
		// [orig: Input_HandleActionBinding_0 @0x4E0420 (Input_IsBindingActive(149)
		//  @0x4E07D9 ahead of the gate @0x4E0968..0x4E097B)]
		const bool undispatched_press = w.fire_pressed && !w.fire_press_dispatched;
		if (player_alive && undispatched_press) {
			// The gate reads the EquippedSlot's def, not the AdmDef byte.
			// [orig: Input_HandleActionBinding_0 @0x4E0420 - the EquippedSlot
			//  @0x4E08DC..0x4E08E2, its def @0x4E08F2, the def's Type @0x4E0961]
			bool slot_has_def = false;
			const uint8_t slot_adm = local_equipped_slot_def_adm(world, w, slot_has_def);
			const WeaponTableEntry *def =
					slot_has_def ? world.tables.weapons.by_index(slot_adm) : nullptr;
			if (def != nullptr && !world.script.weapon_input.record_fire_request(def->category))
				in.fire_pressed = false;
			// So does the press leg's lone-gunner gate (DFX2-37, local_solo_turret_holds_fire).
			if (player->mount_type != SeatType::Controller && local_solo_turret_holds_fire(world))
				in.fire_pressed = false;
		}
		// A release whose windup ran on into this slot (a UseGun mount, a ToSpecial switch:
		// neither walks) requests fire here with its charge, as the PowerThrow branch's throw
		// does: the release leg tests no def, and the request writes the charge byte whatever
		// it then queues. Blocked as the branch's is, it drops the charge.
		// [orig: Input_HandleActionBinding_0 @0x4E0420 - the release leg @0x4E07E9..0x4E086F
		//  (WeaponSlot_RequestFire(EquippedSlot, 0x96, charge) @0x4E086F, no def test);
		//  WeaponSlot_RequestFire @0x53EFA0 (the charge byte +0x5C @0x53EFB5)]
		if (w.power_throw_released) {
			if (accept_weapon_input || input_block == LocalWeaponInputBlock::kSeat) {
				in.fire_pressed = true;
				power_throw_release = true;
			} else {
				w.pending_throw_charge = 0;
			}
		}
	}
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = accept_weapon_input && w.reload_pressed &&
			weapon_fsm_reload_allowed(w.def, active_slot);
	// OnlyFireLocked with no lock at the press's dispatch: its accepted request is the dry
	// click [orig: @0x4E0A16..0x4E0A1E].
	in.fire_dry = in.fire_pressed && w.fire_press_dry;
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
	in.instant_emplaced_switch = local_usegun_switch_is_instant(world, w, io.inventory);
	in.pending_emplaced = local_pending_slot_is_emplaced(world, w, io.inventory);
	// The heat window is a deadline against the logic tick, not a stored level.
	// [orig: current_tick @ 0x24C1968]
	in.current_tick = static_cast<int32_t>(world.logic_tick);
	// The heat window's water gate rides the environment inputs (the walked
	// entity's body Z against the water word).
	if (player != nullptr) weapon_fire_environment_inputs(world, *player, in);
	WeaponFsmEvents ev;
	// The slot runs its own def, the EquippedSlot's from the commit that stored it on (the
	// commit mounts it, commit_mount) [orig: WeaponAction_ProcessAllEntities @0x5426AD ->
	// WeaponAction_ProcessFrame @0x540E60 - weaponSlot->Def @0x540E71].
	weapon_fsm_tick(w.def, active_slot, in, ev);
	// The overheat emitter's leg reads the walked body, the local player, and its own
	// camera word [orig: WeaponAction_ProcessAllEntities @0x5426BF].
	if (player != nullptr) {
		bool slot_has_def = false;
		const uint8_t slot_adm = local_equipped_slot_def_adm(world, w, slot_has_def);
		if (const WeaponTableEntry *slot_def =
						slot_has_def ? world.tables.weapons.by_index(slot_adm) : nullptr) {
			HeatGlowWalk walk = heat_glow_walk_of(world, *player);
			walk.first_person = view.camera_mode == 0;
			weapon_heat_glow_update(world, walk, *slot_def, active_slot, ev);
		}
	}
	// The tick's re-queue leaves as a deferred fire record (its dispatch next input frame
	// runs the record's gates: the head, the cease fire, the live binding, the press leg's).
	io.fire_deferred = active_slot.refire_queued;
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
	w.power_throw_released = false;
	w.fire_press_dispatched = w.fire_press_dry = false;
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
			// The firing slot's def names the round's and the C2S 0x06's adm.
			// [orig: WeaponAction_Fire @0x542B10 - @0x542C1C..0x542C3F]
			const uint8_t adm_index = local_weapon_fire_adm(world, w, *shooter);
			const WeaponTableEntry *adm = world.tables.weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
                int32_t pose[6];
                local_weapon_fire_pose(world, w, ev.fired_clip_before_consume,
                        player_view_scope_settled(view), pose);
                const FixedVec3 fire_origin{pose[0], pose[1], pose[2]};
				// The fire tail reads the gun's point for the slot as the FSM
				// leaves it: next = RECOIL (the mflash column), the clip spent. It
				// runs whatever the send below did: the action ignores the send's
				// return. [orig: WeaponAction_Fire tail @0x542D1F..0x542D5B; the
				//  send's call @0x542C5E]
				world.vehicles.weapon_recoil(*shooter,
						w.def.actions[weapon_action::kFire].action_value, adm,
						active_local_weapon_slot(world, w)->clip, 1, pose);
				// The send refuses every shot under a cease fire at its head: no
				// round, no ring record, no C2S 0x06. Past it, the authority arm
				// passes its own-slot gate, then the round validation.
				// [orig: Entity_FireWeaponAndSendPacket @0x42BD80 - g_InCeaseFire
				//  @0x42BDB3, the exit @0x42BDC5 -> @0x42C07A; @0x42be3a ahead of
				//  Server_ClientFiredRound @0x42bf34]
				const bool accepted = !world.rules.cease_fire && (!io.is_authority ||
						(io.authority_fire_admitted &&
						 weapon_fire_owner_status(world, *shooter, adm, false) == 0 &&
						 weapon_fire_origin_status(world, *shooter, *adm, fire_origin, false) == 0));
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
                // The +0xC word, an entity or a round (a flare the heat lock took).
                const TargetRef fire_target = AiSlot::target_of(p->slot.f[3]);
                shooter->last_fire_target = fire_target;

				RoundEvent round_event;
				round_event.shooter_handle = !io.is_authority
						? io.self_wire_handle
						: world.cached.local_player.packed;
				round_event.origin_x = pose[0];
				round_event.origin_y = pose[1];
				round_event.origin_z = pose[2];
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
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
				RoundSpawnParams round;
				round.owner = world.cached.local_player;
				round.shooter_handle = round_event.shooter_handle;
				round.origin.set_q16({pose[0], pose[1], pose[2]}); // the fire pose's words (D-NET-507)
				round.dir_yaw_bam = dir_yaw;
				round.dir_pitch_bam = dir_pitch;
				round.ammo_index = adm->ammo_index;
				round.adm_index = adm_index;
				// The fire carries the fresh word: the pool mints the round's id from
				// its one counter, and the ring record (the host's) or the C2S 0x06
				// hit_part (a joiner's) takes the spawned round's word.
				// [orig: Entity_FireWeaponAndSendPacket - the authority copy
				//  @0x42BED5 into Server_ClientFiredRound @0x50C774 ->
				//  RoundData_AddRound ring+28 @0x4FDCF5; the client's
				//  RoundData_SpawnRound and pool[ret]+0x78 @0x42C030]
				round.shot_seq = RoundSim::kFreshRoundWord;
				round.subtype = round_event.subtype;
				round.charge = w.pending_throw_charge;
				// The firing slot's tracer cadence byte (MountSlot+0x80) — the
				// resolved ACTIVE slot, so a mounted redirect advances the
				// parent slot's phase exactly like retail's passed weaponSlot
				// [orig: RoundData_SpawnRound @ 0x4ec199 on the fire
				//  descriptor's slot].
				if (WeaponSlotState *fire_slot =
								active_local_weapon_slot(world, w)) {
					round.tracer_counter = &fire_slot->tracer_shot_counter;
					// The slot's carrier word is the round's fourth exclusion: a gun's
					// or vehicle's slot excludes that carrier, a personal slot none
					// [orig: WeaponAction_Fire @0x542B1F / @0x542C4F].
					round.excluded_carrier = EntityHandle{fire_slot->carrier};
				}
				// [orig: Server_ClientFiredRound's offsets @0x50BBAC..0x50BBF2]
				round.kill_zone_offset = round_kill_zone_offset(world, shooter);
				if (!io.is_authority && io.carrier_exclusion)
					// The joiner's OWN predicted round runs the wire-proxy walk with
					// the local mount exclusion dead, so resolve the carrier gate from
					// the self wire row like any decoded remote round — otherwise a
					// mounted joiner's fire stops on its own vehicle's proxy.
					round.shooter_carrier_handle = io.carrier_exclusion();
				world.round_sim.spawn(world, round,
						!io.is_authority ? RoundConsequenceMode::VisualOnly
						                 : RoundConsequenceMode::Authoritative);
				const uint16_t shot_seq = world.round_sim.spawned_round_word();
				round_event.shot_seq = shot_seq;
				if (io.is_authority) world.out.rounds.add(round_event);

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
                    // The C2S 0x06 target word pool-walks the +0xC pointer: a round
                    // lies in no pool, so it writes 0xFFFF, a round's entity word; a
                    // decoded remote person is the client's own pool-0 entity, its word
                    // the person's handle (D-NET-1236).
                    // [orig: NetPacket_WriteEntityPositionUpdate @0x42A610 (the walk
                    //  @0x42A713..0x42A745, its miss @0x42A745)]
                    io.fired.target_handle = fire_target.pool_word();
					io.fired.shooter_pose[0] = p->self->position.x.q16;
					io.fired.shooter_pose[1] = p->self->position.y.q16;
					io.fired.shooter_pose[2] = p->self->position.z.q16;
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
								mounted_def->category * weapon_combo::kRanksPerCategory +
								mounted_def->rank));
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
	// Whichever switch action completes on the EquippedSlot commits the one
	// g_PendingWeaponSlot word the UseGun mount stored, a personal SWITCHFROM the
	// mount found running included: its SWITCHRANK queue writes none over a
	// SWITCHFROM in its active phase, and over one in its done phase it does, and
	// that SWITCHRANK commits instead (@0x5433CE..0x5433F4); the completing action
	// decides the SWITCHTO. [orig: WeaponAction_SwitchFrom @0x5433B0
	//  (the store @0x543475, WeaponSlot_TryQueueSwitchTo @0x5434A3);
	//  WeaponAction_SwitchRank @0x543500 (the store @0x543539, no SWITCHTO);
	//  WeaponSlot_TryQueueSwitchRank @0x53F1C0]
	if (w.usegun_switch != LocalUseGunSwitch::kNone && ev.switch_completed) {
		w.usegun_switch_action = active_slot.current;
		const bool completed_from = active_slot.current == weapon_action::kSwitchFrom;
		const bool completed_rank = active_slot.current == weapon_action::kSwitchRank;
		commit_local_usegun_weapon_switch(world, w);
		// A SWITCHFROM's commit raises the weapon view on the stored slot's def; a SWITCHRANK's
		// drops the promoted scope byte for a def with no optic.
		if (completed_from && w.active) start_local_weapon_raise(world, w, view);
		if (completed_rank && w.active) clear_scope_at_local_switchrank(world, w, view);
		// The switch back hands the personal slot to its readers again, which read
		// a bucket weapon's rounds through the seat as it now stands: the mirror the
		// staging pump read through the gunner seat (the gun's clip) does not carry
		// across the seat. [orig: WeaponSlot_GetSharedLoadedRounds @0x540611..0x540657, read at each use]
		if (borrowed_usegun_slot && !w.usegun_slot_active && io.inventory != nullptr)
			read_personal_bucket(world, w.slot, *io.inventory, ammo_seat);
	}
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
				active_slot.clip = weapon_inventory_reload_slot(world.tables.weapons, inventory,
						inventory.equipped_combo, ammo_seat);
			} else if (eq_def->ammo_bucket == 0 || weapon_seat_writes_buckets(ammo_seat)) {
				weapon_inventory_set_loaded_rounds(world.tables.weapons, inventory,
                        inventory.equipped_combo, active_slot.clip);
			}
			active_slot.reserve = weapon_seat_pool(ammo_seat, eq_def->ammo_class_id,
					weapon_pool_get(inventory, eq_def->ammo_class_id));
			// The post-recoil auto-switch [orig: WeaponAction_Recoil tail @ 0x543062:
			// def+0x168 -> Player_SwitchToWeaponByHandle(def[+0x164]*65) — the
			// grenade/LAW switchback, unconditional per throw].
			if (ev.action_finished == weapon_action::kRecoil &&
					eq_def->has_switchcategory) {
				local_weapon_walk_clears_windup(world, w); // [orig: @0x54307C -> @0x4E019B]
				handle_weapon_switch_outcome(world, w, io.inventory,
						weapon_switch_to_handle(world.tables.weapons, inventory,
								eq_def->switchcategory *
										weapon_combo::kRanksPerCategory,
								local_weapon_switch_gates(world, w,
										io.inventory)), view);
			}
		}
	}
	// A queued manual switch commits at the outgoing SWITCHFROM/SWITCHRANK swap
	// seam, a borrowed slot held on foot's included: a weapon key or a seat-2/3
	// detach mounts the personal slot over it (D-NET-420); the ammo bridge above
	// stays off for the gun's slot [orig: Player_MountWeaponSlot @0x4DFA40 (the
	// g_PendingWeaponSlot store @0x4DFB16, the queue @0x4DFB9E);
	// WeaponAction_SwitchFrom @0x5433B0 (the local completion @0x543464..0x543475);
	// WeaponAction_SwitchRank @0x543500 (@0x543528..0x543539)].
	if (io.inventory != nullptr && (!borrowed_usegun_slot || w.usegun_on_foot) &&
			w.switch_in_flight && ev.switch_completed) {
		// The completing action, read before the commit replaces the slot: a
		// SWITCHFROM's commit draws the stored slot and raises the weapon view, a
		// SWITCHRANK's (the store of the slot already held included) drops the promoted
		// scope byte for a def with no optic.
		const bool completed_from = active_slot.current == weapon_action::kSwitchFrom;
		const bool completed_rank = active_slot.current == weapon_action::kSwitchRank;
		if (commit_pending_weapon_switch(world, w, io.inventory, PendingCommit::kSwitch)) {
			if (completed_from) start_local_weapon_raise(world, w, view);
			if (completed_rank) clear_scope_at_local_switchrank(world, w, view);
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
