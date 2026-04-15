#include "util/nova_data_format.h"

using namespace godot;

void NovaDataFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &NovaDataFile::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &NovaDataFile::get_data);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data"), "set_data", "get_data");
}

void NovaDataFile::set_data(const PackedByteArray &p_data) { data = p_data; }
PackedByteArray NovaDataFile::get_data() const { return data; }
