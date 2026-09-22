#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "mission/mission_object_placer.h"
#include "mission/player_visual_spec.h"
#include "object/object_model.h"
#include "object/weapon_def.h"
#include "player/first_person_arms_witness.h"
#include "player/player_spawn_loadout.h"
#include "player/player_viewmodel_def.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_event.h"
#include "simulation/player_weapon_view.h"

namespace godot {

class MissionData;
class MissionEnvironment;
class MissionRoot;
class ResourceRoot;
class Simulation;
class WeaponDatabase;

// The local-player visuals (the former world_player_visuals.gd, ADR 0043
// slice G8), owned by GameWorld: the third-person avatar/held-gun builders,
// the first-person viewmodel composition (gun + character arms), the armory
// weapon apply/clear + the spawn-loadout projection, and the typed
// local-player view/weapon decodes (ADR 0017 edges). It OWNS the local-weapon
// state that lived on GameWorld -- the resolved weapon.def row, the
// armory-equipped override and its NONE latch, the decoded viewmodel record
// with its name memo, the UseGun slot-preserve latch, the viewmodel parts,
// and the staged spawn loadout -- and reaches the world's mission state
// (runtime, placer, resource root, environment, weapon table) through the
// world's PUBLIC surface, re-resolved per call. Plain RefCounted: it owns no
// Nodes -- built models parent under the world. GameWorld keeps a same-name
// delegate for every public name here (presenters, probes, the debug-control
// table, and the tests all call the world).
class LocalPlayerVisuals : public RefCounted {
	GDCLASS(LocalPlayerVisuals, RefCounted)

public:
	// One-time wiring from the owning GameWorld (constructed in the world's
	// _init, before any load).
	void setup(Node *p_world);
	// Mission unload: forget the staged loadout and every equipped-weapon
	// memo. The root stays mounted across unload, so the next entity's rig
	// resolves against its weapon.def before the next mission loads; the
	// decoded view record is keyed on the resolved name, so the next mission
	// re-decodes from ITS weapon.def even when the name repeats. Armory
	// selections belong to the entity from the mission being torn down: a new
	// spawn resolves from its own equipped AdmDef instead of inheriting either
	// the previous mission's override or its authored NONE state.
	void reset();

	// The local player's staged PLAYER_INFO selection for the next mission
	// spawn, consumed once the runtime exists (or discarded by unload after a
	// failed/abandoned load). Null preserves the historical fallback; a record
	// carrying empty slot names explicitly requests an all-NONE kit.
	void set_spawn_loadout(const Ref<PlayerSpawnLoadout> &p_loadout) { spawn_loadout_ = p_loadout; }
	Ref<PlayerSpawnLoadout> spawn_loadout() const { return spawn_loadout_; }
	// The runtime-start projection of the staged loadout (the engine's
	// spawn_loadout_plan, world/player_present.h): the class commit, the
	// mission-kit precedence, the kit apply and the inventory sync.
	void apply_local_player_spawn_loadout();

