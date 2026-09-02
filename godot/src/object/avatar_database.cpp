#include "object/avatar_database.h"

#include "resource_index/resource_root.h"
#include "util/data_format.h"

#include <formats/avatars/avatars.h>
#include <net/npruntime/join_character_profile.h>
#include <formats/avatars/preview_animation.h>
#include <net/npwire/character_id.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include "object/object_model.h"

#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace godot;

namespace {

// String -> fixed C buffer (truncating, always NUL-terminated), for building the
// transient AvatarsFile passed to avatars_write().
void cset(char *dst, size_t n, const String &s) {
	const CharString utf8 = s.utf8();
	std::snprintf(dst, n, "%s", utf8.get_data());
}

int dict_int(const Dictionary &d, const char *key, int def) {
	return d.has(key) ? (int)d[key] : def;
}

String dict_str(const Dictionary &d, const char *key) {
	return d.has(key) ? String(d[key]) : String();
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

void AvatarDatabase::apply_part_camo(ObjectModel *p_model, const Array &p_camo,
		const String &p_owner) {
	if (p_model == nullptr || p_camo.size() < 3) {
		return;
	}
	const PackedStringArray registers = part_camo_registers();
	p_model->begin_ctrl_update();
	for (int i = 0; i < 3; ++i) {
		// A zero-extended byte store, exactly the retail movzx + mov dword.
		p_model->set_ctrl_override(p_owner, registers[i], int(p_camo[i]) & 0xff);
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
			D_METHOD("part_camo_registers"),
			&AvatarDatabase::part_camo_registers);
	ClassDB::bind_static_method("AvatarDatabase",
			D_METHOD("apply_part_camo", "model", "camo", "owner"),
			&AvatarDatabase::apply_part_camo);
	ClassDB::bind_method(D_METHOD("load", "path"), &AvatarDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &AvatarDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &AvatarDatabase::save_to_path);
	ClassDB::bind_method(D_METHOD("create_empty"), &AvatarDatabase::create_empty);
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
	ClassDB::bind_method(D_METHOD("resolve_combo", "nat_index", "div_index", "combo_index"), &AvatarDatabase::resolve_combo);
	ClassDB::bind_method(D_METHOD("resolve_character_id", "character_id",
			"expected_alignment"), &AvatarDatabase::resolve_character_id,
			DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("first_character_id", "alignment"),
			&AvatarDatabase::first_character_id);
	ClassDB::bind_method(D_METHOD("character_join_profile", "selection"),
			&AvatarDatabase::character_join_profile, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("get_model"), &AvatarDatabase::get_model);
	ClassDB::bind_method(D_METHOD("set_model", "model"), &AvatarDatabase::set_model);

	BIND_CONSTANT(PART_HEAD);
	BIND_CONSTANT(PART_BODY);
	BIND_CONSTANT(PART_ARMS);
	BIND_CONSTANT(SEX_MALE);
	BIND_CONSTANT(ALIGN_GOOD);
	BIND_CONSTANT(ALIGN_EVIL);
	BIND_CONSTANT(DIAG_WARNING);
	BIND_CONSTANT(PREVIEW_INITIAL_YAW_RANGE_DEG);

	ADD_SIGNAL(MethodInfo("changed"));
}

void AvatarDatabase::clear() {
	parts.clear();
	nationalities.clear();
	diagnostics.clear();
	registry_dirty_ = true;
	loaded = false;
}

void AvatarDatabase::adopt_parsed(const AvatarsFile &avatars_file) {
	const AvatarsFile *f = &avatars_file;
	parts.clear();
	nationalities.clear();
	parts.reserve(f->parts_count);
	for (size_t i = 0; i < f->parts_count; ++i) {
		const AvatarPart &sp = f->parts[i];
		Part p;
		p.kind = sp.kind;
		p.name = String(sp.name);
		p.display_name = String(sp.display_name);
		p.graphic = String(sp.graphic);
		p.graphic_j = String(sp.graphic_j);
		p.graphic_s = String(sp.graphic_s);
		p.camo[0] = sp.camo[0];
		p.camo[1] = sp.camo[1];
		p.camo[2] = sp.camo[2];
		p.voice = sp.voice;
		p.sex = sp.sex;
		parts.push_back(p);
	}
	nationalities.reserve(f->nationalities_count);
	for (size_t i = 0; i < f->nationalities_count; ++i) {
		const AvatarNationality &sn = f->nationalities[i];
		Nationality n;
		n.raw_id = String(sn.raw_id);
		n.id = sn.id;
		n.name_key = String(sn.name_key);
		n.flags = String(sn.flags);
		n.alignment = sn.alignment;
		n.has_alignment = sn.has_alignment != 0;
		n.divisions.reserve(sn.divisions_count);
		for (size_t j = 0; j < sn.divisions_count; ++j) {
			const AvatarDivision &sd = sn.divisions[j];
			Division d;
			d.raw_id = String(sd.raw_id);
			d.id = sd.id;
			d.name_key = String(sd.name_key);
			d.flags = String(sd.flags);
			d.combos.reserve(sd.combos_count);
			for (size_t k = 0; k < sd.combos_count; ++k) {
				const AvatarCombo &sc = sd.combos[k];
				Combo c;
				c.raw_id = String(sc.raw_id);
				c.id = sc.id;
				c.head_name = String(sc.head_name);
				c.body_name = String(sc.body_name);
				c.arms_name = String(sc.arms_name);
				c.head = part_from_snapshot(sc.head);
				c.body = part_from_snapshot(sc.body);
				c.arms = part_from_snapshot(sc.arms);
				c.has_arms = sc.has_arms != 0;
				d.combos.push_back(c);
			}
			n.divisions.push_back(std::move(d));
		}
		nationalities.push_back(std::move(n));
	}
	diagnostics.reserve(f->diagnostics_count);
	for (size_t i = 0; i < f->diagnostics_count; ++i) {
		const AvatarDiagnostic &sd = f->diagnostics[i];
		Diagnostic d;
		d.line = static_cast<int>(sd.line);
		d.severity = sd.severity;
		d.code = String(sd.code);
		d.message = String(sd.message);
		diagnostics.push_back(d);
	}
	registry_dirty_ = true;
	loaded = true;
}

const AvatarDatabase::Part *AvatarDatabase::find_part(int kind, const String &name) const {
	for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
		const Part &p = *it;
		if (p.kind == kind && p.name.nocasecmp_to(name) == 0) {
			return &p;
		}
	}
	return nullptr;
}

AvatarDatabase::Part AvatarDatabase::part_from_snapshot(const AvatarPartSnapshot &snapshot) const {
	const AvatarPartSnapshot *sp = &snapshot;
	Part p;
	p.kind = sp->kind;
	p.name = String(sp->name);
	p.display_name = String(sp->display_name);
	p.graphic = String(sp->graphic);
	p.graphic_j = String(sp->graphic_j);
	p.graphic_s = String(sp->graphic_s);
	p.camo[0] = sp->camo[0];
	p.camo[1] = sp->camo[1];
	p.camo[2] = sp->camo[2];
	p.voice = sp->voice;
	p.sex = sp->sex;
	return p;
}

void AvatarDatabase::resolve_combo_snapshots(Combo &combo) {
	if (const Part *p = find_part(PART_HEAD, combo.head_name)) {
		combo.head = *p;
	}
	if (const Part *p = find_part(PART_BODY, combo.body_name)) {
		combo.body = *p;
	}
	combo.has_arms = false;
	combo.arms = Part();
	if (!combo.arms_name.is_empty()) {
		if (const Part *p = find_part(PART_ARMS, combo.arms_name)) {
			combo.arms = *p;
			combo.has_arms = true;
		}
	}
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
	AvatarsFile file = {};
	if (avatars_parse_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("avatars_parse_memory failed for ") + path;
		return ERR_CANT_OPEN;
	}
	adopt_parsed(file);
	avatars_free(&file);
	registry_dirty_ = true;
	emit_signal("changed");
	return OK;
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
	AvatarsFile file = {};
	if (avatars_parse_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("avatars_parse_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}
	adopt_parsed(file);
	avatars_free(&file);
	source_path = file_name;
	registry_dirty_ = true;
	emit_signal("changed");
	return OK;
}

Error AvatarDatabase::save_to_path(const String &path) {
	last_error = String();

	// Build a transient AvatarsFile from the in-memory model. raw_lines stay NULL
	// (the binding does not model unknown lines); avatars_free() tolerates that.
	AvatarsFile f = {};
	f.parts_count = parts.size();
	if (f.parts_count) {
		f.parts = static_cast<AvatarPart *>(std::calloc(f.parts_count, sizeof(AvatarPart)));
	}
	for (size_t i = 0; i < parts.size(); ++i) {
		const Part &p = parts[i];
		AvatarPart &sp = f.parts[i];
		sp.kind = p.kind;
		cset(sp.name, sizeof(sp.name), p.name);
		cset(sp.display_name, sizeof(sp.display_name), p.display_name);
		cset(sp.graphic, sizeof(sp.graphic), p.graphic);
		cset(sp.graphic_j, sizeof(sp.graphic_j), p.graphic_j);
		cset(sp.graphic_s, sizeof(sp.graphic_s), p.graphic_s);
		sp.camo[0] = p.camo[0];
		sp.camo[1] = p.camo[1];
		sp.camo[2] = p.camo[2];
		sp.voice = p.voice;
		sp.sex = p.sex;
	}
	f.nationalities_count = nationalities.size();
	if (f.nationalities_count) {
		f.nationalities = static_cast<AvatarNationality *>(std::calloc(f.nationalities_count, sizeof(AvatarNationality)));
	}
	for (size_t i = 0; i < nationalities.size(); ++i) {
		const Nationality &n = nationalities[i];
		AvatarNationality &sn = f.nationalities[i];
		cset(sn.raw_id, sizeof(sn.raw_id), n.raw_id);
		sn.id = n.id;
		cset(sn.name_key, sizeof(sn.name_key), n.name_key);
		cset(sn.flags, sizeof(sn.flags), n.flags);
		sn.alignment = n.alignment;
		sn.has_alignment = n.has_alignment ? 1 : 0;
		sn.divisions_count = n.divisions.size();
		if (sn.divisions_count) {
			sn.divisions = static_cast<AvatarDivision *>(std::calloc(sn.divisions_count, sizeof(AvatarDivision)));
		}
		for (size_t j = 0; j < n.divisions.size(); ++j) {
			const Division &d = n.divisions[j];
			AvatarDivision &sd = sn.divisions[j];
			cset(sd.raw_id, sizeof(sd.raw_id), d.raw_id);
			sd.id = d.id;
			cset(sd.name_key, sizeof(sd.name_key), d.name_key);
			cset(sd.flags, sizeof(sd.flags), d.flags);
			sd.combos_count = d.combos.size();
			if (sd.combos_count) {
				sd.combos = static_cast<AvatarCombo *>(std::calloc(sd.combos_count, sizeof(AvatarCombo)));
			}
			for (size_t k = 0; k < d.combos.size(); ++k) {
				const Combo &c = d.combos[k];
				AvatarCombo &sc = sd.combos[k];
				cset(sc.raw_id, sizeof(sc.raw_id), c.raw_id);
				sc.id = c.id;
				cset(sc.head_name, sizeof(sc.head_name), c.head_name);
				cset(sc.body_name, sizeof(sc.body_name), c.body_name);
				cset(sc.arms_name, sizeof(sc.arms_name), c.arms_name);
			}
		}
	}

	char *buf = nullptr;
	size_t n = 0;
	const int rc = avatars_write(&f, &buf, &n);
	avatars_free(&f); // frees the calloc'd arrays (raw_lines were NULL)
	if (rc != 0) {
		last_error = "avatars_write failed";
		return FAILED;
	}

	Ref<FileAccess> fa = FileAccess::open(path, FileAccess::WRITE);
	if (fa.is_null()) {
		avatars_free_buffer(buf);
		last_error = String("Cannot open for write: ") + path;
		return ERR_CANT_CREATE;
	}
	PackedByteArray out;
	out.resize(static_cast<int64_t>(n));
	if (n) {
		std::memcpy(out.ptrw(), buf, n);
	}
	fa->store_buffer(out);
	fa->close();
	avatars_free_buffer(buf);
	source_path = path;
	return OK;
}

void AvatarDatabase::create_empty() {
	clear();
	source_path = String();
	last_error = String();
	loaded = true;
	registry_dirty_ = true;
	emit_signal("changed");
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

Array AvatarDatabase::get_diagnostics() const {
	Array out;
	for (const Diagnostic &d : diagnostics) {
		out.push_back(diagnostic_dict(d));
	}
	return out;
}

int AvatarDatabase::get_part_count() const {
	return static_cast<int>(parts.size());
}

int AvatarDatabase::get_nationality_count() const {
	return static_cast<int>(nationalities.size());
}

Dictionary AvatarDatabase::part_dict(const Part &p) const {
	Dictionary d;
	d["kind"] = p.kind;
	d["name"] = p.name;
	d["display_name"] = p.display_name;
	d["graphic"] = p.graphic;
	d["graphic_j"] = p.graphic_j;
	d["graphic_s"] = p.graphic_s;
	Array camo;
	camo.push_back(p.camo[0]);
	camo.push_back(p.camo[1]);
	camo.push_back(p.camo[2]);
	d["camo"] = camo;
	d["voice"] = p.voice;
	d["sex"] = p.sex;
	return d;
}

Dictionary AvatarDatabase::diagnostic_dict(const Diagnostic &d) const {
	Dictionary out;
	out["line"] = d.line;
	out["severity"] = d.severity;
	out["code"] = d.code;
	out["message"] = d.message;
	return out;
}

PackedStringArray AvatarDatabase::get_part_names(int kind) const {
	std::vector<const Part *> sel;
	for (const Part &p : parts) {
		if (p.kind == kind) {
			sel.push_back(&p);
		}
	}
	std::sort(sel.begin(), sel.end(), [](const Part *a, const Part *b) {
		return a->name.naturalnocasecmp_to(b->name) < 0;
	});
	PackedStringArray out;
	out.resize(static_cast<int>(sel.size()));
	for (size_t i = 0; i < sel.size(); ++i) {
		out.set(static_cast<int>(i), sel[i]->name);
	}
	return out;
}

Dictionary AvatarDatabase::get_part(int kind, const String &name) const {
	const Part *p = find_part(kind, name);
	return p ? part_dict(*p) : Dictionary();
}

Dictionary AvatarDatabase::get_nationality(int nat_index) const {
	Dictionary out;
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return out;
	}
	const Nationality &n = nationalities[nat_index];
	out["raw_id"] = n.raw_id;
	out["id"] = n.id;
	out["name_key"] = n.name_key;
	out["flags"] = n.flags;
	out["alignment"] = n.alignment;
	out["has_alignment"] = n.has_alignment;
	out["division_count"] = static_cast<int>(n.divisions.size());
	return out;
}

int AvatarDatabase::get_division_count(int nat_index) const {
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return 0;
	}
	return static_cast<int>(nationalities[nat_index].divisions.size());
}

Dictionary AvatarDatabase::get_division(int nat_index, int div_index) const {
	Dictionary out;
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return out;
	}
	const Nationality &n = nationalities[nat_index];
	if (div_index < 0 || div_index >= static_cast<int>(n.divisions.size())) {
		return out;
	}
	const Division &d = n.divisions[div_index];
	out["raw_id"] = d.raw_id;
	out["id"] = d.id;
	out["name_key"] = d.name_key;
	out["flags"] = d.flags;
	out["combo_count"] = static_cast<int>(d.combos.size());
	return out;
}

int AvatarDatabase::get_combo_count(int nat_index, int div_index) const {
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return 0;
	}
	const Nationality &n = nationalities[nat_index];
	if (div_index < 0 || div_index >= static_cast<int>(n.divisions.size())) {
		return 0;
	}
	return static_cast<int>(n.divisions[div_index].combos.size());
}

