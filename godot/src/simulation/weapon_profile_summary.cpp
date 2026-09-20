#include "simulation/weapon_profile_summary.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

PackedStringArray WeaponProfileSide::get_kit() const {
	PackedStringArray names;
	if (const opennova::playersav::KitPage *page = value_.selected_page()) {
		for (const opennova::playersav::KitEntry &entry : page->entries)
			names.push_back(opennova::to_gd(entry.name));
	}
	return names;
}

void WeaponProfileSide::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSide, Variant::INT, player_class)
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSide, Variant::INT, avatar_a)
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSide, Variant::INT, avatar_b)
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSide, Variant::INT, avatar_packed)
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSide, Variant::PACKED_STRING_ARRAY, kit)
}

Ref<WeaponProfileSide> WeaponProfileSummary::get_blue() const {
	Ref<WeaponProfileSide> out;
	out.instantiate();
	out->assign(value_.blue);
	return out;
}

Ref<WeaponProfileSide> WeaponProfileSummary::get_red() const {
	Ref<WeaponProfileSide> out;
	out.instantiate();
	out->assign(value_.red);
	return out;
}

void WeaponProfileSummary::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(WeaponProfileSummary, Variant::INT, error)
	OPENNOVA_RECORD_READ_ONLY_IS(WeaponProfileSummary, loaded)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(WeaponProfileSummary, blue, WeaponProfileSide)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(WeaponProfileSummary, red, WeaponProfileSide)
}
