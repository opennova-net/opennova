#include "simulation/player_inventory.h"

using namespace godot;

void PlayerInventorySlot::assign(int p_combo, const String &p_name, int p_clip) {
	combo_ = p_combo;
	name_ = p_name;
	clip_ = p_clip;
}

void PlayerInventorySlot::_bind_methods() {
#define PLAYER_INVENTORY_SLOT_PROPERTY(m_variant, m_name)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerInventorySlot::get_##m_name);           \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerInventorySlot::set_##m_name);  \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_INVENTORY_SLOT_PROPERTY(Variant::INT, combo)
	PLAYER_INVENTORY_SLOT_PROPERTY(Variant::STRING, name)
	PLAYER_INVENTORY_SLOT_PROPERTY(Variant::INT, clip)
#undef PLAYER_INVENTORY_SLOT_PROPERTY
}

void PlayerInventory::_bind_methods() {
#define PLAYER_INVENTORY_PROPERTY(m_variant, m_name)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerInventory::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerInventory::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_INVENTORY_PROPERTY(Variant::BOOL, valid)
	PLAYER_INVENTORY_PROPERTY(Variant::INT, equipped_combo)
	PLAYER_INVENTORY_PROPERTY(Variant::STRING, equipped_name)
	PLAYER_INVENTORY_PROPERTY(Variant::INT, carry_flags)
	PLAYER_INVENTORY_PROPERTY(Variant::DICTIONARY, pools)
#undef PLAYER_INVENTORY_PROPERTY
	ClassDB::bind_method(D_METHOD("get_slots"), &PlayerInventory::get_slots);
	ClassDB::bind_method(D_METHOD("set_slots", "value"), &PlayerInventory::set_slots);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "slots", PROPERTY_HINT_ARRAY_TYPE, "PlayerInventorySlot"),
			"set_slots", "get_slots");
}