	// The soldier's THIRD-PERSON gun. Built as a SIBLING of the avatar rather
	// than a child: ObjectModel.rebuild() frees all of its children, so a
	// weapon parented under the avatar would silently vanish whenever the
	// body model rebuilds. It carries no skeleton and no clip -- the original
	// stamps ONE matrix into every bone slot of this model, i.e. it is drawn
	// rigid, posed entirely by its attach basis.
	// [orig: BoneCallback_org0_World draw 5 @0x4e3c87..0x4e3d99; model =
	//  WeaponDef.tpModel (+0x170, weapon.def gfx3) @0x4e3cd3]
	ObjectModel *build_local_player_held_weapon(const String &p_graphic);
	// Build a GameWorld-managed avatar model for the local player (which has
	// no BMS placement of its own). The caller (LocalPlayerPresenter)
	// positions it and swaps its visual/shadow policy per first/third person.
	// In first person the body remains a live SHADOWS_ONLY source, but
	// neither the gameplay beauty camera nor the water mirror renders it:
	// retail's reflected entity collector has no player/person leg [orig:
	// Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0]. Null when the
	// resource root / item graphic is unavailable. The player runtime type id
	// is the bound MissionObjectPlacer.PLAYER_RUNTIME_TYPE_ID [net-re §5.2b].
	ObjectModel *build_local_player_avatar();
	// Resolve the .3DI definitions that LocalPlayerPresenter would otherwise
	// load only on its first visible frame. Retail's
	// Game_ReloadEntityModelsAndCallbacks and HUD model pass load the player +
	// current weapon overlay before CEffectWorld_RebuildAllModelBuffers freezes
	// the C2S 0x3D source; doing the lightweight data lookup here gives our
	// snapshot the same boundary without constructing hidden scene nodes.
	// Later builders hit the placer's cache, so they cannot introduce a
	// definition just after freeze. Returns the graphics warmed, in order.
	PackedStringArray prewarm_loaded_model_challenge_definitions();
	// Armory apply, presentation side: point the FP viewmodel + action FSM at
	// `weapon_name`. Validates against weapon.def; the caller (main_game)
	// drops the old viewmodel so the per-frame pass rebuilds gun/arms/FSM from
	// the new def [orig: the ACCEPT re-mount, WeaponLoadout_ApplyFromBuffer
	// @0x565cd0 -> Player_MountWeaponSlot @0x4dfa40].
	bool set_local_player_weapon_by_name(const String &p_weapon_name, bool p_preserve_slot_state = false);
	// Armory NONE: clear the equipped render/FSM state instead of falling
	// back to the pre-armory default model on the next frame.
	void clear_local_player_weapon();
	// Build a GameWorld-managed FIRST-PERSON weapon viewmodel for the local
	// player (shown in 1st person; the inverse of the 3rd-person avatar).
	// Faithful composition: the equipped weapon's FP gun model PLUS the local
	// player's CHARACTER arms, sharing one skeleton [orig:
	// Player_RenderFirstPersonViewModel @0x4ded60 draws the weapon FP model,
	// then the CharacterEntity's arms model (blip+8, the Avatars.def combo arms
	// graphic @0x4df05f/@0x4deff4) with the same bone matrices, after
	// Avatar_SetArmsCamoCtrl @0x4df008/@0x4df070]. The gun + animadm come from
	// the mounted root's weapon.def (gfx1 / animadm [orig:
	// WeaponDef_ParseProperty @0x54d730]; the file's gfx1a/gfx1b tokens are
	// parsed-and-discarded by retail and never name the arms), the arms from
	// the resolved character. Camera sway / fire-kick / ADS [orig:
	// Player_UpdateFirstPersonCamera @0x4dd380] are follow-ups. Null when the
	// placer or both models fail to resolve.
	Node3D *build_local_player_viewmodel();
	// The FP viewmodel's typed model parts (arms/gun), rebuilt with the
	// container -- the rig consumes this instead of scanning children (the
	// live ones; a freed part drops out).
	TypedArray<ObjectModel> local_player_viewmodel_parts() const;
	// The actual first-person arms submit bound to the authority-stamped
	// character identity. Capture probes consume this public semantic witness
	// rather than guessing from the profile request or scanning the scene tree.
	Ref<FirstPersonArmsWitness> local_player_first_person_arms_witness();
	// The installed FP weapon's name (empty when none) -- the switch-event
	// guard against redundant viewmodel reinstalls.
	String local_player_weapon_name() const;
	// The resolved weapon.def row the viewmodel/HUD slices decode (null until
	// a weapon resolves).
	Ref<WeaponDef> local_weapon() const { return local_weapon_; }
	// Feed only the first-person-visible NVG state into world lighting. The
	// raw active state deliberately survives third person in the simulation.
	void set_local_player_nvg_view(bool p_active, int p_gain);
	// The 62.5 Hz view state (ADS ease, fov policy, 3P anchor), decoded once
	// at this edge (ADR 0017); null without a sim. Observation only: the
	// camera is the last composed view and nothing advances.
	Ref<PlayerLocalView> local_player_view() const;
	// The rendered frame's view: composes this display frame's camera, which
	// advances the composition state. The presenter's per-frame leg only.
	Ref<PlayerLocalView> present_local_player_view() const;
	// The equipped-weapon FSM view; null when no weapon FSM is installed.
	Ref<PlayerWeaponView> local_player_weapon_view() const;
	// Destructively drain the equipped FSM's ordered presentation batch.
	TypedArray<PlayerWeaponEvent> drain_local_player_weapon_events();
	// The resolved weapon.def record driving the FP viewmodel: model/adm
	// names plus the witnessed view-bias fields (pos/tpos raw units + rot
	// degrees, renderfov horizontal degrees) LocalPlayerPresenter consumes --
	// decoded from the WeaponDatabase row at this edge (ADR 0017). Null when
	// the mounted root has no weapon.def or the weapon name is absent --
	// callers keep their witnessed JOX AK-47 defaults then. The weapon is the
	// bring-up fallback until equipped-weapon resolution lands; the debug
	// `set_viewmodel_weapon` control (set_local_player_weapon_by_name over
	// MCP/F3) rigs A/B against another SKU's def. The precedence + memo are
	// the engine's viewmodel_def_pick / viewmodel_def_memo_hit.
	Ref<PlayerViewmodelDef> local_player_viewmodel_def();
	// The player's resolved visual (the combo its packed character id
	// resolves to), null before a mission is placed.
	Ref<PlayerVisualSpec> local_player_visual_spec();
	// The packed character id the authority stamped on the local player (the
	// host's own spawn from its installed profile, a joiner's named 0x0C
	// record) -- the one word its third-person body/head and first-person arms
	// key on, read from the sim rather than re-derived from team + profile.
	int local_player_character_id() const;

protected:
	static void _bind_methods();

private:
	Node *world() const;
	Node3D *world_node3d() const;
	Ref<Simulation> sim() const;
	MissionRoot *runtime() const;
	Ref<MissionObjectPlacer> placer() const;
	Ref<ResourceRoot> resource_root() const;
	Ref<WeaponDatabase> weapon_database() const;
	MissionEnvironment *environment() const;
	Ref<MissionData> loaded_mission() const;
	void set_first_person_model_available(bool p_available);
	void sync_local_player_weapon_from_inventory(const Ref<Simulation> &p_sim);

