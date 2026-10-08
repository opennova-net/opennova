#include "authoring/preview_backdrop.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

using opennova::editor::PreviewBackdrop;
using opennova::editor::PreviewBackground;

const char *const kPreviewBackdropCode = R"(
uniform int backdrop_mode = 0;
uniform vec3 backdrop_own = vec3(0.16);
uniform vec3 backdrop_top = vec3(0.353);
uniform vec3 backdrop_bottom = vec3(0.235);
uniform vec3 backdrop_check_a = vec3(0.4);
uniform vec3 backdrop_check_b = vec3(0.62);
uniform float backdrop_check_px = 8.0;
vec3 preview_backdrop(float down, vec2 pixel) {
	if (backdrop_mode == 1) return mix(backdrop_top, backdrop_bottom, clamp(down, 0.0, 1.0));
	if (backdrop_mode == 2) {
		float odd = mod(floor(pixel.x / backdrop_check_px) + floor(pixel.y / backdrop_check_px), 2.0);
		return mix(backdrop_check_b, backdrop_check_a, odd);
	}
	return backdrop_own;
}
)";

namespace {

// The 3D quad: laid over the whole view in clip space at the far plane (reverse Z: depth 0 is the far plane; a
// hair nearer, so a depth cleared to 0 lets it through), unlit, behind whatever the picture draws.
constexpr const char *kSpatialHead = R"(shader_type spatial;
render_mode unshaded, cull_disabled, shadows_disabled, fog_disabled;
)";
constexpr const char *kSpatialBody = R"(
void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 0.0000005, 1.0);
}
void fragment() {
	ALBEDO = preview_backdrop(SCREEN_UV.y, FRAGCOORD.xy);
}
)";

constexpr const char *kCanvasHead = "shader_type canvas_item;\n";
constexpr const char *kCanvasBody = R"(
void fragment() {
	COLOR = vec4(preview_backdrop(UV.y, FRAGCOORD.xy), 1.0);
}
)";

Vector3 rgb(uint32_t color) {
	return Vector3(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f);
}

Ref<ShaderMaterial> material_of(const char *head, const char *body) {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(String(head) + kPreviewBackdropCode + body);
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	return material;
}

} // namespace

void set_preview_backdrop(ShaderMaterial &material, PreviewBackground background) {
	const PreviewBackdrop look = opennova::editor::preview_backdrop(background);
	material.set_shader_parameter("backdrop_mode", look.own ? 0 : look.checker ? 2 : 1);
	if (look.own) return;
	material.set_shader_parameter("backdrop_top", rgb(look.top));
	material.set_shader_parameter("backdrop_bottom", rgb(look.bottom));
	material.set_shader_parameter("backdrop_check_a", rgb(look.check_a));
	material.set_shader_parameter("backdrop_check_b", rgb(look.check_b));
	material.set_shader_parameter("backdrop_check_px", float(look.check_px));
}

MeshInstance3D *make_preview_backdrop_3d() {
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(1.0f, 1.0f));
	MeshInstance3D *backdrop = memnew(MeshInstance3D);
	backdrop->set_name("PreviewBackdrop");
	backdrop->set_mesh(quad);
	backdrop->set_material_override(material_of(kSpatialHead, kSpatialBody));
	backdrop->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	// Its vertices are placed in clip space: never culled for where its mesh would stand.
	backdrop->set_custom_aabb(AABB(Vector3(-1.0e6f, -1.0e6f, -1.0e6f), Vector3(2.0e6f, 2.0e6f, 2.0e6f)));
	backdrop->set_visible(false);
	return backdrop;
}

void set_preview_backdrop(MeshInstance3D &backdrop, PreviewBackground background) {
	const Ref<ShaderMaterial> material = backdrop.get_material_override();
	if (material.is_valid()) set_preview_backdrop(**material, background);
	backdrop.set_visible(!opennova::editor::preview_backdrop(background).own);
}

Ref<ShaderMaterial> make_preview_backdrop_canvas(const Color &own) {
	Ref<ShaderMaterial> material = material_of(kCanvasHead, kCanvasBody);
	material->set_shader_parameter("backdrop_own", Vector3(own.r, own.g, own.b));
	return material;
}

Ref<ArrayMesh> preview_grid_mesh(PreviewBackground background) {
	// The grid's half side, in metres (a square every metre).
	constexpr int kGridHalf = 10;
	const PreviewBackdrop look = opennova::editor::preview_backdrop(background);
	Color faint(0.45f, 0.45f, 0.45f, 0.35f), axis(0.7f, 0.7f, 0.7f, 0.6f);
	if (look.dark_lines) {
		faint = Color(0.12f, 0.12f, 0.12f, 0.45f);
		axis = Color(0.04f, 0.04f, 0.04f, 0.75f);
	} else if (!look.own) {
		faint = Color(0.66f, 0.66f, 0.66f, 0.45f);
		axis = Color(0.9f, 0.9f, 0.9f, 0.75f);
	}
	PackedVector3Array lines;
	PackedColorArray colors;
	for (int i = -kGridHalf; i <= kGridHalf; ++i) {
		const Color color = i == 0 ? axis : faint;
		lines.push_back(Vector3(float(i), 0.0f, float(-kGridHalf)));
		lines.push_back(Vector3(float(i), 0.0f, float(kGridHalf)));
		lines.push_back(Vector3(float(-kGridHalf), 0.0f, float(i)));
		lines.push_back(Vector3(float(kGridHalf), 0.0f, float(i)));
		for (int k = 0; k < 4; ++k) colors.push_back(color);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = lines;
	arrays[Mesh::ARRAY_COLOR] = colors;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mesh->surface_set_material(0, material);
	return mesh;
}

} // namespace godot
