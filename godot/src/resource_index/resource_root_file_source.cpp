#include "resource_index/resource_root_file_source.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstring>

#include "util/string_convert.h"

namespace godot {

bool ResourceRootFileSource::read(const std::string &p_name, std::vector<uint8_t> &r_out) const {
	if (root_.is_null() || p_name.empty()) {
		return false;
	}
	const PackedByteArray bytes = root_->read_file(opennova::to_gd(p_name).get_file());
	if (bytes.is_empty()) {
		return false;
	}
	r_out.resize(static_cast<size_t>(bytes.size()));
	std::memcpy(r_out.data(), bytes.ptr(), r_out.size());
	return true;
}

uint64_t ResourceRootFileSource::stamp(const std::string &p_name) const {
	if (root_.is_null() || p_name.empty()) {
		return 0;
	}
	if (!root_->has_file(opennova::to_gd(p_name).get_file())) {
		return 0;
	}
	const uint64_t epoch = static_cast<uint64_t>(ResourceRoot::cache_epoch());
	const uint64_t root = root_->get_instance_id();
	return ((epoch * 0x9E3779B97F4A7C15ull) ^ root) | 1u;
}

} // namespace godot
