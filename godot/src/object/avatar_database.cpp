#include "object/avatar_database.h"

#include "object/avatar_records.h"
#include "object/character_join_profile.h"
#include "object/object_model.h"
#include "player/player_spawn_loadout.h"
#include "resource_index/resource_root.h"
#include "util/data_format.h"

#include <formats/avatars/avatars.h>
#include <formats/avatars/preview_animation.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/inmatch/join_character_profile.h>
#include <net/npwire/character_id.h>

#include <algorithm>

using namespace godot;
using namespace opennova::avatars;
using namespace opennova::threedi;

namespace {

int dict_int(const Dictionary &d, const char *key, int def) {
	return d.has(key) ? (int)d[key] : def;
}

} // namespace

float AvatarDatabase::preview_zoom_damp_per_tick() {
	return opennova::avatars::kPreviewZoomDampPerTick;
}
float AvatarDatabase::preview_zoom_in_scale() {
	return opennova::avatars::kPreviewZoomInScale;
}
float AvatarDatabase::preview_idle_speed_deg_per_sec() {
	return opennova::avatars::kPreviewIdleSpeedDegPerSec;
}
float AvatarDatabase::preview_sway_freq_rad_per_sec() {
	return opennova::avatars::kPreviewSwayFreqRadPerSec;
}
float AvatarDatabase::preview_sway_amp_deg() {
	return opennova::avatars::kPreviewSwayAmpDeg;
}
String AvatarDatabase::preview_skeleton_bad() {
	return String(opennova::avatars::kPreviewSkeletonBad);
}
String AvatarDatabase::preview_idle_bad() {
	return String(opennova::avatars::kPreviewIdleBad);
}

PackedStringArray AvatarDatabase::part_camo_registers() {
	PackedStringArray out;
	out.push_back(threedi_ctrl_register_name(THREEDI_CTRL_TEX_CAMO1));
	out.push_back(threedi_ctrl_register_name(THREEDI_CTRL_TEX_CAMO2));
	out.push_back(threedi_ctrl_register_name(THREEDI_CTRL_TEX_CAMO3));
	return out;
}

void AvatarDatabase::apply_part_camo(ObjectModel *p_model, const Vector3i &p_camo,
		const String &p_owner) {
	if (p_model == nullptr) {
		return;
	}
	const PackedStringArray registers = part_camo_registers();
	p_model->begin_ctrl_update();
	for (int i = 0; i < 3; ++i) {
		// A zero-extended byte store, exactly the retail movzx + mov dword.
		p_model->set_ctrl_override(p_owner, registers[i], p_camo[i] & 0xff);
	}
	p_model->end_ctrl_update();
}

