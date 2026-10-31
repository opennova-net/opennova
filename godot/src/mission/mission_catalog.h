#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>
#include <string>

namespace opennova::mission_catalog {
struct Row; // runtime/mission/mission_catalog.h
} // namespace opennova::mission_catalog

namespace godot {

class ResourceRoot;

// One front-end mission-catalog row: the engine catalog entry plus the
// derived session game-type code word. The witnesses live engine-side
// (engine/runtime/mission/mission_catalog.h; engine/base/gameprofile/game_type.h).
class MissionCatalogRow : public RefCounted {
	GDCLASS(MissionCatalogRow, RefCounted)

	String file_;
	String title_;
	String briefing_;
	int64_t game_type_ = 0;
	bool loose_ = false;
	int64_t mission_type_ = 0;

protected:
	static void _bind_methods();

public:
	// Field-complete construction for tests and fakes; the catalog's own rows
	// come from MissionCatalog::rows.
	static Ref<MissionCatalogRow> create(const String &p_file,
			const String &p_title, const String &p_briefing,
			int64_t p_game_type, bool p_loose, int64_t p_mission_type);

	String get_file() const { return file_; }
	String get_title() const { return title_; }
	String get_briefing() const { return briefing_; }
	int64_t get_game_type() const { return game_type_; }
	bool is_loose() const { return loose_; }
	// The header's mission type (engine mission_catalog::Row::mission_type).
	int64_t get_mission_type() const { return mission_type_; }
	// The list row text (engine mission_catalog::display_text).
	String display_text() const;
};

// The front-end mission catalog over a mounted ResourceRoot: the one home for
// "which missions exist" that every shell surface (menu seeding, the session
// config screens) reads.
class MissionCatalog : public RefCounted {
	GDCLASS(MissionCatalog, RefCounted)

protected:
	static void _bind_methods();

public:
	static TypedArray<MissionCatalogRow> rows(const Ref<ResourceRoot> &p_root);
	// The flat filename views the session/browser surfaces consume.
	static PackedStringArray mission_names(const Ref<ResourceRoot> &p_root);
	// The catalog's high scores, the process's one table (engine mission_catalog::HighScoreTable
	// carries the witnesses): load_high_scores rebuilds it over the root's catalog, each score 0,
	// then reads `p_dir`'s hiscore.txt over it (none: all 0); save_high_scores writes it there.
	// high_score / set_high_score read and set a row's by its file.
	static void load_high_scores(const Ref<ResourceRoot> &p_root, const String &p_dir);
	static Error save_high_scores(const String &p_dir);
	static int64_t high_score(const String &p_file);
	static void set_high_score(const String &p_file, int64_t p_score);
	// The process's mission table as load_high_scores last built it (retail's g_MissionList, which
	// the current entry points into): the first row whose file is `p_file`, without case, or null;
	// and its high score (0 for none). C++ only (the end screens' entry, simulation.cpp).
	static const opennova::mission_catalog::Row *process_row(const std::string &p_file);
	static int32_t process_high_score(const std::string &p_file);
};

} // namespace godot
