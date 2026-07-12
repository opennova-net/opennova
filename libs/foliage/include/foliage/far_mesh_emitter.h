#pragma once

// Renderer-neutral FAR foliage mesh emission, witnessed in retail
// Jointops.exe generate_foliage_instances_0 @ 0x5ffdd0. Each accepted FAR
// placement receives a complete copy of the authored def mesh. Terrain is
// sampled independently below every transformed source vertex.

#include <cstdint>
#include <vector>

#include "foliage/placement.h"

namespace opennova::foliage {

constexpr float FAR_HORIZONTAL_SCALE = 1.0f;
constexpr float FAR_VERTICAL_SCALE = 0.5f;
constexpr float FAR_WIND_BYTE_SCALE = 128.0f;

// Source vertices are in the repository renderer/importer convention, ready
// to copy directly from a Godot Mesh surface: importer X is already the
// negation of retail 3DI source X, while Y and Z are unchanged. The emitter
// therefore applies the witnessed FAR basis as rotY(yaw + pi/2):
//
//   world_x = placement_x - source_x*sin(yaw) + source_z*cos(yaw)
//   world_z = placement_z - source_x*cos(yaw) - source_z*sin(yaw)
//
// XZ uses retail scale 1.0. Y is source_y*0.5 above terrain sampled at the
// transformed (world_x, world_z). Normals are intentionally absent: the
// retail FAR shader is unlit, and the observed normal copy is not a reliable
// transform contract.
struct FarSourceVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct FarSourceMesh {
	std::vector<FarSourceVertex> vertices;
	std::vector<uint32_t> indices;
};

struct FarVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	// Retail D3DCOLOR. Only red is populated; the wind shader consumes v5.x.
	uint32_t color = 0;
};

struct FarMesh {
	std::vector<FarVertex> vertices;
	std::vector<uint32_t> indices;
};

// [orig: generate_foliage_instances_0 @ 0x6002DB..0x60030A]
// clamp(trunc(source_y * 128), 0, 255) << 16. Earlier terrain-color writes
// in that routine are dead: this unconditional write is the emitted color.
uint32_t far_wind_color(float source_y) noexcept;

// Replicate source mesh vertices and indices for every accepted FAR
// placement. Returns false (and clears `out`) if placement count or a source
// index is invalid, or if the replicated vertex range cannot fit uint32_t
// indices. An empty height callback means flat ground at Y=0.
bool emit_far_mesh(const FarSourceMesh &source,
                   const PlacementResult &placements,
                   const HeightFn &height_at,
                   FarMesh &out);

} // namespace opennova::foliage
