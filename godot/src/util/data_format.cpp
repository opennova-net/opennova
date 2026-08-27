#include "util/data_format.h"

#include <base/vfs/vfs_decode.h>

#include <godot_cpp/classes/file_access.hpp>

#include <cstring>
#include <vector>

using namespace godot;

bool godot::decode_nova_payload_bytes(PackedByteArray &p_bytes) {
	std::vector<uint8_t> bytes(static_cast<size_t>(p_bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), p_bytes.ptr(), bytes.size());
	}
	if (!opennova::vfs_decode_payload(bytes)) {
		return false;
	}
	p_bytes.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(p_bytes.ptrw(), bytes.data(), bytes.size());
	}
	return true;
}

bool godot::bytes_look_like_dds(const PackedByteArray &p_bytes) {
	return p_bytes.size() >= 4 &&
			p_bytes[0] == 'D' &&
			p_bytes[1] == 'D' &&
			p_bytes[2] == 'S' &&
			p_bytes[3] == ' ';
}

bool godot::read_nova_payload_file(const String &p_path, PackedByteArray &r_bytes) {
	r_bytes = PackedByteArray();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	r_bytes = file->get_buffer(file->get_length());
	file.unref();
	if (!decode_nova_payload_bytes(r_bytes)) {
		r_bytes = PackedByteArray();
		return false;
	}
	return true;
}
