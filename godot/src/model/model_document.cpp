#include "model/model_document.h"

#include "util/string_convert.h"

#include <godot_cpp/classes/project_settings.hpp>

#include <string>
#include <vector>

using namespace godot;
using namespace opennova::threedi;

namespace {

std::string native_path(const String &p_path) {
	String global = p_path;
	if (ProjectSettings::get_singleton() != nullptr) {
		global = ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return opennova::to_std(global);
}

} // namespace

void ModelDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &ModelDocument::load_from_path);
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &ModelDocument::load_from_bytes);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &ModelDocument::save_to_path);
	ClassDB::bind_method(D_METHOD("to_bytes"), &ModelDocument::to_bytes);
	ClassDB::bind_method(D_METHOD("is_loaded"), &ModelDocument::is_loaded);
	ClassDB::bind_method(D_METHOD("get_model_name"), &ModelDocument::get_model_name);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ModelDocument::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ModelDocument::get_last_error);
	ClassDB::bind_method(D_METHOD("get_lod_count"), &ModelDocument::get_lod_count);
	ClassDB::bind_method(D_METHOD("get_part_count", "lod_index"), &ModelDocument::get_part_count);
	ClassDB::bind_method(D_METHOD("get_material_count"), &ModelDocument::get_material_count);
	ClassDB::bind_method(D_METHOD("is_skinned"), &ModelDocument::is_skinned);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &ModelDocument::has_occlusion);
}

ModelDocument::~ModelDocument() {
	_clear();
}

void ModelDocument::_clear() {
	if (parsed_valid_) {
		threedi_3di3_free(&parsed_);
		parsed_ = {};
		parsed_valid_ = false;
	}
	assembled_.reset();
	source_path_ = String();
}

Error ModelDocument::load_from_path(const String &p_path) {
	Threedi3di3 next = {};
	if (threedi_3di3_read(native_path(p_path).c_str(), &next) != 0) {
		last_error_ = "Failed to read 3DI: " + p_path;
		return ERR_FILE_CANT_READ;
	}
	_clear();
	parsed_ = next;
	parsed_valid_ = true;
	source_path_ = p_path;
	last_error_ = String();
	return OK;
}

Error ModelDocument::load_from_bytes(const PackedByteArray &p_bytes) {
	if (p_bytes.is_empty()) {
		last_error_ = "Empty 3DI bytes";
		return ERR_INVALID_DATA;
	}
	Threedi3di3 next = {};
	if (threedi_3di3_read_memory(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()), &next) != 0) {
		last_error_ = "Failed to parse 3DI bytes";
		return ERR_INVALID_DATA;
	}
	_clear();
	parsed_ = next;
	parsed_valid_ = true;
	last_error_ = String();
	return OK;
}

const Threedi3di3 *ModelDocument::model() const {
	if (assembled_ != nullptr) {
		return &assembled_->model;
	}
	return parsed_valid_ ? &parsed_ : nullptr;
}

void ModelDocument::adopt_assembled(std::unique_ptr<ThreediAssembled> p_assembled) {
	_clear();
	assembled_ = std::move(p_assembled);
	last_error_ = String();
}

Error ModelDocument::save_to_path(const String &p_path) {
	const Threedi3di3 *m = model();
	if (m == nullptr) {
		last_error_ = "No model loaded";
		return ERR_UNCONFIGURED;
	}
	if (threedi_3di3_write(native_path(p_path).c_str(), m) != 0) {
		last_error_ = "The 3DI writer refused the model or could not write: " + p_path;
		return ERR_FILE_CANT_WRITE;
	}
	last_error_ = String();
	return OK;
}

PackedByteArray ModelDocument::to_bytes() {
	PackedByteArray out;
	const Threedi3di3 *m = model();
	if (m == nullptr) {
		last_error_ = "No model loaded";
		return out;
	}
	std::vector<uint8_t> bytes;
	if (threedi_3di3_write_memory(m, bytes) != 0) {
		last_error_ = "The 3DI writer refused the model";
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

String ModelDocument::get_model_name() const {
	const Threedi3di3 *m = model();
	if (m == nullptr) {
		return String();
	}
	return opennova::to_gd(std::string(m->header.name));
}

int ModelDocument::get_lod_count() const {
	const Threedi3di3 *m = model();
	return m == nullptr ? 0 : static_cast<int>(m->lod_count);
}

int ModelDocument::get_part_count(int p_lod_index) const {
	const Threedi3di3 *m = model();
	if (m == nullptr || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= m->lod_count) {
		return 0;
	}
	return static_cast<int>(m->lods[p_lod_index].render_object_count);
}

int ModelDocument::get_material_count() const {
	const Threedi3di3 *m = model();
	return m == nullptr ? 0 : static_cast<int>(m->material_count);
}

bool ModelDocument::is_skinned() const {
	const Threedi3di3 *m = model();
	return m != nullptr && m->header.mesh_type == THREEDI_MESH_SKINNED;
}

bool ModelDocument::has_occlusion() const {
	const Threedi3di3 *m = model();
	return m != nullptr && m->occlusion_object_count > 0;
}
