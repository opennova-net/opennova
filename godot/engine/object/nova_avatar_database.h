#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace godot {

class NovaResourceRoot;

// GDExtension wrapper over libs/avatars (Avatars.def parse + write). Surfaces the
// player-character model -- head/body/arms parts composed into combos under a
// nationality -> division tree -- to the editor (authoring + save) and the
// runtime PLAYER_INFO menu (read + resolve). Witnessed format/behavior:
// docs/playerinfo/avatars-re.md. The combo -> 3D-model load is the open
// D-PLAYERINFO-1 seam; resolve_combo() stops at the resolved part graphic names.
class NovaAvatarDatabase : public RefCounted {
	GDCLASS(NovaAvatarDatabase, RefCounted)

private:
	struct Part {
		int kind = 0;
		String name;
		String display_name;
		String graphic;
		String graphic_j;
		String graphic_s;
		int camo[3] = { 0, 0, 0 };
		int voice = 0;
		int sex = 0;
	};
	struct Combo {
		String raw_id;
		int id = 0;
		String head_name;
		String body_name;
		String arms_name;
		Part head;
		Part body;
		Part arms;
		bool has_arms = false;
	};
	struct Division {
		String raw_id;
		int id = 0;
		String name_key;
		String flags;
		std::vector<Combo> combos;
	};
	struct Nationality {
		String raw_id;
		int id = 0;
		String name_key;
		String flags;
		int alignment = 0;
		bool has_alignment = false;
		std::vector<Division> divisions;
	};
	struct Diagnostic {
		int line = 0;
		int severity = 0;
		String code;
		String message;
	};

	std::vector<Part> parts;
	std::vector<Nationality> nationalities;
	std::vector<Diagnostic> diagnostics;
	String source_path;
	String last_error;
	bool loaded = false;

	void clear();
	void adopt_parsed(const void *avatars_file); // const AvatarsFile*
	const Part *find_part(int kind, const String &name) const; // case-insensitive
	Part part_from_snapshot(const void *avatar_part_snapshot) const; // const AvatarPartSnapshot*
	void resolve_combo_snapshots(Combo &combo);
	Dictionary part_dict(const Part &p) const;
	Dictionary diagnostic_dict(const Diagnostic &d) const;

protected:
	static void _bind_methods();

public:
	// AvatarPartKind (libs/avatars/include/avatars/avatars.h).
	enum { PART_HEAD = 0, PART_BODY = 1, PART_ARMS = 2 };
	// AvatarSex.
	enum { SEX_MALE = 0, SEX_FEMALE = 1 };
	// AvatarAlignment (the PLAYER_INFO team filter: good -> team 0, evil -> team 1).
	enum { ALIGN_GOOD = 0, ALIGN_EVIL = 1 };
	// AvatarDiagnosticSeverity.
	enum { DIAG_WARNING = 1, DIAG_ERROR = 2 };

	// Load Avatars.def from an absolute/res path (decrypts if needed) or via the
	// mounted resource root (VFS/PFF). Both emit "changed".
	Error load(const String &path);
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	// Serialize the current model from scratch (docs/adr/0021) to path. The editor
	// save path.
	Error save_to_path(const String &path);
	// Reset to an empty model (the "New" document case). Emits "changed".
	void create_empty();

	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	Array get_diagnostics() const;

	int get_part_count() const;
	int get_nationality_count() const;

	// Parts of a kind (PART_HEAD/BODY/ARMS), sorted by name; get_part() looks one up
	// by (kind, name). Dictionaries carry name/display_name/graphic/graphic_j/
	// graphic_s/camo (Vector3i-as-Array)/voice/sex/kind.
	PackedStringArray get_part_names(int kind) const;
	Array get_parts(int kind) const;
	Dictionary get_part(int kind, const String &name) const;

	// Tree navigation by index (the order they appear in the file).
	Dictionary get_nationality(int nat_index) const;
	int get_division_count(int nat_index) const;
	Dictionary get_division(int nat_index, int div_index) const;
	int get_combo_count(int nat_index, int div_index) const;
	Dictionary get_combo(int nat_index, int div_index, int combo_index) const;

	// Resolve a combo's referenced parts into their data (graphic basenames, camo,
	// voice, sex), the surface the menu/preview consume. Stops before any .3di load
	// (D-PLAYERINFO-1). Missing part references resolve to empty entries.
	Dictionary resolve_combo(int nat_index, int div_index, int combo_index) const;

	// Whole-model bridge for the editor: read the full nested model, edit it in
	// GDScript, set it back, then save_to_path(). set_model() emits "changed".
	Dictionary get_model() const;
	void set_model(const Dictionary &model);
};

} // namespace godot
