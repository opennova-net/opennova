// The fixed-function pipeline states (runtime/renderer/pipeline_state): the
// D3D9 render-state words and every state the particle pipelines, the Q3
// bloom-source copies, the slot captures' PROJSHAD pass and the scene overlay
// tail describe, pinned factor by factor.
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 (case 1/2
//  SRCBLEND=ONE), @ 0x5e85a9 (DESTBLEND=INVSRCALPHA), @ 0x5e84dc/@ 0x5e84d8
//  (SRCALPHA/ONE), @ 0x5E8390..0x5E83A5 (the thermal ZERO/INVSRCCOLOR);
//  CRenderBatchQueue_FlushBatches @ 0x5da0fa..0x5da137 (CULLMODE CCW)]

#include <runtime/renderer/pipeline_state.h>

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/particle_frame.h>
#include <runtime/renderer/q3_frame.h>
#include <runtime/renderer/scene_overlay.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>

using namespace opennova::renderer;

static int failures = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++failures;                                                    \
		}                                                                  \
	} while (0)

namespace {

using F = BlendFactor;

bool blend_is(const BlendState &blend, bool enabled, F src_color, F dst_color, F src_alpha,
		F dst_alpha) {
	return blend.enabled == enabled && blend.src_color == src_color &&
			blend.dst_color == dst_color && blend.src_alpha == src_alpha &&
			blend.dst_alpha == dst_alpha;
}

// Blending off: the device reads no factor, so only the switch is pinned.
bool blend_off(const BlendState &blend) {
	return !blend.enabled;
}

bool depth_is(const FixedFunctionState &state, bool test, bool write) {
	return state.depth_test == test && state.depth_write == write &&
			state.depth_compare == DepthCompare::LessEqual;
}

void test_the_device_words() {
	// D3DBLEND.
	CHECK(static_cast<int>(F::Zero) == 1);
	CHECK(static_cast<int>(F::One) == 2);
	CHECK(static_cast<int>(F::SrcColor) == 3);
	CHECK(static_cast<int>(F::InvSrcColor) == 4);
	CHECK(static_cast<int>(F::SrcAlpha) == 5);
	CHECK(static_cast<int>(F::InvSrcAlpha) == 6);
	CHECK(static_cast<int>(F::DestAlpha) == 7);
	CHECK(static_cast<int>(F::InvDestAlpha) == 8);
	CHECK(static_cast<int>(F::DestColor) == 9);
	CHECK(static_cast<int>(F::InvDestColor) == 10);
	// D3DCULL, D3DCMP.
	CHECK(static_cast<int>(CullMode::None) == 1);
	CHECK(static_cast<int>(CullMode::CW) == 2);
	CHECK(static_cast<int>(CullMode::CCW) == 3);
	CHECK(static_cast<int>(DepthCompare::LessEqual) == 4);
	CHECK(static_cast<int>(DepthCompare::Always) == 8);
	// The D3D9 defaults.
	const FixedFunctionState defaults;
	CHECK(blend_is(defaults.blend, false, F::One, F::Zero, F::One, F::Zero));
	CHECK(defaults.cull == CullMode::CCW);
	CHECK(depth_is(defaults, true, true));
}

void test_the_state_key_tells_every_field_apart() {
	const FixedFunctionState base;
	std::set<std::uint32_t> keys;
	keys.insert(fixed_function_state_key(base));
	FixedFunctionState s = base;
	s.blend.enabled = true;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.blend.src_color = F::InvDestColor;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.blend.dst_color = F::InvDestColor;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.blend.src_alpha = F::InvDestColor;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.blend.dst_alpha = F::InvDestColor;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.cull = CullMode::None;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.depth_test = false;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.depth_write = false;
	keys.insert(fixed_function_state_key(s));
	s = base;
	s.depth_compare = DepthCompare::Always;
	keys.insert(fixed_function_state_key(s));
	CHECK(keys.size() == 10);
}

void test_the_particle_pipelines() {
	constexpr ParticleThermalMaterial kMaterials[] = {
		ParticleThermalMaterial::Primary,
		ParticleThermalMaterial::InvertedBlend,
		ParticleThermalMaterial::DarkeningModulate,
	};
	constexpr ParticlePipeline kPipelines[] = {
		ParticlePipeline::Blend, ParticlePipeline::Additive, ParticlePipeline::Premult,
		ParticlePipeline::Bump, ParticlePipeline::Mod, ParticlePipeline::Mod2x,
		ParticlePipeline::Bumpadd, ParticlePipeline::Distort,
	};
	for (ParticlePipeline pipeline : kPipelines) {
		for (ParticleThermalMaterial material : kMaterials) {
			const FixedFunctionState state = particle_pipeline_state(pipeline, material);
			CHECK(state.blend.enabled);
			CHECK(state.cull == CullMode::None);
			CHECK(depth_is(state, true, false));
			const BlendState &b = state.blend;
			if (material == ParticleThermalMaterial::DarkeningModulate) {
				// ZERO / INVSRCCOLOR, the alpha its component.
				CHECK(blend_is(b, true, F::Zero, F::InvSrcColor, F::Zero, F::InvSrcAlpha));
				continue;
			}
			switch (pipeline) {
				case ParticlePipeline::Blend:
				case ParticlePipeline::Bump:
				case ParticlePipeline::Distort:
					CHECK(blend_is(b, true, F::SrcAlpha, F::InvSrcAlpha, F::SrcAlpha,
							F::InvSrcAlpha));
					break;
				case ParticlePipeline::Additive:
				case ParticlePipeline::Premult:
					CHECK(blend_is(b, true, F::One, F::InvSrcAlpha, F::One, F::InvSrcAlpha));
					break;
				case ParticlePipeline::Bumpadd:
					CHECK(blend_is(b, true, F::SrcAlpha, F::One, F::SrcAlpha, F::One));
					break;
				case ParticlePipeline::Mod:
					CHECK(blend_is(b, true, F::DestColor, F::Zero, F::DestAlpha, F::Zero));
					break;
				case ParticlePipeline::Mod2x:
					CHECK(blend_is(b, true, F::DestColor, F::SrcColor, F::DestAlpha,
							F::SrcAlpha));
					break;
			}
		}
	}
	// The thermal frame binds the darkening secondary only for Additive and Premult.
	CHECK(particle_thermal_material(ParticlePipeline::Additive, true) ==
			ParticleThermalMaterial::DarkeningModulate);
	CHECK(particle_thermal_material(ParticlePipeline::Blend, true) ==
			ParticleThermalMaterial::InvertedBlend);
}

Q3DrawCommand q3_command(Q3Technique technique, ObjectBlendMode object_blend,
		bool two_sided, ObjectBlendMode celestial_blend) {
	Q3DrawCommand command;
	command.technique = technique;
	command.object.classification.blend = object_blend;
	command.object.classification.is_two_sided = two_sided;
	command.celestial.blend = celestial_blend;
	return command;
}

void test_the_q3_techniques() {
	constexpr ObjectBlendMode kBlends[] = {
		ObjectBlendMode::Opaque, ObjectBlendMode::AlphaBlend, ObjectBlendMode::Additive,
		ObjectBlendMode::Multiplicative,
	};
	for (bool two_sided : {false, true}) {
		const CullMode object_cull = two_sided ? CullMode::None : CullMode::CCW;
		for (ObjectBlendMode blend : kBlends) {
			// A NormalCopy takes its material blend; the alpha adds ONE / ONE.
			const FixedFunctionState copy = q3_pipeline_state(q3_command(
					Q3Technique::NormalCopy, blend, two_sided, ObjectBlendMode::Additive));
			CHECK(copy.cull == object_cull);
			CHECK(depth_is(copy, true, false));
			if (blend == ObjectBlendMode::AlphaBlend)
				CHECK(blend_is(copy.blend, true, F::SrcAlpha, F::InvSrcAlpha, F::One, F::One));
			else if (blend == ObjectBlendMode::Additive)
				CHECK(blend_is(copy.blend, true, F::One, F::One, F::One, F::One));
			else
				CHECK(blend_off(copy.blend)); // _OP; an unwitnessed multiplicative replaces

			// The glass GLOW copy adds whatever the material blend.
			const FixedFunctionState glass = q3_pipeline_state(q3_command(
					Q3Technique::RotatedSpecularGlass, blend, two_sided,
					ObjectBlendMode::Additive));
			CHECK(blend_is(glass.blend, true, F::One, F::One, F::One, F::One));
			CHECK(glass.cull == object_cull);
			CHECK(depth_is(glass, true, false));

			// The NV water redraw: ONE + dst x SRCALPHA, both sides.
			const FixedFunctionState water = q3_pipeline_state(q3_command(
					Q3Technique::WaterNightVision, blend, two_sided, ObjectBlendMode::Additive));
			CHECK(blend_is(water.blend, true, F::One, F::SrcAlpha, F::One, F::One));
			CHECK(water.cull == CullMode::None);
			CHECK(depth_is(water, true, false));

			// The discs and the glow: the body's material blend, both sides.
			for (Q3Technique technique : {Q3Technique::CelestialBody, Q3Technique::SunGlow}) {
				const FixedFunctionState body = q3_pipeline_state(q3_command(technique,
						ObjectBlendMode::Additive, two_sided, blend));
				CHECK(body.cull == CullMode::None);
				CHECK(depth_is(body, true, false));
				if (blend == ObjectBlendMode::AlphaBlend)
					CHECK(blend_is(body.blend, true, F::SrcAlpha, F::InvSrcAlpha, F::One,
							F::One));
				else if (blend == ObjectBlendMode::Additive)
					CHECK(blend_is(body.blend, true, F::One, F::One, F::One, F::One));
				else
					CHECK(blend_off(body.blend));
			}
		}
	}
}

void test_the_slot_capture_projshad_pass() {
	constexpr ObjectProjectedShadowPolicy kPolicies[] = {
		ObjectProjectedShadowPolicy::NoPass, ObjectProjectedShadowPolicy::MaterialBlend,
		ObjectProjectedShadowPolicy::Opaque,
	};
	constexpr ObjectBlendMode kBlends[] = {
		ObjectBlendMode::Opaque, ObjectBlendMode::AlphaBlend, ObjectBlendMode::Additive,
		ObjectBlendMode::Multiplicative,
	};
	for (ObjectProjectedShadowPolicy policy : kPolicies) {
		for (ObjectBlendMode blend : kBlends) {
			for (bool two_sided : {false, true}) {
				const FixedFunctionState state =
						object_projected_shadow_pipeline_state(policy, blend, two_sided);
				CHECK(state.cull == (two_sided ? CullMode::None : CullMode::CCW));
				CHECK(depth_is(state, true, true));
				if (policy == ObjectProjectedShadowPolicy::MaterialBlend &&
						blend == ObjectBlendMode::AlphaBlend)
					CHECK(blend_is(state.blend, true, F::SrcAlpha, F::InvSrcAlpha, F::SrcAlpha,
							F::InvSrcAlpha));
				else
					CHECK(blend_off(state.blend));
			}
		}
	}
}

void test_the_scene_overlay_states() {
	for (const SceneOverlayPipelineState &listed : kSceneOverlayPipelineStates) {
		const FixedFunctionState state = scene_overlay_pipeline_state(listed);
		CHECK(state.cull == CullMode::None);
		CHECK(depth_is(state, listed.depth_test, false));
		const BlendState &b = state.blend;
		switch (listed.shading) {
			case SceneOverlayShading::AdditiveModulate:
			case SceneOverlayShading::SelfLumAdditive:
			case SceneOverlayShading::RibbonStock:
				CHECK(blend_is(b, true, F::One, F::One, F::Zero, F::One));
				break;
			case SceneOverlayShading::DimMultiply:
				CHECK(blend_is(b, true, F::DestColor, F::Zero, F::Zero, F::One));
				break;
			case SceneOverlayShading::NvgLaser:
				CHECK(blend_is(b, true, F::SrcAlpha, F::One, F::Zero, F::One));
				break;
			case SceneOverlayShading::Modulate2xBlend:
			case SceneOverlayShading::FlatBlend:
			case SceneOverlayShading::RibbonSmoke:
				CHECK(blend_is(b, true, F::SrcAlpha, F::InvSrcAlpha, F::SrcAlpha,
						F::InvSrcAlpha));
				break;
		}
	}
	// ALWAYS (the glare, the glint, the murk, the dim) is no test.
	CHECK(!scene_overlay_pipeline_state({SceneOverlayShading::FlatBlend, false}).depth_test);
	CHECK(scene_overlay_pipeline_state({SceneOverlayShading::Modulate2xBlend, true}).depth_test);
}

} // namespace

int main() {
	test_the_device_words();
	test_the_state_key_tells_every_field_apart();
	test_the_particle_pipelines();
	test_the_q3_techniques();
	test_the_slot_capture_projshad_pass();
	test_the_scene_overlay_states();
	if (failures != 0) {
		std::printf("renderer_pipeline_state: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_pipeline_state: ok\n");
	return 0;
}
