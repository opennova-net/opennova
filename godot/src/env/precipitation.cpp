#include "env/precipitation.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "env/weather.h"
#include "simulation/simulation.h"

namespace godot {

namespace {

// One Vector3 of the position stream, as the region update writes it — the
// bytes to_byte_array() emits per element.
constexpr int kVertexBytes = static_cast<int>(sizeof(float) * 3);
static_assert(sizeof(Vector3) == static_cast<size_t>(kVertexBytes),
		"the position stream assumes single-precision Vector3");
// The drops live within 32 m of the camera wherever it goes and the fixed
// surface never re-derives its bounds: an unbounded instance AABB keeps the
// frustum cull out of the picture.
constexpr float kAabbHalfExtent = 1.0e6f;

// The per-drop uv triple of the witnessed streak build: {(0.5, 0), (0, 1),
// (1, 1)} for every slot (renderer/precipitation_frame.h) — a constant of
// the layout, never of the frame.
PackedVector2Array streak_uvs(int p_vertices) {
	PackedVector2Array uvs;
	uvs.resize(p_vertices);
	Vector2 *w = uvs.ptrw();
	for (int i = 0; i + 2 < p_vertices; i += 3) {
		w[i] = Vector2(0.5f, 0.0f);
		w[i + 1] = Vector2(0.0f, 1.0f);
		w[i + 2] = Vector2(1.0f, 1.0f);
	}
	return uvs;
}

} // namespace

void Precipitation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&Precipitation::set_resource_root);
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&Precipitation::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"),
			&Precipitation::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("render_frame", "sim", "camera"),
			&Precipitation::render_frame);
	ClassDB::bind_method(D_METHOD("hide_frame"), &Precipitation::hide_frame);
	ClassDB::bind_method(D_METHOD("get_last_drop_count"),
			&Precipitation::get_last_drop_count);
	ClassDB::bind_method(D_METHOD("is_last_frame_snow"),
			&Precipitation::is_last_frame_snow);
}

void Precipitation::set_resource_root(const Ref<ResourceRoot> &p_root) {
	resource_root_ = p_root;
	rain_texture_.unref();
	snow_texture_.unref();
	textures_loaded_ = false;
	texture_bound_ = false;
}

void Precipitation::set_weather_path(const NodePath &p_path) {
	weather_path_ = p_path;
}

Weather *Precipitation::_weather_node() const {
	if (weather_path_.is_empty()) {
		return nullptr;
	}
	if (!is_inside_tree() && weather_path_.is_absolute()) {
		return nullptr;
	}
	return Object::cast_to<Weather>(get_node_or_null(weather_path_));
}

void Precipitation::_ready() {
	_ensure_scene();
}

void Precipitation::_ensure_scene() {
	if (mesh_instance_ != nullptr) {
		return;
	}
	Ref<Shader> shader =
			ResourceLoader::get_singleton()->load("res://shaders/precipitation.gdshader");
	material_.instantiate();
	material_->set_shader(shader);
	mesh_.instantiate();
	mesh_instance_ = memnew(MeshInstance3D);
	mesh_instance_->set_name("PrecipitationStreaks");
	mesh_instance_->set_mesh(mesh_);
	mesh_instance_->set_material_override(material_);
	// The drops draw in world space under an identity matrix, unlit, never
	// shadowed (retail the identity world matrix @ 0x5def50; pass flags
	// 0x10500000 lighting off).
	mesh_instance_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mesh_instance_->set_as_top_level(true);
	mesh_instance_->set_ignore_occlusion_culling(true);
	mesh_instance_->set_custom_aabb(AABB(
			Vector3(-kAabbHalfExtent, -kAabbHalfExtent, -kAabbHalfExtent),
			Vector3(2.0f * kAabbHalfExtent, 2.0f * kAabbHalfExtent, 2.0f * kAabbHalfExtent)));
	mesh_instance_->set_visible(false);
	add_child(mesh_instance_);
	_build_surface();
}

void Precipitation::_build_surface() {
	// The fixed surface: every vertex at the origin (collapsed, zero-area),
	// the constant uv triple in the attribute stream once; from here on only
	// the position stream changes, through surface_update_vertex_region.
	PackedVector3Array positions;
	positions.resize(kMaxVertices);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_TEX_UV] = streak_uvs(kMaxVertices);
	mesh_->clear_surfaces();
	mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	// The in-place write assumes the plain float triple per vertex (no
	// compression was requested); anything else falls back to a rebuild.
	const int64_t format = static_cast<int64_t>(mesh_->surface_get_format(0));
	const uint32_t stride = RenderingServer::get_singleton()->mesh_surface_get_format_vertex_stride(
			BitField<RenderingServer::ArrayFormat>(format), kMaxVertices);
	surface_streams_ = stride == static_cast<uint32_t>(kVertexBytes);
	uploaded_vertices_ = 0;
}

