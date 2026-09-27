#include "env/slot_shadow.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <utility>
#include <vector>

#include <runtime/renderer/render_order.h>

#include "env/mission_environment.h"
#include "env/weather.h"
#include "render/world_environment_lookup.h"
#include "lights/effect_light_director.h"
#include "lights/light_scene.h"
#include "object/object_model.h"
#include "render/visual_layers.h"
#include "terrain/terrain_data.h"

namespace godot {

Ref<ShaderMaterial> SlotShadow::drape_material_;
int SlotShadow::live_instances_ = 0;
Ref<ImageTexture> SlotShadow::shadowztex_;
Ref<Texture2DRD> SlotShadow::capture_textures_[opennova::renderer::kSlotCaptureCount];

namespace {

// A replaced capture target outlives the frames that named it: the render
// side draws frame N while the main thread compiles N + 1, so a target
// published in frame N is free to release two main-thread frames later.
constexpr uint32_t kTargetReleaseFrameLag = 2;

RenderingDevice *main_rendering_device() {
	RenderingServer *server = RenderingServer::get_singleton();
	return server != nullptr ? server->get_rendering_device() : nullptr;
}

} // namespace

const StringName &SlotShadow::caster_group() {
	static const StringName group("slot_shadow_casters");
	return group;
}

// The per-slot uniform names, built once: the device names up to two per
// admitted slot every frame.
struct SlotUniformNames {
	StringName mat[opennova::renderer::kSlotCaptureCount];
	StringName tex[opennova::renderer::kSlotCaptureCount];
	SlotUniformNames() {
		for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
			mat[i] = StringName(vformat("u_slot_mat_%d", i));
			tex[i] = StringName(vformat("u_slot_tex_%d", i));
		}
	}
};

static SlotUniformNames &slot_uniforms() {
	static SlotUniformNames names;
	return names;
}


static void reset_material_slots(const Ref<ShaderMaterial> &p_material) {
	for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
		p_material->set_shader_parameter(slot_uniforms().mat[i], Projection());
	}
	PackedVector4Array terms;
	terms.resize(opennova::renderer::kSlotCaptureCount);
	p_material->set_shader_parameter("u_slot_term", terms);
}

Ref<ShaderMaterial> SlotShadow::get_drape_material() {
	if (drape_material_.is_valid()) {
		return drape_material_;
	}
	const Ref<Shader> shader = ResourceLoader::get_singleton()->load(
			"res://shaders/slot_shadow_drape.gdshader", "Shader");
	drape_material_.instantiate();
	drape_material_->set_shader(shader);
	reset_material_slots(drape_material_);
	// Retail draws the drapes right after the terrain batch, after the sky
	// and the viewmodel and before the sector models, entities and every
	// transparent pass, with Z-write off, so every later draw in front of the
	// terrain overwrites them (renderer::kRungSlotDrape carries the witness).
	// Godot draws the drape in its transparent list, after every opaque: the
	// drape shader reads the stencil mark only the terrain's front-most
	// fragments and the sky dome write, which leaves exactly retail's final
	// pixel, and this rung keeps it after the sky group and the viewmodel.
	drape_material_->set_render_priority(opennova::renderer::kRungSlotDrape);
	// The depth-clip stage's texture: the witnessed 32x4 ARGB step, sampled
	// CLAMP + bilinear by the shader's sampler hints [orig:
	// shadow_system_init_resources @0x5d6260..0x5d62d7 — the planner carries
	// the fill law, opennova::renderer::shadowztex_pixels].
	const auto px = opennova::renderer::shadowztex_pixels();
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(px.size()) * 4);
	for (size_t i = 0; i < px.size(); ++i) {
		const uint32_t argb = px[i];
		bytes[static_cast<int64_t>(i) * 4 + 0] = static_cast<uint8_t>((argb >> 16) & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 2] = static_cast<uint8_t>(argb & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 3] = static_cast<uint8_t>((argb >> 24) & 0xFF);
	}
	const Ref<Image> image = Image::create_from_data(opennova::renderer::kShadowZTexWidth,
			opennova::renderer::kShadowZTexHeight, false, Image::FORMAT_RGBA8, bytes);
	shadowztex_ = ImageTexture::create_from_image(image);
	drape_material_->set_shader_parameter("u_shadowztex", shadowztex_);
	// The twelve capture textures, bound once: a Texture2DRD per slot order
	// that the live device points at its resolve target (the retail RT
	// chain, RenderSlot_InitTextureChain @0x5d5320).
	for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
		if (capture_textures_[i].is_null()) {
			capture_textures_[i].instantiate();
		}
		drape_material_->set_shader_parameter(slot_uniforms().tex[i],
				capture_textures_[i]);
	}
	return drape_material_;
}

Ref<Texture2D> SlotShadow::get_capture_texture(int p_order) {
	if (p_order < 0 || p_order >= opennova::renderer::kSlotCaptureCount) {
		return Ref<Texture2D>();
	}
	get_drape_material();
	return capture_textures_[p_order];
}

int SlotShadow::get_capture_count() {
	return static_cast<int>(opennova::renderer::kSlotCaptureCount);
}

void SlotShadow::cleanup_statics() {
	drape_material_.unref();
	shadowztex_.unref();
	for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
		capture_textures_[i].unref();
	}
	// Release the uniform-name table before the engine tears the StringName
	// table down (the function-local static would otherwise outlive it).
	SlotUniformNames &names = slot_uniforms();
	for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
		names.mat[i] = StringName();
		names.tex[i] = StringName();
	}
}

SlotShadow::SlotShadow() { ++live_instances_; }

