#pragma once

// RenderingDevice plumbing the RD-side render legs share: the sampled-texture
// uniform row and the raw push-constant / uniform-buffer field writers.

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <cstring>

namespace godot {

inline Ref<RDUniform> sampled_texture_uniform(int p_binding, const RID &p_sampler,
		const RID &p_texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	uniform->set_binding(p_binding);
	uniform->add_id(p_sampler);
	uniform->add_id(p_texture);
	return uniform;
}

inline void write_u32(PackedByteArray &p_bytes, std::uint32_t p_offset,
		std::uint32_t p_value) {
	std::memcpy(p_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

inline void write_f32(PackedByteArray &p_bytes, std::uint32_t p_offset, float p_value) {
	std::memcpy(p_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

} // namespace godot
