#pragma once

#include <cstdint>

// Terrain height-brush kernels, ported from
// godot/modtools/terrain/terrain_editor_brushes.gd (apply_raise_lower /
// apply_smooth / apply_flatten and the shared _for_each_brush_pixel falloff).
//
// Godot-agnostic: raw float* heightmap buffer (row-major, width*height, units =
// raw16/256 exactly like FORMAT_RF) + a plain BrushRect clip. The GDExtension
// wrapper (NovaTerrainData) does the Image get_data()/set_data() round-trip.
//
// All math runs in double (GDScript float is 64-bit) and stores back as float32,
// matching the original set_pixel(Color(...)) behaviour. These definitions live
// in libs/terrain (compiled /fp:precise -ffp-contract=off) so the non-exact
// brush multiplies are not FMA-contracted and stay bit-identical to GDScript.

namespace opennova::terrain {

// Clip rect into the buffer. w <= 0 or h <= 0 means "no clip" (matches the
// GDScript guard `clip_rect.size.x > 0 and clip_rect.size.y > 0`).
struct BrushRect {
	int x = 0;
	int z = 0;
	int w = 0;
	int h = 0;
};

// Smoothstep brush falloff. t_edge = 1 - distance/radius (0 at the disc edge, 1
// at the center); shoulder = 1 - clamp(hardness, 0, 1) (width of the soft ramp).
// Returns 1 for a hard core / inside the core, 0 at the edge of a fully soft
// brush, smoothstep(t/shoulder) in the ramp.
double brush_falloff(double t_edge, double shoulder);

// height = clamp(height + amount * falloff^2, 0, 255.996) for each pixel in the
// brush disc. amount may be negative (lower). Returns true if any pixel was
// touched (false when the clipped window is empty or the disc misses it), so the
// caller can skip writing an unchanged buffer back.
bool brush_raise_lower(float *buf, int width, int height, int cx, int cz, int radius,
                       double amount, double hardness, const BrushRect &clip);

// Two-pass 3x3 box smooth toward the neighbourhood average, weighted by
// clamp(strength * falloff, 0, 1). Returns true if any pixel was touched.
bool brush_smooth(float *buf, int width, int height, int cx, int cz, int radius,
                  double strength, double hardness, const BrushRect &clip);

// Lerp each pixel toward target_height by clamp(strength * falloff, 0, 1).
// Returns true if any pixel was touched.
bool brush_flatten(float *buf, int width, int height, int cx, int cz, int radius,
                   double target_height, double strength, double hardness, const BrushRect &clip);

// --- Colour / blend brushes on RGBA8 buffers (4 bytes per pixel, row-major) ---
//
// These reproduce Godot's Image FORMAT_RGBA8 conversions exactly:
//   decode: component = float(byte / 255.0)
//   encode: byte = trunc(clamp(component * 255.0, 0, 255))   (verified: truncation)
// colormap_paint/clone lerp in float32 (matching Color::lerp); blend_paint
// accumulates in double (matching the GDScript var arithmetic) then narrows to
// float32 before encoding. All three weight by falloff*falloff.

float rgba8_to_float(uint8_t b);
uint8_t float_to_rgba8(float c);

// Additive into `channel` (0=R, 1=G, 2=B) then renormalize R+G+B so they sum to
// 1 (or reset to a unit weight on the channel if the sum collapses). Alpha -> 1.
bool brush_blend_paint(uint8_t *buf, int width, int height, int channel, int cx, int cz, int radius,
                       double strength, double hardness, const BrushRect &clip);

// Lerp each pixel's RGBA toward the paint colour by clamp(strength*falloff^2, 0, 1).
bool brush_colormap_paint(uint8_t *buf, int width, int height,
                          float color_r, float color_g, float color_b, float color_a,
                          int cx, int cz, int radius, double strength, double hardness, const BrushRect &clip);

// Lerp each dest pixel toward the source pixel at the clone offset
// (sx = clamp(x - dst_cx + src_cx, 0, src_w-1), likewise sy).
bool brush_colormap_clone(uint8_t *dest, int width, int height, const uint8_t *src, int src_w, int src_h,
                          int src_cx, int src_cy, int dst_cx, int dst_cy, int radius,
                          double strength, double hardness, const BrushRect &clip);

// Nearest-pixel RGBA sample at int-truncated, edge-clamped coords. Writes 4 bytes.
void brush_sample_color(const uint8_t *buf, int width, int height, double world_x, double world_z,
                        uint8_t out_rgba[4]);

} // namespace opennova::terrain
