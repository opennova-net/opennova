#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

namespace godot {

class ResourceRoot;

// One front-end mission-catalog row: the engine catalog entry plus the
// derived session game-type code word. The witnesses live engine-side
// (engine/runtime/mission/mission_catalog.h; engine/net/npwire/game_type.h).
class MissionCatalogRow : public RefCounted {
	GDCLASS(MissionCatalogRow, RefCounted)

	String file_;
	String title_;
	String briefing_;
	int64_t game_type_ = 0;
	bool loose_ = false;

protected:
	static void _bind_methods();

public:
	// Field-complete construction for tests and fakes; the catalog's own rows
	// come from MissionCatalog::rows.
	static Ref<MissionCatalogRow> create(const String &p_file,
			const String &p_title, const String &p_briefing,
			int64_t p_game_type, bool p_loose);

	String get_file() const { return file_; }
	String get_title() const { return title_; }
	String get_briefing() const { return briefing_; }
	int64_t get_game_type() const { return game_type_; }
	bool is_loose() const { return loose_; }
	// The list row text (engine mission_catalog::display_text).
	String display_text() const;
};

// The front-end mission catalog over a mounted ResourceRoot: the one home for
// "which missions exist" that every shell surface (menu seeding, host
// screens, default picks) reads.
class MissionCatalog : public RefCounted {
	GDCLASS(MissionCatalog, RefCounted)

protected:
	static void _bind_methods();

public:
	static TypedArray<MissionCatalogRow> rows(const Ref<ResourceRoot> &p_root);
	// The SP screen's row filter (game_type::is_waypoint_family).
	static bool sp_visible(int64_t p_game_type);
	// The flat filename views the session/browser surfaces consume.
	static PackedStringArray mission_names(const Ref<ResourceRoot> &p_root);
	static String first_mission_name(const Ref<ResourceRoot> &p_root);
};

} // namespace godot
