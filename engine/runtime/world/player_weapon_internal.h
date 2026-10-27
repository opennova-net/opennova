// Internal declarations shared between player_weapon.cpp and its size-gate split
// player_weapon_usegun.cpp (the UseGun transition leg). These are implementation helpers of
// the local player's weapon cluster, not a public surface: nothing outside
// engine/runtime/world/player_weapon*.cpp should include this.

#pragma once

#include <runtime/world/player_weapon.h>

#include <cstdint>
#include <string>

namespace opennova::world {

// The local player's mission-space position (a presentation record's world_position); the
// origin with no bound local player. Defined in player_weapon.cpp.
Vec3 local_player_mission_position(const World &world);

// The commit's own mount of the EquippedSlot's def (player_weapon.cpp carries the witness);
// false when nothing mounts. It writes none of the mount request's pitch and view words (a
// switch's commit, a select, a store no mount runs: the request writes them). Defined in
// player_weapon.cpp.
bool commit_mount(World &world, LocalPlayerWeapon &w, const std::string &name,
		bool preserve_slot_state);

// Player_MountWeaponSlot's request on the mounted slot's def `target`, every local mount's leg
// (player_weapon.cpp carries the witness); `personal_slot`: the mounted slot is one of the
// local player's own (its owner is L), not a gun's; `target_scope_zoom`: the mounted slot's
// zoom word (MountSlot+0xC), clamped to `target`'s span (null: no slot to clamp, or the
// caller clamped it by its own def). Defined in player_weapon.cpp.
void mount_queue_switch_out(World &world, LocalPlayerWeapon &w, PlayerViewState *view,
		bool same_category, const WeaponTableEntry *target, bool personal_slot,
		int32_t *target_scope_zoom);
// Player_MountWeaponSlot's clamp of a mounted slot's zoom word into its def's span (a
// null def leaves it). Defined in player_weapon.cpp.
void mount_clamp_slot_zoom(const World &world, const WeaponTableEntry *def,
		int32_t &scope_zoom);

// Whether the pending slot's def is Emplaced (the FSM's pending_emplaced). Defined in
// player_weapon_usegun.cpp.
bool local_pending_slot_is_emplaced(const World &world, const LocalPlayerWeapon &w,
		const WeaponInventory *inventory);

// The adm of L's EquippedSlot's def (a borrow's resolved slot, else the personal slot);
// `has_def` false for a slot with no def. Defined in player_weapon_usegun.cpp.
uint8_t local_equipped_slot_def_adm(const World &world, const LocalPlayerWeapon &w,
		bool &has_def);

} // namespace opennova::world
