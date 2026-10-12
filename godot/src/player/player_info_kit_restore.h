#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <runtime/menu/player_info_kit.h>

namespace godot {

// What PLAYER_INFO's populate took from the shown side's class page
// (PlayerProfiles.restore_player_info_kit): a value wrapper over the engine's
// menu::PlayerInfoKitRestore plus the class byte whose page it read, the page
// the dialog's next save writes back. Rows are catalog rows: 0 the seeded
// "None", weapon.def's row i at i + 1.
class PlayerInfoKitRestore : public RefCounted {
	GDCLASS(PlayerInfoKitRestore, RefCounted)

	opennova::menu::PlayerInfoKitRestore value_;
	int page_class_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::menu::PlayerInfoKitRestore &p_value, int p_page_class) {
		value_ = p_value;
		page_class_ = p_page_class;
	}

	int get_page_class() const { return page_class_; }
	// The interleaved count pairs by catalog row, -1 where the page set none.
	PackedInt32Array get_ammo_primary() const;
	PackedInt32Array get_ammo_secondary() const;
	// The three grenade slots' catalog rows (0 = "None").
	PackedInt32Array get_grenade_slots() const;
	// The catalog row a category list (0 PRIMARY, 1 SECONDARY, 2 ACCESSORY)
	// selects by value, -1 when the page named none for it.
	int get_selection(int p_category) const;
	// The ammo-type byte a PRIMARY (0) / SECONDARY (1) entry carried.
	bool has_ammo_type(int p_category) const;
	int get_ammo_type(int p_category) const;
};

} // namespace godot
