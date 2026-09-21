// The loadout screens' shared text compositions — see loadout_labels.h for
// the witnesses. [orig: populate_weapon_slot_lists @0x560430;
// update_weapon_weight_display @0x565640]

#include <runtime/menu/loadout_labels.h>

#include <base/io/strutil.h>
#include <formats/def/def.h>

#include <algorithm>
#include <cstdio>
#include <numeric>

using namespace opennova::def;

namespace opennova::menu {

std::string weapon_label(const DefWeaponDef &w, const hud::GameTextLookup &gametext) {
	if (w.loadout_menu_textid[0] == '\0') return w.weapon_name;
	return hud::game_text(gametext, "WepDes", w.loadout_menu_textid, w.weapon_name);
}

std::string ammo_row_label(const DefWeaponDef *w, int clips, const hud::GameTextLookup &gametext) {
	char out[192];
	if (w == nullptr) {
		std::snprintf(out, sizeof(out), "%d - ", clips);
		return out;
	}
	const std::string round_label = w->round_type[0] != '\0'
			? hud::game_text(gametext, "WepDes", w->round_type, w->round_type)
			: std::string();
	std::snprintf(out, sizeof(out), "%d - %s", clips * w->clipsize, round_label.c_str());
	return out;
}

std::vector<int> armory_slot_order(const std::vector<std::string> &labels) {
	std::vector<int> order(labels.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&labels](int a, int b) {
		return strutil::iless(labels[static_cast<size_t>(a)], labels[static_cast<size_t>(b)]);
	});
	return order;
}

std::string loadout_weight_line(double total, const hud::GameTextLookup &menutxt,
		const hud::GameTextLookup &gameui) {
	// The menu-token fold: menutxt's Menu section, then gameui's, else the
	// fallback [orig: TextResource_GetStringWithFallback(resource, "Menu", key)
	// @0x562ee0 against the menu resource].
	const hud::GameTextLookup menu_text =
			[&menutxt, &gameui](const char *section, const char *key, const char *fallback) {
				return hud::game_text(menutxt, section, key,
						hud::game_text(gameui, section, key, fallback).c_str());
			};
	std::string encumbrance;
	switch (def_encumbrance_class(total)) {
		case DEF_ENCUMBRANCE_HEAVY:
			encumbrance = menu_text("Menu", "HEAVY_ENCUMBRANCE", "Heavy");
			break;
		case DEF_ENCUMBRANCE_NORMAL:
			encumbrance = menu_text("Menu", "NORMAL_ENCUMBRANCE", "Normal");
			break;
		default:
			encumbrance = menu_text("Menu", "LIGHT_ENCUMBRANCE", "Light");
			break;
	}
	const std::string total_weight = menu_text("Menu", "TOTAL_WEIGHT", "Total Weight");
	const std::string lbs = menu_text("Menu", "LBS", "lbs");
	char out[256];
	std::snprintf(out, sizeof(out), "%s %.1f %s (%s)", total_weight.c_str(), total, lbs.c_str(),
			encumbrance.c_str());
	return out;
}

} // namespace opennova::menu