void AvatarDatabase::_bind_methods() {
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_zoom_damp_per_tick"),
			&AvatarDatabase::preview_zoom_damp_per_tick);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_zoom_in_scale"),
			&AvatarDatabase::preview_zoom_in_scale);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_idle_speed_deg_per_sec"),
			&AvatarDatabase::preview_idle_speed_deg_per_sec);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_sway_freq_rad_per_sec"),
			&AvatarDatabase::preview_sway_freq_rad_per_sec);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_sway_amp_deg"),
			&AvatarDatabase::preview_sway_amp_deg);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_skeleton_bad"),
			&AvatarDatabase::preview_skeleton_bad);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("preview_idle_bad"), &AvatarDatabase::preview_idle_bad);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("apply_part_camo", "model", "camo", "owner"),
			&AvatarDatabase::apply_part_camo);
	ClassDB::bind_method(D_METHOD("load", "path"), &AvatarDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &AvatarDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &AvatarDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &AvatarDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &AvatarDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_diagnostics"), &AvatarDatabase::get_diagnostics);
	ClassDB::bind_method(D_METHOD("get_part_count"), &AvatarDatabase::get_part_count);
	ClassDB::bind_method(D_METHOD("get_nationality_count"), &AvatarDatabase::get_nationality_count);
	ClassDB::bind_method(D_METHOD("get_part_names", "kind"), &AvatarDatabase::get_part_names);
	ClassDB::bind_method(D_METHOD("get_part", "kind", "name"), &AvatarDatabase::get_part);
	ClassDB::bind_method(D_METHOD("get_nationality", "nat_index"), &AvatarDatabase::get_nationality);
	ClassDB::bind_method(D_METHOD("get_division_count", "nat_index"), &AvatarDatabase::get_division_count);
	ClassDB::bind_method(D_METHOD("get_division", "nat_index", "div_index"), &AvatarDatabase::get_division);
	ClassDB::bind_method(D_METHOD("get_combo_count", "nat_index", "div_index"), &AvatarDatabase::get_combo_count);
	ClassDB::bind_method(D_METHOD("get_combo", "nat_index", "div_index", "combo_index"), &AvatarDatabase::get_combo);
	ClassDB::bind_method(D_METHOD("resolve_character_id", "character_id",
			"expected_alignment"), &AvatarDatabase::resolve_character_id,
			DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("first_character_id", "alignment"),
			&AvatarDatabase::first_character_id);
	ClassDB::bind_method(D_METHOD("character_join_profile", "selection"),
			&AvatarDatabase::character_join_profile, DEFVAL(Dictionary()));

	BIND_ENUM_CONSTANT(PART_HEAD);
	BIND_ENUM_CONSTANT(PART_BODY);
	BIND_ENUM_CONSTANT(PART_ARMS);
	BIND_CONSTANT(SEX_MALE);
	BIND_CONSTANT(SEX_FEMALE);
	BIND_CONSTANT(ALIGN_GOOD);
	BIND_CONSTANT(ALIGN_EVIL);
	BIND_CONSTANT(DIAG_WARNING);
	BIND_CONSTANT(DIAG_ERROR);
	BIND_CONSTANT(PREVIEW_INITIAL_YAW_RANGE_DEG);
}

AvatarDatabase::~AvatarDatabase() {
	avatars_free(&file_);
}

void AvatarDatabase::clear() {
	avatars_free(&file_);
	file_ = {};
	registry_dirty_ = true;
	loaded = false;
}

Error AvatarDatabase::adopt_bytes(const PackedByteArray &p_bytes, const String &p_label) {
	AvatarsFile file = {};
	if (avatars_parse_memory(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()), &file) != 0) {
		last_error = String("avatars_parse_memory failed for ") + p_label;
		return ERR_CANT_OPEN;
	}
	file_ = file;
	registry_dirty_ = true;
	loaded = true;
	return OK;
}

Error AvatarDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	clear();

	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		last_error = String("Cannot open Avatars.def: ") + path;
		return ERR_CANT_OPEN;
	}
	return adopt_bytes(bytes, path);
}

Error AvatarDatabase::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	last_error = String();
	clear();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) {
		last_error = "Avatars.def filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) {
		last_error = String("Avatars.def not found in resource root: ") + file_name;
		return ERR_FILE_NOT_FOUND;
	}
	const Error err = adopt_bytes(bytes, file_name);
	if (err == OK) {
		source_path = file_name;
	}
	return err;
}

bool AvatarDatabase::is_loaded() const {
	return loaded;
}

String AvatarDatabase::get_source_path() const {
	return source_path;
}

String AvatarDatabase::get_last_error() const {
	return last_error;
}

TypedArray<AvatarDiagnosticRow> AvatarDatabase::get_diagnostics() const {
	TypedArray<AvatarDiagnosticRow> out;
	for (size_t i = 0; i < file_.diagnostics_count; ++i) {
		Ref<AvatarDiagnosticRow> row;
		row.instantiate();
		row->assign(file_.diagnostics[i]);
		out.push_back(row);
	}
	return out;
}

int AvatarDatabase::get_part_count() const {
	return static_cast<int>(file_.parts_count);
}

