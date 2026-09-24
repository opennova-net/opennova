// The Render window: the renderer's knobs and counters. The knobs are the
// debug-control table's Terrain / Rendering / Particles rows through the
// control board (the terrain draw mode, LOD quality and culling switches, the
// Godot viewport debug views, occlusion, hiding foliage and particles); the
// counters are the RenderSnapshot the embedder pushes — the terrain frame's
// traversal (LOD histogram, rejects, budget drops), the foliage frame, the
// point-light scene, and the device's own numbers (camera, draw calls,
// objects, primitives, memory) that exist only because a Godot viewport does.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/renderer/foliage_frame.h>
#include <runtime/terrain/terrain_frame.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

// The device side, sampled by the shell from the game viewport.
struct RenderDeviceStats {
	bool camera_valid = false;
	float camera_position[3] = {}; // mission frame
	float camera_yaw_deg = 0.0f;   // mission heading of the view forward
	float camera_pitch_deg = 0.0f;
	float fov_deg = 0.0f;
	float near_m = 0.0f;
	float far_m = 0.0f;
	int32_t viewport_width = 0;
	int32_t viewport_height = 0;
	int64_t visible_objects = 0;
	int64_t visible_primitives = 0;
	int64_t visible_draw_calls = 0;
	int64_t shadow_objects = 0;
	int64_t shadow_primitives = 0;
	int64_t shadow_draw_calls = 0;
	int64_t video_memory = 0;
	int64_t texture_memory = 0;
	int64_t buffer_memory = 0;
	int64_t node_count = 0;
	int64_t object_count = 0;
	std::string adapter;
};

struct RenderSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	RenderDeviceStats device;
	bool terrain_valid = false;
	TerrainFrameDebugCounters terrain;
	bool foliage_valid = false;
	renderer::FoliageFrameDebugCounters foliage;
	bool lights_valid = false;
	int64_t lights_live = 0;
	int64_t lights_high_water = 0;
	int64_t lights_last_query = 0;
};

class RenderWindow : public Window {
public:
	static constexpr double kRefreshSeconds = 0.5;

	explicit RenderWindow(ControlBoard &board) : board_(board) {}

	const char *title() const override { return "Render"; }
	MenuGroup menu_group() const override { return MenuGroup::Render; }
	WindowSizeHint preferred_size() const override { return {560.0f, 620.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	void set_snapshot(const RenderSnapshot &snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }
	bool take_request(ControlRequest &request);

	// The formatted readings, for tests.
	const std::string &device_text() const { return device_text_; }
	const std::string &terrain_text() const { return terrain_text_; }

private:
	void format();

	ControlBoard &board_;
	RenderSnapshot snapshot_{};
	std::string device_text_;
	std::string terrain_text_;
	std::deque<ControlRequest> requests_;
};

}  // namespace opennova::devtools