Dictionary AvatarDatabase::get_combo(int nat_index, int div_index, int combo_index) const {
	Dictionary out;
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return out;
	}
	const Nationality &n = nationalities[nat_index];
	if (div_index < 0 || div_index >= static_cast<int>(n.divisions.size())) {
		return out;
	}
	const Division &d = n.divisions[div_index];
	if (combo_index < 0 || combo_index >= static_cast<int>(d.combos.size())) {
		return out;
	}
	const Combo &c = d.combos[combo_index];
	out["raw_id"] = c.raw_id;
	out["id"] = c.id;
	out["head_name"] = c.head_name;
	out["body_name"] = c.body_name;
	out["arms_name"] = c.arms_name;
	out["head"] = part_dict(c.head);
	out["body"] = part_dict(c.body);
	if (c.has_arms) {
		out["arms"] = part_dict(c.arms);
	}
	out["has_arms"] = c.has_arms;
	return out;
}

Dictionary AvatarDatabase::resolve_combo(int nat_index, int div_index, int combo_index) const {
	Dictionary out = get_combo(nat_index, div_index, combo_index);
	if (out.is_empty()) {
		return out;
	}
	// alignment comes from the owning nationality (D-PLAYERINFO copies it into the combo).
	const Dictionary nat = get_nationality(nat_index);
	if (!nat.is_empty()) {
		out["alignment"] = nat["alignment"];
	}
	return out;
}

