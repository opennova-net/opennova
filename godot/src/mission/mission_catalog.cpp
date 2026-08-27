#include "mission/mission_catalog.h"

#include <runtime/mission/mission_catalog.h>
#include <net/npwire/game_type.h>

#include "resource_index/resource_root.h"

namespace godot {

namespace {

opennova::mission_catalog::Row to_engine_row(const MissionCatalogRow &row) {
	opennova::mission_catalog::Row engine_row;
	engine_row.file = row.get_file().utf8().get_data();
	engine_row.title = row.get_title().utf8().get_data();
	engine_row.loose = row.is_loose();
	return engine_row;
}

} // namespace

void MissionCatalogRow::_bind_methods() {
	ClassDB::bind_static_method("MissionCatalogRow",
			D_METHOD("create", "file", "title", "briefing", "game_type", "loose"),
			&MissionCatalogRow::create);
	ClassDB::bind_method(D_METHOD("get_file"), &MissionCatalogRow::get_file);
	ClassDB::bind_method(D_METHOD("get_briefing"), &MissionCatalogRow::get_briefing);
	ClassDB::bind_method(D_METHOD("get_game_type"), &MissionCatalogRow::get_game_type);
	ClassDB::bind_method(D_METHOD("display_text"), &MissionCatalogRow::display_text);
}

Ref<MissionCatalogRow> MissionCatalogRow::create(const String &p_file,
		const String &p_title, const String &p_briefing, int64_t p_game_type,
		bool p_loose) {
	Ref<MissionCatalogRow> row;
	row.instantiate();
	row->file_ = p_file;
	row->title_ = p_title;
	row->briefing_ = p_briefing;
	row->game_type_ = p_game_type;
	row->loose_ = p_loose;
	return row;
}

String MissionCatalogRow::display_text() const {
	return String::utf8(
			opennova::mission_catalog::display_text(to_engine_row(*this)).c_str());
}

void MissionCatalog::_bind_methods() {
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("rows", "root"),
			&MissionCatalog::rows);
	ClassDB::bind_static_method("MissionCatalog",
			D_METHOD("sp_visible", "game_type"), &MissionCatalog::sp_visible);
	ClassDB::bind_static_method("MissionCatalog",
			D_METHOD("mission_names", "root"), &MissionCatalog::mission_names);
	ClassDB::bind_static_method("MissionCatalog",
			D_METHOD("first_mission_name", "root"),
			&MissionCatalog::first_mission_name);
}

TypedArray<MissionCatalogRow> MissionCatalog::rows(const Ref<ResourceRoot> &p_root) {
	TypedArray<MissionCatalogRow> out;
	if (p_root.is_null()) {
		return out;
	}
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index())) {
		// The session code-word stamp the retail table carries per row is
		// game_type::for_mission_mode over the header's mode bit (the engine
		// catalog stays below the net layer, so the stamp happens here).
		out.push_back(MissionCatalogRow::create(String::utf8(row.file.c_str()),
				String::utf8(row.title.c_str()),
				String::utf8(row.briefing.c_str()),
				static_cast<int64_t>(
						opennova::game_type::for_mission_mode(row.game_mode)),
				row.loose));
	}
	return out;
}

bool MissionCatalog::sp_visible(int64_t p_game_type) {
	return opennova::game_type::is_waypoint_family(
			static_cast<uint32_t>(p_game_type));
}

PackedStringArray MissionCatalog::mission_names(const Ref<ResourceRoot> &p_root) {
	PackedStringArray names;
	if (p_root.is_null()) {
		return names;
	}
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index())) {
		names.push_back(String::utf8(row.file.c_str()));
	}
	return names;
}

String MissionCatalog::first_mission_name(const Ref<ResourceRoot> &p_root) {
	const PackedStringArray names = mission_names(p_root);
	return names.is_empty() ? String() : names[0];
}

} // namespace godot
