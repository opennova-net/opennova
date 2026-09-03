#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/world/weapon_inventory.h> // LocalInventoryView

namespace godot {

// One occupied weapon slot of the local player's inventory: the combo slot
// index, the weapon.def name the slot's ADM index resolves to, and the rounds
// in its magazine (world::LocalInventoryView::Slot).
class PlayerInventorySlot : public RefCounted {
	GDCLASS(PlayerInventorySlot, RefCounted)

	opennova::world::LocalInventoryView::Slot value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::LocalInventoryView::Slot &p_value) { value_ = p_value; }

	int get_combo() const { return value_.combo; }
	String get_name() const;
	int get_clip() const { return value_.clip; }
};

// The local player's inventory snapshot (Simulation.get_local_player_inventory;
// world::LocalInventoryView): whether a kit has been applied, the equipped
// combo slot and its weapon name, the carry flags and the occupied slots.
// The engine's world::WeaponInventory carries the witnesses.
class PlayerInventory : public RefCounted {
	GDCLASS(PlayerInventory, RefCounted)

	opennova::world::LocalInventoryView value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::LocalInventoryView &p_value) { value_ = p_value; }
	// The comparison-probe fixture's sim double authors one.
	static Ref<PlayerInventory> make(const String &p_equipped_name, bool p_valid = true,
			int p_equipped_combo = -1, int p_carry_flags = 0);

	bool get_valid() const { return value_.valid; }
	int get_equipped_combo() const { return value_.equipped_combo; }
	String get_equipped_name() const;
	int get_carry_flags() const { return static_cast<int>(value_.carry_flags); }
	TypedArray<PlayerInventorySlot> get_slots() const;
};

} // namespace godot
