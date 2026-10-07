#include "authoring/preview_range_draw.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/world/impact_scar.h>

#include "util/color_convert.h"
#include "world/scar_draw_list.h"
#include "world/scar_presenter.h"

namespace godot {

namespace {

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

// The scar draw list the range compiled (in the preview's space, the device's) as the record the game's
// ScarPresenter uploads: the shared ring's quads as they stand, the strip table (the TGA name and the mode word
// each strip's effect is built from, the scar texture load's table: world::scar_texture_strip_name).
Ref<ScarDrawList> scar_record(const opennova::renderer::ScarDrawList &list) {
	Ref<ScarDrawList> out;
	out.instantiate();
	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	vertices.resize(int64_t(list.vertices.size()));
	uvs.resize(int64_t(list.vertices.size()));
	colors.resize(int64_t(list.vertices.size()));
	for (size_t i = 0; i < list.vertices.size(); ++i) {
		const opennova::renderer::ScarVertex &v = list.vertices[i];
		vertices[int64_t(i)] = Vector3(v.x, v.y, v.z);
		uvs[int64_t(i)] = Vector2(v.u, v.v);
		colors[int64_t(i)] = opennova::color_from_argb(v.argb);
	}
	PackedInt32Array owner, texture, section, flags, first, count, bms, world_first;
	PackedInt64Array spawn_origin;
	for (const opennova::renderer::ScarDrawBatch &batch : list.batches) {
		if (batch.entity_local) continue; // the range's target writes the shared ring
		owner.push_back(batch.owner_packed);
		texture.push_back(batch.texture);
		section.push_back(batch.section);
		flags.push_back(batch.building ? ScarDrawList::FLAG_BUILDING : 0);
		first.push_back(int32_t(batch.first_vertex));
		count.push_back(int32_t(batch.vertex_count));
		bms.push_back(0);
		spawn_origin.push_back(0);
		world_first.push_back(-1);
	}
	PackedStringArray strip_names;
	PackedInt32Array strip_mode_words;
	strip_names.resize(opennova::world::kScarTextureStripCount);
	strip_mode_words.resize(opennova::world::kScarTextureStripCount);
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		strip_names[strip] = String(opennova::world::scar_texture_strip_name(strip));
		strip_mode_words[strip] = int32_t(opennova::world::scar_texture_strip_mode_word(strip));
	}
	out->set_vertices(vertices);
	out->set_uvs(uvs);
	out->set_colors(colors);
	out->set_batch_owner(owner);
	out->set_batch_texture(texture);
	out->set_batch_section(section);
	out->set_batch_flags(flags);
	out->set_batch_first(first);
	out->set_batch_count(count);
	out->set_batch_bms_id(bms);
	out->set_batch_spawn_origin(spawn_origin);
	out->set_batch_world_first(world_first);
	out->set_strip_names(strip_names);
	out->set_strip_mode_words(strip_mode_words);
	out->set_slots_live(int(list.slots_live));
	out->set_slots_culled(int(list.slots_culled));
	return out;
}

} // namespace

PreviewRangeDraw::PreviewRangeDraw(Node &parent) {
	target_mesh_.instantiate();
	target_ = memnew(MeshInstance3D);
	target_->set_name("Target");
	target_->set_mesh(target_mesh_);
	target_->set_visible(false);
	parent.add_child(target_);
	tracer_mesh_.instantiate();
	tracers_ = memnew(MeshInstance3D);
	tracers_->set_name("Tracers");
	tracers_->set_mesh(tracer_mesh_);
	tracers_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	parent.add_child(tracers_);
	scars_ = memnew(ScarPresenter);
	scars_->set_name("Scars");
	parent.add_child(scars_);
}

PreviewRangeDraw::~PreviewRangeDraw() = default;

void PreviewRangeDraw::set_resource_root(const Ref<ResourceRoot> &root) {
	scars_->set_resource_root(root);
	ribbons_.set_resource_root(root);
	scars_shown_ = UINT64_MAX;
	rooted_ = root.is_valid();
}

void PreviewRangeDraw::clear() {
	if (!drawn_) return;
	drawn_ = false;
	tracer_mesh_->clear_surfaces();
	target_->set_visible(false);
	scars_->clear();
	scars_shown_ = UINT64_MAX;
}

void PreviewRangeDraw::show_target(const opennova::editor::PreviewVec3 *corners) {
	target_->set_visible(corners != nullptr);
	if (!corners) return;
	drawn_ = true;
	target_mesh_->clear_surfaces();
	PackedVector3Array positions;
	PackedVector3Array normals;
	const Vector3 a = to_godot(corners[0]), b = to_godot(corners[1]), c = to_godot(corners[2]), d = to_godot(corners[3]);
	const Vector3 normal = (b - a).cross(d - a).normalized();
	for (const Vector3 &v : {a, b, c, a, c, d}) {
		positions.push_back(v);
		normals.push_back(normal);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	target_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.42f, 0.42f, 0.40f));
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	target_mesh_->surface_set_material(0, material);
}

void PreviewRangeDraw::show_scars(uint64_t serial, int count,
		const std::function<opennova::renderer::ScarDrawList()> &compile) {
	if (!rooted_ || serial == scars_shown_) return;
	scars_shown_ = serial;
	drawn_ = true;
	if (count > 0) scars_->present(scar_record(compile()), Dictionary());
	else scars_->clear();
}

void PreviewRangeDraw::show_tracers(const std::vector<opennova::editor::DefinitionTrail> &trails, Camera3D &camera,
		int64_t time_ms) {
	tracer_mesh_->clear_surfaces();
	if (!rooted_ || trails.empty()) return;
	drawn_ = true;
	size_t floats = 0;
	for (const opennova::editor::DefinitionTrail &trail : trails) floats += trail.points.size() * 4;
	tracer_points_.assign(floats, 0.0f);
	std::vector<opennova::renderer::TracerChannelInput> channels;
	size_t at = 0;
	for (const opennova::editor::DefinitionTrail &trail : trails) {
		opennova::renderer::TracerChannelInput channel;
		channel.style_id = trail.style;
		channel.age = trail.age;
		channel.count = int(trail.points.size());
		channel.points = tracer_points_.data() + at;
		for (size_t i = 0; i < trail.points.size(); ++i) {
			tracer_points_[at++] = trail.points[i].x;
			tracer_points_[at++] = trail.points[i].y;
			tracer_points_[at++] = trail.points[i].z;
			tracer_points_[at++] = i < trail.widths.size() ? trail.widths[i] : 1.0f;
		}
		channels.push_back(channel);
	}
	const Transform3D eye = camera.get_global_transform();
	const Vector3 forward = -eye.basis.get_column(2);
	opennova::renderer::TracerView view;
	view.camera = {float(eye.origin.x), float(eye.origin.y), float(eye.origin.z)};
	view.forward = {float(forward.x), float(forward.y), float(forward.z)};
	view.projection_x_scale = float(camera.get_camera_projection()[0][0]);
	view.tick_ms = uint32_t(time_ms);
	opennova::renderer::compile_tracer_ribbons(channels.data(), channels.size(), view,
			opennova::renderer::TracerPass::Main, tracer_frame_);
	ribbons_.emit(tracer_frame_, tracer_mesh_, opennova::renderer::tracer_rung(true));
}

} // namespace godot
