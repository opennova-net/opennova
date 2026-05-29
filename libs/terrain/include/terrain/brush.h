#pragma once

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

} // namespace opennova::terrain