const opennova::npruntime::CharacterRegistry &
AvatarDatabase::character_registry() const {
	if (registry_dirty_) {
		registry_ = opennova::npruntime::CharacterRegistry();
		for (int ni = 0; ni < static_cast<int>(nationalities.size()); ++ni) {
			const Nationality &nat = nationalities[ni];
			for (int di = 0; di < static_cast<int>(nat.divisions.size()); ++di) {
				const Division &div = nat.divisions[di];
				for (int ci = 0; ci < static_cast<int>(div.combos.size()); ++ci) {
					const Combo &combo = div.combos[ci];
					registry_.add_entry(ni, di, ci, nat.id, div.id, combo.id,
							nat.alignment, combo.head.voice,
							combo.head.sex == SEX_FEMALE);
				}
			}
		}
		registry_dirty_ = false;
	}
	return registry_;
}

Dictionary AvatarDatabase::resolve_character_id(
		int character_id, int expected_alignment) const {
	// The registry decode (npruntime/character_registry.h carries the
	// witness); the resolved combo dictionary is this seam's shape.
	const uint16_t packed = static_cast<uint16_t>(character_id & 0xffff);
	const opennova::npruntime::CharacterEntry *entry =
			character_registry().find_by_packed_id(packed, expected_alignment);
	if (entry == nullptr) {
		return Dictionary();
	}
	Dictionary out = resolve_combo(entry->nationality_index,
			entry->division_index, entry->combo_index);
	out["character_id"] = static_cast<int>(packed);
	out["nationality_index"] = entry->nationality_index;
	out["division_index"] = entry->division_index;
	out["combo_index"] = entry->combo_index;
	return out;
}