int AvatarDatabase::get_nationality_count() const {
	return static_cast<int>(file_.nationalities_count);
}

const AvatarPart *AvatarDatabase::find_part(int kind, const String &name) const {
	// The last prior definition wins (the parse-time snapshot rule).
	for (size_t i = file_.parts_count; i > 0; --i) {
		const AvatarPart &p = file_.parts[i - 1];
		if (p.kind == kind && String(p.name).nocasecmp_to(name) == 0) {
			return &p;
		}
	}
	return nullptr;
}

const AvatarNationality *AvatarDatabase::nationality_at(int nat_index) const {
	if (nat_index < 0 || nat_index >= static_cast<int>(file_.nationalities_count)) {
		return nullptr;
	}
	return &file_.nationalities[nat_index];
}

const AvatarDivision *AvatarDatabase::division_at(int nat_index, int div_index) const {
	const AvatarNationality *n = nationality_at(nat_index);
	if (n == nullptr || div_index < 0 || div_index >= static_cast<int>(n->divisions_count)) {
		return nullptr;
	}
	return &n->divisions[div_index];
}

const AvatarCombo *AvatarDatabase::combo_at(int nat_index, int div_index, int combo_index) const {
	const AvatarDivision *d = division_at(nat_index, div_index);
	if (d == nullptr || combo_index < 0 || combo_index >= static_cast<int>(d->combos_count)) {
		return nullptr;
	}
	return &d->combos[combo_index];
}

PackedStringArray AvatarDatabase::get_part_names(PartKind kind) const {
	std::vector<const AvatarPart *> sel;
	for (size_t i = 0; i < file_.parts_count; ++i) {
		if (file_.parts[i].kind == kind) {
			sel.push_back(&file_.parts[i]);
		}
	}
	std::sort(sel.begin(), sel.end(), [](const AvatarPart *a, const AvatarPart *b) {
		return String(a->name).naturalnocasecmp_to(String(b->name)) < 0;
	});
	PackedStringArray out;
	out.resize(static_cast<int>(sel.size()));
	for (size_t i = 0; i < sel.size(); ++i) {
		out.set(static_cast<int>(i), String(sel[i]->name));
	}
	return out;
}

Ref<AvatarPartRow> AvatarDatabase::get_part(PartKind kind, const String &name) const {
	const AvatarPart *p = find_part(kind, name);
	if (p == nullptr) {
		return Ref<AvatarPartRow>();
	}
	Ref<AvatarPartRow> row;
	row.instantiate();
	row->assign(*p);
	return row;
}

Ref<AvatarNationalityRow> AvatarDatabase::get_nationality(int nat_index) const {
	const AvatarNationality *n = nationality_at(nat_index);
	if (n == nullptr) {
		return Ref<AvatarNationalityRow>();
	}
	Ref<AvatarNationalityRow> row;
	row.instantiate();
	row->assign(*n);
	return row;
}

int AvatarDatabase::get_division_count(int nat_index) const {
	const AvatarNationality *n = nationality_at(nat_index);
	return n ? static_cast<int>(n->divisions_count) : 0;
}

Ref<AvatarDivisionRow> AvatarDatabase::get_division(int nat_index, int div_index) const {
	const AvatarDivision *d = division_at(nat_index, div_index);
	if (d == nullptr) {
		return Ref<AvatarDivisionRow>();
	}
	Ref<AvatarDivisionRow> row;
	row.instantiate();
	row->assign(*d);
	return row;
}

int AvatarDatabase::get_combo_count(int nat_index, int div_index) const {
	const AvatarDivision *d = division_at(nat_index, div_index);
	return d ? static_cast<int>(d->combos_count) : 0;
}

