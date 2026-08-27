#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "imgui_pass_node.h"
#include "nova_frame_stats.h"

#if OPENNOVA_DEVTOOLS
#include <devtools/game_dev_tools.h>

#include <memory>
#endif

namespace godot {

// The game's dev tools (ADR 0039): the ImGui pass behind F3 with the Stats
// window over the frame-stats board. The engine owns the windows
// (engine/runtime/devtools/game_dev_tools.h); this node is their seam —
// the context hand-off and frame bracket come from ImGuiPassNode, and the
// game shell's only involvement is F3 (toggle) and the mouse/pick-click
// policy on open_changed.
//
// The open state is the shell-facing contract (F3, the mouse policy, the
// Stats capture edge) and works whether or not an ImGui context was attached,
// so headless tests pin it; attachment only governs drawing.
//
// Release flavour (OPENNOVA_DEVTOOLS=0): the class still registers so scripts
// keep parsing, but is_available() is false and every call is a no-op; the
// game's release export also strips the addon.
class DevTools : public ImGuiPassNode {
	GDCLASS(DevTools, ImGuiPassNode)

public:
	DevTools();
	~DevTools() override;

	void _exit_tree() override;

	bool is_open() const;
	void set_open(bool p_open);
	void toggle() { set_open(!is_open()); }

	void set_frame_stats(const Ref<FrameStats> &p_stats);
	Ref<FrameStats> get_frame_stats() const { return frame_stats_; }

	// A probe that drains the board itself hands the Stats window its
	// re-accumulated reading (sums/peaks/sample_frames sized FrameStats.SLOT_COUNT).
	void feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
			const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames);

	// The Stats window's last reading, row by row (probes and tests read the
	// same text the window draws).
	int64_t stats_reading_frames() const;
	PackedStringArray stats_row_ids() const;
	String stats_row_average(const String &p_row_id) const;
	String stats_row_peak(const String &p_row_id) const;
	String stats_row_info(const String &p_row_id) const;

protected:
	static void _bind_methods();
	opennova::devtools::ImGuiPass *engine_pass() override;
	void after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) override;

private:
	Ref<FrameStats> frame_stats_;
#if OPENNOVA_DEVTOOLS
	std::unique_ptr<opennova::devtools::GameDevTools> tools_;
	bool open_ = false; // the last state the shell was told about
#endif
};

} // namespace godot
