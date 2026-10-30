#include <runtime/assets/foliage_assets.h>

#include <formats/threedi/threedi_3di3.h>
#include <runtime/renderer/model_mesh_prepare.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <vector>

namespace opennova::assets {

void foliage_slot_bounds(renderer::FoliageSlotGeometry &geometry) {
	geometry.center_x = 0.0f;
	geometry.center_z = 0.0f;
	geometry.radius = 0.0f;
	geometry.valid = false;
	if (geometry.vertices.empty() || geometry.indices.empty()) {
		return;
	}
	// The planar box seeded at +-1000 [orig: Foliage_LoadDefAssets
	// @ 0x60134d..0x60135b], widened by every vertex's x and z
	// [@ 0x60136e..0x6013b3].
	float min_x = 1000.0f;
	float min_z = 1000.0f;
	float max_x = -1000.0f;
	float max_z = -1000.0f;
	for (const renderer::FoliageSourceVertex &vertex : geometry.vertices) {
		if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
			return;
		}
		min_x = std::min(min_x, vertex.x);
		min_z = std::min(min_z, vertex.z);
		max_x = std::max(max_x, vertex.x);
		max_z = std::max(max_z, vertex.z);
	}
	// The centre and the larger half extent [@ 0x6013c5..0x6013eb].
	const float half_x = (max_x - min_x) * 0.5f;
	const float half_z = (max_z - min_z) * 0.5f;
	geometry.center_x = min_x + half_x;
	geometry.center_z = min_z + half_z;
	geometry.radius = half_x > half_z ? half_x : half_z;
	geometry.valid = std::isfinite(geometry.radius) && geometry.radius > 1.0e-6f;
}

FoliageModelAsset foliage_model_asset(const threedi::Threedi3di3 &model) {
	FoliageModelAsset asset;
	if (model.lod_count == 0) {
		return asset;
	}
	const std::vector<renderer::PreparedMeshSurface> surfaces = renderer::prepare_model_mesh(model, 0);
	if (!surfaces.empty()) {
		asset.fd_material_array_index = surfaces.front().material_array_index;
	}
	renderer::FoliageSlotGeometry &geometry = asset.geometry;
	for (const renderer::PreparedMeshSurface &surface : surfaces) {
		if (surface.vertices.empty()) {
			continue;
		}
		const int32_t base = static_cast<int32_t>(geometry.vertices.size());
		const int32_t count = static_cast<int32_t>(surface.vertices.size());
		for (size_t i = 0; i < surface.vertices.size(); ++i) {
			renderer::FoliageSourceVertex vertex;
			vertex.x = surface.vertices[i][0];
			vertex.y = surface.vertices[i][1];
			vertex.z = surface.vertices[i][2];
			if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
				geometry = renderer::FoliageSlotGeometry{};
				return asset;
			}
			if (i < surface.uvs.size()) {
				vertex.u = surface.uvs[i][0];
				vertex.v = surface.uvs[i][1];
			}
			geometry.vertices.push_back(vertex);
		}
		if (surface.indices.empty()) {
			for (int32_t index = 0; index + 2 < count; index += 3) {
				geometry.indices.push_back(base + index);
				geometry.indices.push_back(base + index + 1);
				geometry.indices.push_back(base + index + 2);
			}
			continue;
		}
		for (size_t index = 0; index + 2 < surface.indices.size(); index += 3) {
			const int32_t a = surface.indices[index];
			const int32_t b = surface.indices[index + 1];
			const int32_t c = surface.indices[index + 2];
			if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count) {
				continue;
			}
			geometry.indices.push_back(base + a);
			geometry.indices.push_back(base + b);
			geometry.indices.push_back(base + c);
		}
	}
	if (geometry.vertices.empty() || geometry.indices.empty()) {
		geometry = renderer::FoliageSlotGeometry{};
		return asset;
	}
	foliage_slot_bounds(geometry);
	return asset;
}

std::string foliage_model_basename(const std::string &graphic) {
	const size_t slash = graphic.find_last_of("/\\");
	std::string name = slash == std::string::npos ? graphic : graphic.substr(slash + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) {
		name.resize(dot);
	}
	for (char &c : name) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return name;
}

} // namespace opennova::assets
