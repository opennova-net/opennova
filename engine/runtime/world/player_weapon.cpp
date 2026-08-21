// The local player's equipped-weapon cluster — moved verbatim from the shell
// adapter (S7a, ADR 0028). The pump order, the UseGun borrow, PowerThrow, the
// install bake, and the presentation-event assembly are unchanged; the two
// wire legs (the joiner's fired descriptor, the reload relay) became output
// records the embedder's net layer routes.
#include "world/player_weapon.h"

#include "world/vehicle_mount.h"

#include "world/ai.h"
#include "world/infantry.h"
#include "world/round_sim.h"
#include "world/throwables.h"
#include "world/world.h"

#include <io/bam.h>
#include <io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace opennova::world {

namespace {

constexpr double kFixed16 = 65536.0;

Vec3 local_player_mission_position(const World &world) {
	const Entity *e = world.registry.get(world.cached.local_player);
	return e != nullptr ? e->position : Vec3{};
}

} // namespace

WeaponSlotState *active_local_weapon_slot(World &world, LocalPlayerWeapon &w) {
	if (w.usegun_slot_active && w.usegun_mount.valid()) {
		Entity *mount = world.registry.get(w.usegun_mount);
		if (mount != nullptr) {
			if (WeaponSlotState *slot = resolve_mounted_ammo_slot(world, *mount))
				return slot;
		}
	}
	return &w.slot;
}

