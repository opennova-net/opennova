#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <formats/avatars/avatars.h>
#include <runtime/inmatch/character_registry.h>

#include <cstdint>
#include <vector>

namespace godot {

class AvatarComboRow;
class AvatarDiagnosticRow;
class AvatarDivisionRow;
class AvatarNationalityRow;
class AvatarPartRow;
class CharacterJoinProfile;
class ObjectModel;
class PlayerSpawnLoadout;
class ResourceRoot;

// GDExtension wrapper over engine/formats/avatars (the Avatars.def parse): the
// player-character model -- head/body/arms parts composed into combos under a
// nationality -> division tree -- read by the runtime PLAYER_INFO menu, the
// portrait preview and the player visual. The binding retains the parsed
// engine file and hands out by-value row records. Witnessed format/behavior:
// docs/playerinfo/avatars-re.md. The combo -> 3D-model load is the open
// D-PLAYERINFO-1 seam; a combo row stops at the resolved part graphic names.
class AvatarDatabase : public RefCounted {
	GDCLASS(AvatarDatabase, RefCounted)

private:
	opennova::avatars::AvatarsFile file_ = {};
	bool loaded = false;
	// The engine's wire-identity registry over the parsed tree (rebuilt lazily
	// after a load): every packed-id decode, per-side default and sex row is
	// its witnessed walk.
	mutable opennova::inmatch::CharacterRegistry registry_;
	mutable bool registry_dirty_ = true;

public:
	// The registry the per-side default and every packed-id decode walk, for
	// native callers (the player profile's fresh-record seed).
	const opennova::inmatch::CharacterRegistry &character_registry() const;

private:
	String source_path;
	String last_error;

	void clear();
	Error adopt_bytes(const PackedByteArray &p_bytes, const String &p_label);
	const opennova::avatars::AvatarPart *find_part(int kind, const String &name) const; // case-insensitive
	const opennova::avatars::AvatarNationality *nationality_at(int nat_index) const;
	const opennova::avatars::AvatarDivision *division_at(int nat_index, int div_index) const;
	const opennova::avatars::AvatarCombo *combo_at(int nat_index, int div_index, int combo_index) const;
	Ref<AvatarComboRow> combo_row(int nat_index, int div_index, int combo_index) const;

protected:
	static void _bind_methods();

public:
	~AvatarDatabase();

	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	// AvatarPartKind (engine/formats/avatars/avatars.h), bound as an enum so
	// the part lookups take a typed kind.
	enum PartKind {
		PART_HEAD = opennova::avatars::AVATAR_PART_HEAD,
		PART_BODY = opennova::avatars::AVATAR_PART_BODY,
		PART_ARMS = opennova::avatars::AVATAR_PART_ARMS,
	};
	// AvatarSex.
	enum { SEX_MALE = opennova::avatars::AVATAR_SEX_MALE, SEX_FEMALE = opennova::avatars::AVATAR_SEX_FEMALE };
	// AvatarAlignment (the PLAYER_INFO team filter: good -> team 0, evil -> team 1).
	enum { ALIGN_GOOD = opennova::avatars::AVATAR_ALIGN_GOOD, ALIGN_EVIL = opennova::avatars::AVATAR_ALIGN_EVIL };
	// AvatarDiagnosticSeverity.
	enum { DIAG_WARNING = opennova::avatars::AVATAR_DIAG_WARNING, DIAG_ERROR = opennova::avatars::AVATAR_DIAG_ERROR };
	// The three CTRL registers a combo part's authored `camo` bytes drive, by
	// name (the engine's renderer::kAvatarPartCamoRegisters carries the witness).
	static PackedStringArray part_camo_registers();
	// Store one part's raw camo bytes (AvatarPartRow.camo) on `p_model` under
	// `p_owner`.
	static void apply_part_camo(ObjectModel *p_model, const Vector3i &p_camo,
			const String &p_owner);

	// Load Avatars.def from an absolute/res path (decrypts if needed) or via the
	// mounted resource root (VFS/PFF).
	Error load(const String &path);
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);

	const opennova::avatars::AvatarsFile &native_file() const { return file_; }
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	TypedArray<AvatarDiagnosticRow> get_diagnostics() const;

	int get_part_count() const;
	int get_nationality_count() const;

	// Parts of a kind (PART_HEAD/BODY/ARMS), sorted by name; get_part() looks one
	// up by (kind, name) — the last prior definition wins, as at parse time.
	// Null when the name is unknown.
	PackedStringArray get_part_names(PartKind kind) const;
	Ref<AvatarPartRow> get_part(PartKind kind, const String &name) const;

	// Tree navigation by index (the order they appear in the file); null out of
	// range. A combo row is fully resolved: its part snapshots, the owning
	// nationality's alignment, its tree indices and its packed character id.
	// The part resolve stops before any .3di load (D-PLAYERINFO-1).
	Ref<AvatarNationalityRow> get_nationality(int nat_index) const;
	int get_division_count(int nat_index) const;
	Ref<AvatarDivisionRow> get_division(int nat_index, int div_index) const;
	int get_combo_count(int nat_index, int div_index) const;
	Ref<AvatarComboRow> get_combo(int nat_index, int div_index, int combo_index) const;

	// Resolve the packed character identity (npwire/character_id.h: authored
	// nationality bits 0..4, division 5..8, combo 9..14, alignment 15) the
	// ClientAuth CI0/CI1, the entity+0x15C wire NetId, and the weapon.sav header
	// all carry. Returns the resolved combo row, or null when no combo matches
	// (or the alignment bit contradicts `expected_alignment` when that is
	// ALIGN_GOOD/ALIGN_EVIL)
	// (engine: runtime/inmatch/admin_console_player.cpp).
	Ref<AvatarComboRow> resolve_character_id(int character_id,
			int expected_alignment = -1) const;
	// The player visual the packed id resolves to (engine:
	// inmatch::resolve_player_visual_spec over this file and its registry).
	opennova::inmatch::PlayerVisualSpec player_visual_spec(int character_id) const;
	// Retail's per-side default character: the packed id of the first combo (file
	// order) whose nationality alignment matches; no match -> the first combo of
	// all; empty -> 0 (engine: runtime/inmatch/character_registry.h).
	int first_character_id(int alignment) const;
	// The joiner's profile-to-wire projection (runtime/inmatch/
	// join_character_profile.h): `selection` is the PLAYER_INFO profile shape
	// (side_profiles [blue, red] each carrying nationality/division/combo tree
	// indices and player_class); `in_session` whether the session start that
	// takes it runs in a session (its class clamp, the engine projection).
	Ref<CharacterJoinProfile> character_join_profile(const Dictionary &p_selection,
			bool p_in_session) const;
	// The same projection over the typed spawn record the world stages
	// (player/player_spawn_loadout.h): its two per-side selections feed the
	// engine walk; null = no side present (the per-side defaults).
	Ref<CharacterJoinProfile> character_join_profile_from_loadout(
			const Ref<PlayerSpawnLoadout> &p_loadout, bool p_in_session) const;
	// The same projection with no Avatars.def loaded (engine
	// inmatch::join_character_profile_without_table).
	static Ref<CharacterJoinProfile> character_join_profile_without_table(
			const Ref<PlayerSpawnLoadout> &p_loadout, bool p_in_session);
	// Native projection consumed by Simulation's portable character-traits
	// table. File order is retained and duplicate packed ids are first-wins,
	// matching resolve_character_id's registry walk.
	std::vector<CharacterSexRow> character_sex_rows() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::AvatarDatabase::PartKind);
