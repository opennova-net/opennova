// Runtime material inputs for the terrain PROJSHAD pass: the shared retail
// AlphaGen/UV evaluator, time and controlled diffuse flipbooks, and the
// file-effect rule that ignores material AlphaGen.
#include <terrain/terrain_static_shadow_geometry.h>

#include <threedi/threedi_ctrl_catalog.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float left, float right, float epsilon = 0.0001f) {
	return std::fabs(left - right) <= epsilon;
}

std::shared_ptr<opennova::terrain::TerrainStaticShadowAlphaPyramid>
alpha_identity() {
	return std::make_shared<
			opennova::terrain::TerrainStaticShadowAlphaPyramid>();
}

class NullTextureProvider final :
		public opennova::terrain::TerrainStaticShadowTextureProvider {
public:
	std::shared_ptr<const opennova::terrain::TerrainStaticShadowAlphaPyramid>
	load_alpha(std::string_view) override {
		return {};
	}
};

} // namespace

int main() {
	using namespace opennova::terrain;

	TerrainStaticShadowResolvedGeometry geometry;
	geometry.control_register_names = {"FLICKER"};
	TerrainStaticShadowResolvedMaterial material;
	material.samples_diffuse_alpha = true;
	material.uses_material_alpha = true;
	material.diffuse_alpha_frames.push_back(alpha_identity());
	material.runtime_material.alpha_gen.style = 113;
	material.runtime_material.alpha_gen.reg = 0;
	material.runtime_material.alpha_gen.start = 10;
	material.runtime_material.alpha_gen.end = 210;
	material.runtime_material.u_params.style = 114;
	material.runtime_material.u_params.reg = 0;
	material.runtime_material.u_params.end = 2.0f;
	renderer::ControlRegisterValues controls{};
	controls[THREEDI_CTRL_FLICKER] = 32768;
	const TerrainStaticShadowMaterialState controlled =
			terrain_static_shadow_evaluate_material(
					geometry, material, 9999, controls);
	if (!expect(controlled.issues == kTerrainStaticShadowUnsupportedNone,
			"controlled AlphaGen/UV must be an exact supported state") ||
			!expect(near(controlled.alpha_scale, 110.0f / 255.0f),
					"projected _FFP AlphaGen must use the shared signed CTRL evaluator") ||
			!expect(near(controlled.uv.m00, 1.0f) &&
						near(controlled.uv.m20, 1.0f),
					"projected alpha UVs must retain the complete controlled transform")) {
		return 1;
	}

	TerrainStaticShadowResolvedMaterial file_effect = material;
	file_effect.uses_material_alpha = false;
	const TerrainStaticShadowMaterialState forced_opaque =
			terrain_static_shadow_evaluate_material(
					geometry, file_effect, 9999, controls);
	if (!expect(near(forced_opaque.alpha_scale, 1.0f),
			"file-effect PROJSHAD declarations must ignore _FFP AlphaGen")) {
		return 1;
	}

	TerrainStaticShadowResolvedMaterial time_flipbook;
	time_flipbook.samples_diffuse_alpha = true;
	time_flipbook.runtime_material.animation.num_frames = 2;
	time_flipbook.runtime_material.animation.animation_type = 0;
	time_flipbook.runtime_material.animation.cycle_frame_time = 100;
	const auto frame_zero = alpha_identity();
	const auto frame_one = alpha_identity();
	time_flipbook.diffuse_alpha_frames = {frame_zero, frame_one};
	const TerrainStaticShadowMaterialState before =
			terrain_static_shadow_evaluate_material(
					geometry, time_flipbook, 99, controls);
	const TerrainStaticShadowMaterialState after =
			terrain_static_shadow_evaluate_material(
					geometry, time_flipbook, 100, controls);
	if (!expect(before.diffuse_frame == 0 &&
					before.alpha_texture == frame_zero.get(),
			"time flipbook must retain frame zero until its retail millisecond boundary") ||
			!expect(after.diffuse_frame == 1 &&
					after.alpha_texture == frame_one.get(),
					"non-team time flipbook must select its next authored alpha frame")) {
		return 1;
	}

	geometry.control_register_names = {"TEX_TEAM"};
	TerrainStaticShadowResolvedMaterial team_flipbook = time_flipbook;
	team_flipbook.runtime_material.animation.animation_type = 1;
	team_flipbook.runtime_material.animation.cycle_frame_time = 0;
	controls = {};
	controls[THREEDI_CTRL_TEX_TEAM] = 1;
	const TerrainStaticShadowMaterialState team =
			terrain_static_shadow_evaluate_material(
					geometry, team_flipbook, 0, controls);
	if (!expect(team.diffuse_frame == 1 &&
					team.alpha_texture == frame_one.get(),
			"TEX_TEAM must use the same discrete global-bus selector as object rendering")) {
		return 1;
	}

	team_flipbook.diffuse_alpha_frames[1].reset();
	const TerrainStaticShadowMaterialState missing =
			terrain_static_shadow_evaluate_material(
					geometry, team_flipbook, 0, controls);
	if (!expect((missing.issues &
					kTerrainStaticShadowUnsupportedMissingAlphaTexture) != 0,
			"only the selected missing flipbook frame must fail closed")) {
		return 1;
	}

	Threedi3di3 malformed{};
	malformed.material_count = 1;
	NullTextureProvider textures;
	if (!expect(resolve_terrain_static_shadow_geometry(
				malformed, "malformed.3di", textures) == nullptr,
			"a declared material table without storage must fail closed")) {
		return 1;
	}

	std::puts("terrain_static_shadow_material_test: OK");
	return 0;
}
