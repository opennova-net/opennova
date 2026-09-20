#include "object/avatar_records.h"

#include <cstring>

using namespace godot;
using namespace opennova::avatars;

namespace {

#define READ_ONLY_PROPERTY(m_type, m_name, m_getter)                                  \
	ADD_PROPERTY(PropertyInfo(m_type, m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),          \
			"", m_getter)

} // namespace

// --- AvatarPartRow -------------------------------------------------------------

void AvatarPartRow::assign(const AvatarPart &p_value) {
	AvatarPartSnapshot snapshot = {};
	snapshot.kind = p_value.kind;
	std::memcpy(snapshot.name, p_value.name, sizeof(snapshot.name));
	std::memcpy(snapshot.display_name, p_value.display_name, sizeof(snapshot.display_name));
	std::memcpy(snapshot.graphic, p_value.graphic, sizeof(snapshot.graphic));
	std::memcpy(snapshot.graphic_j, p_value.graphic_j, sizeof(snapshot.graphic_j));
	std::memcpy(snapshot.graphic_s, p_value.graphic_s, sizeof(snapshot.graphic_s));
	snapshot.camo[0] = p_value.camo[0];
	snapshot.camo[1] = p_value.camo[1];
	snapshot.camo[2] = p_value.camo[2];
	snapshot.voice = p_value.voice;
	snapshot.sex = p_value.sex;
	value_ = snapshot;
}

String AvatarPartRow::get_name() const { return String(value_.name); }
String AvatarPartRow::get_display_name() const { return String(value_.display_name); }
String AvatarPartRow::get_graphic() const { return String(value_.graphic); }

Vector3i AvatarPartRow::get_camo() const {
	return Vector3i(value_.camo[0], value_.camo[1], value_.camo[2]);
}

void AvatarPartRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_kind"), &AvatarPartRow::get_kind);
	ClassDB::bind_method(D_METHOD("get_name"), &AvatarPartRow::get_name);
	ClassDB::bind_method(D_METHOD("get_display_name"), &AvatarPartRow::get_display_name);
	ClassDB::bind_method(D_METHOD("get_graphic"), &AvatarPartRow::get_graphic);
	ClassDB::bind_method(D_METHOD("get_camo"), &AvatarPartRow::get_camo);
	ClassDB::bind_method(D_METHOD("get_voice"), &AvatarPartRow::get_voice);
	ClassDB::bind_method(D_METHOD("get_sex"), &AvatarPartRow::get_sex);
	READ_ONLY_PROPERTY(Variant::INT, "kind", "get_kind");
	READ_ONLY_PROPERTY(Variant::STRING, "name", "get_name");
	READ_ONLY_PROPERTY(Variant::STRING, "display_name", "get_display_name");
	READ_ONLY_PROPERTY(Variant::STRING, "graphic", "get_graphic");
	READ_ONLY_PROPERTY(Variant::VECTOR3I, "camo", "get_camo");
	READ_ONLY_PROPERTY(Variant::INT, "voice", "get_voice");
	READ_ONLY_PROPERTY(Variant::INT, "sex", "get_sex");
}

// --- AvatarComboRow ------------------------------------------------------------

void AvatarComboRow::assign(const AvatarCombo &p_value, int p_alignment,
		int p_nationality_index, int p_division_index, int p_combo_index,
		int p_character_id) {
	value_ = p_value;
	head_.instantiate();
	head_->assign(value_.head);
	body_.instantiate();
	body_->assign(value_.body);
	arms_.unref();
	if (value_.has_arms) {
		arms_.instantiate();
		arms_->assign(value_.arms);
	}
	alignment_ = p_alignment;
	nationality_index_ = p_nationality_index;
	division_index_ = p_division_index;
	combo_index_ = p_combo_index;
	character_id_ = p_character_id;
}

String AvatarComboRow::get_raw_id() const { return String(value_.raw_id); }
String AvatarComboRow::get_head_name() const { return String(value_.head_name); }

void AvatarComboRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_raw_id"), &AvatarComboRow::get_raw_id);
	ClassDB::bind_method(D_METHOD("get_id"), &AvatarComboRow::get_id);
	ClassDB::bind_method(D_METHOD("get_head_name"), &AvatarComboRow::get_head_name);
	ClassDB::bind_method(D_METHOD("get_head"), &AvatarComboRow::get_head);
	ClassDB::bind_method(D_METHOD("get_body"), &AvatarComboRow::get_body);
	ClassDB::bind_method(D_METHOD("get_arms"), &AvatarComboRow::get_arms);
	ClassDB::bind_method(D_METHOD("has_arms"), &AvatarComboRow::has_arms);
	ClassDB::bind_method(D_METHOD("get_alignment"), &AvatarComboRow::get_alignment);
	ClassDB::bind_method(D_METHOD("get_nationality_index"), &AvatarComboRow::get_nationality_index);
	ClassDB::bind_method(D_METHOD("get_division_index"), &AvatarComboRow::get_division_index);
	ClassDB::bind_method(D_METHOD("get_combo_index"), &AvatarComboRow::get_combo_index);
	ClassDB::bind_method(D_METHOD("get_character_id"), &AvatarComboRow::get_character_id);
	READ_ONLY_PROPERTY(Variant::STRING, "raw_id", "get_raw_id");
	READ_ONLY_PROPERTY(Variant::INT, "id", "get_id");
	READ_ONLY_PROPERTY(Variant::STRING, "head_name", "get_head_name");
	READ_ONLY_PROPERTY(Variant::INT, "alignment", "get_alignment");
	READ_ONLY_PROPERTY(Variant::INT, "nationality_index", "get_nationality_index");
	READ_ONLY_PROPERTY(Variant::INT, "division_index", "get_division_index");
	READ_ONLY_PROPERTY(Variant::INT, "combo_index", "get_combo_index");
	READ_ONLY_PROPERTY(Variant::INT, "character_id", "get_character_id");
}

