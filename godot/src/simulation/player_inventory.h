#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// One occupied weapon slot of the local player's inventory: the combo slot
// index, the weapon.def name the slot's ADM index resolves to, and the rounds
// in its magazine.
class PlayerInventorySlot : public RefCounted {
	GDCLASS(PlayerInventorySlot, RefCounted)

	int combo_ = 0;
	String name_;
	int clip_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(int p_combo, const String &p_name, int p_clip);

	int get_combo() const { return combo_; }
	void set_combo(int p_value) { combo_ = p_value; }
	String get_name() const { return name_; }
	void set_name(const String &p_value) { name_ = p_value; }
	int get_clip() const { return clip_; }
	void set_clip(int p_value) { clip_ = p_value; }
};

// The local player's inventory snapshot (Simulation.get_local_player_inventory):
// whether a kit has been applied, the equipped combo slot and its weapon name,
// the carry flags, the occupied slots, and the ammo pools by class name
// (a genuine map: `pools[class_name] = rounds`). Read-write so a stub sim
// authors one; the engine's world::WeaponInventory carries the witnesses.
class PlayerInventory : public RefCounted {
	GDCLASS(PlayerInventory, RefCounted)

	bool valid_ = false;
	int equipped_combo_ = -1;
	String equipped_name_;
	int carry_flags_ = 0;
	TypedArray<PlayerInventorySlot> slots_;
	Dictionary pools_;

protected:
	static void _bind_methods();

public:
	bool get_valid() const { return valid_; }
	void set_valid(bool p_value) { valid_ = p_value; }
	int get_equipped_combo() const { return equipped_combo_; }
	void set_equipped_combo(int p_value) { equipped_combo_ = p_value; }
	String get_equipped_name() const { return equipped_name_; }
	void set_equipped_name(const String &p_value) { equipped_name_ = p_value; }
	int get_carry_flags() const { return carry_flags_; }
	void set_carry_flags(int p_value) { carry_flags_ = p_value; }
	TypedArray<PlayerInventorySlot> get_slots() const { return slots_; }
	void set_slots(const TypedArray<PlayerInventorySlot> &p_value) { slots_ = p_value; }
	void add_slot(const Ref<PlayerInventorySlot> &p_slot) { slots_.push_back(p_slot); }
	Dictionary get_pools() const { return pools_; }
	void set_pools(const Dictionary &p_value) { pools_ = p_value; }
};

} // namespace godot