void SlotShadow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_node", "environment"),
			&SlotShadow::set_environment_node);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain"),
			&SlotShadow::set_terrain_data);
	ClassDB::bind_method(D_METHOD("set_light_director", "director"),
			&SlotShadow::set_light_director);
	ClassDB::bind_method(
			D_METHOD("set_light_context", "gain", "time_ms", "weather"),
			&SlotShadow::set_light_context);
	ClassDB::bind_method(D_METHOD("set_shadow_detail", "detail"),
			&SlotShadow::set_shadow_detail);
	ClassDB::bind_method(D_METHOD("set_local_player_model", "model"),
			&SlotShadow::set_local_player_model);
	ClassDB::bind_method(D_METHOD("set_local_player_parent_model", "model"),
			&SlotShadow::set_local_player_parent_model);
	ClassDB::bind_method(
			D_METHOD("set_local_player_first_person", "first_person"),
			&SlotShadow::set_local_player_first_person);
	ClassDB::bind_method(D_METHOD("set_local_player_prone", "prone"),
			&SlotShadow::set_local_player_prone);
	ClassDB::bind_method(D_METHOD("set_terrain_pass_drawn", "drawn"),
			&SlotShadow::set_terrain_pass_drawn);
	ClassDB::bind_method(D_METHOD("is_terrain_pass_drawn"),
			&SlotShadow::is_terrain_pass_drawn);
	ClassDB::bind_method(D_METHOD("advance_frame"), &SlotShadow::advance_frame);
	ClassDB::bind_method(D_METHOD("get_report"), &SlotShadow::get_report);
	ClassDB::bind_static_method("SlotShadow", D_METHOD("get_capture_count"),
			&SlotShadow::get_capture_count);
	ClassDB::bind_static_method("SlotShadow",
			D_METHOD("get_capture_texture", "order"),
			&SlotShadow::get_capture_texture);
	ClassDB::bind_method(D_METHOD("get_armed_capture_mask"),
			&SlotShadow::get_armed_capture_mask);
	ClassDB::bind_method(D_METHOD("get_capture_order_of", "model"),
			&SlotShadow::get_capture_order_of);
	ClassDB::bind_method(D_METHOD("get_capture_caster_count", "order"),
			&SlotShadow::get_capture_caster_count);
	ClassDB::bind_method(D_METHOD("get_capture_target_size", "order"),
			&SlotShadow::get_capture_target_size);
	ClassDB::bind_method(D_METHOD("is_capture_effect_installed"),
			&SlotShadow::is_capture_effect_installed);
	ClassDB::bind_method(D_METHOD("get_capture_image", "order"),
			&SlotShadow::get_capture_image);
	ClassDB::bind_method(D_METHOD("get_patch_vertices", "order"),
			&SlotShadow::get_patch_vertices);
	ClassDB::bind_static_method("SlotShadow", D_METHOD("get_drape_material"),
			&SlotShadow::get_drape_material);
}

void SlotShadow::set_environment_node(MissionEnvironment *p_environment) {
	environment_node_id_ = p_environment != nullptr
			? ObjectID(p_environment->get_instance_id())
			: ObjectID();
}

void SlotShadow::set_terrain_data(const Ref<TerrainData> &p_terrain) {
	if (terrain_data_ != p_terrain) {
		// The patch heights sample the terrain: a new field rebuilds them.
		for (PatchMesh &patch : patch_meshes_) {
			patch.built = false;
		}
	}
	terrain_data_ = p_terrain;
}

void SlotShadow::set_light_director(const Ref<EffectLightDirector> &p_director) {
	light_director_ = p_director;
}

void SlotShadow::set_light_context(const Vector3 &p_gain, int p_time_ms,
		Weather *p_weather) {
	light_gain_ = p_gain;
	light_time_ms_ = p_time_ms;
	weather_id_ = p_weather != nullptr ? ObjectID(p_weather->get_instance_id())
									   : ObjectID();
}

void SlotShadow::set_shadow_detail(int p_detail) {
	shadow_detail_ = CLAMP(p_detail, 0, 4);
}

void SlotShadow::set_local_player_model(ObjectModel *p_model) {
	local_player_id_ = p_model != nullptr ? ObjectID(p_model->get_instance_id())
										  : ObjectID();
}

void SlotShadow::set_local_player_parent_model(ObjectModel *p_model) {
	local_player_parent_id_ = p_model != nullptr
			? ObjectID(p_model->get_instance_id())
			: ObjectID();
}

void SlotShadow::set_local_player_first_person(bool p_first_person) {
	local_first_person_ = p_first_person;
}

void SlotShadow::set_local_player_prone(bool p_prone) {
	local_prone_ = p_prone;
}

void SlotShadow::set_terrain_pass_drawn(bool p_drawn) {
	if (terrain_pass_drawn_ == p_drawn) {
		return;
	}
	terrain_pass_drawn_ = p_drawn;
	if (!p_drawn) {
		// No frame between the gate and the next plan shows a drape.
		_hide_patches_from(0u);
	}
}

void SlotShadow::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Re-entry after an exit-tree shutdown (the FrameFx contract): the
		// released effect and targets are rebuilt by the READY leg below.
		if (shutdown_) {
			shutdown_ = false;
			request_ready();
		}
	} else if (p_what == NOTIFICATION_READY) {
		_ensure_captures();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		_release_captures();
	}
}

void SlotShadow::_install_effect() {
	if (effect_.is_null() || installed_into_.is_valid()) {
		return;
	}
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment == nullptr) {
		return;
	}
	// The effect joins the WorldEnvironment's compositor in place (created
	// when the environment has none): FrameFx keeps its identity check on
	// that compositor, and the particle renderer mirrors its effect list
	// onto the beauty camera's own compositor when the ids change.
	Ref<Compositor> compositor = world_environment->get_compositor();
	if (compositor.is_null()) {
		compositor.instantiate();
		world_environment->set_compositor(compositor);
	}
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	bool present = false;
	for (int64_t i = 0; i < effects.size(); ++i) {
		Ref<CompositorEffect> existing = effects[i];
		if (existing.is_valid() &&
				existing->get_instance_id() == effect_->get_instance_id()) {
			present = true;
			break;
		}
	}
	if (!present) {
		effects.push_back(effect_);
		compositor->set_compositor_effects(effects);
	}
	installed_into_ = compositor;
	world_environment_id_ = ObjectID(world_environment->get_instance_id());
}

void SlotShadow::_uninstall_effect() {
	if (installed_into_.is_valid() && effect_.is_valid()) {
		TypedArray<Ref<CompositorEffect>> effects =
				installed_into_->get_compositor_effects();
		TypedArray<Ref<CompositorEffect>> kept;
		for (int64_t i = 0; i < effects.size(); ++i) {
			Ref<CompositorEffect> existing = effects[i];
			if (existing.is_valid() &&
					existing->get_instance_id() == effect_->get_instance_id()) {
				continue;
			}
			kept.push_back(existing);
		}
		installed_into_->set_compositor_effects(kept);
	}
	installed_into_.unref();
	world_environment_id_ = ObjectID();
}

bool SlotShadow::is_capture_effect_installed() const {
	if (installed_into_.is_null() || effect_.is_null()) {
		return false;
	}
	WorldEnvironment *world_environment = Object::cast_to<WorldEnvironment>(
			ObjectDB::get_instance(world_environment_id_));
	if (world_environment == nullptr ||
			world_environment->get_compositor() != installed_into_) {
		return false;
	}
	const TypedArray<Ref<CompositorEffect>> effects =
			installed_into_->get_compositor_effects();
	for (int64_t i = 0; i < effects.size(); ++i) {
		Ref<CompositorEffect> existing = effects[i];
		if (existing.is_valid() &&
				existing->get_instance_id() == effect_->get_instance_id()) {
			return true;
		}
	}
	return false;
}

