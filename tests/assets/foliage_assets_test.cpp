// A foliage definition's model asset (runtime/assets/foliage_assets.h): the
// slot's centre and bound radius over its vertices as Foliage_LoadDefAssets
// takes them (the +-1000 seeds, the larger half extent), the LOD0 geometry the
// slot expands, the material its :fd texture bakes from, and the model key a
// graphic names. RE record: docs/foliage/foliage-re.md.
#include <runtime/assets/foliage_assets.h>

#include <formats/threedi/threedi_3di3.h>
#include <runtime/renderer/model_mesh_prepare.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "common/test_paths.h"

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

bool near(float a, float b) {
	return std::fabs(a - b) <= 1.0e-5f;
}

opennova::renderer::FoliageSlotGeometry triangle(float x0, float z0, float x1, float z1, float x2,
		float z2) {
	opennova::renderer::FoliageSlotGeometry geometry;
	geometry.vertices = {{x0, 0.0f, z0, 0.0f, 0.0f}, {x1, 1.0f, z1, 1.0f, 0.0f},
			{x2, 2.0f, z2, 0.0f, 1.0f}};
	geometry.indices = {0, 1, 2};
	return geometry;
}

void test_bounds() {
	using opennova::assets::foliage_slot_bounds;
	// The planar box's centre and the larger half extent; y plays no part.
	opennova::renderer::FoliageSlotGeometry box = triangle(-2.0f, 1.0f, 4.0f, 2.0f, 0.0f, 5.0f);
	foliage_slot_bounds(box);
	expect(box.valid, "a planar triangle is a valid slot");
	expect(near(box.center_x, 1.0f) && near(box.center_z, 3.0f), "the box centre");
	expect(near(box.radius, 3.0f), "the larger half extent (x: 3, z: 2)");

	// The +-1000 seeds: a model wholly past +1000 on x keeps the seed as its
	// minimum, as the load's walk does.
	opennova::renderer::FoliageSlotGeometry far = triangle(1100.0f, 0.0f, 1200.0f, 1.0f, 1150.0f, 2.0f);
	foliage_slot_bounds(far);
	expect(near(far.center_x, 1100.0f) && near(far.radius, 100.0f),
			"the minimum seeded at 1000 stays below a far model");

	// A flat point, a non-finite vertex and an empty slot stay invalid.
	opennova::renderer::FoliageSlotGeometry point = triangle(3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f);
	foliage_slot_bounds(point);
	expect(!point.valid, "a zero radius disables the slot");
	opennova::renderer::FoliageSlotGeometry broken =
			triangle(0.0f, 0.0f, std::numeric_limits<float>::infinity(), 1.0f, 2.0f, 2.0f);
	foliage_slot_bounds(broken);
	expect(!broken.valid, "a non-finite vertex disables the slot");
	opennova::renderer::FoliageSlotGeometry empty;
	foliage_slot_bounds(empty);
	expect(!empty.valid, "no triangle, no slot");
}

void test_model_basename() {
	using opennova::assets::foliage_model_basename;
	expect(foliage_model_basename("Mveg6.3di") == "mveg6", "the extension and the case go");
	expect(foliage_model_basename("mveg6") == "mveg6", "a bare name keeps itself");
	expect(foliage_model_basename("models\\Mveg5b.3DI") == "mveg5b", "the directory goes");
	expect(foliage_model_basename("").empty(), "an empty name names nothing");
}

void test_model_asset() {
	using namespace opennova;
	threedi::Threedi3di3 model = {};
	const std::string path = test_paths_fixture("threedi/synth/bird.3di");
	if (threedi::threedi_3di3_read(path.c_str(), &model) != 0) {
		expect(false, "the minted bird model reads");
		return;
	}
	const assets::FoliageModelAsset asset = assets::foliage_model_asset(model);
	const std::vector<renderer::PreparedMeshSurface> surfaces =
			renderer::prepare_model_mesh(model, 0);
	size_t vertex_count = 0;
	size_t index_count = 0;
	for (const renderer::PreparedMeshSurface &surface : surfaces) {
		vertex_count += surface.vertices.size();
		index_count += surface.indices.size();
	}
	expect(asset.geometry.valid, "the bird's LOD0 is a valid slot");
	expect(asset.geometry.vertices.size() == vertex_count,
			"every LOD0 surface's vertices, in surface order");
	expect(asset.geometry.indices.size() == index_count, "every surface's triangles");
	bool in_range = true;
	for (const int32_t index : asset.geometry.indices) {
		in_range = in_range && index >= 0 &&
				static_cast<size_t>(index) < asset.geometry.vertices.size();
	}
	expect(in_range, "each surface's indices are offset past the vertices before it");
	expect(!surfaces.empty() &&
					asset.fd_material_array_index == surfaces.front().material_array_index,
			"the :fd texture bakes from the first surface's material");
	renderer::FoliageSlotGeometry again = asset.geometry;
	assets::foliage_slot_bounds(again);
	expect(near(again.center_x, asset.geometry.center_x) &&
					near(again.center_z, asset.geometry.center_z) &&
					near(again.radius, asset.geometry.radius),
			"the slot's bounds are foliage_slot_bounds over its vertices");
	threedi::threedi_3di3_free(&model);
}

} // namespace

int main() {
	test_bounds();
	test_model_basename();
	test_model_asset();
	if (failures != 0) {
		std::fprintf(stderr, "foliage_assets: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("foliage_assets: ok\n");
	return 0;
}
