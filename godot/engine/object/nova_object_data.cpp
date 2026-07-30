// NovaObjectData — the document: open/save/export lifecycle for .3di/.3dp/.ase
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
	const int64_t epoch = NovaResourceRoot::cache_epoch();
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

NovaObjectData::NovaObjectData() {
	threedi_ir_init(&ir);
	tdp_init(&source_project);
}

NovaObjectData::~NovaObjectData() {
	_clear();
}

void NovaObjectData::_clear_oed_session() {
	if (oed_session != nullptr) {
		oed_session_destroy(oed_session);
		oed_session = nullptr;
	}
}

void NovaObjectData::_clear_source_model() {
	if (has_source_model) {
		threedi_3di3_free(&source_model);
		std::memset(&source_model, 0, sizeof(source_model));
		has_source_model = false;
	}
}

void NovaObjectData::_clear_source_project() {
	if (has_source_project) {
		tdp_free(&source_project);
		tdp_init(&source_project);
		has_source_project = false;
	}
}

void NovaObjectData::_clear() {
	_clear_oed_session();
	_clear_source_model();
	_clear_source_project();
	submesh_cache.clear();
	_invalidate_panm_cache();
	threedi_ir_free(&ir);
	threedi_ir_init(&ir);
	has_ir = false;
	source_kind = SourceKind::Empty;
	source_path = String();
	source_dir = String();
	resource_root.unref();
	object_name = "untitled";
	last_error = String();
	oed_dirty_mask = UPDATE_NONE;
	last_oed_update_mask = UPDATE_NONE;
}

void NovaObjectData::_mark_oed_dirty(uint8_t p_update_mask) {
	oed_dirty_mask |= (p_update_mask & UPDATE_ALL);
}

void NovaObjectData::_clear_oed_dirty(uint8_t p_update_mask) {
	const uint8_t update_mask = p_update_mask & UPDATE_ALL;
	if (update_mask == UPDATE_NONE || update_mask == UPDATE_ALL) {
		oed_dirty_mask = UPDATE_NONE;
		return;
	}
	oed_dirty_mask &= static_cast<uint8_t>(~update_mask) & UPDATE_ALL;
}

uint8_t NovaObjectData::_normalize_oed_update_mask(int p_update_mask) const {
	uint8_t update_mask = static_cast<uint8_t>(p_update_mask) & UPDATE_ALL;
	if (update_mask == UPDATE_NONE) {
		update_mask = oed_dirty_mask & UPDATE_ALL;
	}
	if (update_mask == UPDATE_NONE) {
		update_mask = UPDATE_ALL;
	}
	return update_mask;
}

void NovaObjectData::_notify_object_changed(uint8_t p_update_mask) {
	// Every document mutation (all OED setters, opens, LOD/scene swaps) funnels
	// through here or _clear() — the memoized submesh builds die with the data
	// they were built from, and the PANM frame cache re-arms a full re-apply.
	submesh_cache.clear();
	_invalidate_panm_cache();
	last_oed_update_mask = p_update_mask & UPDATE_ALL;
	_mark_oed_dirty(p_update_mask);
	emit_signal("object_changed");
	emit_changed();
}

void NovaObjectData::reset_empty(const String &p_name) {
	_clear();
	object_name = sanitized_basename(p_name);
	copy_cstr(ir.name, sizeof(ir.name), to_std(object_name).c_str());
	has_ir = true;
	_notify_object_changed();
}