void SlotShadow::_ensure_captures() {
	if (shutdown_ || !is_inside_tree()) {
		return;
	}
	get_drape_material();
	if (effect_.is_null()) {
		effect_.instantiate();
	}
	effect_->set_gpu_timing_enabled(gpu_timing_enabled_);
	// Follow the live compositor: FrameFx::install_compositor and
	// DisplayDecode::install replace the scope WorldEnvironment's compositor
	// with a fresh one (carrying the previous effects) after their own READY,
	// and a scope may swap its WorldEnvironment; an install keyed on the old
	// compositor object would leave the report false and the uninstall
	// editing a compositor nothing renders. Re-install into whatever the
	// WorldEnvironment holds now (a present effect is not added twice).
	if (installed_into_.is_valid() && !is_capture_effect_installed()) {
		_uninstall_effect();
	}
	_install_effect();
}

// The resolve target of one order at the retail chain size for this order
// (render_slot_shadow.h carries the witness): RGBA8, sampled by the drape
// through the order's Texture2DRD, resolved into from the effect's 4x MSAA
// colour target (the MSAA rationale sits with the adapter's target). A
// replaced target is released after the render side is done with it.
bool SlotShadow::_ensure_capture_target(int p_order, int p_size) {
	if (p_order < 0 || p_order >= opennova::renderer::kSlotCaptureCount || p_size <= 0) {
		return false;
	}
	RenderingDevice *rd = main_rendering_device();
	if (rd == nullptr) {
		return false;
	}
	RID &target = capture_targets_[p_order];
	if (target.is_valid() && capture_target_sizes_[p_order] == p_size &&
			rd->texture_is_valid(target)) {
		return true;
	}
	if (target.is_valid()) {
		deferred_frees_.push_back({target, frame_});
		target = RID();
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format->set_width(p_size);
	format->set_height(p_size);
	format->set_depth(1);
	format->set_array_layers(1);
	format->set_mipmaps(1);
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	// Sampled by the drape, the resolve destination, and readable for the
	// GUT capture pins.
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	target = rd->texture_create(format, view);
	capture_target_sizes_[p_order] = target.is_valid() ? p_size : 0;
	if (target.is_valid()) {
		// A fresh target reads as the retail cleared RT (white RGB, the
		// no-shadow sample) until its first capture resolves into it: the
		// drape samples every published order the same frame, and an order
		// whose pass has not drawn yet (no WorldEnvironment in scope, a
		// latched device failure) must multiply white into the terrain, not
		// undefined texels (the viewport chain this replaced was always cleared
		// by its background colour).
		rd->texture_clear(target, slot_capture_clear_color(), 0, 1, 0, 1);
	}
	if (capture_textures_[p_order].is_valid()) {
		capture_textures_[p_order]->set_texture_rd_rid(target);
	}
	return target.is_valid();
}

void SlotShadow::_flush_deferred_frees(bool p_all) {
	RenderingDevice *rd = main_rendering_device();
	std::vector<DeferredFree> kept;
	for (const DeferredFree &entry : deferred_frees_) {
		if (p_all || frame_ >= entry.frame + kTargetReleaseFrameLag) {
			if (rd != nullptr && entry.rid.is_valid() && rd->texture_is_valid(entry.rid)) {
				rd->free_rid(entry.rid);
			}
		} else {
			kept.push_back(entry);
		}
	}
	deferred_frees_ = std::move(kept);
}

void SlotShadow::_release_captures() {
	shutdown_ = true;
	_uninstall_effect();
	// Detaching only changes the next render setup: drain any callback
	// already submitted before freeing what it may still read.
	RenderingServer *server = RenderingServer::get_singleton();
	RenderingDevice *rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd != nullptr) {
		server->force_sync();
	}
	if (effect_.is_valid()) {
		effect_->release_device_resources();
		effect_.unref();
	}
	for (int i = 0; i < opennova::renderer::kSlotCaptureCount; ++i) {
		if (capture_textures_[i].is_valid() &&
				capture_textures_[i]->get_texture_rd_rid() == capture_targets_[i]) {
			capture_textures_[i]->set_texture_rd_rid(RID());
		}
		if (capture_targets_[i].is_valid()) {
			deferred_frees_.push_back({capture_targets_[i], frame_});
			capture_targets_[i] = RID();
		}
		capture_target_sizes_[i] = 0;
	}
	_flush_deferred_frees(true);
	requests_.clear();
	capture_orders_.clear();
	armed_capture_mask_ = 0;
	_clear_all_terms();
	_release_patches();
}

// The drape patch of one order [orig: RenderSlot_DrawSilhouetteDrape
// @0x5d5ca0 -> RenderSlot_RebuildPatchVertexBuffer @0x5d5130, the rebuild
// gate @0x5d51a3..0x5d51bc: origin, lod and resolution unchanged keep the
// vertices]. The vertices are world positions (identity instance); the
// shared drape material reads the order from its instance uniform.
void SlotShadow::_draw_patch(int p_order, const opennova::renderer::SlotPatch &p_patch,
		int p_lod) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || !is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	PatchMesh &slot = patch_meshes_[p_order];
	if (!slot.mesh.is_valid()) {
		slot.mesh = rs->mesh_create();
		slot.instance = rs->instance_create2(slot.mesh, get_world_3d()->get_scenario());
		rs->instance_set_layer_mask(slot.instance, visual_layers::WORLD);
		rs->instance_geometry_set_cast_shadows_setting(slot.instance,
				RenderingServer::SHADOW_CASTING_SETTING_OFF);
		rs->instance_geometry_set_shader_parameter(slot.instance, "u_slot", p_order);
		slot.built = false;
		slot.visible = true;
	}
	if (!slot.built || slot.origin_x != p_patch.min_x ||
			slot.origin_north != p_patch.max_north || slot.lod != p_lod) {
		const TerrainData *terrain = terrain_data_.is_valid() ? terrain_data_.ptr() : nullptr;
		opennova::renderer::slot_patch_vertices(p_patch, p_lod,
				[terrain](float x, float z) {
					return terrain != nullptr ? terrain->get_height_world(Vector3(x, 0.0f, z))
											  : 0.0f;
				},
				slot.vertices);
		opennova::renderer::slot_patch_indices(p_lod, patch_indices_scratch_);
		PackedVector3Array positions;
		positions.resize(static_cast<int64_t>(slot.vertices.size()));
		PackedVector3Array normals;
		normals.resize(positions.size());
		for (int64_t i = 0; i < positions.size(); ++i) {
			const std::array<float, 3> &v = slot.vertices[static_cast<size_t>(i)];
			positions[i] = Vector3(v[0], v[1], v[2]);
			normals[i] = Vector3(0.0f, 1.0f, 0.0f);
		}
		PackedInt32Array indices;
		indices.resize(static_cast<int64_t>(patch_indices_scratch_.size()));
		for (int64_t i = 0; i < indices.size(); ++i) {
			indices[i] = patch_indices_scratch_[static_cast<size_t>(i)];
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = positions;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_INDEX] = indices;
		rs->mesh_clear(slot.mesh);
		rs->mesh_add_surface_from_arrays(slot.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
		rs->mesh_surface_set_material(slot.mesh, 0, get_drape_material()->get_rid());
		slot.origin_x = p_patch.min_x;
		slot.origin_north = p_patch.max_north;
		slot.lod = p_lod;
		slot.built = true;
	}
	if (!slot.visible) {
		rs->instance_set_visible(slot.instance, true);
		slot.visible = true;
	}
}

