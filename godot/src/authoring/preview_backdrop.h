#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/color.hpp>

#include <string>

#include <editor/session/preview_background.h>

namespace godot {

// The editor's preview background (the Preview background preference, editor/session/preview_background.h) as a
// preview's device draws it behind its picture: the editor's tool, not the game's look. Each colour goes in as it
// shows on screen: a 2D picture draws its numbers as they are, and a 3D picture's are the gamma-domain numbers
// every engine shader writes, which its one display decode (render/frame_fx.h, DisplayDecode) shows as they are,
// particles drawing or not (the particle renderer composes its passes around the decode).
// Dark leaves each picture's own (a 3D picture's clear colour, `own` for a 2D one).

// The background's uniforms and `vec3 preview_backdrop(float down, vec2 pixel)` (`down` 0 at the picture's top
// edge, 1 at its bottom; `pixel` the picture's pixel, the checker's squares counted from its top left), for a
// shader of either kind that draws the background itself after its `shader_type` line (the texture's).
extern const char *const kPreviewBackdropCode;

// A 3D picture's background (the model's, the effect's, the definition's): a quad, named "PreviewBackdrop", whose
// shader lays it over the whole view at the far plane, behind everything the picture draws; hidden on Dark (the
// view's clear colour shows, as before the preference). Added to the SubViewport by the applier that owns it.
MeshInstance3D *make_preview_backdrop_3d();
void set_preview_backdrop(MeshInstance3D &backdrop, opennova::editor::PreviewBackground background);

// A 2D picture's background over a whole ColorRect (the HUD's): its material, `own` the colour Dark draws.
Ref<ShaderMaterial> make_preview_backdrop_canvas(const Color &own);
// The background's uniforms on a material whose shader holds kPreviewBackdropCode.
void set_preview_backdrop(ShaderMaterial &material, opennova::editor::PreviewBackground background);

// The ground grid an effect's and a definition's picture draw (a line every metre, the two through the spawn point
// brighter), its lines' colours read against `background`: light over the Dark and the Grey ones, dark over the
// Light and the Checker ones (PreviewBackdrop::dark_lines); on Dark as before the preference.
Ref<ArrayMesh> preview_grid_mesh(opennova::editor::PreviewBackground background);

} // namespace godot