const WeaponSlotState *active_local_weapon_slot(const World &world,
		const LocalPlayerWeapon &w) {
	if (w.usegun_slot_active && w.usegun_mount.valid()) {
		const Entity *mount = world.registry.get(w.usegun_mount);
		if (mount != nullptr) {
			if (const WeaponSlotState *slot =
					resolve_mounted_ammo_slot(world, *mount))
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
	const WeaponTableEntry *from = world.weapons.by_index(from_adm);
	const WeaponTableEntry *to = world.weapons.by_index(to_adm);
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
				? resolve_mounted_ammo_slot(world, *mount)
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
	const WeaponTableEntry *next_def = world.weapons.by_index(next_adm);
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

void sync_local_usegun_weapon_transition(World &world, LocalPlayerWeapon &w) {
	if (!world.cached.local_player.valid()) return;
	Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr) return;
	Entity *mounted_parent =
			player->mounted && player->mount_type == SeatType::Gunner
			? world.registry.get(player->mount_target)
			: nullptr;
	WeaponSlotState *mounted_slot = mounted_parent != nullptr
			? resolve_mounted_ammo_slot(world, *mounted_parent)
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
		const WeaponTableEntry *from = world.weapons.by_index(p_from);
		const WeaponTableEntry *to = world.weapons.by_index(p_to);
		return from != nullptr && to != nullptr &&
				from->category == to->category;
	};
	const auto stage_parent = [&](Entity &p_mount, uint8_t target_adm) {
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
			// A later attach overwrites g_pendingWeaponSlot without changing the
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

void commit_pending_weapon_switch(World &world, LocalPlayerWeapon &w,
		WeaponInventory *inventory) {
	// The pending -> equipped commit [orig: the switchfrom/switchrank completion
	// consumes g_pendingWeaponSlot; EquippedSlot swap + the equippedAdmIndex stamp
	// @ 0x4dd727; the FP model re-resolve runs shell-side off the event].
	w.switch_in_flight = false;
	w.switch_deferred_action = -1;
	w.nvg_scope_restore = false;
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
			world.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	w.start_in_switchto = true;
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
		WeaponInventory *inventory, const WeaponSwitchOutcome &out) {
	switch (out.kind) {
		case WeaponSwitchOutcome::kDeny: {
			// [orig: PlaySoundOnDedicatedServer(dword_24E08C4) @ 0x4e0354]
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

WeaponClipRing *weapon_ring_for(LocalPlayerWeapon &w,
		const std::string &key_lower) {
	for (std::pair<std::string, WeaponClipRing> &kv : w.clip_rings) {
		if (kv.first == key_lower) return &kv.second;
	}
	return nullptr;
}

float weapon_ring_take_length(LocalPlayerWeapon &w, const char *key) {
	// Serve the ring head's duration, then advance the head — the consuming read
	// [orig: Anim_GetDurationTicks @ 0x53ee10: currentEntry = *slot;
	//  *slot = *(currentEntry + 36); duration from currentEntry's data].
	WeaponClipRing *ring = weapon_ring_for(w, strutil::to_lower(key != nullptr ? key : ""));
	if (ring == nullptr || ring->lengths.empty()) return -1.0f;
	const float served = ring->lengths[static_cast<size_t>(ring->head)];
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

int weapon_ring_take_variant(LocalPlayerWeapon &w, const std::string &key) {
	// Serve the head as the PLAYED variant and advance — the play latch: playback
	// follows the served entry while the ring moves on [orig: AnimMap_PlayAnimBySlot
	// @ 0x40bda0: animEntry = slot[i]; slot[i] = next; animState+68 = animEntry].
	WeaponClipRing *ring = weapon_ring_for(w, strutil::to_lower(key));
	if (ring == nullptr || ring->lengths.empty()) return 0;
	const int served = ring->head;
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
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
	// A real mount invalidates the one-shot scope restore latch. The late
	// first-person-model rebake is render-side only and must not mutate view state.
	if (!same_weapon_rebake) w.nvg_scope_restore = false;
	// A mount is a new presentation epoch: no payload from the previous weapon may
	// cross this seam, even though its strings were copied into the pending records.
	// The same-weapon rebake is NOT an epoch — undrained records (including a
	// racing switch commit or deny) must survive it.
	if (!same_weapon_rebake) w.events.clear();
	// Clip lengths come from the loaded viewmodel's .adm (seconds) as per-key VARIANT
	// arrays; they seed the slot rings the bake and the play events consume
	// serve-then-advance [orig: the animState slot heads (+72) built by
	// AnimMap_RegisterBoneNode @ 0x40c2d0; Anim_GetDurationTicks @ 0x53ee10].
	w.clip_rings.clear();
	w.anim_variant = 0;
	for (const std::pair<std::string, std::vector<float>> &kv : data.clip_rings) {
		if (kv.second.empty()) continue;
		WeaponClipRing ring;
		ring.lengths = kv.second;
		w.clip_rings.emplace_back(strutil::to_lower(kv.first), std::move(ring));
	}
	// The bake probes existence as a pure lookup and reads durations ring-wise —
	// one consuming read per 'auto' field [orig: Anim_InitActions @ 0x541fa0;
	// the lookup @ 0x5421ae, the reads @ 0x5421c5 / @ 0x5421d8].
	const auto resolve_fn = [](void *p_ctx, const char *key) -> int {
		LocalPlayerWeapon *self = static_cast<LocalPlayerWeapon *>(p_ctx);
		return weapon_ring_for(*self,
				strutil::to_lower(key != nullptr ? key : "")) != nullptr ? 1 : 0;
	};
	const auto clip_fn = [](void *p_ctx, const char *key) -> float {
		return weapon_ring_take_length(
				*static_cast<LocalPlayerWeapon *>(p_ctx), key);
	};
	w.def = WeaponFsmDef{};
	weapon_fsm_bake(data.rows.data(), data.rows.size(), resolve_fn, clip_fn,
			&w, w.def);
	const int32_t flags = data.flags;
	w.def.auto_fire = (flags & weapon_flag::kAuto) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	w.def.burst3 = (flags & weapon_flag::kBurst) != 0;   // [orig: WeaponAction_Fire @ 0x542c8a]
	w.def.flags = flags;              // raw mask: the scope gate + fov policy read it
	w.def.flags2 = data.flags2;       // Inset (0x200) picks the 7-step ease
	// The heat model [orig: WeaponDef +0x36C/+0x370/+0x374]. Absent keys leave 0,
	// which disables the model exactly as the original's zero-init does.
	w.def.heat_per_shot = data.heat_per_shot;
	w.def.heat_decay_per_tick = data.heat_decay_per_tick;
	w.def.heat_glow_threshold = data.heat_glow_threshold;
	w.scope_max_mag = data.scope_max_mag;
	// The 3P fire attack-stamp kind [orig: weapon.def attack_anim -> the AdmDefs record
	// +0xA8; world-wac-ai-re.md §14.8.4]. The sibling special_hold (+0xA4) is NOT cached
	// here: the body updater re-reads it from the ADM table by the posed entity's own
	// equipped index every selection pass [orig: @ 0x4b5dba], which is the single source
	// both the local player and every remote player resolve through.
	w.attack_kind = data.attack_anim;
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	w.run_anim = data.run_anim;
	w.force_crouch = (flags & weapon_flag::kForceCrouch) != 0;
	// A held-AnimMap CHANGE advances a binding serial; the local InfantryState observes
	// that edge pre-tick and stamps its own 20-tick arms-dip window. Compare the
	// resolved map identity, not the weapon name: two weapon records sharing one
	// AnimMap do NOT dip. A fresh mount advances even when the map key is empty.
	// [orig: previous/current AdmDefs record +0 comparison @0x4b46d0..0x4b4701].
	if (!w.active || !strutil::iequals(data.animadm, w.anim_map)) {
		w.anim_map = data.animadm;
		++w.anim_map_serial;
		if (w.anim_map_serial == 0) ++w.anim_map_serial; // reserve 0 = none
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
		// Ammo comes from the slot pool when the installed def IS the equipped
		// inventory slot: clip = the slot's loaded rounds, reserve = the def's
		// ammo-class pool [orig: MountSlot+0x10 +
		// Entity_GetScoreValueBySlotType @0x5406e0].
		bool ammo_from_inventory = false;
		if (inventory != nullptr) {
			const WeaponInventorySlot *eq = inventory->slot(inventory->equipped_combo);
			const WeaponTableEntry *def = (eq != nullptr && eq->adm_index >= 0)
					? world.weapons.by_index(static_cast<uint8_t>(eq->adm_index))
					: nullptr;
			if (def != nullptr && strutil::iequals(data.name, def->name)) {
				w.slot.clip = eq->clip;
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
	// g_fireChargeStartTick on the walk, before the mount].
	w.power_throw_start_tick = 0;
	w.pending_throw_charge = 0;
	w.play_serial = 0;
	w.anim_key.clear();
	w.anim_variant = 0;
	w.anim_tick = 0;
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
	// A fresh mount starts at the hip with the interp cleared and the hipfire
	// latch reset [orig: Player_MountWeaponSlot zeroes the view biases @ 0x4dfbcf].
	view.scope_engaged = false;
	view.scope_step = 0;
	view.ease_steps = kScopeEaseSteps;
	view.scope_hipfire = true;
	w.def_name = data.name;
	w.active = true;
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
	w.clip_rings.clear();
	w.anim_variant = 0;
	w.anim_tick = 0;
	view.scope_engaged = false;
	view.scope_step = 0;
	view.ease_steps = kScopeEaseSteps;
	view.scope_hipfire = true;
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
			world.weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	if (def == nullptr) return false;             // no Def [orig: @0x4dcbda]
	// The ammo leg, for defs that carry the flag: an empty pool hides the
	// weapon. [orig: @0x4dcbea -> Entity_GetScoreValueBySlotType @0x5406E0,
	// ported as weapon_pool_get]
	if ((def->flags & weapon_flag::kNoClipsNoDraw) != 0 &&
			weapon_pool_get(inventory, def->ammo_class_id) == 0 &&
			slot->clip <= 0)
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
void local_weapon_pump_tick(World &world, LocalPlayerWeapon &w,
		LocalWeaponPumpIO &io) {
	io.fired = LocalWeaponFiredWire{};
	io.reload = LocalWeaponReloadWire{};
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
	WeaponFsmInputs in;
	// The pilot's trigger is dead: retail's fire gate rejects a Controller or
	// Driver seat before any slot work [orig: Player_CanFireWeapon @0x5cf780].
	// A gunner seat is deliberately NOT in this set.
	const bool accept_weapon_input = player_alive && !usegun_switch_pending &&
			!(player != nullptr && mount_blocks_firing(*player));
	in.fire_held = accept_weapon_input && w.fire_held;
	in.fire_pressed = accept_weapon_input && w.fire_pressed;
	// PowerThrow: the press never fires — it starts the windup; the release
	// converts the held time into the charge byte and fires. [orig: press gate
	// @ 0x4e08fd (def Flags sign bit 0x80000000, fireable + ammo ->
	// g_fireChargeStartTick = tick), release @ 0x4e07e9 -> WeaponSlot_RequestFire
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
	}
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = accept_weapon_input && w.reload_pressed &&
			weapon_fsm_reload_allowed(w.def, active_slot);
	in.is_local = true;
	in.is_authority = io.is_authority; // the joiner defers the refill to the §5.58 round-trip
	in.auto_reload = true;             // [orig: g_autoReloadEnabled @ 0x24D2118, default on]
	// The weapon FSM consumes the promoted/settled scope bit, not the raw
	// requested-engagement bit. Player_UpdatePerFrame runs before the weapon
	// pump in retail and only promotes g_weaponScopeActive after the ease has
	// completed [orig: promoter @ 0x4de4f7; weapon pump @ 0x526786].
	in.scope_active =
			view.scope_engaged && !player_view_scope_ease_active(view);
	in.instant_emplaced_switch = local_usegun_switch_is_instant(world, w);
	// The heat window is a deadline against the logic tick, not a stored level.
	// `submerged` stays false: the sim has no per-entity water test at the weapon
	// site yet, and above water is what the runtime actually plays (D-WPN-29).
	// [orig: current_tick @ 0x24C1968; the water gate @ 0x54101c]
	in.current_tick = static_cast<int32_t>(world.logic_tick);
	if (!accept_weapon_input) active_slot.refire_queued = false;
	WeaponFsmEvents ev;
	weapon_fsm_tick(w.def, active_slot, in, ev);
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
	if (ev.play_anim) {
		++w.play_serial;
		w.anim_key = ev.anim_key;
		w.anim_tick = world.logic_tick;
		// The play consumes the slot ring and latches the served variant — the shell
		// plays exactly this variant on every viewmodel part
		// [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 advances the head and latches
		//  the served entry at animState+68].
		w.anim_variant = weapon_ring_take_variant(w, w.anim_key);
		pending.anim_key = w.anim_key;
		pending.anim_variant = w.anim_variant;
		has_presentation_event = true;
	}
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
		pending.scope_settled =
				view.scope_engaged && !player_view_scope_ease_active(view);
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
		AiEntity *p = world.ai != nullptr
				? world.ai->for_handle(world.cached.local_player)
				: nullptr;
		if (p != nullptr && p->inf.active) {
			const int stamped = w.attack_kind == 1 ? anim_state::kKnifeAttack
					: w.attack_kind == 2 ? anim_state::kGrenadeAttack : -1;
			const int ring = (stamped >= 0 && world.ai->root_motion != nullptr)
					? world.ai->root_motion->variant_count(p->inf.adm_id, stamped) : 1;
			infantry_weapon_attack_stamp(p->inf, w.attack_kind, ring);
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
			const WeaponTableEntry *adm = world.weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
				Vec3 origin = shooter->position;
				if (w.eye_valid) {
					origin.x = w.eye_mission[0];
					origin.y = w.eye_mission[1];
					origin.z = w.eye_mission[2];
				} else {
					origin.z += 1.0f;
				}
				// The round bearing frame IS the engine heading frame: RoundSim's
				// (cos, sin) mission-axis mapping is wire-validated on the 0x06 yaw
				// BAM (round_sim.cpp spawn, D-NET-153), and the retail spawner runs
				// raw descriptor angles through sin/cos [orig: RoundData_SpawnRound
				// trig @ 0x4ec511..0x4ec5fb]. Applying the (90 - heading)
				// mission-yaw flip here mirrored every local shot across the NE
				// diagonal (impacts landed 90 deg off the aim ray - the
				// fp_impact_probe pin; the same mistake D-NET-153 records for the
				// wire leg).
				const int32_t dir_yaw = p->heading;
				// Fire position/direction sees the undoubled recoil accumulator;
				// the camera is the separate 2*R consumer. Spread below still
				// samples R>>8 before this shot adds its own impulse.
				// [orig: Entity_CalcWeaponFirePosition @0x4DC847]
				const int32_t dir_pitch =
						opennova::io::bam_add(p->pitch, p->inf.recoil_pitch);
				w.round_sequence = static_cast<uint16_t>(w.round_sequence + 1u);
				const uint16_t shot_seq = w.round_sequence;

				RoundEvent round_event;
				round_event.shooter_handle = !io.is_authority
						? io.self_wire_handle
						: world.cached.local_player.packed;
				round_event.origin_x = static_cast<int32_t>(
						std::lround(double(origin.x) * kFixed16));
				round_event.origin_y = static_cast<int32_t>(
						std::lround(double(origin.y) * kFixed16));
				round_event.origin_z = static_cast<int32_t>(
						std::lround(double(origin.z) * kFixed16));
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
				round_event.shot_seq = shot_seq;
				const uint32_t clip_before_consume = static_cast<uint32_t>(
						std::max(0, ev.fired_clip_before_consume));
				round_event.mode_flags = static_cast<uint8_t>(
						((clip_before_consume & 0x3u) << 4u) | 0x02u);
				const bool vehicle_attack_context =
						mount_blocks_weapon_channel(*shooter);
				const bool scope_settled = view.scope_engaged &&
						!player_view_scope_ease_active(view);
				// The ordinary on-foot hip-fire leg is exact: retail passes
				// Weapon_GetScopeZoomLevel(false, 12), which returns 12, and the
				// server's bit-6-clearing composite preserves it. The predicate
				// reads the PROMOTED scope bit, so ADS raise and third-person use
				// the same 12. Settled-FP/mounted zoom levels remain D-WPN-8.
				if (view.third_person ||
						(!scope_settled && !vehicle_attack_context &&
								(w.def.flags & weapon_flag::kForceScoped) == 0)) {
					round_event.subtype = 12;
				}
				round_event.adm_index = adm_index;
				// The PowerThrow charge rides the ring/wire slot_byte (ring+32,
				// wire flags|0x80 leg) and scales the spawned round's launch
				// speed [orig: WeaponAction_Fire arg 6 <- MountSlot+0x5C ->
				// descriptor +20 @ 0x4ec5bb; deserializer restore @ 0x42f769].
				round_event.slot_byte = w.pending_throw_charge;
				if (io.is_authority) world.rounds.add(round_event);

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
					io.fired.shooter_pose[0] = p->pos[0];
					io.fired.shooter_pose[1] = p->pos[1];
					io.fired.shooter_pose[2] = p->pos[2];
					io.fired.shooter_pose[3] = p->heading;
					io.fired.shooter_pose[4] = p->pitch;
				}
				w.pending_throw_charge = 0;
			}
		}
	}
	if (ev.dry_fired) ++w.dry_serial;
	if (ev.reload_requested) {
		++w.reload_serial;
		if (!borrowed_usegun_slot && io.inventory != nullptr &&
				io.inventory->equipped_combo >= 0) {
			io.reload.valid = true;
			io.reload.entity_handle = !io.is_authority
					? io.self_wire_handle
					: world.cached.local_player.packed;
			io.reload.reload_param =
					static_cast<uint16_t>(io.inventory->equipped_combo);
		} else if (borrowed_usegun_slot && w.usegun_mount.valid()) {
			// parentSlot==3 addresses the ENTITY THAT OWNS the selected slot,
			// not the actor. Wire-header materialization preserves the host's
			// packed handles, so this path is identical for host and joiner.
			// [orig: WeaponAction_Reload @0x543108..0x543157]
			Entity *mount = world.registry.get(w.usegun_mount);
			WeaponSlotState *mounted_slot = mount != nullptr
					? resolve_mounted_ammo_slot(world, *mount)
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
					world.weapons.by_index(mounted_adm);
			if (mounted_slot != nullptr && slot_owner != nullptr &&
					mounted_def != nullptr) {
				io.reload.valid = true;
				io.reload.entity_handle = slot_owner->handle.packed;
				io.reload.reload_param = static_cast<uint16_t>(
						static_cast<uint16_t>(mounted_def->category) * 65u +
						static_cast<uint16_t>(mounted_def->rank));
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
		AiEntity *p = world.ai != nullptr
				? world.ai->for_handle(world.cached.local_player)
				: nullptr;
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
				? world.weapons.by_index(static_cast<uint8_t>(eq->adm_index))
				: nullptr;
		if (eq != nullptr && eq_def != nullptr) {
			if (ev.reload_applied) {
				// The witnessed refill: refund the remaining clip to the pool, draw a
				// full clip clamped by it [orig: WeaponSlot_ReloadAmmo @ 0x541720,
				// §5.58] — overriding the FSM's single-class transfer (D-WPN-2).
				// eq->clip still holds the pre-reload remaining rounds (mirrored on
				// the previous tick); the refund below consumes it.
				weapon_inventory_reload_slot(world.weapons, inventory,
						inventory.equipped_combo);
				active_slot.clip = eq->clip;
			} else {
				eq->clip = active_slot.clip; // fire consume mirrors down
			}
			active_slot.reserve =
					weapon_pool_get(inventory, eq_def->ammo_class_id);
			// The post-recoil auto-switch [orig: WeaponAction_Recoil tail @ 0x543062:
			// def+0x168 -> Player_SwitchToWeaponByHandle(def[+0x164]*65) — the
			// grenade/LAW switchback, unconditional per throw].
			if (ev.action_finished == weapon_action::kRecoil &&
					eq_def->has_switchcategory) {
				handle_weapon_switch_outcome(world, w, io.inventory,
						weapon_switch_to_handle(world.weapons, inventory,
								eq_def->switchcategory *
										weapon_combo::kRanksPerCategory,
								local_weapon_switch_gates(world, w,
										io.inventory)));
			}
		}
		// A queued manual switch commits at the outgoing SWITCHFROM/SWITCHRANK
		// swap seam [orig: the completion consumes g_pendingWeaponSlot].
		if (w.switch_in_flight && ev.switch_completed) {
			commit_pending_weapon_switch(world, w, io.inventory);
		}
	}
	// The FSM's scope side effects land on the sim-owned engaged bit: forced
	// unscope (one-shot / reload stash) and the pump's rescope-after-reload
	// [orig: g_weaponScopeActive writes; the rescope block @ 0x54139e].
	if (ev.unscope) {
		++w.unscope_serial;
		// The forced paths run the same refusing toggle — a mid-ease unscope keeps
		// the scope (rare: a reload requested inside the raise ease)
		// [orig: @ 0x543136 calls Player_ToggleWeaponScope, activeFlag-gated].
		player_view_set_engaged(view, false,
				(w.def.flags2 & weapon_flag2::kInset) != 0);
	}
	if (ev.rescope) {
		++w.rescope_serial;
		player_view_set_engaged(view, true,
				(w.def.flags2 & weapon_flag2::kInset) != 0);
	}
}

} // namespace opennova::world