void SlotShadow::_hide_patches_from(uint32_t p_drawn_mask) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (int order = 0; order < opennova::renderer::kSlotCaptureCount; ++order) {
		PatchMesh &slot = patch_meshes_[order];
		if ((p_drawn_mask & (1u << order)) != 0 || !slot.instance.is_valid() || !slot.visible) {
			continue;
		}
		if (rs != nullptr) {
			rs->instance_set_visible(slot.instance, false);
		}
		slot.visible = false;
	}
}

void SlotShadow::_release_patches() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (PatchMesh &slot : patch_meshes_) {
		if (rs != nullptr) {
			if (slot.instance.is_valid()) {
				rs->free_rid(slot.instance);
			}
			if (slot.mesh.is_valid()) {
				rs->free_rid(slot.mesh);
			}
		}
		slot = PatchMesh{};
	}
}

PackedVector3Array SlotShadow::get_patch_vertices(int p_order) const {
	PackedVector3Array out;
	if (p_order < 0 || p_order >= opennova::renderer::kSlotCaptureCount) {
		return out;
	}
	const PatchMesh &slot = patch_meshes_[p_order];
	if (!slot.visible || !slot.built) {
		return out;
	}
	out.resize(static_cast<int64_t>(slot.vertices.size()));
	for (int64_t i = 0; i < out.size(); ++i) {
		const std::array<float, 3> &v = slot.vertices[static_cast<size_t>(i)];
		out[i] = Vector3(v[0], v[1], v[2]);
	}
	return out;
}

void SlotShadow::_clear_all_terms() {
	const PackedVector4Array zero = [] {
		PackedVector4Array terms;
		terms.resize(opennova::renderer::kSlotCaptureCount);
		return terms;
	}();
	get_drape_material()->set_shader_parameter("u_slot_term", zero);
	_hide_patches_from(0u);
	// This direct write bypasses the frame's identical-value elision, so the
	// stamps must forget what they think is resident.
	_invalidate_uniform_stamps();
}

void SlotShadow::_invalidate_uniform_stamps() {
	last_silhouette_terms_ = PackedVector4Array();
	last_clip_u_ = PackedVector4Array();
	last_clip_v_ = PackedVector4Array();
	last_light_pos_ = PackedVector4Array();
	last_light_diffuse_ = PackedVector4Array();
	drape_mat_stamps_.fill(SlotParamStamp{});
}

// World -> (u, v) projector for a camera-style pose (local -Z forward): the
// drape samples the capture along the same slot direction it was rendered
// from [orig: the shared unscaled direction of the capture and drape
// matrices, setup_shadow_cascade_matrices @0x58d300 /
// build_shadow_cascade_uv_matrices @0x58cf10 lookat_dir1; the person 4x
// belongs to the separate depth-clip stage — render_slot_shadow.h]. The
// stage-0 texgen has no depth bound: the capture's own near/far band never
// clips the drape (the clamped sampler covers the rest of the patch).
Projection SlotShadow::_drape_projection(const Transform3D &p_pose,
		float p_half_u, float p_half_v) const {
	const Transform3D view = p_pose.affine_inverse();
	const float inv_u = 1.0f / (2.0f * MAX(p_half_u, 0.001f));
	const float inv_v = 1.0f / (2.0f * MAX(p_half_v, 0.001f));
	// Columns (godot-cpp Projection(x, y, z, w) takes column vectors):
	// u = x*inv_u + 0.5, v = 0.5 - y*inv_v (image y-down).
	const Projection to_uv(
			Vector4(inv_u, 0, 0, 0),
			Vector4(0, -inv_v, 0, 0),
			Vector4(0, 0, 0, 0),
			Vector4(0.5f, 0.5f, 0, 1));
	return to_uv * Projection(view);
}

int SlotShadow::get_capture_order_of(ObjectModel *p_model) const {
	if (p_model == nullptr) {
		return -1;
	}
	const int *order = capture_orders_.getptr(uint64_t(p_model->get_instance_id()));
	return order != nullptr ? *order : -1;
}

int SlotShadow::get_capture_caster_count(int p_order) const {
	for (const SlotCaptureRequest &request : requests_) {
		if (request.order == p_order) {
			return static_cast<int>(request.casters.size());
		}
	}
	return 0;
}

int SlotShadow::get_capture_target_size(int p_order) const {
	if (p_order < 0 || p_order >= opennova::renderer::kSlotCaptureCount) {
		return 0;
	}
	return capture_target_sizes_[p_order];
}

Ref<Image> SlotShadow::get_capture_image(int p_order) const {
	if (p_order < 0 || p_order >= opennova::renderer::kSlotCaptureCount) {
		return Ref<Image>();
	}
	RenderingDevice *rd = main_rendering_device();
	const RID target = capture_targets_[p_order];
	if (rd == nullptr || !target.is_valid() || !rd->texture_is_valid(target)) {
		return Ref<Image>();
	}
	const PackedByteArray data = rd->texture_get_data(target, 0);
	if (data.is_empty()) {
		return Ref<Image>();
	}
	const int size = capture_target_sizes_[p_order];
	return Image::create_from_data(size, size, false, Image::FORMAT_RGBA8, data);
}

