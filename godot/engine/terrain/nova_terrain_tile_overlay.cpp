#include "nova_terrain_tile_overlay.h"

#include "nova_terrain_data.h"
#include "nova_terrain_tile_entry.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <til/til.h>

namespace godot {

namespace {

constexpr float INVALID_HEIGHT_THRESHOLD = -1.0e6f;

PackedVector2Array til_uv_quad_to_packed(const opennova::TilUvQuad &quad) {
	PackedVector2Array out;
	if (!quad.valid) {
		return out;
	}
	out.resize(4);
	for (int i = 0; i < 4; ++i) {
		out.set(i, Vector2(quad.corners[static_cast<size_t>(i)].u, quad.corners[static_cast<size_t>(i)].v));
	}
	return out;
}

} // namespace

NovaTerrainTileOverlay::NovaTerrainTileOverlay() = default;
NovaTerrainTileOverlay::~NovaTerrainTileOverlay() = default;

void NovaTerrainTileOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tile_info", "tile_info"), &NovaTerrainTileOverlay::set_tile_info);
	ClassDB::bind_method(D_METHOD("get_tile_info"), &NovaTerrainTileOverlay::get_tile_info);
	ClassDB::bind_method(D_METHOD("set_tilestrip", "tex"), &NovaTerrainTileOverlay::set_tilestrip);
	ClassDB::bind_method(D_METHOD("get_tilestrip"), &NovaTerrainTileOverlay::get_tilestrip);
	ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"), &NovaTerrainTileOverlay::set_height_sampler);
	ClassDB::bind_method(D_METHOD("get_height_sampler"), &NovaTerrainTileOverlay::get_height_sampler);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &NovaTerrainTileOverlay::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &NovaTerrainTileOverlay::get_terrain_data);
	ClassDB::bind_method(D_METHOD("set_surface_offset", "offset"), &NovaTerrainTileOverlay::set_surface_offset);
	ClassDB::bind_method(D_METHOD("get_surface_offset"), &NovaTerrainTileOverlay::get_surface_offset);
	ClassDB::bind_method(D_METHOD("set_draw_outline_flag", "enabled"), &NovaTerrainTileOverlay::set_draw_outline_flag);
	ClassDB::bind_method(D_METHOD("get_draw_outline_flag"), &NovaTerrainTileOverlay::get_draw_outline_flag);
	ClassDB::bind_method(D_METHOD("rebuild"), &NovaTerrainTileOverlay::rebuild);
	ClassDB::bind_method(D_METHOD("build_entry_uvs", "entry", "atlas_width", "atlas_height"), &NovaTerrainTileOverlay::build_entry_uvs);
	ClassDB::bind_method(D_METHOD("clear"), &NovaTerrainTileOverlay::clear);
	ClassDB::bind_method(D_METHOD("get_entry_count_rendered"), &NovaTerrainTileOverlay::get_entry_count_rendered);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainTileInfo"),
	             "set_tile_info", "get_tile_info");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tilestrip", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"),
	             "set_tilestrip", "get_tilestrip");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "height_sampler"),
	             "set_height_sampler", "get_height_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
	             "set_terrain_data", "get_terrain_data");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_offset"),
	             "set_surface_offset", "get_surface_offset");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "draw_outline_flag"),
	             "set_draw_outline_flag", "get_draw_outline_flag");
}

void NovaTerrainTileOverlay::set_tile_info(const Ref<NovaTerrainTileInfo> &p_info) { tile_info_ = p_info; }
Ref<NovaTerrainTileInfo> NovaTerrainTileOverlay::get_tile_info() const { return tile_info_; }

void NovaTerrainTileOverlay::set_tilestrip(const Ref<Texture2D> &p_tex) { tilestrip_ = p_tex; }
Ref<Texture2D> NovaTerrainTileOverlay::get_tilestrip() const { return tilestrip_; }

void NovaTerrainTileOverlay::set_height_sampler(const Callable &p_sampler) { height_sampler_ = p_sampler; }
Callable NovaTerrainTileOverlay::get_height_sampler() const { return height_sampler_; }

