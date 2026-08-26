// ObjectData — the document: open/save/export lifecycle for .3di/.3dp/.ase
// sources, the OED session behind project-backed documents, and the retail
// network-challenge model registry fed by mounted .3DI loads. The class spans
// several TUs; see nova_object_data_internal.h for the map.
#include "object/nova_object_data_internal.h"

#include <cstdlib>
#include <unordered_set>
#include <vector>

using namespace novaobj;

namespace {

// Retail's C2S 0x3D producer is a renderer-side cache of unique loaded .3DI
// definitions, not the live entity pool. The normal mission path destroys this
// cache, loads entity/celestial/HUD definitions, then freezes a non-foliage
// snapshot in sub_5B3A80. Keep the load registry here, at the one production
// boundary every mounted .3DI crosses. The resource epoch prevents names from a
// previous mount/rescan leaking into a later renderer generation.
struct NetworkChallengeModelRegistry {
	int64_t epoch = -1;
	std::unordered_set<std::string> loaded;
	std::unordered_set<std::string> foliage;
};

NetworkChallengeModelRegistry &network_challenge_model_registry() {
	static NetworkChallengeModelRegistry registry;
	const int64_t epoch = ResourceRoot::cache_epoch();
	if (registry.epoch != epoch) {
		registry.epoch = epoch;
		registry.loaded.clear();
		registry.foliage.clear();
	}
	return registry;
}

std::string network_challenge_model_key(const String &name) {
	return std::string(name.get_file().to_lower().utf8().get_data());
}

void register_network_challenge_model(const String &name, bool include) {
	const std::string key = network_challenge_model_key(name);
	if (key.empty()) return;
	NetworkChallengeModelRegistry &registry = network_challenge_model_registry();
	registry.loaded.insert(key);
	// Retail's foliage mark is sticky on the shared model-def node. A definition
	// loaded through both paths therefore remains excluded until the cache reset.
	if (!include) registry.foliage.insert(key);
}

String oed_error_detail(OedSession *session, const char *fallback) {
	const char *detail = session != nullptr ? oed_session_last_error(session) : nullptr;
	if (detail != nullptr && detail[0] != '\0') {
		return String(fallback) + ": " + from_native(detail);
	}
	return String(fallback);
}

String filename_stem(const String &path) {
	const String stem = path.get_file().get_basename();
	return stem.is_empty() ? String("untitled") : stem;
}

String sanitized_basename(const String &value) {
	String name = value.strip_edges();
	if (name.is_empty()) {
		name = "untitled";
	}
	const String ext = name.get_extension().to_lower();
	if (ext == "3di" || ext == "3dp" || ext == "ase") {
		name = name.get_basename();
	}
	name = name.replace(" ", "_").replace("/", "_").replace("\\", "_").replace(":", "_");
	return name;
}

bool copy_scene_file_to_project_dir(const std::filesystem::path &source_path,
		const std::filesystem::path &dest_path, std::string &error) {
	std::error_code ec;
	if (!std::filesystem::exists(source_path, ec)) {
		error = "missing scene source " + source_path.string();
		return false;
	}

	ec.clear();
	if (std::filesystem::exists(dest_path, ec)) {
		ec.clear();
		if (std::filesystem::equivalent(source_path, dest_path, ec)) {
			return true;
		}
	}

	const std::filesystem::path parent = dest_path.parent_path();
	if (!parent.empty()) {
		ec.clear();
		std::filesystem::create_directories(parent, ec);
		if (ec) {
			error = "could not create scene directory " + parent.string() + ": " + ec.message();
			return false;
		}
	}

	ec.clear();
	std::filesystem::copy_file(source_path, dest_path, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "could not copy " + source_path.string() + " to " + dest_path.string() + ": " + ec.message();
		return false;
	}
	return true;
}

bool copy_project_scene_sources_to_dir(const TdpProject &project, const String &source_dir,
		const String &dest_dir, std::string &error) {
	if (source_dir.is_empty() || dest_dir.is_empty()) {
		return true;
	}

	const std::filesystem::path dest_root(to_native_path(dest_dir));
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		const char *scene_file = project.lods[i].scene_file;
		if (scene_file[0] == '\0') {
			break;
		}

		std::filesystem::path scene_rel(scene_file);
		const std::filesystem::path source_path = scene_rel.is_absolute() ?
				scene_rel :
				std::filesystem::path(resolve_relative_file(source_dir, scene_file));
		const std::filesystem::path dest_path = scene_rel.is_absolute() ?
				dest_root / scene_rel.filename() :
				dest_root / scene_rel;
		if (!copy_scene_file_to_project_dir(source_path, dest_path, error)) {
			return false;
		}
	}
	return true;
}