void SlotShadow::advance_frame() {
	if (!is_inside_tree() || shutdown_) {
		return;
	}
	_ensure_captures();
	++frame_;
	_flush_deferred_frees(false);
	armed_capture_mask_ = 0;
	requests_.clear();
	capture_orders_.clear();
	const Ref<ShaderMaterial> drape = get_drape_material();
	// The elision stamps describe what THIS instance last pushed into THIS
	// material object; a second live writer or a recreated material makes
	// them lies, so every frame under either condition pushes everything.
	if (live_instances_ != 1 || drape->get_rid() != stamped_drape_rid_) {
		_invalidate_uniform_stamps();
		stamped_drape_rid_ = drape->get_rid();
	}
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			ObjectDB::get_instance(environment_node_id_));
	Viewport *viewport = get_viewport();
	Camera3D *camera =
			viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const bool live = env != nullptr && env->is_loaded() &&
			camera != nullptr && shadow_detail_ > 0;
	// The caster registry replaces the per-frame get_nodes_in_group walk:
	// records rebuild only when the group revision moved (every membership or
	// cached-fact mutation site bumps it), and a freed node self-heals here
	// through its null ObjectDB resolve.
	std::vector<CasterInfo> &casters = casters_scratch_;
	HashMap<uint64_t, size_t> &caster_index = caster_index_scratch_;
	casters.clear();
	caster_index.clear();
	if (live) {
		if (!caster_records_valid_ ||
				caster_records_revision_ != caster_group_revision()) {
			_rebuild_caster_records();
		}
		casters.reserve(caster_records_.size());
		bool pruned = false;
		for (const CasterRecord &record : caster_records_) {
			ObjectModel *model = Object::cast_to<ObjectModel>(
					ObjectDB::get_instance(record.id));
			if (model == nullptr) {
				pruned = true;
				continue;
			}
			if (!model->is_inside_tree()) {
				continue;
			}
			CasterInfo info;
			info.model = model;
			info.capture_radius = record.capture_radius;
			info.state.bound_radius = record.slot_radius;
			if (record.radius_fallback) {
				// Unstamped models (previews/tests) keep the live-bounds
				// derivation the per-frame walk used.
				const AABB bounds = model->get_world_bounds();
				info.capture_radius =
						MAX(0.5f, float(bounds.size.length()) * 0.5f);
				info.state.bound_radius = info.capture_radius;
			}
			info.state.is_person = record.is_person;
			info.state.seat_parented = record.seat_parented_ancestor;
			casters.push_back(info);
			caster_index[uint64_t(record.id)] = casters.size() - 1;
		}
		if (pruned) {
			caster_records_valid_ = false;
		}
	}

	// Registration diff against the plan.
	std::vector<uint64_t> stale;
	for (const uint64_t id : registered_ids_) {
		if (!caster_index.has(id)) {
			stale.push_back(id);
		}
	}
	for (uint64_t id : stale) {
		plan_.release_entity(id);
		registered_ids_.erase(id);
	}
	if (!live) {
		_clear_all_terms();
		if (effect_.is_valid()) {
			effect_->adapter().clear_frame();
		}
		report_bound_ = report_captures_ = 0;
		return;
	}

	const uint64_t local_id = uint64_t(local_player_id_);
	// The vehicle the local player rides (entity+0x16C) shares the local
	// player's halved priority and every-frame refresh (retail:
	// RenderSlot_SortAndAssign @0x5d669c..0x5d66a6; RenderSlot_RenderEntity-
	// AndChildren @0x5d7707..0x5d7734).
	const uint64_t local_parent_id = uint64_t(local_player_parent_id_);
	// The frame-open slot projection direction: get_light_direction now
	// serves the Godot-axes vector (the util/axes.h x/z swap IS the witnessed
	// (g2, g1, g0) surface->light mapping of the raw getter tuple), so the
	// planner law only clamps and negates it
	// [orig: Environment_GetLightDirectionFloat @0x57d870 into
	// render_shadow_pass @0x5d7b70, see docs/render/render-lighting-re.md].
	const Vector3 tuple = env->get_light_direction();
	const std::array<float, 3> sun_dir = opennova::renderer::slot_projection_direction(
			{float(tuple.x), float(tuple.y), float(tuple.z)});
	const Vector3 default_dir =
			Vector3(sun_dir[0], sun_dir[1], sun_dir[2]).normalized();
	const Vector3 sun_rgb = env->get_sun_light();
	const Vector3 sky_rgb = env->get_sky_ambient();
	// The drape fogs with the device's primary fog config — the terrain's —
	// toward WHITE (retail: RenderSlot_DrawAllDrapes @0x5d6ea1 selects
	// CD3DDevice_SetFogAndBlendMode mode 3 = primary fog, white fog colour;
	// the drape technique's intrinsic pass flags 0x1520000 carry FOGENABLE,
	// shadow_system_init_resources @0x5d62f7). The shader shares the
	// terrain's fog law and uniforms.
	env->apply_terrain_uniforms(drape);

	// Build per-caster planner state. capture_links collects (child, parent)
	// for models linked capture-with another caster. The stamped radii, the
	// person flag, and the ancestor half of seat_parented
	// come off the registry record (their mutation sites bump the group
	// revision); only the live per-frame facts — position, visibility, the
	// capture-with link — read the node here.
	std::vector<std::pair<uint64_t, uint64_t>> capture_links;
	for (CasterInfo &info : casters) {
		ObjectModel *model = info.model;
		const uint64_t id = uint64_t(model->get_instance_id());
		plan_.register_entity(id);
		registered_ids_.insert(id);
		const Vector3 pos = model->get_global_position();
		opennova::renderer::SlotCandidateState &state = info.state;
		state.pos2d = {float(pos.x), float(pos.z)};
		// Hidden = the sim's present intent (retail Flags & 1), never the
		// render-occlusion claim: retail's slot pass reads no render gate, so
		// a caster hidden by the blink or outdoors latch keeps its shadow
		// (retail: RenderSlot_SortAndAssign @0x5d657d, RenderSlot_DrawAllDrapes
		// @0x5d6e6a).
		const bool shown = model->is_occlusion_hidden() ? model->is_present_visible()
													 : model->is_visible();
		const Node3D *parent = Object::cast_to<Node3D>(model->get_parent());
		state.hidden = !shown || (parent != nullptr && !parent->is_visible_in_tree());
		// A caster parented under another caster renders with its parent in
		// retail (the seat/standing child walk of the parent's slot RT);
		// its own slot is excluded.
		ObjectModel *capture_with = model->get_slot_shadow_capture_with();
		if (capture_with != nullptr) {
			state.seat_parented = true;
			capture_links.push_back(
					{ id, uint64_t(capture_with->get_instance_id()) });
		}
		state.on_vehicle = false;
		state.is_local_player_or_parent =
				id == local_id || (local_parent_id != 0 && id == local_parent_id);
		// entity+0x1D0: a contained entity zeroes the pick threshold
		// (retail RenderSlot_UpdateEntityLight @0x5d6adc..0x5d6aed); the
		// entity lighting feed stamps the same first-blink-hit fact as the
		// model's interior lerp.
		ObjectModel *entity_model = model->get_entity_light_owner();
		state.interior = (entity_model != nullptr ? entity_model : model)->is_interior_lerp();
		state.dynamic = true;
	}

	const Vector3 cam_pos = camera->get_global_position();
	const Vector3 cam_forward =
			-camera->get_global_transform().basis.get_column(2);
	const std::array<float, 2> cam2d{float(cam_pos.x), float(cam_pos.z)};
	const std::array<float, 2> view2d{float(cam_forward.x),
			float(cam_forward.z)};
	const auto state_for = [&](uint64_t id) -> opennova::renderer::SlotCandidateState {
		const size_t *index = caster_index.getptr(id);
		if (index == nullptr) {
			opennova::renderer::SlotCandidateState gone;
			gone.hidden = true;
			return gone;
		}
		return casters[*index].state;
	};
	const std::vector<opennova::renderer::SlotAssignment> assignments =
			plan_.assign(cam2d, view2d, state_for);

	PackedVector4Array silhouette_terms;
	silhouette_terms.resize(opennova::renderer::kSlotCaptureCount);
	// Per-slot drape patch (world min_x, min_z, max_x, max_z) and the
	// depth-clip texgen rows.
	PackedVector4Array clip_u;
	clip_u.resize(opennova::renderer::kSlotCaptureCount);
	PackedVector4Array clip_v;
	clip_v.resize(opennova::renderer::kSlotCaptureCount);
	// The attached-light drape light as (position, range) and (rescaled
	// diffuse, quadratic); range 0 = the sun leg.
	PackedVector4Array light_pos;
	light_pos.resize(opennova::renderer::kSlotCaptureCount);
	PackedVector4Array light_diffuse;
	light_diffuse.resize(opennova::renderer::kSlotCaptureCount);
	// The slot's lod x lod patch around the marched anchor
	// (opennova::renderer::slot_patch_bounds; the march probes TerrainData when set).
	// Planar mission north is Godot -z, so the patch's north range maps to
	// z in [-max_north, -min_north].
	const auto slot_patch = [&](const Vector3 &p_pos, const Vector3 &p_dir, int lod) {
		std::array<float, 2> anchor = {float(p_pos.x), float(p_pos.z)};
		// p_pos is the march start (renderer::slot_march_start_offset).
		if (terrain_data_.is_valid()) {
			const TerrainData *terrain = terrain_data_.ptr();
			const auto height_at = [terrain](float x, float z) {
				return terrain->get_height_world(Vector3(x, 0.0f, z));
			};
			// Retail marches from the live simulated entity. Static mission
			// presentation has no vehicle-settle tick, so grounded vehicles can
			// retain the authored handful of Q16 ticks above the raw16 terrain
			// (03TR M939: 2.500107 over 2.5). Reconcile only that sub-quantum
			// contact before the exact march; otherwise the tiny gap becomes a
			// whole planar step and crosses the lod-20 four-unit patch snap.
			const float start_y = opennova::renderer::slot_march_start_height(
					float(p_pos.y), height_at(float(p_pos.x), float(p_pos.z)),
					opennova::renderer::kSlotTerrainHeightQuantumUnits);
			anchor = opennova::renderer::march_shadow_anchor(
					{float(p_pos.x), start_y, float(p_pos.z)},
					{float(p_dir.x), float(p_dir.y), float(p_dir.z)},
					height_at);
		}
		return opennova::renderer::slot_patch_bounds(anchor[0], -anchor[1], lod);
	};
	// The orders whose drape patch draws this frame.
	uint32_t drawn_patch_mask = 0;
	report_bound_ = report_captures_ = 0;

	// The retail child walk: models linked capture-with an admitted caster
	// (held weapons, mounted children) render into the parent's slot
	// [orig: RenderSlot_RenderEntityAndChildren @0x5d78ef..0x5d79d6]. The
	// walk follows the entity hierarchy, not the slot table: a linked child
	// the full table refused (no row of its own) still rides its parent.
	const auto claimed_children = [&](uint64_t p_parent) {
		std::vector<uint64_t> children;
		for (const std::pair<uint64_t, uint64_t> &link : capture_links) {
			if (link.second == p_parent) {
				children.push_back(link.first);
			}
		}
		return children;
	};

	for (const opennova::renderer::SlotAssignment &assignment : assignments) {
		const size_t *index = caster_index.getptr(assignment.id);
		if (index == nullptr) {
			continue;
		}
		CasterInfo &info = casters[*index];
		ObjectModel *model = info.model;
		const bool captures =
				assignment.draws_silhouette && !assignment.excluded;
		if (assignment.bound && !assignment.excluded) {
			++report_bound_;
		}
		// Only a slot with a silhouette RT draws anything; the light pick,
		// patch and depth clip below feed that drape alone.
		if (!captures) {
			continue;
		}
		// The dominant-light pick (the clamped sun by default; the strongest
		// nearby point light overrides) [orig: RenderSlot_UpdateEntityLight
		// @0x5d6a30 <- Entity_UpdateAllEntities]. Every slot quantity keys on
		// the entity origin (entity+4..+0xC) — the light query box, the pick
		// distance, the capture's view origin, the drape projection — never on
		// the render bounds, which move with part animation and RLOD switches
		// (retail: the query box @0x5d6af1..0x5d6b39, the pick distance origin
		// @0x5d6bbd..0x5d6bc9, the entity rendered at the view origin by
		// Entity_RenderWithLODCallback @0x5d6fc4..0x5d6fd9).
		const Vector3 center = model->get_global_position();
		opennova::renderer::SlotLightPick pick;
		pick.direction = {default_dir.x, default_dir.y, default_dir.z};
		pick.attached_handle = 0;
		opennova::renderer::SlotPointLight attached_light;
		bool attached_found = false;
		if (light_director_.is_valid()) {
			Weather *weather = Object::cast_to<Weather>(
					ObjectDB::get_instance(weather_id_));
			// The entity's interior light group from its first blink hit
			// (retail @0x5d6b40..0x5d6b77 -> Lighting_SetInteriorLightGroup).
			ObjectModel *entity_model = model->get_entity_light_owner();
			if (entity_model == nullptr) entity_model = model;
			const int interior_bms = entity_model->get_interior_light_group_bms();
			const int64_t interior_owner = interior_bms != 0
					? light_director_->interior_owner_for_bms(interior_bms) : 0;
			light_director_->scene()->slot_shadow_lights(center, info.state.bound_radius,
					interior_owner, entity_model->get_interior_light_group_section(),
					light_gain_, light_time_ms_, weather, slot_lights_);
			pick = opennova::renderer::pick_dominant_light(
					{float(center.x), float(center.y), float(center.z)},
					{default_dir.x, default_dir.y, default_dir.z},
					slot_lights_.data(), slot_lights_.size(),
					info.state.interior);
			if (pick.attached_handle != 0) {
				for (const opennova::renderer::SlotPointLight &point : slot_lights_) {
					if (point.handle == pick.attached_handle) {
						attached_light = point;
						attached_found = true;
						break;
					}
				}
			}
		}
		const Vector3 dir = Vector3(pick.direction[0], pick.direction[1],
				pick.direction[2])
									.normalized();
		// The slot direction as stored: the RAW clamped-negated sun, or the
		// unit attached-light direction — what the grazing rescale's vertical
		// and the depth clip read [orig: slot+0x68..0x70, RenderSlot_Update-
		// EntityLight @0x5d6d5c; RenderSlot_DrawSilhouetteDrape @0x5d5d66].
		const float dir_y_raw = pick.attached_handle != 0 ? dir.y : sun_dir[1];
		const std::array<float, 3> stored_dir = pick.attached_handle != 0
				? std::array<float, 3>{float(dir.x), float(dir.y), float(dir.z)}
				: sun_dir;

		// The capture view along the slot direction, sized from the MODEL
		// sphere [orig: RenderSlot_RenderEntityAndChildren @0x5d7835 —
		// float24 = min(1.25 gpm[5], gpm[5] + 0.75)]: the witnessed
		// rotation-only look-at mapped into presentation axes
		// (renderer::slot_capture_view_axes: right-handed, so the back-face
		// cull keeps the light-facing faces like retail's CULLMODE CCW).
		const int order = assignment.capture_order;
		const float radius = info.capture_radius;
		const float half_extent = opennova::renderer::silhouette_half_extent(radius);
		const opennova::renderer::SlotCaptureViewAxes axes =
				opennova::renderer::slot_capture_view_axes(
						{float(dir.x), float(dir.y), float(dir.z)});
		const Vector3 forward(-axes.z[0], -axes.z[1], -axes.z[2]);
		// The RenderingDevice depth band (the device fold D-RLIT-10 in
		// docs/render/render-lighting-re.md): retail renders the entity at the
		// origin of that rotation-only view under its 0.2..5000.2 band
		// (renderer::kSilhouetteCaptureNear/Far), a band that as read starts
		// in front of the entity's own origin; the RD ortho clips outside
		// [near, far] the same way, so this eye backs off along -forward by
		// two sphere diameters plus the retail near margin and the band
		// spans the sphere. An orthographic silhouette is invariant under that
		// translation, so the capture is the same image.
		const float eye_distance = radius * 2.0f + 2.0f;
		const float eye_near = opennova::renderer::kSilhouetteCaptureNear * 0.25f;
		const float eye_far = eye_distance * 2.0f + radius;
		Transform3D pose;
		pose.basis = Basis(Vector3(axes.x[0], axes.x[1], axes.x[2]),
				Vector3(axes.y[0], axes.y[1], axes.y[2]),
				Vector3(axes.z[0], axes.z[1], axes.z[2]));
		pose.origin = center - forward * eye_distance;

		// The refresh cadence (opennova::renderer::slot_refresh_mask_for carries the
		// local-player exception).
		const uint32_t effective_mask = opennova::renderer::slot_refresh_mask_for(
				shadow_detail_, info.state.is_local_player_or_parent);
		const int want_size =
				opennova::renderer::slot_texture_size(order, shadow_detail_);
		const std::vector<uint64_t> children = claimed_children(assignment.id);
		capture_orders_[assignment.id] = order;
		for (const uint64_t child : children) {
			capture_orders_[child] = order;
		}
		if (opennova::renderer::slot_refresh_due(assignment.record_index, frame_,
					effective_mask, assignment.capture_dirty)) {
			SlotCaptureRequest request;
			request.order = order;
			request.size = want_size;
			request.view = pose;
			request.projection = Projection::create_orthogonal(-half_extent,
					half_extent, -half_extent, half_extent, eye_near, eye_far);
			if (_ensure_capture_target(order, want_size)) {
				request.target = capture_targets_[order];
			}
			request.casters.push_back(assignment.id);
			for (const uint64_t child : children) {
				request.casters.push_back(child);
			}
			requests_.push_back(std::move(request));
			armed_capture_mask_ |= 1u << order;
		}

		// The local player's first-person drape gate
		// (opennova::renderer::local_first_person_drape_skipped carries the law).
		if (assignment.id == local_id &&
				opennova::renderer::local_first_person_drape_skipped(local_first_person_,
						local_prone_, shadow_detail_)) {
			silhouette_terms[order] = Vector4();
			continue;
		}

		// The drape distance: the entity-to-camera 3D distance, taken once
		// per slot. At >= 80 u retail skips the drape outright; inside it the
		// one fade scales the whole patch's material ambient (retail:
		// RenderSlot_DrawSilhouetteDrape @0x5d5cc4..0x5d5d59; renderer::
		// drape_culled / drape_fade). The capture above is unaffected.
		const Vector3 entity_pos = model->get_global_position();
		const float camera_distance = float((entity_pos - cam_pos).length());
		if (opennova::renderer::drape_culled(camera_distance)) {
			silhouette_terms[order] = Vector4();
			continue;
		}
		const float fade = opennova::renderer::drape_fade(camera_distance);

		// The slot's fixed-function material ambient, per channel.
		std::array<float, 3> ambient{};
		light_pos[order] = Vector4();
		light_diffuse[order] = Vector4();
		if (pick.attached_handle != 0) {
			// An attached light lights the patch with D3D light 4 over a
			// white material under a white D3DRS_AMBIENT: the term is 1 plus
			// the light's negative diffuse at each patch point, which the
			// shader evaluates (renderer::drape_attached_light and
			// drape_attached_light_color). A handle the fill cannot read
			// leaves the white material alone.
			ambient = {1.0f, 1.0f, 1.0f};
			if (attached_found) {
				const opennova::renderer::SlotDrapeLight light =
						opennova::renderer::drape_attached_light(attached_light, fade);
				light_pos[order] = Vector4(light.position[0], light.position[1],
						light.position[2], light.range);
				light_diffuse[order] = Vector4(light.diffuse[0], light.diffuse[1],
						light.diffuse[2], light.quadratic);
			}
		} else {
			// The sun leg reads the STORED slot vertical — the raw
			// clamped-negated sun, not its normalized twin (retail: fabs of
			// slot+0x6C @0x5d5f63).
			ambient = opennova::renderer::drape_sun_ambient(
					{float(sun_rgb.x), float(sun_rgb.y), float(sun_rgb.z)},
					{float(sky_rgb.x), float(sky_rgb.y), float(sky_rgb.z)},
					dir_y_raw, fade);
		}
		const Projection drape_mat =
				_drape_projection(pose, half_extent, half_extent);
		SlotParamStamp &mat_stamp = drape_mat_stamps_[order];
		if (!mat_stamp.valid || mat_stamp.mat != drape_mat) {
			drape->set_shader_parameter(slot_uniforms().mat[order], drape_mat);
			mat_stamp.valid = true;
			mat_stamp.mat = drape_mat;
		}
		silhouette_terms[order] = Vector4(ambient[0], ambient[1], ambient[2], 1.0f);
		// The patch around the marched anchor, from the stored direction, at
		// the grazing-rescaled lod.
		const int patch_lod = opennova::renderer::grazing_slot_lod(
				opennova::renderer::slot_lod_for_radius(info.state.bound_radius),
				dir_y_raw);
		// The march starts at the rotated collision-bbox centre while the
		// entity's Flags dword is zero, else at its position (retail:
		// RenderSlot_UpdateEntityLight @0x5d6ce7..0x5d6d31); every role's
		// present rows carry that offset (renderer::slot_march_start_offset).
		const Vector3 march_start = entity_pos + model->get_slot_march_offset();
		// A closed terrain sector pass skips its drapes; the capture above
		// and the terms keep running (set_terrain_pass_drawn).
		if (terrain_pass_drawn_) {
			_draw_patch(order, slot_patch(march_start, dir, patch_lod), patch_lod);
			drawn_patch_mask |= 1u << order;
		}
		// The depth-clip texgen from the stored direction, the capture half
		// size and the person steepening (opennova::renderer::slot_depth_clip).
		const opennova::renderer::SlotDepthClip clip = opennova::renderer::slot_depth_clip(
				stored_dir, half_extent, info.state.is_person,
				{float(entity_pos.x), float(entity_pos.y), float(entity_pos.z)});
		clip_u[order] = Vector4(clip.u_axis[0], clip.u_axis[1], clip.u_axis[2],
				clip.u_offset);
		clip_v[order] = Vector4(clip.v_axis[0], clip.v_axis[1], clip.v_axis[2],
				clip.v_offset);
		++report_captures_;
	}

	// Identical-value pushes elided: the materials retain what was last set
	// (the T1 material-gating precedent); a static camera over a settled
	// scene pushes nothing.
	if (silhouette_terms != last_silhouette_terms_) {
		drape->set_shader_parameter("u_slot_term", silhouette_terms);
		last_silhouette_terms_ = silhouette_terms;
	}
	if (clip_u != last_clip_u_) {
		drape->set_shader_parameter("u_slot_clip_u", clip_u);
		last_clip_u_ = clip_u;
	}
	if (clip_v != last_clip_v_) {
		drape->set_shader_parameter("u_slot_clip_v", clip_v);
		last_clip_v_ = clip_v;
	}
	if (light_pos != last_light_pos_) {
		drape->set_shader_parameter("u_slot_light_pos", light_pos);
		last_light_pos_ = light_pos;
	}
	if (light_diffuse != last_light_diffuse_) {
		drape->set_shader_parameter("u_slot_light_diffuse", light_diffuse);
		last_light_diffuse_ = light_diffuse;
	}
	_hide_patches_from(drawn_patch_mask);
	// The armed requests compile into this frame's device draw list; an
	// unarmed order keeps its previous capture (retail's sticky RT).
	if (effect_.is_valid()) {
		effect_->adapter().compile_frame(requests_);
	}
}

