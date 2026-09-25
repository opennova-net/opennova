#pragma once

#include <cstdint>
#include <vector>

// Water-side render state math: the noise/DuDv texture tables and the
// screen-space water strip tessellation.

namespace opennova::env {

// ---------------------------------------------------------------------------
// Water surface — the witnessed pipeline of render_water_surface @ 0x5c32c0
// (the frame pass: FrameFX_RenderGlowSource @ 0x582a5d / @ 0x610650 call it per
// side; camera-side gate against Env_WaterHeightFixed). Per frame it
// regenerates the animated noise texture pair [orig: Water_GenerateNoiseTextures
// @ 0x5c0360], derives the strip depth curve from the SMOOTHED fog distance
// [orig: @ 0x5c332d..0x5c3362], then draws the screen-marched water strips
// (render_water_strip @ 0x5c1d60 low detail with sin-table Y displacement;
// render_water_strip_detailed @ 0x5c27d0 high detail, FLAT strips — the
// animated textures carry the look). Both tiers texture the noise pair at the
// ABSOLUTE render-basis world x/32, z/32 (texcoords 0 and 3, see
// WaterStripRows::uv0): no scale, bias, offset or scroll reaches a texcoord.
// render_water_surface also stores two cloud-scroll "offsets" and a zero pair
// [orig: Water_UvOffsetU_Unread/V @ 0x5c33b9/@ 0x5c33db, flt_29169E8/EC @ 0x5c3379/
// @ 0x5c3385] that nothing in the binary reads back; they are not ported.
// This section owns the texture + depth math both paths share.

inline constexpr int kWaterNoiseSize = 128; // 128x128 field and textures

// The static tables built once at renderer init [orig:
// Water_InitNoiseFieldAndSineLut @ 0x5c01a0, called from
// Water_InitSurfaceShaders @ 0x5c19b0 (call site @ 0x5c19f8; renamed from the
// kong misnomer Terrain_InitShaders at REN-4 - it builds the WATER surface
// shader/material set)]: a normalized random field and the 128 + 64*sin(2*pi*i/256)
// byte LUT (truncating float->int like the original ftol).
struct WaterNoiseTables {
	uint8_t field[kWaterNoiseSize * kWaterNoiseSize]; // Water_NoiseField
	uint8_t sine_lut[256];                            // Water_SineLut
};

// One step of the init PRNG [orig: PRNG_Next16 @ 0x6130a0]:
// state = rol4(state + rol11(state)) ^ 1; the caller consumes state & 0xFFFF.
uint32_t water_noise_prng_step(uint32_t state);

// Builds the tables with the witnessed algorithm. Retail's field CONTENT
// depends on the shared PRNG's state at Water_InitSurfaceShaders time (a
// value-history quirk, recorded in env-tod-re.md); the reimpl seeds from the
// boot state 0 for a deterministic, witnessed-faithful instance.
WaterNoiseTables water_init_noise_tables();

// Passes 1+2 of Water_GenerateNoiseTextures: animate the field through the
// LUT (per byte: lut[(uint8)(field + (counter << (field & 1)))] — two speed
// classes), then the toroidal 9-tap kernel (3x corners + 4x cross, >> 5),
// folded to a ridge intensity i = max(0, 128 - |k - 128|) and packed as
// A = 255 - max(0, i*i >> 9), R = G = B = i. out_pixels holds 128*128 ARGB.
void water_noise_color_pixels(uint32_t *out_pixels, const WaterNoiseTables &tables,
                              uint32_t frame_counter);

// Pass 3: the DuDv/normal map [orig: @ 0x5c07c2..0x5c087d, MMX]: per pixel,
// from the color texture's intensity byte c (blue channel):
// R = sat8(2 * satsub8(c - c_up)) + 0x80 (wrapping), G = same against c_left,
// B = 0xFF, A = 0; rows and columns wrap toroidally.
void water_noise_normal_pixels(uint32_t *out_pixels, const uint32_t *color_pixels);

// The strip depth curve [orig: render_water_surface @ 0x5c332d..0x5c3362]:
// scale = 0.99996948 * w / (w - 0.2) with w = the INTEGER part of the smoothed
// fog distance (movsx of the word at Env_FogDistCurrent+2 @ 0x5c332d);
// bias = 0.2 * scale; stored to flt_8412B0 @ 0x5c3356 / flt_8412B4 @ 0x5c3362.
// Their only readers are the per-vertex depth chains of both strip tiers
// (water_strip_depth; detailed @ 0x5c2c0e/@ 0x5c2c8b/@ 0x5c2cf7, low tier
// @ 0x5c2201/@ 0x5c22a6/@ 0x5c230a): z = scale - bias/t is an arithmetic
// replica of the scene projection with near 0.2 and far = w.
struct WaterDepthCurve {
	float scale = 1.0f;
	float bias = 0.0f;
};

// The scene projection's near plane, the 0.2 of the curve above: flt_7C3340
// (fld @ 0x5c333e) is the constant Render_SetProjectionDepthRange stores to
// g_ProjectionNearZ [orig: @ 0x58ac04..0x58ac0a]. The scene far plane is the
// same fog word + 1 (Render_ProcessMainSceneFrame @ 0x5ca4ba..0x5ca4d0).
inline constexpr float kWaterSceneNear = 0.2f;

WaterDepthCurve water_depth_curve(float fog_distance_world);

// ---------------------------------------------------------------------------
// Water strip tessellation (env #29) — the screen-space row march of the
// DETAILED water surface tier [orig: render_water_strip_detailed @ 0x5c27d0,
// substrate terrain_project_sector_to_screen @ 0x5c0bf0 + clip_line_to_viewport
// @ 0x5c0a30; caller render_water_surface @ 0x5c3492/@ 0x5c3542]. The water
// plane projects to a screen block, rows advance along the screen march
// direction with an adaptive per-row stride, each row's screen line clips to
// the viewport, and the row emits 3 vertices (left / mid / right of the
// clipped span) unprojected through the cached inverted view matrix
// [orig: Math_InvertMatrix4x4_Float_ToStatic @ 0x611960]. The LOW tier
// (render_water_strip @ 0x5c1d60, water detail <= 1: 40-byte verts, sin-table
// Y displacement, the dbl_7DBF70 = 229.5 alpha-scale swap) is the remaining
// unported variant — the reimpl runs the detailed path (detail > 1).
//
// Caller-arg semantics witnessed at the call sites [orig: @ 0x5c3489..0x5c3497
// camera-above (blend material): (height, 0, nightvision); @ 0x5c3539..0x5c3547
// underwater (opaque material): (height, underwater_view, nightvision)]: arg 2
// is the UNDERWATER-VIEW pass (env-tod-re.md's "isReflection" reading — the
// V-flip/murk-skip variant is the underwater view), arg 3 the NIGHTVISION
// redraw.

// The camera/view state the originals read from renderer globals. All floats
// are render-basis (d3d = (-engY, engZ, engX)); matrices are D3D row-major
// row-vector (v' = v * M), so world-space camera basis vectors sit in the
// view matrix COLUMNS.
struct WaterStripView {
	float view[16];     // world->view [orig: viewMatrix @ 0xA7845C]
	float view_inv[16]; // its inverse, cached per pass [orig: @ 0x611960 result]
	// Embedder projection converted to the render basis/row-vector convention.
	// X/Y clip rows and clip-W are complete: perspective/frustum use depth W,
	// orthographic uses constant W, and the translation/shear terms preserve
	// off-center embedder projections. The witnessed retail path is the centered
	// perspective subset [orig: mat @ 0x2721980; m11 @ 0x2721994].
	float proj[16];
	// Camera world-basis rows of the render context's camera matrix
	// [orig: flt_27219C0 row 0 (right) / row 2 (forward), Math_CopyVec3Row0/2
	// @ 0x611fb0/@ 0x611f70].
	float cam_right[3] = {1.0f, 0.0f, 0.0f};
	float cam_forward[3] = {0.0f, 0.0f, 1.0f};
	// Camera position, render basis, 16.16 fixed like the camera block the
	// originals fild [orig: 0xA78364 (eng X = render z) / 0xA78368 (eng Y,
	// negated = render x) / 0xA7836C (eng Z = render y)].
	int32_t cam_x_fp = 0;
	int32_t cam_y_fp = 0;
	int32_t cam_z_fp = 0;
	// Viewport rect + center, pixels [orig: 0xA78384/0xA78388 min,
	// 0xA7838C/0xA78390 max, 0xA783A4/0xA783A8 center]. The projection maps
	// x/(2w) across (max - min); the clip rect right/bottom edges are max + 1.
	int32_t vp_min_x = 0;
	int32_t vp_min_y = 0;
	int32_t vp_max_x = 0;
	int32_t vp_max_y = 0;
	int32_t vp_center_x = 0;
	int32_t vp_center_y = 0;
	// The pass fog end distance, 16.16 [orig: Environment_GetFogEndDistance
	// @ 0x57e3e0, called with the underwater flag @ 0x5c28a2].
	int32_t fog_end_fp = 0;
};

// The camera/viewport half of a strip view from the embedder's camera. Inputs
// are render-basis floats (the Godot world coincides componentwise with the
// render basis, both (-engY, engZ, engX) of the engine axes [orig:
// Math_FixedPointToFloat3_YNegated @ 0x611210]); proj_columns is the
// embedder's 4x4 flattened column by column ([input][output]).
//  - view: the D3D row-vector view matrix, the camera's world-basis vectors in
//    the COLUMNS (0 right / 1 up / 2 forward) and row 3 = -dot(axis, eye)
//    [orig: viewMatrix @ 0xA7845C, consumed row-vector by
//    Math_TransformPoint4ByMatrix4x4_Float @ 0x612e80];
//  - view_inv: retail inverts the cached view numerically per pass [orig:
//    Math_InvertMatrix4x4_Float_ToStatic @ 0x611960]; for the rigid camera
//    that inverse IS the transposed rotation with the eye in row 3, built here
//    from the same source data;
//  - proj: the complete embedder matrix (orthographic and off-center frustums
//    included) with the view-Z input sign flipped, since the embedder looks
//    down -Z while the D3D/render view measures +forward [orig: mat
//    @ 0x2721980; m11 read @ 0x2721994];
//  - cam_right / cam_forward: the basis rows for the texm3x2 bump rows [orig:
//    flt_27219C0 row 0 / row 2, Math_CopyVec3Row0/2 @ 0x611fb0 / @ 0x611f70];
//  - cam_*_fp: the 16.16 camera block [orig: 0xA78364 / 0xA78368 / 0xA7836C];
//  - vp_*: min 0, max = px - 1 (the clip rect's right/bottom edges are
//    max + 1 = px), center = px / 2 [orig: 0xA78384..0xA783A8];
//  - fog_end_fp: the pass fog end, 16.16, clamped to one unit because the row
//    colors integer-divide by it [orig: Environment_GetFogEndDistance
//    @ 0x57e3e0, fetched with the underwater flag @ 0x5c28a2].
void water_strip_view_from_camera(const float right[3], const float up[3],
                                  const float forward[3], const float eye[3],
                                  const float proj_columns[16], int viewport_w,
                                  int viewport_h, float fog_end_world,
                                  WaterStripView &out);

// The 40-byte screen block [orig: terrain_project_sector_to_screen @ 0x5c0bf0]:
// the water plane at the strip's height projected at camera +
// horizontal-forward x 2000 -> origin [0..1]; the screen delta of a
// 1000-unit horizontal RIGHT step (view matrix column 0) -> row_delta [2..3]
// (dy forced 1e-6 when 0 [orig: @ 0x5c0deb]); camera + horizontal-forward x
// 1000 -> ref_point [4..5]; march_dir [6..7] = normalize(ref - origin),
// degenerate (0, 1); visible [8] = in-viewport OR row-line-crosses OR the
// halfplane test (the reference point and the viewport center strictly on
// the same side of the row line through the origin — marching in from
// off-screen), with the origin clamped onto the entry edge when only the
// halfplane passes
// [orig: @ 0x5c0fd1..0x5c1023]; origin_row_visible [9] keeps the
// pre-halfplane value.
struct WaterScreenBlock {
	float origin[2] = {0.0f, 0.0f};
	float row_delta[2] = {0.0f, 0.0f};
	float ref_point[2] = {0.0f, 0.0f};
	float march_dir[2] = {0.0f, 0.0f};
	int32_t visible = 0;
	int32_t origin_row_visible = 0;
};

void water_project_plane_to_screen(const WaterStripView &view, int32_t plane_height_fp,
                                   WaterScreenBlock &out);

// One row's screen line clipped to the viewport rect
// [orig: clip_line_to_viewport @ 0x5c0a30]. The line passes through (x0, y0)
// with slope dx_over_dy (the block's row_delta ratio [orig: @ 0x5c291e]);
// endpoints seed at x = min_x and x = max_x + 1 through the 1/slope form,
// then clamp against y = min_y / max_y + 1 through the slope form; crossed
// is true when both endpoints land inside the rect.
struct WaterRowClip {
	float left[2] = {0.0f, 0.0f};  // out[0..1]
	float right[2] = {0.0f, 0.0f}; // out[2..3]
	bool crossed = false;          // out[4]
};

void water_clip_row_to_viewport(const WaterStripView &view, float x0, float y0,
                                float dx_over_dy, WaterRowClip &out);

// The adaptive row-march stride: steps = clamp(int(row_rhw * 500), 2, 9),
// re-derived per row from the row's homogeneous 1/w (= 1/view-depth of the
// left vertex) [orig: @ 0x5c30c7..0x5c30eb, flt_7D6FB4 = 500.0; low tier
// @ 0x5c265a]. The underwater pass never re-derives — it keeps the boot
// stride 4 [orig: var init @ 0x5c286d, the outWidth gate @ 0x5c30c5].
int water_strip_stride(float row_rhw);

// The per-vertex depth ("fog W") chain: rhw = 1/t, z = (t * depth_scale -
// depth_bias) * rhw, clamped to [4.0e-5 (0x3827C5AC), 0.99996948
// (0x3F7FFE00 = 1 - 2^-15)] [orig: @ 0x5c2c0c..0x5c2c4a; clamp constants
// flt_7DBF7C / flt_7C4658]. depth_scale/depth_bias are the WaterDepthCurve
// pair (flt_8412B0/B4): z hits the 0.99996948 * w/(w-0.2) shape, =
// 0.99996948 exactly at t = fog-int w.
float water_strip_depth(float view_depth, float depth_scale, float depth_bias);

// Depth clamp bounds, the same pair in both tiers [orig: flt_7DBF7C
// @ 0x5c2c35 and flt_7C4658 @ 0x5c2c1f; low tier @ 0x5c2212/@ 0x5c2226].
// The upper bound is the scene viewport MaxZ (the render_main_scene clear
// depth reads the same constant @ 0x5c15af).
inline constexpr float kWaterStripDepthMin = 4.0e-5f;     // 0x3827C5AC
inline constexpr float kWaterStripDepthMax = 0.99996948f; // 0x3F7FFE00 = 1 - 2^-15

// The per-row color pipeline [orig: @ 0x5c2d3f..0x5c2ef6] — diffuse and
// specular are ROW-CONSTANT (written to all 3 vertices @ 0x5c2f0a..0x5c2f2b).
// base = 1 - murk (underwater view: 1 — the murk term is skipped
// [orig: @ 0x5c2d4c]; nightvision: the flat 0.1 [orig: flt_7C69F4
// @ 0x5c2d5a]); k = 0.2 + 0.8*base [orig: flt_7C6F9C/flt_7C3340];
// sin = |dy|/dist of the right-edge camera ray [orig: @ 0x5c2dd2..0x5c2def].
// Normal path [orig: @ 0x5c2e22..0x5c2e9d]:
//   brightness = int(lerp(192*k, 38.4*k, sin))        [flt_7DBFA4/flt_7DBFA8]
//   alpha_term = int(lerp(0.0, 229.5*base, sin))      [flt_7C3284/flt_7DBFA0]
//   a = clamp(int(t * 255 * 2^16 / fog_end_fp), 0, 255) [dbl_7DBF98 = 16711680.0]
//   dist_alpha = 255 - a*a/255
//   diffuse = (alpha_term * dist_alpha / 255) << 24 | 0x10101 * brightness
// Underwater view [orig: @ 0x5c2df3..0x5c2e20]: diffuse = 0xFFFFFFFF and
// dist_alpha = clamp(255 - int(t * 255 * 2^16 / fog_end_fp), 0, 255) — LINEAR, no
// square. (The doc's "x255 <-> x229.5 doubles" swap is the LOW tier's
// dbl_7DBF70 @ 0x5c244b; the detailed tier multiplies 255 x 2^16 on both paths and
// its 229.5 is the float alpha_term scale.)
// Specular [orig: @ 0x5c2eb5..0x5c2ef4]: (dist_alpha << 24) |
// WaterColorLit RGB * int(lerp(255*(1-base), 128*(1-base), sin)) >> 8
// [flt_7CA29C/flt_7C461C]; the nightvision redraw drops the RGB
// [orig: @ 0x5c2ef8].
struct WaterRowColors {
	uint32_t diffuse = 0;
	uint32_t specular = 0;
};

WaterRowColors water_strip_row_colors(float row_view_depth, const float right_delta[3],
                                      int32_t fog_end_fp, float water_murk,
                                      uint32_t water_color_lit_packed,
                                      bool underwater_view, bool nightvision);

// Inputs the strip builder reads beside the view block.
struct WaterStripParams {
	int32_t plane_height_fp = 0;     // Env_WaterHeightFixed (16.16 render y)
	bool underwater_view = false;    // caller arg 2 [orig: @ 0x5c3540]
	bool nightvision = false;        // caller arg 3 [orig: @ 0x5c348e/@ 0x5c353f]
	float water_murk = 0.8f;         // Env_WaterMurk @ 0x26c6458
	uint32_t water_color_lit = 0;    // Env_WaterColorLit @ 0x26c6804 (0x00RRGGBB)
	float depth_scale = 1.0f;        // flt_8412B0 (WaterDepthCurve::scale)
	float depth_bias = 0.0f;         // flt_8412B4 (WaterDepthCurve::bias)
};

// The emitted rows, 3 vertices each (left / mid / right), field-for-field the
// witnessed 64-byte FVF 0x1404C4 vertex (XYZRHW | DIFFUSE | SPECULAR | TEX4,
// texcoord sizes 2/3/3/2) [orig: FVF push @ 0x5c28e1]. Texcoord 3 duplicates
// texcoord 0 in retail (@ 0x5c3095..0x5c30bf) and is not stored twice here.
// World positions are recoverable as (uv0 * 32, plane height): uv0 is the
// unprojected world x/z * 0.03125 [orig: flt_7DBFAC @ 0x5c2899].
struct WaterStripRows {
	std::vector<float> screen_pos;  // x, y pixel pairs        (+0x00/+0x04)
	std::vector<float> depth;       // the clamped depth/fog W (+0x08)
	// Reciprocal clip W. Retail perspective makes this 1 / view depth;
	// the reimpl orthographic extension carries constant clip W = 1.
	std::vector<float> rhw;                                 // (+0x0C)
	std::vector<uint32_t> diffuse;  // packed ARGB             (+0x10)
	std::vector<uint32_t> specular; // packed ARGB             (+0x14)
	std::vector<float> uv0;         // world x/32, z/32 pairs  (+0x18)
	// The texm3x2 reflection-bump rows [orig: @ 0x5c2f83..0x5c3067]:
	// t1 = (right.x, right.z) * (-min(rhw, 0.05)/2), screen U = (sx-minX)/W;
	// t2 = (fwd.x, fwd.z) * (-5*min(rhw, 0.05)), screen V = vbase -
	// (sy-minY)/H with vbase = 1 - min(300*rhw + 0.15, 2)/256
	// [flt_7C59B0=-0.5, flt_7DBF94=-5, flt_7C68E8=0.05, flt_7DBF68=300,
	//  flt_7C6FA4=0.15, flt_7C3B90=2, flt_7C3DD4=1/128]. The underwater view
	// flips t2's V to 1 - V on all three vertices [orig: @ 0x5c306f..0x5c3085].
	std::vector<float> t1; // 3 per vertex                     (+0x20)
	std::vector<float> t2; // 3 per vertex                     (+0x2C)
};

// The march loop of the detailed tier [orig: render_water_strip_detailed
// @ 0x5c27d0]: project the plane, march rows from the block origin along
// march_dir by the adaptive stride, clip each row (hunting backward by
// single steps up to stride-1 when the line left the viewport
// [orig: @ 0x5c297d..0x5c29c3]), emit 3 vertices per row, stop at the
// 1024-row cap [orig: /192 counter @ 0x5c3135]. Returns the row count.
int water_build_strip_rows(const WaterStripView &view, const WaterStripParams &params,
                           WaterStripRows &out);

inline constexpr int kWaterStripMaxRows = 1024;

// Rows submit as <=5-row triangle-strip batches stepping 4 rows (1-row
// overlap) through the static index table: vertex_count = 8*rows - 10
// vertices locked per batch, drawn as a TRIANGLESTRIP of vertex_count - 2
// primitives [orig: @ 0x5c3195 (8n-10); DrawPrimitive(5, start, 8n-12)
// @ 0x5c3209; batch walk @ 0x5c3164..0x5c329e]. (env-tod-re.md's "8*rows-10
// primitives" is the witnessed VERTEX count; the draw call submits two
// fewer.) Batches with fewer than 2 rows are skipped.
struct WaterStripBatch {
	int32_t first_row = 0;
	int32_t rows = 0;
	int32_t vertex_count = 0;    // 8*rows - 10
	int32_t primitive_count = 0; // vertex_count - 2
};

std::vector<WaterStripBatch> water_strip_batches(int row_count);

// The static strip index table [orig: word_841328] — a batch of n rows
// consumes the first 8n-10 entries; entry values are row-relative vertex
// indices (global vertex = 3*first_row + entry). Row-pair blocks
// {3k+3, 3k, 3k+4, 3k+1, 3k+5, 3k+2} joined by {3k+2, 3k+6} degenerate
// stitches.
inline constexpr uint16_t kWaterStripIndexTable[30] = {
	3, 0, 4, 1, 5, 2, 2, 6, 6, 3, 7, 4, 8, 5, 5, 9,
	9, 6, 10, 7, 11, 8, 8, 12, 12, 9, 13, 10, 14, 11,
};


} // namespace opennova::env