bool same_directory(const String &a, const String &b) {
	if (a == b) {
		return true;
	}
	std::error_code ec;
	const std::filesystem::path pa(to_native_path(a));
	const std::filesystem::path pb(to_native_path(b));
	if (std::filesystem::exists(pa, ec) && std::filesystem::exists(pb, ec)) {
		ec.clear();
		return std::filesystem::equivalent(pa, pb, ec);
	}
	return false;
}

void set_default_project_lod_fields(TdpLod &lod) {
	if (lod.attributes == 0) {
		lod.attributes = 5;
	}
	if (lod.render_function[0] == '\0') {
		copy_cstr(lod.render_function, sizeof(lod.render_function), "gnrc");
	}
}

void copy_lod_binding(TdpLod &dst, const TdpLod &src) {
	if (src.scene_file[0] != '\0') {
		copy_cstr(dst.scene_file, sizeof(dst.scene_file), src.scene_file);
	}
	if (src.attributes != 0) {
		dst.attributes = src.attributes;
	}
	if (src.render_function[0] != '\0') {
		copy_cstr(dst.render_function, sizeof(dst.render_function), src.render_function);
	}
}

} // namespace

ObjectData::ObjectData() {
	tdp_init(&source_project);
}

ObjectData::~ObjectData() {
	_clear();
}

void ObjectData::_clear_oed_session() {
	if (oed_session != nullptr) {
		oed_session_destroy(oed_session);
		oed_session = nullptr;
	}
}

void ObjectData::_clear_source_model() {
	// Safe on an empty document too: reset_empty leaves a zeroed model whose
	// pointers are all null, so the free is a no-op and the memset clears the
	// header name it stamped.
	threedi_3di3_free(&source_model);
	std::memset(&source_model, 0, sizeof(source_model));
	has_source_model = false;
}

void ObjectData::_clear_source_project() {
	if (has_source_project) {
		tdp_free(&source_project);
		tdp_init(&source_project);
		has_source_project = false;
	}
}

void ObjectData::_clear() {
	_clear_oed_session();
	_clear_source_model();
	_clear_source_project();
	submesh_cache.clear();
	_invalidate_panm_cache();
	_invalidate_runtime_control_names();
	source_kind = SourceKind::Empty;
	source_path = String();
	source_dir = String();
	resource_root.unref();
	object_name = "untitled";
	last_error = String();
	oed_dirty_mask = UPDATE_NONE;
	last_oed_update_mask = UPDATE_NONE;
}

void ObjectData::_mark_oed_dirty(uint8_t p_update_mask) {
	oed_dirty_mask |= (p_update_mask & UPDATE_ALL);
}

void ObjectData::_clear_oed_dirty(uint8_t p_update_mask) {
	const uint8_t update_mask = p_update_mask & UPDATE_ALL;
	if (update_mask == UPDATE_NONE || update_mask == UPDATE_ALL) {
		oed_dirty_mask = UPDATE_NONE;
		return;
	}
	oed_dirty_mask &= static_cast<uint8_t>(~update_mask) & UPDATE_ALL;
}

uint8_t ObjectData::_normalize_oed_update_mask(int p_update_mask) const {
	uint8_t update_mask = static_cast<uint8_t>(p_update_mask) & UPDATE_ALL;
	if (update_mask == UPDATE_NONE) {
		update_mask = oed_dirty_mask & UPDATE_ALL;
	}
	if (update_mask == UPDATE_NONE) {
		update_mask = UPDATE_ALL;
	}
	return update_mask;
}

std::atomic<uint64_t> ObjectData::global_change_counter_{0};

void ObjectData::_notify_object_changed(uint8_t p_update_mask) {
	// Every document mutation (all OED setters, opens, LOD/scene swaps) funnels
	// through here or _clear() — the memoized submesh builds die with the data
	// they were built from, and the PANM frame cache re-arms a full re-apply.
	submesh_cache.clear();
	_invalidate_panm_cache();
	_invalidate_runtime_control_names();
	++change_revision_;
	if (change_revision_ == 0) ++change_revision_;
	global_change_counter_.fetch_add(1, std::memory_order_relaxed);
	last_oed_update_mask = p_update_mask & UPDATE_ALL;
	_mark_oed_dirty(p_update_mask);
	emit_signal("object_changed");
	emit_changed();
}

