#pragma once

#include "render/q3_geometry_cache.h"

#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

namespace godot {

// The RenderingDevice description of the Q3GeometryCache stream layout: the
// eight interleaved per-vertex attributes of one kQ3VertexStride vertex, at
// the offsets the cache packs. Shared by every adapter that draws the cache's
// streams (the focused Q3 frame, the slot capture).
inline TypedArray<Ref<RDVertexAttribute>> q3_vertex_attributes() {
	TypedArray<Ref<RDVertexAttribute>> attributes;
	auto add_attribute = [&](std::uint32_t p_location,
			RenderingDevice::DataFormat p_format, std::uint32_t p_offset) {
		Ref<RDVertexAttribute> attribute;
		attribute.instantiate();
		attribute->set_location(p_location);
		attribute->set_binding(0);
		attribute->set_format(p_format);
		attribute->set_offset(p_offset);
		attribute->set_stride(kQ3VertexStride);
		attribute->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
		attributes.push_back(attribute);
	};
	add_attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0);
	add_attribute(1, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 12);
	add_attribute(2, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 24);
	add_attribute(3, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 32);
	add_attribute(4, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 48);
	add_attribute(5, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 64);
	add_attribute(6, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 80);
	add_attribute(7, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 96);
	return attributes;
}

} // namespace godot
