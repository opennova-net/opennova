// ObjectData: read-only document identity and summary inspection.
#include "object/object_data_internal.h"

bool ObjectData::has_document() const {
	return has_source_model;
}

String ObjectData::get_source_path() const {
	return source_path;
}

String ObjectData::get_last_error() const {
	return last_error;
}

int ObjectData::get_lod_count() const {
	return has_source_model ? static_cast<int>(source_model.lod_count) : 0;
}
