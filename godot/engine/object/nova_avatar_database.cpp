#include "nova_avatar_database.h"

#include "resource_index/nova_resource_root.h"
#include "util/nova_data_format.h"

#include <avatars/avatars.h>

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

void NovaAvatarDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaAvatarDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &NovaAvatarDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &NovaAvatarDatabase::save_to_path);
	ClassDB::bind_method(D_METHOD("create_empty"), &NovaAvatarDatabase::create_empty);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaAvatarDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaAvatarDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaAvatarDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_diagnostics"), &NovaAvatarDatabase::get_diagnostics);
	ClassDB::bind_method(D_METHOD("get_part_count"), &NovaAvatarDatabase::get_part_count);
	ClassDB::bind_method(D_METHOD("get_nationality_count"), &NovaAvatarDatabase::get_nationality_count);
	ClassDB::bind_method(D_METHOD("get_part_names", "kind"), &NovaAvatarDatabase::get_part_names);
	ClassDB::bind_method(D_METHOD("get_parts", "kind"), &NovaAvatarDatabase::get_parts);
	ClassDB::bind_method(D_METHOD("get_part", "kind", "name"), &NovaAvatarDatabase::get_part);
	ClassDB::bind_method(D_METHOD("get_nationality", "nat_index"), &NovaAvatarDatabase::get_nationality);
	ClassDB::bind_method(D_METHOD("get_division_count", "nat_index"), &NovaAvatarDatabase::get_division_count);
	ClassDB::bind_method(D_METHOD("get_division", "nat_index", "div_index"), &NovaAvatarDatabase::get_division);
	ClassDB::bind_method(D_METHOD("get_combo_count", "nat_index", "div_index"), &NovaAvatarDatabase::get_combo_count);
	ClassDB::bind_method(D_METHOD("get_combo", "nat_index", "div_index", "combo_index"), &NovaAvatarDatabase::get_combo);
	ClassDB::bind_method(D_METHOD("resolve_combo", "nat_index", "div_index", "combo_index"), &NovaAvatarDatabase::resolve_combo);
	ClassDB::bind_method(D_METHOD("get_model"), &NovaAvatarDatabase::get_model);
	ClassDB::bind_method(D_METHOD("set_model", "model"), &NovaAvatarDatabase::set_model);

	BIND_CONSTANT(PART_HEAD);
	BIND_CONSTANT(PART_BODY);
	BIND_CONSTANT(PART_ARMS);
	BIND_CONSTANT(SEX_MALE);
	BIND_CONSTANT(SEX_FEMALE);
	BIND_CONSTANT(ALIGN_GOOD);
	BIND_CONSTANT(ALIGN_EVIL);
	BIND_CONSTANT(DIAG_WARNING);
	BIND_CONSTANT(DIAG_ERROR);

	ADD_SIGNAL(MethodInfo("changed"));
}

void NovaAvatarDatabase::clear() {
	parts.clear();
	nationalities.clear();
	diagnostics.clear();
	loaded = false;
}

void NovaAvatarDatabase::adopt_parsed(const void *avatars_file) {
	const AvatarsFile *f = static_cast<const AvatarsFile *>(avatars_file);
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
				c.head = part_from_snapshot(&sc.head);
				c.body = part_from_snapshot(&sc.body);
				c.arms = part_from_snapshot(&sc.arms);
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
	loaded = true;
}

const NovaAvatarDatabase::Part *NovaAvatarDatabase::find_part(int kind, const String &name) const {
	for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
		const Part &p = *it;
		if (p.kind == kind && p.name.nocasecmp_to(name) == 0) {
			return &p;
		}
	}
	return nullptr;
}

NovaAvatarDatabase::Part NovaAvatarDatabase::part_from_snapshot(const void *avatar_part_snapshot) const {
	const AvatarPartSnapshot *sp = static_cast<const AvatarPartSnapshot *>(avatar_part_snapshot);
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

void NovaAvatarDatabase::resolve_combo_snapshots(Combo &combo) {
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

Error NovaAvatarDatabase::load(const String &path) {
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
	adopt_parsed(&file);
	avatars_free(&file);
	emit_signal("changed");
	return OK;
}

Error NovaAvatarDatabase::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
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
	adopt_parsed(&file);
	avatars_free(&file);
	source_path = file_name;
	emit_signal("changed");
	return OK;
}

Error NovaAvatarDatabase::save_to_path(const String &path) {
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

void NovaAvatarDatabase::create_empty() {
	clear();
	source_path = String();
	last_error = String();
	loaded = true;
	emit_signal("changed");
}

bool NovaAvatarDatabase::is_loaded() const {
	return loaded;
}

String NovaAvatarDatabase::get_source_path() const {
	return source_path;
}

String NovaAvatarDatabase::get_last_error() const {
	return last_error;
}

Array NovaAvatarDatabase::get_diagnostics() const {
	Array out;
	for (const Diagnostic &d : diagnostics) {
		out.push_back(diagnostic_dict(d));
	}
	return out;
}

int NovaAvatarDatabase::get_part_count() const {
	return static_cast<int>(parts.size());
}

int NovaAvatarDatabase::get_nationality_count() const {
	return static_cast<int>(nationalities.size());
}

Dictionary NovaAvatarDatabase::part_dict(const Part &p) const {
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

Dictionary NovaAvatarDatabase::diagnostic_dict(const Diagnostic &d) const {
	Dictionary out;
	out["line"] = d.line;
	out["severity"] = d.severity;
	out["code"] = d.code;
	out["message"] = d.message;
	return out;
}

PackedStringArray NovaAvatarDatabase::get_part_names(int kind) const {
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

Array NovaAvatarDatabase::get_parts(int kind) const {
	std::vector<const Part *> sel;
	for (const Part &p : parts) {
		if (p.kind == kind) {
			sel.push_back(&p);
		}
	}
	std::sort(sel.begin(), sel.end(), [](const Part *a, const Part *b) {
		return a->name.naturalnocasecmp_to(b->name) < 0;
	});
	Array out;
	for (const Part *p : sel) {
		out.push_back(part_dict(*p));
	}
	return out;
}

Dictionary NovaAvatarDatabase::get_part(int kind, const String &name) const {
	const Part *p = find_part(kind, name);
	return p ? part_dict(*p) : Dictionary();
}

Dictionary NovaAvatarDatabase::get_nationality(int nat_index) const {
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

int NovaAvatarDatabase::get_division_count(int nat_index) const {
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return 0;
	}
	return static_cast<int>(nationalities[nat_index].divisions.size());
}

Dictionary NovaAvatarDatabase::get_division(int nat_index, int div_index) const {
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

int NovaAvatarDatabase::get_combo_count(int nat_index, int div_index) const {
	if (nat_index < 0 || nat_index >= static_cast<int>(nationalities.size())) {
		return 0;
	}
	const Nationality &n = nationalities[nat_index];
	if (div_index < 0 || div_index >= static_cast<int>(n.divisions.size())) {
		return 0;
	}
	return static_cast<int>(n.divisions[div_index].combos.size());
}

Dictionary NovaAvatarDatabase::get_combo(int nat_index, int div_index, int combo_index) const {
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

Dictionary NovaAvatarDatabase::resolve_combo(int nat_index, int div_index, int combo_index) const {
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

Dictionary NovaAvatarDatabase::get_model() const {
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

void NovaAvatarDatabase::set_model(const Dictionary &model) {
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
	emit_signal("changed");
}