void NovaTerrainTileOverlay::set_terrain_data(const Ref<NovaTerrainData> &p_data) { terrain_data_ = p_data; }
Ref<NovaTerrainData> NovaTerrainTileOverlay::get_terrain_data() const { return terrain_data_; }

void NovaTerrainTileOverlay::set_surface_offset(float p_offset) { surface_offset_ = p_offset; }
float NovaTerrainTileOverlay::get_surface_offset() const { return surface_offset_; }

void NovaTerrainTileOverlay::set_draw_outline_flag(bool p_enabled) { draw_outline_flag_ = p_enabled; }
bool NovaTerrainTileOverlay::get_draw_outline_flag() const { return draw_outline_flag_; }

int NovaTerrainTileOverlay::get_entry_count_rendered() const { return entries_rendered_; }

float NovaTerrainTileOverlay::_sample_height(float world_x, float world_z) const {
	// Fast path: bypass Callable/Variant for runtime where NovaTerrainData is
	// directly available. Matches NovaFoliageDispatcher's pattern; tile
	// rebuild samples N×4 corners per entry so the boxing cost adds up.
	if (terrain_data_.is_valid()) {
		const float h = terrain_data_->get_height_world_bilinear(Vector3(world_x, 0.0f, world_z));
		return h <= INVALID_HEIGHT_THRESHOLD ? 0.0f : h;
	}
	if (!height_sampler_.is_valid()) {
		return 0.0f;
	}
	Array args;
	args.push_back(world_x);
	args.push_back(world_z);
	const float h = static_cast<float>(static_cast<double>(height_sampler_.callv(args)));
	return h <= INVALID_HEIGHT_THRESHOLD ? 0.0f : h;
}

void NovaTerrainTileOverlay::_ensure_children() {
	if (overlay_instance_ == nullptr) {
		overlay_instance_ = memnew(MeshInstance3D);
		overlay_instance_->set_name("OverlayMesh");
		overlay_instance_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(overlay_instance_);
	}
	if (overlay_material_.is_null()) {
		overlay_material_.instantiate();
		overlay_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		overlay_material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		overlay_material_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		overlay_material_->set_render_priority(1);
	}
	if (outline_instance_ == nullptr) {
		outline_instance_ = memnew(MeshInstance3D);
		outline_instance_->set_name("OutlineMesh");
		outline_instance_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(outline_instance_);
	}
	if (outline_material_.is_null()) {
		outline_material_.instantiate();
		outline_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		outline_material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		outline_material_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		outline_material_->set_albedo(Color(1.0f, 1.0f, 1.0f, 0.9f));
		outline_material_->set_render_priority(3);
	}
}

PackedVector2Array NovaTerrainTileOverlay::build_entry_uvs(const Ref<NovaTerrainTileEntry> &entry,
                                                           int atlas_width,
                                                           int atlas_height) const {
	if (entry.is_null()) {
		return PackedVector2Array();
	}
	const opennova::TilOverlayEntry native = entry->to_native();
	return til_uv_quad_to_packed(
	    opennova::til_build_entry_uv_quad(native.tile_index, native.flags, atlas_width, atlas_height));
}

void NovaTerrainTileOverlay::clear() {
	entries_rendered_ = 0;
	if (overlay_instance_ != nullptr) {
		overlay_instance_->set_mesh(Ref<Mesh>());
		overlay_instance_->set_visible(false);
	}
	if (outline_instance_ != nullptr) {
		outline_instance_->set_mesh(Ref<Mesh>());
		outline_instance_->set_visible(false);
	}
}