int AvatarDatabase::first_character_id(int alignment) const {
	return character_registry().first_character_id(alignment);
}

Dictionary AvatarDatabase::character_join_profile(
		const Dictionary &p_selection) const {
	opennova::npruntime::JoinSideSelection saved[2];
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
				opennova::npruntime::kJoinDefaultPlayerClass);
	}
	const opennova::npruntime::JoinCharacterProfile profile =
			opennova::npruntime::join_character_profile(character_registry(), saved);
	Array ids;
	ids.push_back(static_cast<int>(profile.character_ids[0]));
	ids.push_back(static_cast<int>(profile.character_ids[1]));
	Array classes;
	classes.push_back(profile.player_classes[0]);
	classes.push_back(profile.player_classes[1]);
	Array avatars;
	avatars.push_back(profile.avatars[0]);
	avatars.push_back(profile.avatars[1]);
	Dictionary out;
	out["character_ids"] = ids;
	out["player_classes"] = classes;
	out["avatars"] = avatars;
	out["team_request"] = profile.team_request;
	return out;
}

std::vector<AvatarDatabase::CharacterSexRow>
AvatarDatabase::character_sex_rows() const {
	// File order, duplicate packed ids first-wins (the registry's walk order).
	std::vector<CharacterSexRow> rows;
	for (const opennova::npruntime::CharacterEntry &entry :
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

Dictionary AvatarDatabase::get_model() const {
	Dictionary m;
	Array parr;
	for (const Part &p : parts) {
		parr.push_back(part_dict(p));
	}
	m["parts"] = parr;
	Array narr;
	for (const Nationality &n : nationalities) {
		Dictionary nd;
		nd["raw_id"] = n.raw_id;
		nd["id"] = n.id;
		nd["name_key"] = n.name_key;
		nd["flags"] = n.flags;
		nd["alignment"] = n.alignment;
		nd["has_alignment"] = n.has_alignment;
		Array darr;
		for (const Division &d : n.divisions) {
			Dictionary dd;
			dd["raw_id"] = d.raw_id;
			dd["id"] = d.id;
			dd["name_key"] = d.name_key;
			dd["flags"] = d.flags;
			Array carr;
			for (const Combo &c : d.combos) {
				Dictionary cd;
				cd["raw_id"] = c.raw_id;
				cd["id"] = c.id;
				cd["head_name"] = c.head_name;
				cd["body_name"] = c.body_name;
				cd["arms_name"] = c.arms_name;
				cd["head"] = part_dict(c.head);
				cd["body"] = part_dict(c.body);
				if (c.has_arms) {
					cd["arms"] = part_dict(c.arms);
				}
				cd["has_arms"] = c.has_arms;
				carr.push_back(cd);
			}
			dd["combos"] = carr;
			darr.push_back(dd);
		}
		nd["divisions"] = darr;
		narr.push_back(nd);
	}
	m["nationalities"] = narr;
	return m;
}

void AvatarDatabase::set_model(const Dictionary &model) {
	clear();
	const Array parr = model.has("parts") ? (Array)model["parts"] : Array();
	for (int i = 0; i < parr.size(); ++i) {
		const Dictionary pd = parr[i];
		Part p;
		p.kind = dict_int(pd, "kind", PART_HEAD);
		p.name = dict_str(pd, "name");
		p.display_name = dict_str(pd, "display_name");
		p.graphic = dict_str(pd, "graphic");
		p.graphic_j = dict_str(pd, "graphic_j");
		p.graphic_s = dict_str(pd, "graphic_s");
		const Array camo = pd.has("camo") ? (Array)pd["camo"] : Array();
		for (int c = 0; c < 3 && c < camo.size(); ++c) {
			p.camo[c] = (int)camo[c];
		}
		p.voice = dict_int(pd, "voice", 0);
		p.sex = dict_int(pd, "sex", SEX_MALE);
		parts.push_back(p);
	}
	const Array narr = model.has("nationalities") ? (Array)model["nationalities"] : Array();
	for (int i = 0; i < narr.size(); ++i) {
		const Dictionary nd = narr[i];
		Nationality n;
		n.raw_id = dict_str(nd, "raw_id");
		n.id = dict_int(nd, "id", 0);
		n.name_key = dict_str(nd, "name_key");
		n.flags = dict_str(nd, "flags");
		n.alignment = dict_int(nd, "alignment", ALIGN_GOOD);
		n.has_alignment = nd.has("has_alignment") ? (bool)nd["has_alignment"] : false;
		const Array darr = nd.has("divisions") ? (Array)nd["divisions"] : Array();
		for (int j = 0; j < darr.size(); ++j) {
			const Dictionary dd = darr[j];
			Division d;
			d.raw_id = dict_str(dd, "raw_id");
			d.id = dict_int(dd, "id", 0);
			d.name_key = dict_str(dd, "name_key");
			d.flags = dict_str(dd, "flags");
			const Array carr = dd.has("combos") ? (Array)dd["combos"] : Array();
			for (int k = 0; k < carr.size(); ++k) {
				const Dictionary cd = carr[k];
				Combo c;
				c.raw_id = dict_str(cd, "raw_id");
				c.id = dict_int(cd, "id", 0);
				c.head_name = dict_str(cd, "head_name");
				c.body_name = dict_str(cd, "body_name");
				c.arms_name = dict_str(cd, "arms_name");
				resolve_combo_snapshots(c);
				d.combos.push_back(c);
			}
			n.divisions.push_back(std::move(d));
		}
		nationalities.push_back(std::move(n));
	}
	loaded = true;
	registry_dirty_ = true;
	emit_signal("changed");
}