void ObjectData::reset_empty(const String &p_name) {
	_clear();
	object_name = sanitized_basename(p_name);
	// An empty document is a zeroed model carrying only its name; every count
	// is zero, so all views read empty.
	copy_cstr(source_model.header.name, sizeof(source_model.header.name),
			to_std(object_name).c_str());
	has_source_model = true;
	_notify_object_changed();
}

Error ObjectData::set_lod_scene(int p_lod_index, const String &p_path) {
	if (p_path.is_empty() || p_lod_index < -1 || p_lod_index >= TDP_MAX_LODS) {
		return ERR_INVALID_PARAMETER;
	}
	if (source_kind == SourceKind::Threedi) {
		last_error = "3DI documents cannot bind project LOD scenes";
		return ERR_UNAVAILABLE;
	}

	const std::filesystem::path native_path(to_native_path(p_path));
	std::error_code ec;
	if (!std::filesystem::exists(native_path, ec)) {
		last_error = "LOD scene does not exist: " + p_path;
		return ERR_FILE_NOT_FOUND;
	}

	if (!has_source_project) {
		tdp_init(&source_project);
		has_source_project = true;
	}

	const int current_count = project_lod_count(source_project);
	const int lod_index = p_lod_index < 0 ? current_count : p_lod_index;
	if (lod_index < 0 || lod_index >= TDP_MAX_LODS || lod_index > current_count) {
		last_error = "LOD scenes must be added without gaps";
		return ERR_INVALID_PARAMETER;
	}

	const String scene_dir = p_path.get_base_dir();
	if (source_dir.is_empty() || current_count == 0) {
		source_dir = scene_dir;
	} else if (!same_directory(source_dir, scene_dir)) {
		last_error = "LOD scenes must be in the same source directory";
		return ERR_INVALID_PARAMETER;
	}

	TdpLod &lod = source_project.lods[lod_index];
	copy_cstr(lod.scene_file, sizeof(lod.scene_file), to_std(p_path.get_file()).c_str());
	set_default_project_lod_fields(lod);
	if (lod_index == 0 && (object_name.is_empty() || object_name == "untitled")) {
		object_name = filename_stem(p_path);
	}
	if (source_path.is_empty()) {
		source_path = p_path;
	}

	return _rebuild_oed_session_from_project(UPDATE_ALL);
}

Error ObjectData::open_file(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	if (ext == "3di") {
		return _open_3di(p_path);
	}
	if (ext == "3dp") {
		return _open_3dp(p_path);
	}
	if (ext == "ase") {
		return _open_ase(p_path);
	}
	last_error = "Unsupported object source: " + ext;
	return ERR_FILE_UNRECOGNIZED;
}

Error ObjectData::open_from_resource_root(
		const Ref<ResourceRoot> &p_resource_root, const String &p_name,
		bool p_include_in_network_challenge) {
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.get_extension().to_lower() != "3di") {
		last_error = "Only mounted .3di object files are supported";
		return ERR_FILE_UNRECOGNIZED;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		last_error = "Object file not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}

	const Error err = _open_3di_bytes(file, bytes);
	if (err == OK) {
		resource_root = p_resource_root;
		source_dir = p_resource_root->get_root_dir();
		register_network_challenge_model(
				file, p_include_in_network_challenge);
		_notify_object_changed();
	}
	return err;
}

void ObjectData::mark_cached_network_challenge_foliage_model(
		const String &p_name) {
	register_network_challenge_model(p_name, false);
}

void ObjectData::reset_network_challenge_model_registry() {
	NetworkChallengeModelRegistry &registry =
			network_challenge_model_registry();
	registry.loaded.clear();
	registry.foliage.clear();
}

int64_t ObjectData::network_challenge_model_count() {
	const NetworkChallengeModelRegistry &registry =
			network_challenge_model_registry();
	std::size_t included = 0;
	for (const std::string &name : registry.loaded) {
		if (registry.foliage.find(name) == registry.foliage.end()) ++included;
	}
	return static_cast<int64_t>(included);
}

Error ObjectData::_open_3di(const String &p_path) {
	_clear();
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_read(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to read 3DI";
		return ERR_FILE_CANT_READ;
	}
	has_source_model = true;
	source_kind = SourceKind::Threedi;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = source_model.header.name[0] != '\0'
			? from_native(source_model.header.name)
			: filename_stem(p_path);
	_notify_object_changed();
	return OK;
}

