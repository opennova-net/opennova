#include "simulation/player_inventory.h"

#include "util/record_bind.h"

using namespace godot;

String PlayerInventorySlot::get_name() const {
	return String::utf8(value_.name.c_str());
}

void PlayerInventorySlot::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(PlayerInventorySlot, Variant::INT, combo)
	OPENNOVA_RECORD_READ_ONLY(PlayerInventorySlot, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(PlayerInventorySlot, Variant::INT, clip)
}

Ref<PlayerInventory> PlayerInventory::make(const String &p_equipped_name, bool p_valid,
		int p_equipped_combo, int p_carry_flags) {
	opennova::world::LocalInventoryView v;
	v.valid = p_valid;
	v.equipped_combo = p_equipped_combo;
	v.equipped_name = p_equipped_name.utf8().get_data();
	v.carry_flags = static_cast<uint32_t>(p_carry_flags);
	Ref<PlayerInventory> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String PlayerInventory::get_equipped_name() const {
	return String::utf8(value_.equipped_name.c_str());
}

TypedArray<PlayerInventorySlot> PlayerInventory::get_slots() const {
	TypedArray<PlayerInventorySlot> out;
	for (const opennova::world::LocalInventoryView::Slot &slot : value_.slots) {
		Ref<PlayerInventorySlot> row;
		row.instantiate();
		row->assign(slot);
		out.push_back(row);
	}
	return out;
}

void PlayerInventory::_bind_methods() {
	ClassDB::bind_static_method("PlayerInventory",
			D_METHOD("make", "equipped_name", "valid", "equipped_combo", "carry_flags"),
			&PlayerInventory::make, DEFVAL(true), DEFVAL(-1), DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(PlayerInventory, Variant::BOOL, valid)
	OPENNOVA_RECORD_READ_ONLY(PlayerInventory, Variant::INT, equipped_combo)
	OPENNOVA_RECORD_READ_ONLY(PlayerInventory, Variant::STRING, equipped_name)
	OPENNOVA_RECORD_READ_ONLY(PlayerInventory, Variant::INT, carry_flags)
	OPENNOVA_RECORD_READ_ONLY_ROWS(PlayerInventory, slots, PlayerInventorySlot)
}
