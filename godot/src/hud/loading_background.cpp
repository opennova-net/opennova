#include "hud/loading_background.h"

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <runtime/hud/loading_background.h>

using namespace godot;

Ref<LoadingBackground> LoadingBackground::resolve(const Ref<ResourceRoot> &p_root,
		const String &p_mission_file) {
	const opennova::hud::LoadingBackground pick = opennova::hud::loading_background(
			opennova::to_std(p_mission_file), [&p_root](const std::string &p_name) {
				// UI image probes force loose-first for this lookup, independent of /d
				// (the UI image loader's order, engine menu/menu_assets.h).
				return p_root.is_valid() &&
						p_root->has_file(opennova::to_gd(p_name), ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST);
			});
	Ref<LoadingBackground> out;
	out.instantiate();
	out->name_ = opennova::to_gd(pick.image);
	out->custom_ = pick.custom;
	out->band_right_ = opennova::hud::loading_band_right(pick.custom);
	return out;
}

bool LoadingBackground::start_mission_splash_due(bool p_in_session, bool p_custom_background) {
	return opennova::hud::start_mission_splash_due(p_in_session, p_custom_background);
}

void LoadingBackground::_bind_methods() {
	ClassDB::bind_static_method("LoadingBackground", D_METHOD("resolve", "root", "mission_file"),
			&LoadingBackground::resolve);
	ClassDB::bind_static_method("LoadingBackground",
			D_METHOD("start_mission_splash_due", "in_session", "custom_background"),
			&LoadingBackground::start_mission_splash_due);
	ClassDB::bind_method(D_METHOD("get_name"), &LoadingBackground::get_name);
	ClassDB::bind_method(D_METHOD("get_custom"), &LoadingBackground::get_custom);
	ClassDB::bind_method(D_METHOD("get_band_right"), &LoadingBackground::get_band_right);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "", "get_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "custom"), "", "get_custom");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "band_right"), "", "get_band_right");
}
