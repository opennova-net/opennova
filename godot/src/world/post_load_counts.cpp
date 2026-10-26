// The post-load object counts (world/post_load_counts.h): an instrument, --verbose only.

#include "world/post_load_counts.h"

#include "resource_index/resource_root.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

// What a target's camera admits (an instrument, --verbose only; LF-s19 row 2a): every visual
// instance of its world inside the camera's frustum whose layers its cull mask takes, counted by
// node class, name, mesh and material, and the server-only instances (no node) beside them. Read
// passively: the culling query changes nothing.
String target_draw_census(SubViewport *p_target) {
	Camera3D *camera = p_target->get_camera_3d();
	const Ref<World3D> world = p_target->find_world_3d();
	if (camera == nullptr || world.is_null()) return String();
	const uint32_t mask = camera->get_cull_mask();
	const PackedInt64Array ids =
			RenderingServer::get_singleton()->instances_cull_convex(camera->get_frustum(), world->get_scenario());
	HashMap<String, int64_t> rows;
	int64_t serverside = 0;
	for (int64_t i = 0; i < ids.size(); ++i) {
		VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(ObjectDB::get_instance(ObjectID(uint64_t(ids[i]))));
		if (visual == nullptr) {
			++serverside;
			continue;
		}
		if ((visual->get_layer_mask() & mask) == 0 || !visual->is_visible_in_tree()) continue;
		String key = vformat("%s %s", visual->get_class(), visual->get_name());
		if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(visual)) {
			const Ref<Mesh> resource = mesh->get_mesh();
			const Ref<Material> material = mesh->get_material_override().is_valid()
					? mesh->get_material_override()
					: (resource.is_valid() && resource->get_surface_count() > 0 ? resource->surface_get_material(0) : Ref<Material>());
			key += vformat(" mesh %s %d surf, %s", resource.is_valid() ? resource->get_class() : String("-"),
					resource.is_valid() ? resource->get_surface_count() : 0,
					material.is_valid() ? material->get_class() : String("-"));
		}
		rows[key] = (rows.has(key) ? rows[key] : 0) + 1;
	}
	PackedStringArray out;
	for (const KeyValue<String, int64_t> &row : rows) {
		out.push_back(vformat("%s x%d", row.key, row.value));
	}
	out.push_back(vformat("server-only x%d", serverside));
	return String("; ").join(out);
}

// A target's draw by the renderer's own sections (an instrument, --verbose with --gpu-profile,
// which turns the renderer's section timestamps on; LF-s19 row 2a): the device's captured
// timestamps of the frame it last read back, from the target's "vp_begin_" mark to its "vp_end_",
// each section with the CPU it took to the next mark. Empty when no section was captured.
String target_draw_sections(SubViewport *p_target) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) return String();
	const String id = String::num_uint64(p_target->get_viewport_rid().get_id());
	const String begin = String("vp_begin_") + id;
	const String end = String("vp_end_") + id;
	const uint32_t count = rd->get_captured_timestamps_count();
	PackedStringArray out;
	bool inside = false;
	for (uint32_t i = 0; i + 1 < count; ++i) {
		const String name = rd->get_captured_timestamp_name(i);
		if (!inside) {
			inside = name == begin;
			if (!inside) continue;
		} else if (name == end) {
			break;
		}
		const double cpu_ms = double(rd->get_captured_timestamp_cpu_time(i + 1) -
									  rd->get_captured_timestamp_cpu_time(i)) / 1000.0;
		if (cpu_ms >= 0.05) out.push_back(vformat("%s %.2f", name, cpu_ms));
	}
	if (out.is_empty()) return String();
	return vformat("rd frame %d: %s", int64_t(rd->get_captured_timestamps_frame()), String("; ").join(out));
}

