#include "vehicle_trail_presenter.h"
#include "simulation.h"
#include "particle/effect_world.h"
#include "env/water.h"
#include <runtime/renderer/render_order.h>
#include <runtime/renderer/texture_filter.h>
#include <runtime/renderer/water_wake_frame.h>
#include "util/texture_path_resolver.h"
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

using namespace godot;

// Device upload for the simulation's compiled water-ring draw.
void VehicleTrailPresenter::sync_water_wakes() {
	EffectWorld *effects = fx();
	Simulation *simulation = sim();
	if (!effects || !simulation || !effects->is_inside_tree())
		return;
	auto *node = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(water_mesh_id_));
	Camera3D *camera = effects->get_viewport()->get_camera_3d();
	opennova::renderer::WaterWakeFrame frame;
	if (camera && !effects->are_particles_hidden())
		simulation->fill_water_wake_frame(camera->get_global_position(), frame);
	if (frame.vertices.empty()) {
		if (node)
			node->hide();
		return;
	}
	if (!node) {
		// The ring textures load through Texture_LoadFromArchive with an
		// empty alpha name (renderer::TextureLoader::Archive).
		const Ref<ResourceRoot> &root = effects->resource_root();
		if (root.is_null())
			return;
		const Ref<Texture2D> wake =
				root->load_texture(opennova::renderer::kWakeTexture, ResourceRoot::TEXTURE_LOADER_ARCHIVE);
		const Ref<Texture2D> gradient =
				root->load_texture(opennova::renderer::kWakeGradientTexture, ResourceRoot::TEXTURE_LOADER_ARCHIVE);
		if (wake.is_null() || gradient.is_null())
			return;
		Ref<Shader> shader =
				ResourceLoader::get_singleton()->load("res://shaders/water_wake.gdshader");
		if (shader.is_null())
			return;
		water_mesh_.instantiate();
		water_material_.instantiate();
		water_material_->set_shader(shader);
		water_material_->set_shader_parameter("wake_texture", wake);
		water_material_->set_shader_parameter("gradient_texture", gradient);
		// Both sample bilinear with point mips over their pixel chains
		// (renderer::TextureStage::WaterWake).
		const uint32_t flags =
				opennova::renderer::texture_stage_flags(opennova::renderer::TextureStage::WaterWake);
		water_material_->set_shader_parameter("wake_max_lod", opennova::texture_max_lod(wake, flags));
		water_material_->set_shader_parameter("gradient_max_lod",
				opennova::texture_max_lod(gradient, flags));
		// The wake rings draw inside the water pass, right after the surface
		// strip (renderer::kRungWaterDecals carries the witness).
		water_material_->set_render_priority(opennova::renderer::kRungWaterDecals);
		node = memnew(MeshInstance3D);
		node->set_name("VehicleWaterWakes");
		node->set_mesh(water_mesh_);
		node->set_material_override(water_material_);
		node->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		node->set_layer_mask(Water::VISUAL_LAYER_WATER);
		effects->add_child(node);
		node->set_as_top_level(true);
		node->set_global_transform(Transform3D());
		water_mesh_id_ = node->get_instance_id();
	}
	PackedVector3Array positions;
	PackedVector2Array uvs, gradient_uvs;
	PackedColorArray colors;
	const int count = int(frame.vertices.size());
	positions.resize(count);
	uvs.resize(count);
	gradient_uvs.resize(count);
	colors.resize(count);
	for (int i = 0; i < count; ++i) {
		const auto &v = frame.vertices[i];
		positions.set(i, Vector3(v.x, v.y, v.z));
		uvs.set(i, Vector2(v.u, v.v));
		gradient_uvs.set(i, Vector2(v.u2, v.v2));
		colors.set(i, Color(1, 1, 1, v.alpha));
	}
	PackedInt32Array indices;
	indices.resize(int(frame.indices.size()));
	for (int i = 0; i < indices.size(); ++i)
		indices.set(i, frame.indices[i]);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = gradient_uvs;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	water_mesh_->clear_surfaces();
	water_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	node->show();
}