void Precipitation::_upload_positions(const PackedVector3Array &p_positions, int p_live_vertices) {
	// This frame's streaks over the head of the stream, then the collapse of
	// whatever the previous frame left live past them — two region writes at
	// most, no allocation of a surface.
	// Straight to the server: the ArrayMesh wrapper would broadcast a mesh
	// change per write.
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID mesh_rid = mesh_->get_rid();
	if (p_live_vertices > 0) {
		PackedByteArray bytes = p_positions.to_byte_array();
		const int64_t live_bytes = static_cast<int64_t>(p_live_vertices) * kVertexBytes;
		if (bytes.size() > live_bytes) {
			bytes.resize(live_bytes);
		}
		rs->mesh_surface_update_vertex_region(mesh_rid, 0, 0, bytes);
	}
	if (uploaded_vertices_ > p_live_vertices) {
		PackedByteArray zeros;
		zeros.resize(static_cast<int64_t>(uploaded_vertices_ - p_live_vertices) * kVertexBytes);
		zeros.fill(0);
		rs->mesh_surface_update_vertex_region(mesh_rid, 0,
				static_cast<int32_t>(p_live_vertices) * kVertexBytes, zeros);
	}
	uploaded_vertices_ = p_live_vertices;
}

void Precipitation::_rebuild_surface(const PackedVector3Array &p_positions, int p_live_vertices) {
	PackedVector3Array positions = p_positions;
	positions.resize(p_live_vertices);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_TEX_UV] = streak_uvs(p_live_vertices);
	mesh_->clear_surfaces();
	mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	uploaded_vertices_ = p_live_vertices;
}

Ref<Texture2D> Precipitation::_texture_for(bool p_snow) {
	if (!textures_loaded_ && resource_root_.is_valid()) {
		// (retail WeatherParticle_LoadTextures @ 0x5de840 — eraindrp.tga and
		//  jsnwflk.tga through the archive texture loader)
		rain_texture_ = resource_root_->load_texture(kRainTexture);
		snow_texture_ = resource_root_->load_texture(kSnowTexture);
		textures_loaded_ = true;
	}
	return p_snow ? snow_texture_ : rain_texture_;
}

void Precipitation::render_frame(Object *p_sim, Camera3D *p_camera) {
	_ensure_scene();
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	Weather *weather = _weather_node();
	if (sim == nullptr || p_camera == nullptr || weather == nullptr) {
		hide_frame();
		return;
	}
	const Transform3D xform = p_camera->get_global_transform();
	const Basis basis = xform.basis;
	const Dictionary frame = sim->compile_precipitation_frame(xform.origin,
			basis.get_column(0).normalized(), basis.get_column(1).normalized(),
			weather->get_terrain_light_combined_rgb());
	int drops = static_cast<int>(frame.get("drops", 0));
	last_snow_ = static_cast<bool>(frame.get("snow", false));
	if (drops <= 0) {
		hide_frame();
		return;
	}
	if (drops * 3 > kMaxVertices) {
		drops = kMaxVertices / 3;
	}
	last_drops_ = drops;
	const PackedVector3Array positions = frame.get("positions", PackedVector3Array());
	const int live_vertices = static_cast<int>(
			positions.size() < static_cast<int64_t>(drops) * 3 ? positions.size() : static_cast<int64_t>(drops) * 3);
	if (surface_streams_) {
		_upload_positions(positions, live_vertices);
	} else {
		_rebuild_surface(positions, live_vertices);
	}
	const int64_t argb = static_cast<int64_t>(frame.get("color", 0xFF000000));
	// Env_TerrainLightCombined | 0xFF000000: the diffuse the fixed-function
	// combine modulates (x2) the texture with.
	const Color diffuse(
			static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
			static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
			static_cast<float>(argb & 0xFF) / 255.0f,
			static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
	material_->set_shader_parameter(param_diffuse_, diffuse);
	if (!texture_bound_ || bound_texture_snow_ != last_snow_) {
		Ref<Texture2D> texture = _texture_for(last_snow_);
		if (texture.is_valid()) {
			material_->set_shader_parameter(param_drop_texture_, texture);
			texture_bound_ = true;
			bound_texture_snow_ = last_snow_;
		}
	}
	mesh_instance_->set_visible(true);
}

void Precipitation::hide_frame() {
	if (mesh_instance_ == nullptr || (last_drops_ == 0 && !mesh_instance_->is_visible())) {
		return;
	}
	// The live vertices stay where they are: the next frame's upload
	// overwrites the head and collapses the tail past its own count.
	mesh_instance_->set_visible(false);
	last_drops_ = 0;
}

} // namespace godot
