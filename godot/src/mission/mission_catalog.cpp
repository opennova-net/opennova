#include "mission/mission_catalog.h"

#include <base/io/file_io.h>
#include <runtime/mission/mission_catalog.h>

#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/utility_functions.hpp>

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
			D_METHOD("create", "file", "title", "briefing", "game_type", "loose", "mission_type"),
			&MissionCatalogRow::create);
	ClassDB::bind_method(D_METHOD("get_file"), &MissionCatalogRow::get_file);
}

Ref<MissionCatalogRow> MissionCatalogRow::create(const String &p_file,
		const String &p_title, const String &p_briefing, int64_t p_game_type,
		bool p_loose, int64_t p_mission_type) {
	Ref<MissionCatalogRow> row;
	row.instantiate();
	row->file_ = p_file;
	row->title_ = p_title;
	row->briefing_ = p_briefing;
	row->game_type_ = p_game_type;
	row->loose_ = p_loose;
	row->mission_type_ = p_mission_type;
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
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("load_high_scores", "root", "dir"),
			&MissionCatalog::load_high_scores);
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("save_high_scores", "dir"),
			&MissionCatalog::save_high_scores);
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("high_score", "file"),
			&MissionCatalog::high_score);
	ClassDB::bind_static_method("MissionCatalog", D_METHOD("set_high_score", "file", "score"),
			&MissionCatalog::set_high_score);
}

TypedArray<MissionCatalogRow> MissionCatalog::rows(const Ref<ResourceRoot> &p_root) {
	TypedArray<MissionCatalogRow> out;
	if (p_root.is_null()) {
		return out;
	}
	// The titles search the process's override table first (D-NET-675).
	const std::shared_ptr<const opennova::rtxt::File> text_override =
			RtxtStringFile::installed_text_override();
	// In the mounted title's order (engine Title::mission_list_order carries the witness).
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index(), text_override.get(),
					ResourceRoot::session_title(p_root).mission_list_order)) {
		// The session code-word stamp the retail table carries per row.
		out.push_back(MissionCatalogRow::create(opennova::to_gd(row.file),
				opennova::to_gd(row.title),
				opennova::to_gd(row.briefing),
				static_cast<int64_t>(opennova::mission_catalog::game_type_of(row)),
				row.loose, static_cast<int64_t>(row.mission_type)));
	}
	return out;
}

namespace {

opennova::mission_catalog::HighScoreTable &high_score_table() {
	static opennova::mission_catalog::HighScoreTable table;
	return table;
}

} // namespace

void MissionCatalog::load_high_scores(const Ref<ResourceRoot> &p_root, const String &p_dir) {
	opennova::mission_catalog::HighScoreTable &table = high_score_table();
	std::vector<opennova::mission_catalog::Row> rows;
	if (p_root.is_valid()) {
		const std::shared_ptr<const opennova::rtxt::File> text_override =
				RtxtStringFile::installed_text_override();
		rows = opennova::mission_catalog::build(p_root->engine_index(), text_override.get(),
				ResourceRoot::session_title(p_root).mission_list_order);
	}
	table.reset(rows);
	if (p_dir.is_empty()) return;
	std::string text;
	std::string error;
	// No file reads as no lines (File_ParseASCIIFileWithCallback's failed fopen).
	if (opennova::io::read_file_text(
				opennova::to_std(p_dir.path_join(opennova::mission_catalog::kHighScoreFile)), text, error))
		table.apply_text(text);
}

Error MissionCatalog::save_high_scores(const String &p_dir) {
	if (p_dir.is_empty()) return ERR_FILE_BAD_PATH;
	const std::string text = high_score_table().text();
	std::string error;
	if (!opennova::io::write_file_atomic(
				opennova::to_std(p_dir.path_join(opennova::mission_catalog::kHighScoreFile)), text, error)) {
		UtilityFunctions::push_warning(String("MissionCatalog: ") + opennova::to_gd(error));
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

int64_t MissionCatalog::high_score(const String &p_file) {
	return high_score_table().score(opennova::to_std(p_file));
}

void MissionCatalog::set_high_score(const String &p_file, int64_t p_score) {
	high_score_table().set_score(opennova::to_std(p_file), static_cast<int32_t>(p_score));
}

PackedStringArray MissionCatalog::mission_names(const Ref<ResourceRoot> &p_root) {
	PackedStringArray names;
	if (p_root.is_null()) {
		return names;
	}
	const std::shared_ptr<const opennova::rtxt::File> text_override =
			RtxtStringFile::installed_text_override();
	for (const opennova::mission_catalog::Row &row :
			opennova::mission_catalog::build(p_root->engine_index(), text_override.get(),
					ResourceRoot::session_title(p_root).mission_list_order)) {
		names.push_back(opennova::to_gd(row.file));
	}
	return names;
}

} // namespace godot
