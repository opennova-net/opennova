#include "player/player_info_kit_restore.h"

#include "util/record_bind.h"

#include <vector>

using namespace godot;

namespace {

PackedInt32Array to_packed(const int32_t *p_values, size_t p_count) {
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(p_count));
	for (size_t i = 0; i < p_count; ++i) out.set(static_cast<int64_t>(i), p_values[i]);
	return out;
}

PackedInt32Array to_packed(const std::vector<int32_t> &p_values) {
	return to_packed(p_values.data(), p_values.size());
}

} // namespace

PackedInt32Array PlayerInfoKitRestore::get_ammo_primary() const {
	return to_packed(value_.ammo_primary);
}

PackedInt32Array PlayerInfoKitRestore::get_ammo_secondary() const {
	return to_packed(value_.ammo_secondary);
}

PackedInt32Array PlayerInfoKitRestore::get_grenade_slots() const {
	return to_packed(value_.grenade_slots, 3);
}

int PlayerInfoKitRestore::get_selection(int p_category) const {
	return p_category >= 0 && p_category < 3 ? value_.select[p_category] : -1;
}

bool PlayerInfoKitRestore::has_ammo_type(int p_category) const {
	return p_category >= 0 && p_category < 2 && value_.has_type[p_category];
}

int PlayerInfoKitRestore::get_ammo_type(int p_category) const {
	return has_ammo_type(p_category) ? value_.type[p_category] : 0;
}

void PlayerInfoKitRestore::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(PlayerInfoKitRestore, Variant::INT, page_class)
	OPENNOVA_RECORD_READ_ONLY(PlayerInfoKitRestore, Variant::PACKED_INT32_ARRAY, ammo_primary)
	OPENNOVA_RECORD_READ_ONLY(PlayerInfoKitRestore, Variant::PACKED_INT32_ARRAY, ammo_secondary)
	OPENNOVA_RECORD_READ_ONLY(PlayerInfoKitRestore, Variant::PACKED_INT32_ARRAY, grenade_slots)
	ClassDB::bind_method(D_METHOD("get_selection", "category"), &PlayerInfoKitRestore::get_selection);
	ClassDB::bind_method(D_METHOD("has_ammo_type", "category"), &PlayerInfoKitRestore::has_ammo_type);
	ClassDB::bind_method(D_METHOD("get_ammo_type", "category"), &PlayerInfoKitRestore::get_ammo_type);
}
