#pragma once

#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <runtime/renderer/d3d9_raster.h>

namespace godot {

class Camera3D;

// The device leg of the engine's D3D9 pixel-centre rule (runtime/renderer/d3d9_raster.h):
// the original's pixel centres sit on the integer window coordinates, this raster's at
// i + 0.5, so what the original draws at window coordinate x lands here at x + 0.5.

// Draw `p_camera`'s perspective (its fov under its keep-aspect mode over its viewport's
// visible rect, its near and far planes) through Camera3D's frustum form, translated by
// `p_shift` in normalized device coordinates. The fov property keeps its value for the
// cameras' readers; a caller that changes it draws through this again.
void draw_camera_through_d3d9_raster(Camera3D *p_camera, const opennova::renderer::NdcShift &p_shift);

// The clip-space translation by `p_shift` (x right, y up), applied on the left of a
// projection: translation * projection draws that projection's image shifted.
Projection ndc_translation(const opennova::renderer::NdcShift &p_shift);

// The canvas transform of a D3D9 window coordinate: half a pixel right and down. A
// canvas item that draws the original's screen coordinates (the HUD's draw lists)
// draws under it.
Transform2D d3d9_screen_to_canvas();

} // namespace godot