// --- AvatarNationalityRow ------------------------------------------------------

void AvatarNationalityRow::assign(const AvatarNationality &p_value) {
	raw_id_ = String(p_value.raw_id);
	id_ = p_value.id;
	name_key_ = String(p_value.name_key);
	flags_ = String(p_value.flags);
	alignment_ = p_value.alignment;
	division_count_ = static_cast<int>(p_value.divisions_count);
}

void AvatarNationalityRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_raw_id"), &AvatarNationalityRow::get_raw_id);
	ClassDB::bind_method(D_METHOD("get_id"), &AvatarNationalityRow::get_id);
	ClassDB::bind_method(D_METHOD("get_name_key"), &AvatarNationalityRow::get_name_key);
	ClassDB::bind_method(D_METHOD("get_flags"), &AvatarNationalityRow::get_flags);
	ClassDB::bind_method(D_METHOD("get_alignment"), &AvatarNationalityRow::get_alignment);
	ClassDB::bind_method(D_METHOD("get_division_count"), &AvatarNationalityRow::get_division_count);
	READ_ONLY_PROPERTY(Variant::STRING, "raw_id", "get_raw_id");
	READ_ONLY_PROPERTY(Variant::INT, "id", "get_id");
	READ_ONLY_PROPERTY(Variant::STRING, "name_key", "get_name_key");
	READ_ONLY_PROPERTY(Variant::STRING, "flags", "get_flags");
	READ_ONLY_PROPERTY(Variant::INT, "alignment", "get_alignment");
	READ_ONLY_PROPERTY(Variant::INT, "division_count", "get_division_count");
}

// --- AvatarDivisionRow ---------------------------------------------------------

void AvatarDivisionRow::assign(const AvatarDivision &p_value) {
	raw_id_ = String(p_value.raw_id);
	id_ = p_value.id;
	name_key_ = String(p_value.name_key);
	flags_ = String(p_value.flags);
	combo_count_ = static_cast<int>(p_value.combos_count);
}

void AvatarDivisionRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_raw_id"), &AvatarDivisionRow::get_raw_id);
	ClassDB::bind_method(D_METHOD("get_id"), &AvatarDivisionRow::get_id);
	ClassDB::bind_method(D_METHOD("get_name_key"), &AvatarDivisionRow::get_name_key);
	ClassDB::bind_method(D_METHOD("get_flags"), &AvatarDivisionRow::get_flags);
	ClassDB::bind_method(D_METHOD("get_combo_count"), &AvatarDivisionRow::get_combo_count);
	READ_ONLY_PROPERTY(Variant::STRING, "raw_id", "get_raw_id");
	READ_ONLY_PROPERTY(Variant::INT, "id", "get_id");
	READ_ONLY_PROPERTY(Variant::STRING, "name_key", "get_name_key");
	READ_ONLY_PROPERTY(Variant::STRING, "flags", "get_flags");
	READ_ONLY_PROPERTY(Variant::INT, "combo_count", "get_combo_count");
}

// --- AvatarDiagnosticRow -------------------------------------------------------

String AvatarDiagnosticRow::get_code() const { return String(value_.code); }
String AvatarDiagnosticRow::get_message() const { return String(value_.message); }

void AvatarDiagnosticRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_line"), &AvatarDiagnosticRow::get_line);
	ClassDB::bind_method(D_METHOD("get_severity"), &AvatarDiagnosticRow::get_severity);
	ClassDB::bind_method(D_METHOD("get_code"), &AvatarDiagnosticRow::get_code);
	ClassDB::bind_method(D_METHOD("get_message"), &AvatarDiagnosticRow::get_message);
	READ_ONLY_PROPERTY(Variant::INT, "line", "get_line");
	READ_ONLY_PROPERTY(Variant::INT, "severity", "get_severity");
	READ_ONLY_PROPERTY(Variant::STRING, "code", "get_code");
	READ_ONLY_PROPERTY(Variant::STRING, "message", "get_message");
}