Ref<AvatarComboRow> AvatarDatabase::combo_row(int nat_index, int div_index, int combo_index) const {
	const AvatarCombo *c = combo_at(nat_index, div_index, combo_index);
	if (c == nullptr) {
		return Ref<AvatarComboRow>();
	}
	// alignment comes from the owning nationality (D-PLAYERINFO copies it into
	// the combo); the packed id is the registry's derivation for these indices.
	const AvatarNationality *n = nationality_at(nat_index);
	const opennova::inmatch::CharacterEntry *entry =
			character_registry().find_by_indices(nat_index, div_index, combo_index);
	Ref<AvatarComboRow> row;
	row.instantiate();
	row->assign(*c, n->alignment, nat_index, div_index, combo_index,
			entry ? static_cast<int>(entry->packed_id) : 0);
	return row;
}

Ref<AvatarComboRow> AvatarDatabase::get_combo(int nat_index, int div_index, int combo_index) const {
	return combo_row(nat_index, div_index, combo_index);
}

const opennova::inmatch::CharacterRegistry &
AvatarDatabase::character_registry() const {
	if (registry_dirty_) {
		registry_ = opennova::inmatch::CharacterRegistry::from_file(file_);
		registry_dirty_ = false;
	}
	return registry_;
}

Ref<AvatarComboRow> AvatarDatabase::resolve_character_id(
		int character_id, int expected_alignment) const {
	// The registry decode (runtime/inmatch/character_registry.h carries the witness).
	const uint16_t packed = static_cast<uint16_t>(character_id & 0xffff);
	const opennova::inmatch::CharacterEntry *entry =
			character_registry().find_by_packed_id(packed, expected_alignment);
	if (entry == nullptr) {
		return Ref<AvatarComboRow>();
	}
	return combo_row(entry->nationality_index, entry->division_index, entry->combo_index);
}

int AvatarDatabase::first_character_id(int alignment) const {
	return character_registry().first_character_id(alignment);
}

Ref<CharacterJoinProfile> AvatarDatabase::character_join_profile(
		const Dictionary &p_selection) const {
	opennova::inmatch::JoinSideSelection saved[2];
	const Array sides = p_selection.has("side_profiles")
			? (Array)p_selection["side_profiles"]
			: Array();
	for (int side = 0; side < 2 && side < sides.size(); ++side) {
		if (sides[side].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary sd = sides[side];
		if (sd.is_empty()) {
			continue;
		}
		saved[side].present = true;
		saved[side].nationality_index = dict_int(sd, "nationality", -1);
		saved[side].division_index = dict_int(sd, "division", -1);
		saved[side].combo_index = dict_int(sd, "combo", -1);
		saved[side].player_class = dict_int(sd, "player_class",
				opennova::inmatch::kJoinDefaultPlayerClass);
	}
	Ref<CharacterJoinProfile> out;
	out.instantiate();
	out->assign(opennova::inmatch::join_character_profile(character_registry(), saved));
	return out;
}

Ref<CharacterJoinProfile> AvatarDatabase::character_join_profile_from_loadout(
		const Ref<PlayerSpawnLoadout> &p_loadout) const {
	opennova::inmatch::JoinSideSelection saved[2];
	if (p_loadout.is_valid()) {
		p_loadout->fill_join_sides(saved);
	}
	Ref<CharacterJoinProfile> out;
	out.instantiate();
	out->assign(opennova::inmatch::join_character_profile(character_registry(), saved));
	return out;
}

std::vector<AvatarDatabase::CharacterSexRow>
AvatarDatabase::character_sex_rows() const {
	// File order, duplicate packed ids first-wins (the registry's walk order).
	std::vector<CharacterSexRow> rows;
	for (const opennova::inmatch::CharacterEntry &entry :
			character_registry().entries()) {
		const bool duplicate = std::any_of(rows.begin(), rows.end(),
				[&entry](const CharacterSexRow &row) {
					return row.character_id == entry.packed_id;
				});
		if (!duplicate)
			rows.push_back(CharacterSexRow{ entry.packed_id, entry.head_female });
	}
	return rows;
}
