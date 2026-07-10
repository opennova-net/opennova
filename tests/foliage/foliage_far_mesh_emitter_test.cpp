#include <foliage/far_mesh_emitter.h>
#include <foliage/placement.h>

#include <cmath>
#include <cstdio>

using namespace opennova;
using namespace opennova::foliage;

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float EPS = 1.0e-4f;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= EPS;
}

Fixed16_16 fixed(float value) {
	return static_cast<Fixed16_16>(value * FIXED_SCALE);
}

FarSourceMesh asymmetric_source_mesh() {
	FarSourceMesh source;
	source.vertices = {
	    {1.0f, 2.0f, 3.0f, 0.10f, 0.20f},
	    {-2.0f, -1.0f, 0.5f, 0.30f, 0.40f},
	    {0.25f, 2.5f, -4.0f, 0.70f, 0.90f},
	};
	source.indices = {2, 0, 1, 2, 1, 0};
	return source;
}

bool wind_color_boundaries() {
	return expect(far_wind_color(-1.0f) == 0u, "negative source Y clamps to zero") &&
	       expect(far_wind_color(-1.0f / 256.0f) == 0u,
	              "negative fractions truncate toward zero before clamping") &&
	       expect(far_wind_color(0.0f) == 0u, "zero source Y emits zero wind") &&
	       expect(far_wind_color(1.0f / 256.0f) == 0u,
	              "positive half-byte truncates toward zero") &&
	       expect(far_wind_color(1.0f / 128.0f) == 0x00010000u,
	              "one wind byte occupies D3DCOLOR red") &&
	       expect(far_wind_color(127.0f / 128.0f) == 0x007F0000u,
	              "interior wind byte is not rounded") &&
	       expect(far_wind_color(255.0f / 128.0f) == 0x00FF0000u,
	              "255 is the highest un-clamped wind byte") &&
	       expect(far_wind_color(2.0f) == 0x00FF0000u,
	              "source Y producing 256 clamps to 255");
}

bool asymmetric_mesh_is_replicated() {
	PlacementResult placements{};
	placements.count = 2;
	placements.instances[0].world_x_fixed = fixed(10.0f);
	placements.instances[0].world_z_fixed = fixed(-20.0f);
	placements.instances[0].rotation_radians = 0.0f;
	placements.instances[1].world_x_fixed = fixed(-4.0f);
	placements.instances[1].world_z_fixed = fixed(7.0f);
	placements.instances[1].rotation_radians = PI * 0.5f;

	int height_calls = 0;
	HeightFn height = [&height_calls](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		++height_calls;
		return static_cast<Fixed16_16>(wx / 4 + wz / 8);
	};

	FarMesh emitted;
	if (!expect(emit_far_mesh(asymmetric_source_mesh(), placements, height, emitted),
	            "valid source mesh emits successfully")) return false;
	if (!expect(emitted.vertices.size() == 6, "all source vertices are copied per placement")) return false;
	if (!expect(emitted.indices.size() == 12, "all source indices are copied per placement")) return false;
	if (!expect(height_calls == 6, "terrain height is sampled once per emitted vertex")) return false;

	const uint32_t expected_indices[] = {2, 0, 1, 2, 1, 0, 5, 3, 4, 5, 4, 3};
	for (size_t i = 0; i < emitted.indices.size(); ++i) {
		if (!expect(emitted.indices[i] == expected_indices[i],
		            "replicated indices preserve topology with a vertex-base offset")) return false;
	}

	// Input vertices already use the renderer/importer axes. At yaw zero the
	// retail FAR basis is rotY(pi/2): render X += source Z, render Z -= source X.
	const FarVertex &a0 = emitted.vertices[0];
	if (!expect(near(a0.x, 13.0f) && near(a0.z, -21.0f),
	            "yaw-zero FAR transform uses the witnessed source-axis mapping")) return false;
	const float a0_ground = 13.0f / 4.0f + -21.0f / 8.0f;
	if (!expect(near(a0.y, a0_ground + 1.0f),
	            "source Y is scaled by 0.5 and added to per-vertex ground height")) return false;
	if (!expect(near(a0.u, 0.10f) && near(a0.v, 0.20f), "source UVs are preserved")) return false;
	if (!expect(a0.color == 0x00FF0000u, "source Y drives the packed red wind byte")) return false;

	const FarVertex &b0 = emitted.vertices[1];
	if (!expect(near(b0.x, 10.5f) && near(b0.z, -18.0f),
	            "asymmetric source vertex is transformed independently")) return false;
	const float b0_ground = 10.5f / 4.0f + -18.0f / 8.0f;
	if (!expect(near(b0.y, b0_ground - 0.5f), "negative source Y retains the 0.5 vertical scale")) return false;
	if (!expect(b0.color == 0u, "negative source Y clamps wind to zero")) return false;

	// At yaw pi/2 the basis is rotY(pi): X -= source X, Z -= source Z.
	const FarVertex &a1 = emitted.vertices[3];
	if (!expect(near(a1.x, -5.0f) && near(a1.z, 4.0f),
	            "FAR yaw rotates the full source mesh around each placement")) return false;
	if (!expect(near(a1.u, 0.10f) && near(a1.v, 0.20f),
	            "UV preservation is independent of placement rotation")) return false;
	return true;
}

bool all_accept_emits_36_full_mesh_copies() {
	int height_calls = 0;
	PlacementSamplers samplers;
	samplers.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	samplers.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	samplers.height_at = [&height_calls](Fixed16_16, Fixed16_16) -> Fixed16_16 {
		++height_calls;
		return 0;
	};
	PlacementConfig config{};
	config.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;
	const uint32_t key = pack_cell_key(fixed(16.0f), fixed(16.0f));
	const PlacementResult placements = place_cell(0, key, config, samplers);

	if (!expect(placements.count == FAR_CELL_CAP, "all-accept FAR placement yields 36 candidates")) return false;
	if (!expect(height_calls == 0,
	            "FAR placement does not sample a synthetic quad before source geometry is known")) return false;

	FarMesh emitted;
	if (!expect(emit_far_mesh(asymmetric_source_mesh(), placements, samplers.height_at, emitted),
	            "all accepted placements emit")) return false;
	if (!expect(emitted.vertices.size() == static_cast<size_t>(FAR_CELL_CAP * 3),
	            "36 complete source vertex arrays are emitted")) return false;
	if (!expect(emitted.indices.size() == static_cast<size_t>(FAR_CELL_CAP * 6),
	            "36 complete source index arrays are emitted")) return false;
	if (!expect(height_calls == FAR_CELL_CAP * 3,
	            "terrain sampling count follows emitted source vertices")) return false;
	return true;
}

} // namespace

int main() {
	if (!wind_color_boundaries()) return 1;
	if (!asymmetric_mesh_is_replicated()) return 1;
	if (!all_accept_emits_36_full_mesh_copies()) return 1;
	std::printf("OK: FAR emits 36 full source meshes with retail transform, terrain bend, UV/index, and wind color\n");
	return 0;
}
