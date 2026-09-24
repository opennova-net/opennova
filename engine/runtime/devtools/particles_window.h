// The Particles window: the effect scene over the ParticleSnapshot the
// embedder pushes (particle::EffectScene::inspect, by value, and the effect
// world's active-entry count). The top of the window is the retail particle
// stats page — "Current Particle Count: n / peak", where n is the effect
// world's active-entry count (0 while particles are disabled) and the peak
// latches until n returns to zero, then the emitter list [orig:
// Debug_DrawParticleStats @0x44c840; docs/particles/ptl-format-re.md section
// 11]; below it the port's own counters (live particles, pools, suppressed /
// rejected spawns). The hide toggle is the debug-control table's
// hide_particles row.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/particle/effect_scene.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

struct ParticleSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	// The retail count: the effect world's active entries (its live groups;
	// 0 while particles are disabled).
	int32_t active_entries = 0;
	particle::EffectDebugSnapshot scene;
};

class ParticlesWindow : public Window {
public:
	static constexpr double kRefreshSeconds = 0.25;

	explicit ParticlesWindow(ControlBoard &board) : board_(board) {}

	const char *title() const override { return "Particles"; }
	MenuGroup menu_group() const override { return MenuGroup::Render; }
	WindowSizeHint preferred_size() const override { return {560.0f, 560.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	void set_snapshot(ParticleSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }
	bool take_request(ControlRequest &request);

	// The retail page's reading, for tests: "Current Particle Count: n / peak"
	// and the emitter list rows ("NN   effect" for a group's first emitter,
	// "      emitter" for the rest).
	const std::string &count_text() const { return count_text_; }
	int emitter_row_count() const { return static_cast<int>(emitter_rows_.size()); }
	const char *emitter_row(int row) const;
	std::size_t peak() const { return peak_; }

private:
	void format();

	ControlBoard &board_;
	ParticleSnapshot snapshot_{};
	std::size_t peak_ = 0;
	std::string count_text_;
	std::vector<std::string> emitter_rows_;
	std::deque<ControlRequest> requests_;
};

}  // namespace opennova::devtools
