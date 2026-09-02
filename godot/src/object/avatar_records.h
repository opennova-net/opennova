#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <formats/avatars/avatars.h>

namespace godot {

// One Avatars.def part (`define head|body|arms <name> { ... }`) as the menu,
// the preview and the player visual read it: a by-value copy of the parse's
// snapshot (engine/formats/avatars/avatars.h carries the field witnesses).
// A combo's head/body/arms rows are the parse-time snapshots (last prior
// duplicate wins), not a live lookup.
class AvatarPartRow : public RefCounted {
	GDCLASS(AvatarPartRow, RefCounted)

	AvatarPartSnapshot value_ = {};

protected:
	static void _bind_methods();

public:
	void assign(const AvatarPartSnapshot &p_value) { value_ = p_value; }
	void assign(const AvatarPart &p_value);

	// AvatarDatabase.PART_HEAD / PART_BODY / PART_ARMS.
	int get_kind() const { return value_.kind; }
	// The `define` identifier (the combo lookup key).
	String get_name() const;
	// The "Avatars" RTXT string key of the display name.
	String get_display_name() const;
	String get_graphic() const;
	String get_graphic_j() const;
	String get_graphic_s() const;
	// The authored `camo r g b` bytes: texture-variant selectors stored raw
	// into TEX_CAMO1..3 (AvatarDatabase.apply_part_camo).
	Vector3i get_camo() const;
	int get_voice() const { return value_.voice; }
	// AvatarDatabase.SEX_MALE / SEX_FEMALE.
	int get_sex() const { return value_.sex; }
};

// One resolved combo: the authored id, the three part names, the parse-time
// part snapshots (arms null when the combo has none), and its place in the
// tree — the owning nationality's alignment, the three tree indices, and the
// packed character id the registry derives for it (npwire/character_id.h).
class AvatarComboRow : public RefCounted {
	GDCLASS(AvatarComboRow, RefCounted)

	AvatarCombo value_ = {};
	Ref<AvatarPartRow> head_;
	Ref<AvatarPartRow> body_;
	Ref<AvatarPartRow> arms_;
	int alignment_ = 0;
	int nationality_index_ = -1;
	int division_index_ = -1;
	int combo_index_ = -1;
	int character_id_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const AvatarCombo &p_value, int p_alignment, int p_nationality_index,
			int p_division_index, int p_combo_index, int p_character_id);

	// The id token verbatim (e.g. "001") and its parsed number.
	String get_raw_id() const;
	int get_id() const { return value_.id; }
	String get_head_name() const;
	String get_body_name() const;
	String get_arms_name() const;
	Ref<AvatarPartRow> get_head() const { return head_; }
	Ref<AvatarPartRow> get_body() const { return body_; }
	// Null when the combo authors no arms.
	Ref<AvatarPartRow> get_arms() const { return arms_; }
	bool has_arms() const { return value_.has_arms != 0; }
	// AvatarDatabase.ALIGN_GOOD / ALIGN_EVIL (the owning nationality's).
	int get_alignment() const { return alignment_; }
	int get_nationality_index() const { return nationality_index_; }
	int get_division_index() const { return division_index_; }
	int get_combo_index() const { return combo_index_; }
	// The packed identity (authored nationality bits 0..4, division 5..8,
	// combo 9..14, alignment 15) the wire and weapon.sav carry.
	int get_character_id() const { return character_id_; }
};

// One `nationality <id> <name_key> [flags]` block header.
class AvatarNationalityRow : public RefCounted {
	GDCLASS(AvatarNationalityRow, RefCounted)

	String raw_id_;
	int id_ = 0;
	String name_key_;
	String flags_;
	int alignment_ = 0;
	bool has_alignment_ = false;
	int division_count_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const AvatarNationality &p_value);

	String get_raw_id() const { return raw_id_; }
	int get_id() const { return id_; }
	String get_name_key() const { return name_key_; }
	// Trailing tokens after the name key (e.g. "skipdemo"); empty if none.
	String get_flags() const { return flags_; }
	// AvatarDatabase.ALIGN_GOOD / ALIGN_EVIL.
	int get_alignment() const { return alignment_; }
	// Whether an `alignment` line was authored.
	bool has_alignment() const { return has_alignment_; }
	int get_division_count() const { return division_count_; }
};

// One `division <id> <name_key> [flags]` block header.
class AvatarDivisionRow : public RefCounted {
	GDCLASS(AvatarDivisionRow, RefCounted)

	String raw_id_;
	int id_ = 0;
	String name_key_;
	String flags_;
	int combo_count_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const AvatarDivision &p_value);

	String get_raw_id() const { return raw_id_; }
	int get_id() const { return id_; }
	String get_name_key() const { return name_key_; }
	String get_flags() const { return flags_; }
	int get_combo_count() const { return combo_count_; }
};

// One parse diagnostic (a skipped combo reference, an unknown line).
class AvatarDiagnosticRow : public RefCounted {
	GDCLASS(AvatarDiagnosticRow, RefCounted)

	AvatarDiagnostic value_ = {};

protected:
	static void _bind_methods();

public:
	void assign(const AvatarDiagnostic &p_value) { value_ = p_value; }

	// 1-based source line; 0 when synthesized.
	int get_line() const { return static_cast<int>(value_.line); }
	// AvatarDatabase.DIAG_WARNING / DIAG_ERROR.
	int get_severity() const { return value_.severity; }
	String get_code() const;
	String get_message() const;
};

} // namespace godot
