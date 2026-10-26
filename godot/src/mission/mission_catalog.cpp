#include "mission/mission_catalog.h"

#include <runtime/mission/mission_catalog.h>

#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

namespace godot {

namespace {

opennova::mission_catalog::Row to_engine_row(const MissionCatalogRow &row) {
	opennova::mission_catalog::Row engine_row;
	engine_row.file = opennova::to_std(row.get_file());
	engine_row.title = opennova::to_std(row.get_title());
	engine_row.loose = row.is_loose();
	return engine_row;
}

} // namespace

void MissionCatalogRow::_bind_methods() {
	ClassDB::bind_static_method("MissionCatalogRow",
			D_METHOD("create", "file", "title", "briefing", "game_type", "loose"),
			&MissionCatalogRow::create);
	ClassDB::bind_method(D_METHOD("get_file"), &MissionCatalogRow::get_file);
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
	return opennova::to_gd(opennova::mission_catalog::display_text(to_engine_row(*this)));
}

void MissionCatalog::_bind_methods() {
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("rows", "root"),
			&MissionCatalog::rows);
	ClassDB::bind_static_method("MissionCatalog",
			D_METHOD("mission_names", "root"), &MissionCatalog::mission_names);
}

TypedArray<MissionCatalogRow> MissionCatalog::rows(const Ref<ResourceRoot> &p_root) {
	TypedArray<MissionCatalogRow> out;
	if (p_root.is_null()) {
		return out;
	}
	// The titles search the process's override table first (D-NET-675).
	const std::shared_ptr<const opennova::rtxt::File> text_override =
			RtxtStringFile::installed_text_override();
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index(), text_override.get())) {
		// The session code-word stamp the retail table carries per row.
		out.push_back(MissionCatalogRow::create(opennova::to_gd(row.file),
				opennova::to_gd(row.title),
				opennova::to_gd(row.briefing),
				static_cast<int64_t>(opennova::mission_catalog::game_type_of(row)),
				row.loose));
	}
	return out;
}

PackedStringArray MissionCatalog::mission_names(const Ref<ResourceRoot> &p_root) {
	PackedStringArray names;
	if (p_root.is_null()) {
		return names;
	}
	const std::shared_ptr<const opennova::rtxt::File> text_override =
			RtxtStringFile::installed_text_override();
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index(), text_override.get())) {
		names.push_back(opennova::to_gd(row.file));
	}
	return names;
}

} // namespace godot
