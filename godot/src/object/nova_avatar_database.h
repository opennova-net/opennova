#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <avatars/preview_animation.h>

#include <cstdint>
#include <vector>

namespace godot {

class ResourceRoot;

// GDExtension wrapper over engine/formats/avatars (Avatars.def parse + write). Surfaces the
// player-character model -- head/body/arms parts composed into combos under a
// nationality -> division tree -- to the editor (authoring + save) and the
// runtime PLAYER_INFO menu (read + resolve). Witnessed format/behavior:
// docs/playerinfo/avatars-re.md. The combo -> 3D-model load is the open
// D-PLAYERINFO-1 seam; resolve_combo() stops at the resolved part graphic names.
class ObjectModel;

class AvatarDatabase : public RefCounted {
	GDCLASS(AvatarDatabase, RefCounted)

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

	// The witnessed menu preview-animation constants, re-exported from engine
	// avatars/preview_animation.h (float/String values cannot be class
	// constants).
	static float preview_zoom_damp_per_tick();
	static float preview_zoom_in_scale();
	static float preview_idle_speed_deg_per_sec();
	static float preview_sway_freq_rad_per_sec();
	static float preview_sway_amp_deg();
	static String preview_skeleton_bad();
	static String preview_idle_bad();

protected:
	static void _bind_methods();

public:
	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	// AvatarPartKind (engine/formats/avatars/avatars.h).
	enum { PART_HEAD = 0, PART_BODY = 1, PART_ARMS = 2 };
	// AvatarSex.
	enum { SEX_MALE = 0, SEX_FEMALE = 1 };
	// AvatarAlignment (the PLAYER_INFO team filter: good -> team 0, evil -> team 1).
	enum { ALIGN_GOOD = 0, ALIGN_EVIL = 1 };
	// AvatarDiagnosticSeverity.
	enum { DIAG_WARNING = 1, DIAG_ERROR = 2 };
	// The preview initial-yaw span: a fresh preview model spawns at
	// rand() % PREVIEW_INITIAL_YAW_RANGE_DEG degrees (engine
	// avatars/preview_animation.h carries the witness).
	enum { PREVIEW_INITIAL_YAW_RANGE_DEG =
			opennova::avatars::kPreviewInitialYawRangeDeg };

	// The three CTRL registers a combo part's authored `camo` bytes drive
	// (TEX_CAMO1..3 = .3di control-catalog ordinals 93..95). Retail zero-extends
	// the bytes into the shared CTRL bus immediately before submitting THAT
	// part, so head, body and arms each carry their own triplet — no scaling, no
	// tint: the bytes are texture-variant selectors (retail: Avatar_SetHeadCamoCtrl
	// @0x57a370 / Avatar_SetBodyCamoCtrl @0x57a390 / Avatar_SetArmsCamoCtrl
	// @0x57a3b0 -> CTRL slots 0x83FFD0/D8/E0; world submits @0x5c7fec/@0x5c800f,
	// preview @0x56113c/@0x56110b, first-person arms @0x4df008/@0x4df070, see docs/playerinfo/avatars-re.md).
	static PackedStringArray part_camo_registers();
	// Store one part's raw camo bytes ([r, g, b] as parsed) on `p_model` under
	// `p_owner`. Fewer than three bytes stores nothing.
	static void apply_part_camo(ObjectModel *p_model, const Array &p_camo,
			const String &p_owner);

	// Load Avatars.def from an absolute/res path (decrypts if needed) or via the
	// mounted resource root (VFS/PFF). Both emit "changed".
	Error load(const String &path);
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
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
	// Resolve the packed character identity (npwire/character_id.h: authored
	// nationality bits 0..4, division 5..8, combo 9..14, alignment 15) the
	// ClientAuth CI0/CI1, the entity+0x15C wire NetId, and the weapon.sav header
	// all carry. Returns the resolved combo plus its tree indices, or an empty
	// Dictionary when no combo matches (or the alignment bit contradicts
	// `expected_alignment` when that is ALIGN_GOOD/ALIGN_EVIL)
	// (retail: MinimapSlot_FindByPackedId @0x57a270, see docs/playerinfo/avatars-re.md).
	Dictionary resolve_character_id(int character_id,
			int expected_alignment = -1) const;
	// Retail's per-side default character: the packed id of the first combo (file
	// order) whose nationality alignment matches; no match -> the first combo of
	// all; empty -> 0 (retail: lookup_entity_slot_and_pack_entry @0x57ad40, see docs/playerinfo/avatars-re.md).
	int first_character_id(int alignment) const;
	// Native projection consumed by Simulation's portable character-traits
	// table. File order is retained and duplicate packed ids are first-wins,
	// matching resolve_character_id's registry walk.
	std::vector<CharacterSexRow> character_sex_rows() const;

	// Whole-model bridge for the editor: read the full nested model, edit it in
	// GDScript, set it back, then save_to_path(). set_model() emits "changed".
	Dictionary get_model() const;
	void set_model(const Dictionary &model);
};

} // namespace godot
