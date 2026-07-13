#include "foliage/far_mesh_emitter.h"

#include <cmath>
#include <limits>
#include <utility>

// [orig: generate_foliage_instances_0 @ 0x5ffdd0; source-vertex transform,
// terrain sample, UV/index copy, and final COLOR write @ 0x6001f6..0x60030a]

namespace opennova::foliage {

namespace {

Fixed16_16 world_to_fixed(float value) noexcept {
	const double scaled = static_cast<double>(value) * static_cast<double>(FIXED_SCALE);
	if (scaled <= static_cast<double>(std::numeric_limits<Fixed16_16>::min())) {
		return std::numeric_limits<Fixed16_16>::min();
	}
	if (scaled >= static_cast<double>(std::numeric_limits<Fixed16_16>::max())) {
		return std::numeric_limits<Fixed16_16>::max();
	}
	return static_cast<Fixed16_16>(scaled);
}

} // namespace

uint32_t far_wind_color(float source_y) noexcept {
	const float scaled = source_y * FAR_WIND_BYTE_SCALE;
	uint32_t wind_byte = 0;
	if (scaled >= 255.0f) {
		wind_byte = 255;
	} else if (scaled > 0.0f) {
		// C++ conversion truncates toward zero, matching the witnessed ftoi path.
		wind_byte = static_cast<uint32_t>(scaled);
	}
	return wind_byte << 16;
}

bool emit_far_mesh(const FarSourceMesh &source,
                   const PlacementResult &placements,
                   const HeightFn &height_at,
                   FarMesh &out) {
	out.vertices.clear();
	out.indices.clear();

	if (placements.count < 0 || placements.count > FAR_CELL_CAP) {
		return false;
	}
	for (uint32_t index : source.indices) {
		if (index >= source.vertices.size()) {
			return false;
		}
	}

	const size_t placement_count = static_cast<size_t>(placements.count);
	if (!source.vertices.empty() &&
	    placement_count > std::numeric_limits<size_t>::max() / source.vertices.size()) {
		return false;
	}
	const size_t vertex_count = placement_count * source.vertices.size();
	if (vertex_count > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	if (!source.indices.empty() &&
	    placement_count > std::numeric_limits<size_t>::max() / source.indices.size()) {
		return false;
	}

	FarMesh emitted;
	emitted.vertices.reserve(vertex_count);
	emitted.indices.reserve(placement_count * source.indices.size());

	for (int placement_index = 0; placement_index < placements.count; ++placement_index) {
		const PlacementInstance &placement = placements.instances[placement_index];
		const float center_x = static_cast<float>(placement.world_x_fixed) * FIXED_TO_FLOAT;
		const float center_z = static_cast<float>(placement.world_z_fixed) * FIXED_TO_FLOAT;
		const float sin_yaw = std::sin(placement.rotation_radians);
		const float cos_yaw = std::cos(placement.rotation_radians);
		const uint32_t vertex_base = static_cast<uint32_t>(emitted.vertices.size());

		bool ground_valid = true;
		for (const FarSourceVertex &source_vertex : source.vertices) {
			FarVertex vertex{};
			// Witnessed native transform: X' = x*cos - z*sin + cx,
			// Z' = x*sin + z*cos + cz [orig: generate_foliage_instances_0
			// @ 0x600112..0x60014d; the VB stores (Z', Y, X') at +0/+4/+8].
			// Host source vertices carry the importer's -X mesh flip
			// (nova_object_data.cpp godot_from_native) and world z = -native
			// z; composing both gives x = cx - (sx*cos + sz*sin),
			// z = cz + (sx*sin - sz*cos).
			vertex.x = center_x - FAR_HORIZONTAL_SCALE *
			                         (source_vertex.x * cos_yaw + source_vertex.z * sin_yaw);
			vertex.z = center_z + FAR_HORIZONTAL_SCALE *
			                         (source_vertex.x * sin_yaw - source_vertex.z * cos_yaw);
			const Fixed16_16 ground_fixed = height_at
			                                     ? height_at(world_to_fixed(vertex.x),
			                                                 world_to_fixed(vertex.z))
			                                     : 0;
			if (ground_fixed == HEIGHT_INVALID) {
				// "No terrain here" (host contract, placement.h): drop the
				// whole instance rather than bend a blade to the sentinel.
				ground_valid = false;
				break;
			}
			vertex.y = static_cast<float>(ground_fixed) * FIXED_TO_FLOAT +
			           source_vertex.y * FAR_VERTICAL_SCALE;
			vertex.u = source_vertex.u;
			vertex.v = source_vertex.v;
			vertex.color = far_wind_color(source_vertex.y);
			emitted.vertices.push_back(vertex);
		}
		if (!ground_valid) {
			emitted.vertices.resize(vertex_base);
			continue;
		}

		for (uint32_t source_index : source.indices) {
			emitted.indices.push_back(vertex_base + source_index);
		}
	}

	out = std::move(emitted);
	return true;
}

} // namespace opennova::foliage
