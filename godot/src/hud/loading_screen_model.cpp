#include "hud/loading_screen_model.h"

#include "util/string_convert.h"
#include "world/loading_screen_info.h"

namespace godot {

namespace {

opennova::hud::LoadingSessionInfo native_info(const Ref<LoadingScreenInfo> &p_info) {
	opennova::hud::LoadingSessionInfo info;
	if (p_info.is_null()) return info;
	info.mission_file = opennova::gd_to_retail_text(p_info->get_mission_file());
	info.in_session = p_info->get_in_session();
	info.server_name = opennova::gd_to_retail_text(p_info->get_server_name());
	info.mission_name = opennova::gd_to_retail_text(p_info->get_mission_name());
	info.game_type = p_info->get_game_type();
	info.custom_text = opennova::gd_to_retail_text(p_info->get_custom_text());
	return info;
}

} // namespace

void LoadingScreenModel::setup(const Ref<LoadingScreenInfo> &p_info, bool p_custom_background,
		bool p_background_loaded) {
	model_.setup(native_info(p_info), p_custom_background, p_background_loaded);
}

void LoadingScreenModel::adopt_background(bool p_custom_background) {
	model_.adopt_background(p_custom_background);
}

void LoadingScreenModel::update_session_info(const Ref<LoadingScreenInfo> &p_info) {
	if (p_info.is_null()) return;
	model_.update_session_info(native_info(p_info));
}

PackedStringArray LoadingScreenModel::session_overlay_lines() const {
	PackedStringArray out;
	out.push_back(opennova::cp1252_to_gd(model_.title()));
	out.push_back(opennova::cp1252_to_gd(model_.mission_name()));
	out.push_back(opennova::cp1252_to_gd(model_.game_type_text()));
	out.push_back(opennova::cp1252_to_gd(model_.custom_text()));
	return out;
}

String LoadingScreenModel::server_message_label() {
	return opennova::cp1252_to_gd(opennova::hud::loading_server_message_label());
}

String LoadingScreenModel::splash_continue_text() {
	return opennova::cp1252_to_gd(opennova::hud::loading_splash_continue_text());
}

void LoadingScreenModel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "info", "custom_background", "background_loaded"),
			&LoadingScreenModel::setup);
	ClassDB::bind_method(D_METHOD("adopt_background", "custom_background"),
			&LoadingScreenModel::adopt_background);
	ClassDB::bind_method(D_METHOD("update_session_info", "info"),
			&LoadingScreenModel::update_session_info);
	ClassDB::bind_method(D_METHOD("set_progress", "percent"), &LoadingScreenModel::set_progress);
	ClassDB::bind_method(D_METHOD("get_progress"), &LoadingScreenModel::get_progress);
	ClassDB::bind_method(D_METHOD("present_due", "now_ms", "force"),
			&LoadingScreenModel::present_due);
	ClassDB::bind_method(D_METHOD("is_in_session"), &LoadingScreenModel::is_in_session);
	ClassDB::bind_method(D_METHOD("has_custom_background"),
			&LoadingScreenModel::has_custom_background);
	ClassDB::bind_method(D_METHOD("get_band_right"), &LoadingScreenModel::get_band_right);
	ClassDB::bind_method(D_METHOD("session_overlay_lines"),
			&LoadingScreenModel::session_overlay_lines);
	ClassDB::bind_static_method("LoadingScreenModel", D_METHOD("server_message_label"),
			&LoadingScreenModel::server_message_label);
	ClassDB::bind_static_method("LoadingScreenModel", D_METHOD("splash_continue_text"),
			&LoadingScreenModel::splash_continue_text);
}

} // namespace godot
