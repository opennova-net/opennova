#include "dbf/nova_dbf_data.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

void NovaDbfData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &NovaDbfData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name"), &NovaDbfData::open_from_resource_root);
	ClassDB::bind_method(D_METHOD("load_bytes", "bytes"), &NovaDbfData::load_bytes);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaDbfData::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaDbfData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaDbfData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_dialog_count"), &NovaDbfData::get_dialog_count);
	ClassDB::bind_method(D_METHOD("get_dialog_ids"), &NovaDbfData::get_dialog_ids);
	ClassDB::bind_method(D_METHOD("has_dialog", "id"), &NovaDbfData::has_dialog);
	ClassDB::bind_method(D_METHOD("resolve_dialog", "id"), &NovaDbfData::resolve_dialog);
}

bool NovaDbfData::decode(const PackedByteArray &bytes) {
	std::string err;
	if (!opennova::dbf::parse_dbf_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), file_, err)) {
		last_error_ = String(err.c_str());
		loaded_ = false;
		return false;
	}
	loaded_ = true;
	return true;
}

Error NovaDbfData::open_file(const String &p_path) {
	last_error_ = String();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		last_error_ = "Cannot open file: " + p_path;
		return ERR_CANT_OPEN;
	}
	PackedByteArray bytes = file->get_buffer(file->get_length());
	file->close();
	if (!decode(bytes)) {
		return ERR_FILE_CORRUPT;
	}
	source_path_ = p_path;
	return OK;
}

Error NovaDbfData::open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	last_error_ = String();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error_ = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		last_error_ = "Dialog bank filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		last_error_ = "Dialog bank not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}
	if (!decode(bytes)) {
		return ERR_CANT_OPEN;
	}
	source_path_ = file;
	return OK;
}

bool NovaDbfData::load_bytes(const PackedByteArray &p_bytes) {
	last_error_ = String();
	return decode(p_bytes);
}

int NovaDbfData::get_dialog_count() const {
	return static_cast<int>(file_.groups.size());
}

PackedStringArray NovaDbfData::get_dialog_ids() const {
	PackedStringArray out;
	for (const auto &group : file_.groups) {
		out.push_back(String::utf8(group.group_name.c_str()));
	}
	return out;
}

bool NovaDbfData::has_dialog(const String &p_id) const {
	return opennova::dbf::find_group(file_, p_id.utf8().get_data()) != nullptr;
}

PackedStringArray NovaDbfData::resolve_dialog(const String &p_id) const {
	PackedStringArray out;
	const opennova::dbf::Group *group = opennova::dbf::find_group(file_, p_id.utf8().get_data());
	if (group == nullptr) {
		return out;
	}
	for (const auto &line : group->lines) {
		out.push_back(String::utf8(line.def_id_name.c_str()));
	}
	return out;
}

} // namespace godot