Error ObjectData::_open_3di_bytes(const String &p_name, const PackedByteArray &p_bytes) {
	_clear();
	if (p_bytes.is_empty()) {
		last_error = "Mounted 3DI entry is empty";
		return ERR_FILE_CANT_READ;
	}
	if (threedi_3di3_read_memory(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()), &source_model) != 0) {
		last_error = "Failed to read mounted 3DI";
		return ERR_FILE_CANT_READ;
	}
	has_source_model = true;
	source_kind = SourceKind::Threedi;
	source_path = p_name.get_file();
	source_dir = String();
	object_name = source_model.header.name[0] != '\0'
			? from_native(source_model.header.name)
			: filename_stem(p_name);
	return OK;
}

Error ObjectData::_open_3dp(const String &p_path) {
	_clear();
	const std::string native_path = to_native_path(p_path);
	if (tdp_parse(native_path.c_str(), &source_project) != 0) {
		last_error = "Failed to parse 3DP";
		return ERR_FILE_CANT_READ;
	}
	has_source_project = true;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = filename_stem(p_path);

	std::vector<std::string> ase_paths;
	std::vector<const char *> ase_ptrs;
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (source_project.lods[i].scene_file[0] == '\0') {
			break;
		}
		ase_paths.push_back(resolve_relative_file(source_dir, source_project.lods[i].scene_file));
		ase_ptrs.push_back(ase_paths.back().c_str());
	}
	if (ase_ptrs.empty()) {
		last_error = "3DP has no render LOD ASE files";
		_clear();
		return ERR_FILE_CORRUPT;
	}

	const OedStatus create_rc = oed_session_create(
			ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &source_project, &oed_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session";
		_clear();
		return ERR_FILE_CANT_READ;
	}

	source_kind = SourceKind::Project;
	return _build_model_from_project_session(nullptr);
}

Error ObjectData::_open_ase(const String &p_path) {
	_clear();
	tdp_init(&source_project);
	copy_cstr(source_project.lods[0].scene_file, sizeof(source_project.lods[0].scene_file),
			to_std(p_path.get_file()).c_str());
	source_project.lods[0].attributes = 5;
	copy_cstr(source_project.lods[0].render_function, sizeof(source_project.lods[0].render_function), "gnrc");
	source_project.lods[0].threshold = 0.0f;
	source_project.poly_collision_lod = 0;
	has_source_project = true;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = filename_stem(p_path);

	const std::string native_path = to_native_path(p_path);
	const char *ase_ptr = native_path.c_str();
	const OedStatus create_rc = oed_session_create(&ase_ptr, 1, &source_project, &oed_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session from ASE";
		_clear();
		return ERR_FILE_CANT_READ;
	}

	source_kind = SourceKind::Ase;
	_seed_project_materials_from_ase_session();
	return _build_model_from_project_session(nullptr);
}

void ObjectData::_seed_project_materials_from_ase_session() {
	// A bare .ase carries its materials only in the scene (`Material_<i>_<shader>` names +
	// diffuse bitmaps). The OED session seeds its material table from those at conversion,
	// but every later build/export re-seeds that table from the PROJECT (OED_UPDATE_MTRL),
	// and a project born from a bare .ase has no material list yet -- the ASE materials
	// would be dropped and the object would compile with an empty MTRL chunk (untextured).
	// The original OED populates the project's material list from the ASE at import; do the
	// same: build once WITHOUT the MTRL re-seed and adopt the ASE-seeded materials.
	if (oed_session == nullptr || source_project.material_count > 0) {
		return;
	}
	Threedi3di3 seeded = {};
	const OedStatus build_rc = oed_session_build_model(
			oed_session, &source_project, static_cast<uint8_t>(OED_UPDATE_LGHT | OED_UPDATE_PANM),
			nullptr, &seeded);
	if (build_rc != OED_STATUS_OK) {
		return;
	}
	TdpProject seeded_project = {};
	tdp_init(&seeded_project);
	if (tdp_from_3di(&seeded, &seeded_project) == 0 && seeded_project.material_count > 0) {
		tdp_alloc_materials(&source_project, seeded_project.material_count);
		for (size_t i = 0; i < source_project.material_count; ++i) {
			source_project.materials[i] = seeded_project.materials[i];
		}
	}
	tdp_free(&seeded_project);
	threedi_3di3_free(&seeded);
}

Error ObjectData::_build_model_from_project_session(const char *p_model_name, uint8_t p_dirty_mask) {
	Threedi3di3 built_model = {};
	const OedStatus build_rc = oed_session_build_model(
			oed_session, &source_project, static_cast<uint8_t>(OED_UPDATE_ALL), p_model_name, &built_model);
	if (build_rc != OED_STATUS_OK) {
		last_error = oed_error_detail(oed_session, "Failed to build OED model");
		return ERR_FILE_CANT_READ;
	}

	// The OED-built model IS the document for project/ASE sources.
	_clear_source_model();
	source_model = built_model;
	has_source_model = true;
	_notify_object_changed(p_dirty_mask);
	return OK;
}

