#include <runtime/devtools/render_window.h>

#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

// The rows this window draws, in draw order.
const char *const kViewRows[] = {
		control_id::kViewportDebugDraw,
		control_id::kOcclusionCulling,
		control_id::kHideFoliage,
		control_id::kHideParticles,
};
const char *const kTerrainRows[] = {
		control_id::kTerrainDrawMode,
		control_id::kTerrainLodQuality,
		control_id::kTerrainNoFrustum,
		control_id::kTerrainNoNearfar,
		control_id::kTerrainNoSideplanes,
		control_id::kTerrainNoPartialSubdiv,
		control_id::kTerrainForceLeaves,
		control_id::kTerrainForceLod0,
};

std::string mb(int64_t bytes) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return buf;
}

}  // namespace

void RenderWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = RenderSnapshot{};
		format();
	}
}

void RenderWindow::wanted_controls(std::vector<const char *> &out) const {
	for (const char *id : kViewRows) out.push_back(id);
	for (const char *id : kTerrainRows) out.push_back(id);
}

void RenderWindow::set_snapshot(const RenderSnapshot &snapshot) {
	snapshot_ = snapshot;
	format();
}

bool RenderWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void RenderWindow::format() {
	device_text_.clear();
	terrain_text_.clear();
	if (!snapshot_.valid) return;
	const RenderDeviceStats &d = snapshot_.device;
	char buf[320];
	std::snprintf(buf, sizeof(buf),
			"%dx%d | visible %lld draws, %lld objects, %lld prims | shadows %lld draws, %lld objects",
			d.viewport_width, d.viewport_height, static_cast<long long>(d.visible_draw_calls),
			static_cast<long long>(d.visible_objects), static_cast<long long>(d.visible_primitives),
			static_cast<long long>(d.shadow_draw_calls), static_cast<long long>(d.shadow_objects));
	device_text_ = buf;
	if (snapshot_.terrain_valid) {
		const TerrainFrameDebugCounters &t = snapshot_.terrain;
		std::snprintf(buf, sizeof(buf),
				"compile %llu | sectors %d | patches %d visible, %d emitted, %d empty drops | "
				"rejects near/far %d, sides %d/%d/%d/%d | partial %d | budget drops %d | fallbacks %d",
				static_cast<unsigned long long>(t.compile_index), t.sectors_walked, t.visible_patches,
				t.emitted_patches, t.empty_mesh_drops, t.traversal.rej_nearfar, t.traversal.rej_left,
				t.traversal.rej_right, t.traversal.rej_bottom, t.traversal.rej_top,
				t.traversal.partial_subdiv_count, t.traversal.budget_drops, t.traversal.lod_fallbacks);
		terrain_text_ = buf;
	}
}

void RenderWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (const char *id : kViewRows) draw_control(board_, id, requests_);
		ImGui::TextDisabled("The VoxelGI / SDFGI / GI-buffer views show nothing in this renderer.");
	}
	if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (const char *id : kTerrainRows) draw_control(board_, id, requests_);
		if (snapshot_.terrain_valid) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(terrain_text_.c_str());
			ImGui::PopTextWrapPos();
			const TerrainFrameDebugCounters &t = snapshot_.terrain;
			float lods[8];
			int peak = 1;
			for (int i = 0; i < 8; ++i) {
				lods[i] = static_cast<float>(t.lod_distribution[i]);
				if (t.lod_distribution[i] > peak) peak = t.lod_distribution[i];
			}
			ImGui::PlotHistogram("LOD 0..7", lods, 8, 0, nullptr, 0.0f, static_cast<float>(peak),
					ImVec2(0.0f, 60.0f));
			ImGui::Text("view distance %.1f .. %.1f", static_cast<double>(t.traversal.dist_min),
					static_cast<double>(t.traversal.dist_max));
		} else if (snapshot_.valid) {
			ImGui::TextDisabled("No terrain frame this world.");
		}
	}
	if (!snapshot_.valid) {
		ImGui::TextDisabled("No renderer counters pushed (load a mission).");
		return;
	}
	if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen)) {
		const RenderDeviceStats &d = snapshot_.device;
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(device_text_.c_str());
		ImGui::PopTextWrapPos();
		if (d.camera_valid) {
			ImGui::Text("camera (%.1f, %.1f, %.1f) yaw %.1f pitch %.1f | fov %.1f | near %.2f far %.0f",
					static_cast<double>(d.camera_position[0]), static_cast<double>(d.camera_position[1]),
					static_cast<double>(d.camera_position[2]), static_cast<double>(d.camera_yaw_deg),
					static_cast<double>(d.camera_pitch_deg), static_cast<double>(d.fov_deg),
					static_cast<double>(d.near_m), static_cast<double>(d.far_m));
		}
		ImGui::Text("memory: video %s, textures %s, buffers %s | %lld nodes, %lld objects",
				mb(d.video_memory).c_str(), mb(d.texture_memory).c_str(), mb(d.buffer_memory).c_str(),
				static_cast<long long>(d.node_count), static_cast<long long>(d.object_count));
		if (!d.adapter.empty()) ImGui::TextDisabled("%s", d.adapter.c_str());
	}
	if (snapshot_.foliage_valid && ImGui::CollapsingHeader("Foliage")) {
		const renderer::FoliageFrameDebugCounters &f = snapshot_.foliage;
		ImGui::Text("compile %llu | detail cells %lld | batches %lld",
				static_cast<unsigned long long>(f.compile_index), static_cast<long long>(f.detail_cells),
				static_cast<long long>(f.render_batches));
		ImGui::Text("detail instances %lld high / %lld low (%lld verts) | silhouettes %lld (%lld verts)",
				static_cast<long long>(f.detail_high_instances), static_cast<long long>(f.detail_low_instances),
				static_cast<long long>(f.detail_vertices), static_cast<long long>(f.silhouette_instances),
				static_cast<long long>(f.silhouette_vertices));
		ImGui::Text("silhouette anchors %lld / %lld visible | intents %lld detail, %lld silhouette",
				static_cast<long long>(f.silhouette_anchors_visible),
				static_cast<long long>(f.silhouette_anchors_input),
				static_cast<long long>(f.runtime_detail_intents),
				static_cast<long long>(f.runtime_silhouette_intents));
		ImGui::Text("mesh cache: detail %lld hits / %lld uploads, models %lld / %lld",
				static_cast<long long>(f.detail_mesh_hits), static_cast<long long>(f.detail_mesh_uploads),
				static_cast<long long>(f.model_mesh_hits), static_cast<long long>(f.model_mesh_uploads));
	}
	if (snapshot_.lights_valid && ImGui::CollapsingHeader("Point lights")) {
		ImGui::Text("live %lld | high water %lld | last query %lld",
				static_cast<long long>(snapshot_.lights_live),
				static_cast<long long>(snapshot_.lights_high_water),
				static_cast<long long>(snapshot_.lights_last_query));
	}
}

}  // namespace opennova::devtools
