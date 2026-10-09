#include "authoring/preview_range_draw.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "world/scar_draw_list.h"
#include "world/scar_presenter.h"

namespace godot {

namespace {

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

ScarDrawList::CompiledFrame device_scar_frame() {
	// Compiled in the device's space, with no entity rings to resolve (ScarDrawList::from_compiled).
	ScarDrawList::CompiledFrame frame;
	frame.mission_space = false;
	return frame;
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
	if (count > 0) scars_->present(ScarDrawList::from_compiled(compile(), device_scar_frame()), Dictionary());
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
	const opennova::renderer::TracerView view = tracer_view_from_camera(camera, uint32_t(time_ms));
	opennova::renderer::compile_tracer_ribbons(channels.data(), channels.size(), view,
			opennova::renderer::TracerPass::Main, tracer_frame_);
	ribbons_.emit(tracer_frame_, tracer_mesh_, opennova::renderer::tracer_rung(true));
}

} // namespace godot
