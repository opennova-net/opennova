// ObjectData: read-only document identity and summary inspection.
#include "object/object_data_internal.h"

bool ObjectData::has_document() const {
	return bool(source_model_);
}

String ObjectData::get_source_path() const {
	return source_path;
}

String ObjectData::get_last_error() const {
	return last_error;
}

int ObjectData::get_lod_count() const {
	return source_model_ ? static_cast<int>(native_model().lod_count) : 0;
}