void NovaTerrainTileOverlay::rebuild() {
	_ensure_children();
	clear();

	if (tile_info_.is_null() || tilestrip_.is_null()) {
		return;
	}

	const int entry_count = tile_info_->get_entry_count();
	if (entry_count <= 0) {
		return;
	}

	const int atlas_w = tilestrip_->get_width();
	const int atlas_h = tilestrip_->get_height();
	const opennova::TilAtlasLayout atlas_layout = opennova::til_make_atlas_layout(atlas_w, atlas_h);
	if (atlas_layout.tiles_x <= 0 || atlas_layout.tiles_y <= 0) {
		return;
	}

	const float cell = static_cast<float>(opennova::TIL_CELL_WORLD_UNITS);
	PackedVector3Array verts;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	verts.resize(entry_count * 4);
	uvs.resize(entry_count * 4);
	indices.resize(entry_count * 6);

	PackedVector3Array outline_verts;

	int vertex_offset = 0;
	int index_offset = 0;
	int emitted = 0;
	const opennova::TilFile native = tile_info_->to_native();
	for (int i = 0; i < entry_count; ++i) {
		if (i >= static_cast<int>(native.entries.size())) {
			break;
		}
		const opennova::TilOverlayEntry &entry = native.entries[static_cast<size_t>(i)];
		const opennova::TilUvQuad uv_quad =
		    opennova::til_build_entry_uv_quad(entry.tile_index, entry.flags, atlas_w, atlas_h);
		if (!uv_quad.valid) {
			continue;
		}

		const float origin_x = opennova::til_world_x_from_fixed(entry.x_fixed);
		const float origin_z = opennova::til_world_z_from_fixed(entry.z_fixed);
		const float x1 = origin_x + cell;
		const float z1 = origin_z + cell;

		// Vertex layout: TL, TR, BL, BR (matches the editor preview and shared UV helper).
		const Vector3 v_tl(origin_x, _sample_height(origin_x, origin_z) + surface_offset_, origin_z);
		const Vector3 v_tr(x1, _sample_height(x1, origin_z) + surface_offset_, origin_z);
		const Vector3 v_bl(origin_x, _sample_height(origin_x, z1) + surface_offset_, z1);
		const Vector3 v_br(x1, _sample_height(x1, z1) + surface_offset_, z1);

		verts[vertex_offset + 0] = v_tl;
		verts[vertex_offset + 1] = v_tr;
		verts[vertex_offset + 2] = v_bl;
		verts[vertex_offset + 3] = v_br;
		for (int c = 0; c < 4; ++c) {
			const opennova::TilUv &uv = uv_quad.corners[static_cast<size_t>(c)];
			uvs[vertex_offset + c] = Vector2(uv.u, uv.v);
		}

		// Two triangles: (0,1,2) and (1,3,2).
		indices[index_offset + 0] = vertex_offset + 0;
		indices[index_offset + 1] = vertex_offset + 1;
		indices[index_offset + 2] = vertex_offset + 2;
		indices[index_offset + 3] = vertex_offset + 1;
		indices[index_offset + 4] = vertex_offset + 3;
		indices[index_offset + 5] = vertex_offset + 2;

		if (draw_outline_flag_ && (entry.flags & opennova::TIL_FLAG_OUTLINE)) {
			// Top, right, bottom, left edges. Slight y bump to stay above base overlay.
			const float bump = 0.02f;
			const Vector3 o_tl = v_tl + Vector3(0, bump, 0);
			const Vector3 o_tr = v_tr + Vector3(0, bump, 0);
			const Vector3 o_bl = v_bl + Vector3(0, bump, 0);
			const Vector3 o_br = v_br + Vector3(0, bump, 0);
			outline_verts.push_back(o_tl);
			outline_verts.push_back(o_tr);
			outline_verts.push_back(o_tr);
			outline_verts.push_back(o_br);
			outline_verts.push_back(o_br);
			outline_verts.push_back(o_bl);
			outline_verts.push_back(o_bl);
			outline_verts.push_back(o_tl);
		}

		vertex_offset += 4;
		index_offset += 6;
		++emitted;
	}

	if (emitted != entry_count) {
		verts.resize(emitted * 4);
		uvs.resize(emitted * 4);
		indices.resize(emitted * 6);
	}

	if (emitted > 0) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

		overlay_material_->set_albedo(Color(1.0f, 1.0f, 1.0f, 1.0f));
		overlay_material_->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tilestrip_);
		overlay_instance_->set_material_override(overlay_material_);
		overlay_instance_->set_mesh(mesh);
		overlay_instance_->set_visible(true);
		overlay_instance_->set_extra_cull_margin(200.0f);
	}

	if (!outline_verts.is_empty()) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = outline_verts;

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);

		outline_instance_->set_material_override(outline_material_);
		outline_instance_->set_mesh(mesh);
		outline_instance_->set_visible(true);
	}

	entries_rendered_ = emitted;
}

} // namespace godot