Error NovaObjectData::set_lod_scene(int p_lod_index, const String &p_path) {
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

Error NovaObjectData::open_file(const String &p_path) {
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

Error NovaObjectData::open_from_resource_root(
		const Ref<NovaResourceRoot> &p_resource_root, const String &p_name,
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

void NovaObjectData::mark_cached_network_challenge_foliage_model(
		const String &p_name) {
	register_network_challenge_model(p_name, false);
}

void NovaObjectData::reset_network_challenge_model_registry() {
	NetworkChallengeModelRegistry &registry =
			network_challenge_model_registry();
	registry.loaded.clear();
	registry.foliage.clear();
}

int64_t NovaObjectData::network_challenge_model_count() {
	const NetworkChallengeModelRegistry &registry =
			network_challenge_model_registry();
	std::size_t included = 0;
	for (const std::string &name : registry.loaded) {
		if (registry.foliage.find(name) == registry.foliage.end()) ++included;
	}
	return static_cast<int64_t>(included);
}

Error NovaObjectData::_open_3di(const String &p_path) {
	_clear();
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_read(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to read 3DI";
		return ERR_FILE_CANT_READ;
	}
	has_source_model = true;
	if (threedi_ir_from_3di3(&source_model, &ir) != 0) {
		last_error = "Failed to convert 3DI to IR";
		_clear();
		return ERR_FILE_CORRUPT;
	}
	has_ir = true;
	source_kind = SourceKind::Threedi;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = ir.name[0] != '\0' ? from_native(ir.name) : filename_stem(p_path);
	_notify_object_changed();
	return OK;
}

Error NovaObjectData::_open_3di_bytes(const String &p_name, const PackedByteArray &p_bytes) {
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
	if (threedi_ir_from_3di3(&source_model, &ir) != 0) {
		last_error = "Failed to convert mounted 3DI to IR";
		_clear();
		return ERR_FILE_CORRUPT;
	}
	has_ir = true;
	source_kind = SourceKind::Threedi;
	source_path = p_name.get_file();
	source_dir = String();
	object_name = ir.name[0] != '\0' ? from_native(ir.name) : filename_stem(p_name);
	return OK;
}

Error NovaObjectData::_open_3dp(const String &p_path) {
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
	return _build_ir_from_project_session(nullptr);
}

Error NovaObjectData::_open_ase(const String &p_path) {
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
	return _build_ir_from_project_session(nullptr);
}

Error NovaObjectData::_build_ir_from_project_session(const char *p_model_name, uint8_t p_dirty_mask) {
	Threedi3di3 built_model = {};
	const OedStatus build_rc = oed_session_build_model(
			oed_session, &source_project, static_cast<uint8_t>(OED_UPDATE_ALL), p_model_name, &built_model);
	if (build_rc != OED_STATUS_OK) {
		last_error = oed_error_detail(oed_session, "Failed to build OED model");
		return ERR_FILE_CANT_READ;
	}

	if (has_ir) {
		threedi_ir_free(&ir);
		threedi_ir_init(&ir);
		has_ir = false;
	}
	if (threedi_ir_from_3di3(&built_model, &ir) != 0) {
		threedi_3di3_free(&built_model);
		last_error = "Failed to convert OED model to IR";
		return ERR_FILE_CORRUPT;
	}
	threedi_3di3_free(&built_model);
	has_ir = true;
	_notify_object_changed(p_dirty_mask);
	return OK;
}

Error NovaObjectData::_rebuild_oed_session_from_project(uint8_t p_dirty_mask) {
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
	return _build_ir_from_project_session(nullptr, p_dirty_mask);
}

TdpProject NovaObjectData::_build_project_from_ir() const {
	TdpProject out = {};
	tdp_from_ir(&ir, &out);
	if (has_source_project) {
		for (int i = 0; i < TDP_MAX_LODS; ++i) {
			copy_lod_binding(out.lods[i], source_project.lods[i]);
		}
		out.poly_collision_lod = source_project.poly_collision_lod;
	}
	return out;
}

Error NovaObjectData::save_project_to_dir(const String &p_dir_path) {
	if (!has_ir || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	TdpProject project = _build_project_from_ir();
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

Error NovaObjectData::export_3di_to_dir(const String &p_dir_path, int p_update_mask) {
	if (!has_ir || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const String output_path = p_dir_path.path_join(_export_basename() + ".3di");
	if (source_kind == SourceKind::Threedi && has_source_model) {
		const Error err = _export_patched_3di(output_path);
		if (err == OK) {
			_clear_oed_dirty(UPDATE_ALL);
		}
		return err;
	}
	const uint8_t update_mask = _normalize_oed_update_mask(p_update_mask);
	return _export_project_backed_3di(output_path, update_mask);
}

Error NovaObjectData::_export_project_backed_3di(const String &p_path, uint8_t p_update_mask) {
	if (oed_session == nullptr) {
		last_error = "Object has no OED geometry session";
		return ERR_UNCONFIGURED;
	}

	TdpProject export_project = _build_project_from_ir();
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

Error NovaObjectData::_apply_ir_to_source_model() {
	if (!has_ir || !has_source_model) {
		return ERR_UNCONFIGURED;
	}

	if (source_model.material_count != ir.material_count || source_model.materials == nullptr) {
		std::free(source_model.materials);
		source_model.materials = nullptr;
		source_model.material_count = static_cast<uint32_t>(ir.material_count);
		if (ir.material_count > 0) {
			source_model.materials = static_cast<ThreediMaterial *>(
					std::calloc(ir.material_count, sizeof(ThreediMaterial)));
			if (source_model.materials == nullptr) {
				return ERR_OUT_OF_MEMORY;
			}
		}
	}
	for (size_t i = 0; i < ir.material_count; ++i) {
		copy_ir_material(ir.materials[i], source_model.materials[i]);
	}

	if (source_model.light_count != ir.light_count || source_model.lights == nullptr) {
		std::free(source_model.lights);
		source_model.lights = nullptr;
		source_model.light_count = ir.light_count;
		if (ir.light_count > 0) {
			source_model.lights = static_cast<ThreediLight *>(std::calloc(ir.light_count, sizeof(ThreediLight)));
			if (source_model.lights == nullptr) {
				return ERR_OUT_OF_MEMORY;
			}
		}
	}
	for (size_t i = 0; i < ir.light_count; ++i) {
		copy_ir_light(ir.lights[i], source_model.lights[i]);
	}

	if (source_model.ctrl.count != ir.control_register_count ||
			(source_model.ctrl.count > 0 &&
			 source_model.ctrl.registers == nullptr)) {
		std::free(source_model.ctrl.registers);
		source_model.ctrl.registers = nullptr;
		source_model.ctrl.count =
				static_cast<uint32_t>(ir.control_register_count);
		if (ir.control_register_count > 0) {
			source_model.ctrl.registers =
					static_cast<ThreediControlRegister *>(std::calloc(
							ir.control_register_count,
							sizeof(ThreediControlRegister)));
			if (source_model.ctrl.registers == nullptr) {
				return ERR_OUT_OF_MEMORY;
			}
		}
	}
	source_model.ctrl.record_size = 24;
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		copy_cstr(source_model.ctrl.registers[i].name,
				sizeof(source_model.ctrl.registers[i].name),
				ir.control_registers[i].name);
	}

	const size_t lod_count = std::min(source_model.lod_count, ir.lod_count);
	for (size_t lod_index = 0; lod_index < lod_count; ++lod_index) {
		ThreediLod &dst_lod = source_model.lods[lod_index];
		const ThreediIRLod &src_lod = ir.lods[lod_index];
		if (dst_lod.part_animation_count != src_lod.part_animation_count || dst_lod.part_animations == nullptr) {
			std::free(dst_lod.part_animations);
			dst_lod.part_animations = nullptr;
			dst_lod.part_animation_count = src_lod.part_animation_count;
			if (src_lod.part_animation_count > 0) {
				dst_lod.part_animations = static_cast<ThreediPartAnimation *>(
						std::calloc(src_lod.part_animation_count, sizeof(ThreediPartAnimation)));
				if (dst_lod.part_animations == nullptr) {
					return ERR_OUT_OF_MEMORY;
				}
			}
		}
		for (size_t i = 0; i < src_lod.part_animation_count; ++i) {
			copy_ir_part_animation(src_lod.part_animations[i], dst_lod.part_animations[i]);
		}
	}

	return OK;
}

Error NovaObjectData::_export_patched_3di(const String &p_path) {
	const Error apply_err = _apply_ir_to_source_model();
	if (apply_err != OK) {
		return apply_err;
	}
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_write(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to write patched 3DI";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

String NovaObjectData::_export_basename() const {
	return sanitized_basename(object_name);
}

String NovaObjectData::_source_kind_name() const {
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
