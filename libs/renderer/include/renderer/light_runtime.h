#pragma once

#include <array>
#include <cstdint>

namespace renderer {

// ---------------------------------------------------------------------------
// OED-side helpers (the editor's light preview path). These predate REN-5 and
// stay OED-cited; the runtime attenuation below shares their math.

// Original OED reference: PrepareLightParams @ 0x46A500.
std::array<float, 4> build_oed_light_attenuation(float light_range);

// Build a depth-mask plane (a, b, c, d so that ax + by + cz + d = 0 marks
// the slice of space occluded by the light cone).  Used by spotlight
// projection passes; same math runs server-side for physics/AI cone tests.
std::array<float, 4> build_depth_mask_plane(float pos_x, float pos_y, float pos_z,
                                             float dir_x, float dir_y, float dir_z,
                                             float atten_start, float atten_end);

// Convert a raw view-projection matrix into a shadow-sample matrix (offsets
// xy by 0.5 with a half-texel bias).  shadow_resolution=0 falls back to 512.
std::array<float, 16> build_shadow_sample_matrix(const std::array<float, 16> &raw_view_proj,
                                                  uint32_t shadow_resolution = 512);

// ---------------------------------------------------------------------------
// The runtime lighting chain (REN-5), witnessed in retail Jointops.exe.
// RE record: docs/render/render-lighting-re.md (D-RLIT catalog).
//
// Color convention: packed engine colors are 0x00RRGGBB (byte 2 = R, byte 1 =
// G, byte 0 = B); unpacked floats are linear 0..1 (byte / 255) unless a
// function says otherwise.

// The modulator -> shader gain unpack: byte / 64 per channel, 64 = identity
// 1.0. The modulator block carries the iris auto-exposure (env #17); this is
// how it reaches the shader path (ColorSrcGlobalGain, handle slot 232) and
// the effects/foliage/point-light renderers.
// [orig: Render_UnpackModulatorToLightScale @ 0x58db30 ->
//  Render_LightScaleR/G/B @ 0x8409f4..fc, consumed by apply_shader_parameters
//  @ 0x58e05d; EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0 ->
//  EffectWorld_AmbientScaleR/G/B @ 0x840b24..2c]
std::array<float, 3> unpack_modulator_scale(uint32_t packed_rgb);

// The per-scene-pass world lighting block, built from the environment color
// blocks' post-modulator render colors ([0] slots) and stored on the batch
// context for every entity draw.
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 ->
//  RenderBatchCtx_StoreLightingConstants @ 0x5d89e0 (ctx+112..236)]
struct WorldLightingInputs {
	uint32_t light_packed = 0;   // Env_LightBlock[0]  @ 0x26c6574 (sun by day, moon by night)
	uint32_t sky_packed = 0;     // Env_SkyBlock[0]    @ 0x26c65dc
	uint32_t ground_packed = 0;  // Env_GroundBlock[0] @ 0x26c6610
	uint32_t ceiling_packed = 0; // Env_CeilingBlock[0] @ 0x26c6470 (indoor "sky")
	uint32_t floor_packed = 0;   // Env_FloorBlock[0]  @ 0x26c64d8 (indoor "ground")
	// The environment light direction (sun/moon), pre-negation
	// [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	std::array<float, 3> light_dir = { 0.0f, 0.0f, 0.0f };
	// NVG hemisphere rewrite: sky/ground/ceiling/floor become
	// c * 0.25*f + modulator * f/640 with f = (level + 1) * 0.2
	// [orig: @ 0x5c8205..0x5c82e9; g_NVGActive @ 0xb7654c,
	//  g_NVGBrightnessLevel @ 0xb76550].
	bool nvg_hemi_rewrite = false;
	int nvg_level = 0;              // 0..4
	uint32_t modulator_packed = 0;  // Env_ModulatorBlock[0] @ 0x26c6644
	// The vehicle-scope grey override: dir 0.1, everything else 0.5, dir
	// disabled [orig: @ 0x5c8389..0x5c843c, gated on Player_CanFireWeapon &&
	// Player_IsVehicleSeatHasFlag4].
	bool vehicle_scope_grey = false;
	// The NVG world-dim override (the function's bool arg): everything 0.25,
	// dir zeroed and disabled [orig: @ 0x5c8448..0x5c84f0].
	bool nvg_world_dim = false;
};

struct WorldLightingBlock {
	// ctx+112: when false the store zeroes dir_color (the overrides clear it)
	// [orig: @ 0x5d8b06..0x5d8b33].
	bool dir_enabled = true;
	std::array<float, 3> dir_color = { 0.0f, 0.0f, 0.0f };  // ctx+116..124
	std::array<float, 3> dir = { 0.0f, 0.0f, 0.0f };        // ctx+132..140, -normalize(light_dir)
	std::array<float, 3> hemi_sky = { 0.0f, 0.0f, 0.0f };    // ctx+144..152
	std::array<float, 3> hemi_ground = { 0.0f, 0.0f, 0.0f }; // ctx+160..168
	std::array<float, 3> floor_color = { 0.0f, 0.0f, 0.0f }; // ctx+176..184
	std::array<float, 3> ceiling_color = { 0.0f, 0.0f, 0.0f }; // ctx+192..200
	// Derived at store time [orig: RenderBatchCtx_StoreLightingConstants
	// @ 0x5d89e0]: ctx+208 = (ceiling + floor) / 2, ctx+224 = (sky + ground)/2.
	std::array<float, 3> indoor_ambient = { 0.0f, 0.0f, 0.0f };
	std::array<float, 3> outdoor_ambient = { 0.0f, 0.0f, 0.0f };
};

WorldLightingBlock build_world_lighting(const WorldLightingInputs &in);

// The per-entity shader uniform set (the reader side of the ctx block) —
// .fx parameter slots 227 DirLightColor / 228 HemiGroundColor /
// 229 HemiSkyColor / 230 AmbientColor (handle names resolved at load
// [orig: HLSLEffect_LoadFromFile @ 0x5af417..0x5af49e]).
// interior_lerp = batch entry flag bit 1 (submit 0x80, interior-parented
// entities + the armed viewmodel); interior_daylight = the parent interior's
// daylight-openness float (interior model data +536, captured into the
// render-state stack aux -> entry[10]); effect_scale = the per-entity
// sun-visibility factor (entry[9]).
// [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0 — lerp branch
//  @ 0x5d9a6a.., direct branch @ 0x5d9db5..]
struct EntityLightingUniforms {
	std::array<float, 3> dir_color = { 0.0f, 0.0f, 0.0f };   // slot 227 (x scale [x t])
	std::array<float, 3> hemi_ground = { 0.0f, 0.0f, 0.0f }; // slot 228
	std::array<float, 3> hemi_sky = { 0.0f, 0.0f, 0.0f };    // slot 229
	std::array<float, 3> ambient = { 0.0f, 0.0f, 0.0f };     // slot 230
};

EntityLightingUniforms compute_entity_lighting(const WorldLightingBlock &block,
                                               float effect_scale,
                                               bool interior_lerp,
                                               float interior_daylight);

// The fixed-function vertex-lighting evaluation the D3D light set produces:
//   clamp01(ambient                                  (MaterialEmissive = AmbientColor)
//         + dir_color * max(0, dot(N, L))            (D3D light 0, directional)
//         + (hemi_sky - ambient)    * max(0,  N.y)   (D3D light 2, dir {0,-1,0})
//         + (hemi_ground - ambient) * max(0, -N.y))  (D3D light 3, dir {0,+1,0})
// The two hemisphere lights carry the DELTA colors so a +Y normal resolves to
// hemi_sky, a -Y normal to hemi_ground, and the horizon to the average —
// identically mix(hemi_ground, hemi_sky, N.y * 0.5 + 0.5) + dir + ambient
// pivot. `to_light` points TOWARD the light (= -block.dir).
// [orig: Lighting_SetHemisphereD3DLights @ 0x5d8cb0; light 0 setup
//  @ 0x5d9ce2..0x5d9d76 (defaults g_DefaultLightDir {0,1,0} @ 0x8437e0 when
//  dir_enabled is false — with the color already zeroed)]
std::array<float, 3> ff_vertex_light(const EntityLightingUniforms &u,
                                     const std::array<float, 3> &normal,
                                     const std::array<float, 3> &to_light);

// The fixed-function output combine is MODULATE2X(Texture, Diffuse): pixel =
// texture * saturated vertex light * 2 [orig: _FFP.fx TBoringFFP TSSColor;
// decode_mode_color_stage @ 0x681080 mode 0x600]. VS_TRACER alone modulates
// 1x (MODULATE, unlit — the OSCAP_VIEW_FADE path).
inline constexpr float kFFModulate2x = 2.0f;

// The per-entity sun-visibility factor: 3 raycasts toward the light, each
// blocked ray steps 4 -> 3 -> 2 -> 1, result * 0.25 (so 1.0 fully sunlit,
// floor 0.25 under full cover). Multiplies DirLightColor via the render-state
// stack effectScale (entry[9]).
// [orig: Entity_ComputeSunVisibility @ 0x5c6800; stack write @ 0x5c7fa5]
float sun_visibility_factor(int blocked_rays);

// Dynamic point-light color: base rgb x intensity x the modulator ambient
// scale (the iris chain reaching dynamic lights), optionally x an RgbGen
// animated color evaluated by the caller. The D3D-light path (up to 4
// concurrent hardware lights) adds a 1.5x boost the shader-constant path
// does not have.
// [orig: Light_GetPointLightParams @ 0x5a9180 (shader constants);
//  Light_FillD3DPointLight @ 0x5aa450 (D3DLIGHT9, the 1.5x)]
std::array<float, 3> point_light_color(const std::array<float, 3> &rgb,
                                       float intensity,
                                       const std::array<float, 3> &modulator_scale,
                                       bool d3d_light_path);

// Runtime point-light attenuation: {atten0 1, atten1 0, atten2 15 / range^2,
// 1} with range = fixed_range * 1.25 / 65536 world units — byte-identical in
// shape to the OED preview's build_oed_light_attenuation above.
// [orig: Light_GetPointLightParams @ 0x5a9251..0x5a9272;
//  Light_FillD3DPointLight @ 0x5aa53b..0x5aa553]
std::array<float, 4> point_light_attenuation(int32_t range_fixed);

// The terrain-surface pixel light: colormap-alpha (the baked per-texel sun
// mask) blends the light color over the sky color —
//   lit = sun_mask * light + sky
// (the ps.1.1 form is ((t0.a * c1 + c0) / 2) followed by the MODULATE2X
// doubling; the /2 and x2 cancel). c1 = the light block, c0 = the sky block,
// both /255 — pushed per terrain draw; the foliage/sector-model blend PS
// inherits the same two device constants (its combine is
// t0 x (t1 x (t1.a*c1 + c0)) x v0 x 8 with t1 = the planar lightmap tile).
// [orig: compile_terrain_pixel_shaders @ 0x605260 (the shared PS shape);
//  terrain_setup_lighting_and_shader @ 0x604420 (c0 <- PolyTrn_PSConstC0_Sky,
//  c1 <- PolyTrn_PSConstC1_Light); init_terrain_lighting_color_ramps
//  @ 0x604ee0 <- Render_TerrainScene @ 0x610c80 (Env_LightBlock/Env_SkyBlock;
//  NVG / vehicle-scope overrides); Foliage_CreateLightmapBlendPS @ 0x5ff7a0]
std::array<float, 3> terrain_surface_light(float sun_mask,
                                           const std::array<float, 3> &light,
                                           const std::array<float, 3> &sky);

}  // namespace renderer