Error ObjectData::_rebuild_oed_session_from_project(uint8_t p_dirty_mask) {
	if (!has_source_project) {
		last_error = "Object has no source project";
		return ERR_UNCONFIGURED;
	}

	std::vector<std::string> ase_paths;
	std::vector<const char *> ase_ptrs;
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (source_project.lods[i].scene_file[0] == '\0') {
			break;
		}
		ase_paths.push_back(resolve_relative_file(source_dir, source_project.lods[i].scene_file));
		ase_ptrs.push_back(ase_paths.back().c_str());
	}
	if (ase_ptrs.empty()) {
		_clear_oed_session();
		last_error = "Object project has no render LOD ASE files";
		return ERR_UNCONFIGURED;
	}

	OedSession *next_session = nullptr;
	const OedStatus create_rc = oed_session_create(
			ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &source_project, &next_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session";
		return ERR_FILE_CANT_READ;
	}

	_clear_oed_session();
	oed_session = next_session;
	source_kind = SourceKind::Project;
	return _build_model_from_project_session(nullptr, p_dirty_mask);
}

TdpProject ObjectData::_build_project_from_model() const {
	TdpProject out = {};
	tdp_from_3di(&source_model, &out);
	if (has_source_project) {
		for (int i = 0; i < TDP_MAX_LODS; ++i) {
			copy_lod_binding(out.lods[i], source_project.lods[i]);
		}
		out.poly_collision_lod = source_project.poly_collision_lod;
	}
	return out;
}

Error ObjectData::save_project_to_dir(const String &p_dir_path) {
	if (!has_source_model || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	TdpProject project = _build_project_from_model();
	const String output_path = p_dir_path.path_join(_export_basename() + ".3dp");
	std::string scene_copy_error;
	if (has_source_project && !copy_project_scene_sources_to_dir(project, source_dir, p_dir_path, scene_copy_error)) {
		tdp_free(&project);
		last_error = "Failed to copy 3DP scene source: " + from_native(scene_copy_error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	const std::string native_path = to_native_path(output_path);
	const int rc = tdp_write(native_path.c_str(), &project);
	tdp_free(&project);
	if (rc != 0) {
		last_error = "Failed to write 3DP";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

Error ObjectData::export_3di_to_dir(const String &p_dir_path, int p_update_mask) {
	if (!has_source_model || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const String output_path = p_dir_path.path_join(_export_basename() + ".3di");
	if (source_kind == SourceKind::Threedi) {
		const Error err = _export_patched_3di(output_path);
		if (err == OK) {
			_clear_oed_dirty(UPDATE_ALL);
		}
		return err;
	}
	const uint8_t update_mask = _normalize_oed_update_mask(p_update_mask);
	return _export_project_backed_3di(output_path, update_mask);
}

Error ObjectData::_export_project_backed_3di(const String &p_path, uint8_t p_update_mask) {
	if (oed_session == nullptr) {
		last_error = "Object has no OED geometry session";
		return ERR_UNCONFIGURED;
	}

	TdpProject export_project = _build_project_from_model();
	const std::string native_path = to_native_path(p_path);
	OedExportRequest request = {};
	request.project = &export_project;
	request.output_path = native_path.c_str();
	request.update_mask = p_update_mask;
	const OedStatus rc = oed_session_export(oed_session, &request);
	tdp_free(&export_project);
	if (rc != OED_STATUS_OK) {
		last_error = oed_error_detail(oed_session, "Failed to export 3DI");
		return ERR_FILE_CANT_WRITE;
	}
	_clear_oed_dirty(p_update_mask);
	return OK;
}

Error ObjectData::_export_patched_3di(const String &p_path) {
	if (!has_source_model) {
		return ERR_UNCONFIGURED;
	}
	// The document IS the edited model; write it back out directly.
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_write(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to write patched 3DI";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

String ObjectData::_export_basename() const {
	return sanitized_basename(object_name);
}

String ObjectData::_source_kind_name() const {
	switch (source_kind) {
		case SourceKind::Threedi:
			return "3di";
		case SourceKind::Project:
			return "3dp";
		case SourceKind::Ase:
			return "ase";
		case SourceKind::Empty:
		default:
			return "empty";
	}
}
