#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/devtools/frame_stats_board.h>

#include <cstdint>

namespace godot {

// One drained capture window of a FrameStats board (ADR 0017 typed record):
// the render frames it spanned and every slot's sum, worst-frame peak and
// sampled-frame count. Read-only; produced by FrameStats::drain().
class FrameStatsWindow : public RefCounted {
	GDCLASS(FrameStatsWindow, RefCounted)

public:
	int64_t get_frames() const { return frames_; }
	PackedInt64Array get_sums() const { return sums_; }
	PackedInt64Array get_peaks() const { return peaks_; }
	PackedInt32Array get_sample_frames() const { return sample_frames_; }

	void assign(const opennova::devtools::CaptureWindow &p_window);

protected:
	static void _bind_methods();

private:
	int64_t frames_ = 0;
	PackedInt64Array sums_;
	PackedInt64Array peaks_;
	PackedInt32Array sample_frames_;
};

// The frame-stats board (engine/runtime/devtools) as the shell sees it: the
// slot enum, the hot-path feed and the capture edge. Every FrameStats owns its
// own board; the game shell wires ONE instance into its producers and into the
// DevTools node, whose Stats window drains it. The capture edge is observable
// from both sides — set_capture_active() here and the Stats window's own
// visibility — so the signal is emitted from sync_capture_signal(), which the
// DevTools node calls after every layout pass.
class FrameStats : public RefCounted {
	GDCLASS(FrameStats, RefCounted)

public:
	// The slot table, one constant per engine slot (devtools/frame_stats_slots.h).
	enum Slot {
#define OPENNOVA_FRAME_STATS_SLOT_ENUM(name, description) name,
		OPENNOVA_FRAME_STATS_SLOTS(OPENNOVA_FRAME_STATS_SLOT_ENUM)
#undef OPENNOVA_FRAME_STATS_SLOT_ENUM
		SLOT_COUNT
	};

	void set_capture_active(bool p_active);
	bool is_capture_active() const { return board_.is_capture_active(); }
	// Hot-path feed: amount is microseconds (or a count for the VALUE slots);
	// keyed on the current render frame.
	void add(int p_slot, int64_t p_amount);
	Ref<FrameStatsWindow> drain();

	static int slot_count() { return opennova::devtools::kSlotCount; }
	static String slot_name(int p_slot);
	static String slot_description(int p_slot);

	opennova::devtools::FrameStatsBoard &board() { return board_; }
	// Emit capture_changed when the board's capture state moved since the
	// last emission (the engine's Stats window flips it directly).
	void sync_capture_signal();

protected:
	static void _bind_methods();

private:
	static uint64_t frame_index();

	opennova::devtools::FrameStatsBoard board_;
	bool last_signaled_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::FrameStats::Slot);