	// The GameWorld whose local player these visuals present (public surface only).
	ObjectID world_id_;
	Ref<PlayerSpawnLoadout> spawn_loadout_;
	// The resolved weapon.def row (the viewmodel/HUD slices decode it).
	Ref<WeaponDef> local_weapon_;
	// The armory-equipped weapon name; overrides the bring-up fallback/env
	// once the player accepts a loadout (the engine's viewmodel_def_pick
	// carries the witness). NONE is distinct from the pre-armory empty
	// override, which falls back to the witnessed bring-up default until an
	// equipped weapon is resolved.
	String viewmodel_weapon_override_;
	bool viewmodel_weapon_cleared_ = false;
	// The decoded weapon.def view record and the resolved name it was built
	// from (the mounted slot's def pointer; re-decoded only when the name
	// changes).
	String viewmodel_def_name_;
	Ref<PlayerViewmodelDef> viewmodel_def_;
	// A UseGun presentation rebuild follows a slot-pointer commit that has
	// already selected a persistent parent/personal slot. Both the dict-only
	// install and the later ADM-duration rebake must preserve that slot's
	// action/ammo state.
	bool local_weapon_preserve_slot_state_ = false;
	// The FP viewmodel's typed model parts (arms/gun), rebuilt with the
	// container.
	Vector<ObjectID> local_viewmodel_parts_;
};

} // namespace godot
