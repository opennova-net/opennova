#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/string.hpp>

#include "object/weapon_database.h"
#include "player/player_viewmodel_def.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"

namespace godot {

class GameWorld;

// The narrow, live view of the running mission the in-world screens (deploy,
// end of round) and the debug click picker read: the simulation and the
// mounted resource root, both re-resolved per call (each mission brings new
// ones). The base answers none (no world); GameWorld.world_view() serves the
// live one over itself; a test fakes it by overriding the two hooks (ADR 0043
// rule 11: a presenter depends on the narrowest surface it uses, never on a
// world double). The bound verbs dispatch to a script override of the hook
// first, else to the C++ implementation behind them.
class WorldView : public RefCounted {
	GDCLASS(WorldView, RefCounted)

protected:
	static void _bind_methods();

	GDVIRTUAL0R(Ref<Simulation>, _sim)
	GDVIRTUAL0R(Ref<ResourceRoot>, _resource_root)

	// The C++ implementations behind the bound verbs; the base answers none.
	virtual Ref<Simulation> sim_impl() const { return Ref<Simulation>(); }
	virtual Ref<ResourceRoot> resource_root_impl() const { return Ref<ResourceRoot>(); }

public:
	int64_t get_frame_clock_ms() const;
	Ref<Simulation> sim();
	Ref<ResourceRoot> resource_root();
};

// The armory screen's view of the world: the WorldView pair plus the weapon
// table and the local player's viewmodel verbs the ACCEPT leg drives. The
// base answers none; GameWorld.armory_view() serves the live one; a test
// fakes it by overriding the hooks (ADR 0043 rule 11).
class ArmoryWorldView : public WorldView {
	GDCLASS(ArmoryWorldView, WorldView)

protected:
	static void _bind_methods();

	GDVIRTUAL0R(Ref<WeaponDatabase>, _weapon_database)
	GDVIRTUAL0R(Ref<PlayerViewmodelDef>, _local_player_viewmodel_def)
	GDVIRTUAL2R(bool, _set_local_player_weapon_by_name, String, bool)
	GDVIRTUAL0(_clear_local_player_weapon)

	virtual Ref<WeaponDatabase> weapon_database_impl() const { return Ref<WeaponDatabase>(); }
	virtual Ref<PlayerViewmodelDef> local_player_viewmodel_def_impl() const {
		return Ref<PlayerViewmodelDef>();
	}
	virtual bool set_local_player_weapon_by_name_impl(const String &p_weapon_name,
			bool p_preserve_slot_state) {
		return false;
	}
	virtual void clear_local_player_weapon_impl() {}

public:
	Ref<WeaponDatabase> weapon_database();
	// The equipped weapon's viewmodel definition, null when none is mounted.
	Ref<PlayerViewmodelDef> local_player_viewmodel_def();
	// Point the FP viewmodel + action FSM at `weapon_name` (the armory apply).
	bool set_local_player_weapon_by_name(const String &p_weapon_name,
			bool p_preserve_slot_state = false);
	// The authored NONE row: no rendered/action weapon.
	void clear_local_player_weapon();
};

// GameWorld's own live views (world_view() / armory_view()): the world by
// identity, every read re-resolved per call and null once the world is gone.
class LiveWorldView : public WorldView {
	GDCLASS(LiveWorldView, WorldView)

public:
	void bind(GameWorld *p_world);

protected:
	static void _bind_methods() {}
	Ref<Simulation> sim_impl() const override;
	Ref<ResourceRoot> resource_root_impl() const override;
	GameWorld *world() const;

private:
	ObjectID world_id_;
};

class LiveArmoryWorldView : public ArmoryWorldView {
	GDCLASS(LiveArmoryWorldView, ArmoryWorldView)

public:
	void bind(GameWorld *p_world);

protected:
	static void _bind_methods() {}
	Ref<Simulation> sim_impl() const override;
	Ref<ResourceRoot> resource_root_impl() const override;
	Ref<WeaponDatabase> weapon_database_impl() const override;
	Ref<PlayerViewmodelDef> local_player_viewmodel_def_impl() const override;
	bool set_local_player_weapon_by_name_impl(const String &p_weapon_name,
			bool p_preserve_slot_state) override;
	void clear_local_player_weapon_impl() override;
	GameWorld *world() const;

private:
	ObjectID world_id_;
};

} // namespace godot