// The whole frame by the renderer's sections (the same instrument): every captured mark of the
// frame the device last read back whose CPU to the next mark is at least 1 ms, in order, so a
// first frame's draw shows which step outside the viewports' own passes took it.
String frame_draw_sections() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) return String();
	const uint32_t count = rd->get_captured_timestamps_count();
	PackedStringArray out;
	for (uint32_t i = 0; i + 1 < count; ++i) {
		const double cpu_ms = double(rd->get_captured_timestamp_cpu_time(i + 1) -
									  rd->get_captured_timestamp_cpu_time(i)) / 1000.0;
		if (cpu_ms >= 1.0) out.push_back(vformat("%s %.1f", rd->get_captured_timestamp_name(i), cpu_ms));
	}
	if (count < 2) return String();
	return vformat("rd frame %d, %d marks, %.1f ms: %s", int64_t(rd->get_captured_timestamps_frame()),
			int64_t(count),
			double(rd->get_captured_timestamp_cpu_time(count - 1) - rd->get_captured_timestamp_cpu_time(0)) / 1000.0,
			String("; ").join(out));
}

} // namespace

namespace godot {

// The process's object counts at the load's end and at each post-load frame's start (an
// instrument, --verbose only): a flush that frees or builds in bulk between two frames shows as
// the difference of two lines.
void print_post_load_counts(Node *p_world, const ResourceRoot *p_root, const String &p_label) {
	if (!OS::get_singleton()->is_stdout_verbose()) return;
	Performance *perf = Performance::get_singleton();
	RenderingServer *rs = RenderingServer::get_singleton();
	UtilityFunctions::print_verbose(vformat(
			"Post-load counts %s: nodes %d, objects %d, resources %d, orphan nodes %d, video memory %d MB, "
			"textures %d MB, buffers %d MB; decoded textures %d, registered material textures %d",
			p_label, static_cast<int64_t>(perf->get_monitor(Performance::OBJECT_NODE_COUNT)),
			static_cast<int64_t>(perf->get_monitor(Performance::OBJECT_COUNT)),
			static_cast<int64_t>(perf->get_monitor(Performance::OBJECT_RESOURCE_COUNT)),
			static_cast<int64_t>(perf->get_monitor(Performance::OBJECT_ORPHAN_NODE_COUNT)),
			static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_VIDEO_MEM_USED) >> 20),
			static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED) >> 20),
			static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED) >> 20),
			p_root != nullptr ? p_root->get_decoded_texture_count() : int64_t(-1),
			p_root != nullptr ? p_root->get_registered_material_texture_count() : int64_t(-1)));
	if (p_world == nullptr || !p_world->is_inside_tree()) return;
	// The offscreen targets, each with its server-side update mode, its size, its last
	// frame's 3D draw calls (a first 3D draw builds a target's render buffers) and its
	// last measured GPU and CPU draw; each line turns on the timing of every target it
	// lists, so a target made after the load's end reads its own draw from its next line
	// on. The root's CPU and GPU draw lead the list: a first frame's server-side work
	// lands on whichever viewport draws first, so the CPU of every viewport is read
	// together (LF-s18).
	const bool load_end = p_label == String("load_end");
	PackedStringArray targets;
	// Each served (XR-projected) target's last draw split at its FrameFX callback: the renderer's
	// scene passes ahead of the callback, the callback, the rest of the draw (LF-s19, row 2a).
	TargetProjectionXrInterface::set_draw_stamping(true);
	{
		const RID root_rid = p_world->get_tree()->get_root()->get_viewport_rid();
		rs->viewport_set_measure_render_time(root_rid, true);
		targets.push_back(vformat("root gpu %.1f ms cpu %.1f ms",
				rs->viewport_get_measured_render_time_gpu(root_rid),
				rs->viewport_get_measured_render_time_cpu(root_rid)));
	}
	TypedArray<Node> found =
			p_world->get_tree()->get_root()->find_children("*", "SubViewport", true, false);
	for (int64_t i = 0; i < found.size(); ++i) {
		SubViewport *target = Object::cast_to<SubViewport>(found[i]);
		if (target == nullptr) continue;
		const RID rid = target->get_viewport_rid();
		rs->viewport_set_measure_render_time(rid, true);
		const int64_t mode = static_cast<int64_t>(rs->viewport_get_update_mode(rid));
		const int64_t draws = rs->viewport_get_render_info(rid,
				RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
				RenderingServer::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME);
		if (mode == RenderingServer::VIEWPORT_UPDATE_DISABLED && draws == 0) continue;
		targets.push_back(vformat("%s %dx%d mode %d draws %d gpu %.1f ms cpu %.1f ms",
				target->get_name(), target->get_size().x, target->get_size().y, mode, draws,
				rs->viewport_get_measured_render_time_gpu(rid),
				rs->viewport_get_measured_render_time_cpu(rid)));
		int64_t split[3] = { -1, -1, -1 };
		if (TargetProjectionXrInterface::draw_split_us(target, split)) {
			targets.push_back(vformat("%s split pre-fx %.1f fx %.1f fx-post %.1f ms", target->get_name(),
					split[0] / 1000.0, split[1] / 1000.0, split[2] / 1000.0));
		}
	}
	UtilityFunctions::print_verbose(vformat("Post-load targets %s: [%s]", p_label,
			String(", ").join(targets)));
	// The water mirror's admitted instances at the load's end (the forced frame's warm drew
	// them) and over the first frames (each line the previous frame's): row 2a's census.
	if (load_end || p_label == String("frame_1") || p_label == String("frame_2") ||
			p_label == String("frame_3") || p_label == String("frame_4")) {
		for (int64_t i = 0; i < found.size(); ++i) {
			SubViewport *target = Object::cast_to<SubViewport>(found[i]);
			if (target == nullptr || target->get_name() != StringName("WaterReflectionViewport")) continue;
			UtilityFunctions::print_verbose(vformat("Post-load mirror census %s: [%s]", p_label,
					target_draw_census(target)));
		}
	}
	// The served targets' draws by the renderer's sections (--gpu-profile beside --verbose), after
	// the whole frame's.
	const String frame_sections = frame_draw_sections();
	if (!frame_sections.is_empty()) {
		UtilityFunctions::print_verbose(vformat("Post-load frame sections %s: [%s]", p_label, frame_sections));
	}
	for (int64_t i = 0; i < found.size(); ++i) {
		SubViewport *target = Object::cast_to<SubViewport>(found[i]);
		if (target == nullptr || !target->is_using_xr()) continue;
		const String sections = target_draw_sections(target);
		if (!sections.is_empty()) {
			UtilityFunctions::print_verbose(vformat("Post-load sections %s %s: [%s]", p_label,
					target->get_name(), sections));
		}
	}
	// The orphan nodes by class and name: at the load's end (the next flush frees them,
	// and the device objects they hold go with them two frames on) and on any later
	// line that still has some.
	const TypedArray<int> ids = Node::get_orphan_node_ids();
	if (!load_end && ids.is_empty()) return;
	HashMap<String, int64_t> orphans;
	for (int64_t i = 0; i < ids.size(); ++i) {
		const Node *orphan = Object::cast_to<Node>(ObjectDB::get_instance(ObjectID(uint64_t(int64_t(ids[i])))));
		if (orphan == nullptr) continue;
		const String key = vformat("%s %s", orphan->get_class(), orphan->get_name());
		orphans[key] = (orphans.has(key) ? orphans[key] : 0) + 1;
	}
	PackedStringArray orphan_rows;
	for (const KeyValue<String, int64_t> &row : orphans) {
		orphan_rows.push_back(vformat("%s x%d", row.key, row.value));
	}
	UtilityFunctions::print_verbose(vformat("Post-load orphans %s: [%s]", p_label,
			String(", ").join(orphan_rows)));
}

} // namespace godot
