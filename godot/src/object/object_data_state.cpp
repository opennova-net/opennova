// ObjectData: read-only document identity and summary inspection.
#include "object/nova_object_data_internal.h"

bool ObjectData::has_document() const {
	return has_source_model;
}

String ObjectData::get_source_path() const {
	return source_path;
}

String ObjectData::get_last_error() const {
	return last_error;
}

Dictionary ObjectData::get_summary() const {
	Dictionary result;
	result["name"] = object_name;
	result["source_kind"] = has_source_model ? String("3di") : String("empty");
	result["lod_count"] = static_cast<int64_t>(
			has_source_model ? source_model.lod_count : 0);
	result["material_count"] = static_cast<int64_t>(
			has_source_model ? source_model.material_count : 0);
	result["light_count"] = static_cast<int64_t>(
			has_source_model ? source_model.light_count : 0);
	result["userpoint_count"] = static_cast<int64_t>(
			has_source_model ? source_model.user_point_count : 0);
	return result;
}