void SlotShadow::set_gpu_timing_enabled(bool p_enabled) {
	gpu_timing_enabled_ = p_enabled;
	if (effect_.is_valid())
		effect_->set_gpu_timing_enabled(p_enabled);
}

// Main-thread only (every bump site is a device/present leg).
static uint64_t g_caster_group_revision = 1;

uint64_t SlotShadow::caster_group_revision() {
	return g_caster_group_revision;
}

void SlotShadow::bump_caster_group_revision() {
	++g_caster_group_revision;
}

void SlotShadow::_rebuild_caster_records() {
	caster_records_.clear();
	if (!is_inside_tree()) {
		caster_records_valid_ = false;
		return;
	}
	TypedArray<Node> nodes = get_tree()->get_nodes_in_group(caster_group());
	caster_records_.reserve(static_cast<size_t>(nodes.size()));
	for (int64_t i = 0; i < nodes.size(); ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(
				static_cast<Object *>(nodes[i]));
		if (model == nullptr) {
			continue;
		}
		CasterRecord record;
		record.id = ObjectID(model->get_instance_id());
		// The two radii the slot reads: the model sphere (gpm[5]) sizes the
		// capture extent and the depth clip; the entity bound (entity+0 —
		// the sphere raised to the husk's, + 0x1000, and 0 without a
		// collision block) sizes the lod/patch and the light query [orig:
		// RenderSlot_RenderEntityAndChildren @0x5d7835 reads the model's
		// +0x14; RenderSlot_AllocSlot @0x5d5773 and the light query read
		// entity+0, Entity_InitFromModel @0x40dc30]. A model the placer did
		// not stamp falls back to half its render-bounds diagonal for both,
		// re-read live each frame.
		record.capture_radius = model->get_model_sphere_radius();
		record.slot_radius = model->get_entity_bound_radius();
		record.radius_fallback = record.capture_radius <= 0.0f;
		record.is_person = model->is_slot_shadow_person();
		for (Node *ancestor = model->get_parent(); ancestor != nullptr;
				ancestor = ancestor->get_parent()) {
			ObjectModel *parent_model = Object::cast_to<ObjectModel>(ancestor);
			if (parent_model != nullptr &&
					parent_model->is_in_group(caster_group())) {
				record.seat_parented_ancestor = true;
				break;
			}
		}
		caster_records_.push_back(record);
	}
	caster_records_revision_ = caster_group_revision();
	caster_records_valid_ = true;
}

Dictionary SlotShadow::get_report() const {
	Dictionary report;
	report["bound"] = report_bound_;
	report["captures"] = report_captures_;
	report["registered"] = int(plan_.registered_count());
	report["detail"] = shadow_detail_;
	report["armed"] = static_cast<int>(requests_.size());
	report["capture_effect_installed"] = is_capture_effect_installed();
	if (effect_.is_valid()) {
		const Dictionary backend = effect_->get_backend_report();
		const Array keys = backend.keys();
		for (int64_t i = 0; i < keys.size(); ++i) {
			report[keys[i]] = backend[keys[i]];
		}
	}
	return report;
}

}  // namespace godot
