#include "nova_frame_stats.h"

#include <godot_cpp/classes/engine.hpp>

namespace godot {

void FrameStatsWindow::assign(const opennova::devtools::CaptureWindow &p_window) {
	frames_ = static_cast<int64_t>(p_window.frames);
	sums_.resize(opennova::devtools::kSlotCount);
	peaks_.resize(opennova::devtools::kSlotCount);
	sample_frames_.resize(opennova::devtools::kSlotCount);
	int64_t *sums = sums_.ptrw();
	int64_t *peaks = peaks_.ptrw();
	int32_t *samples = sample_frames_.ptrw();
	for (int i = 0; i < opennova::devtools::kSlotCount; ++i) {
		sums[i] = p_window.sums[static_cast<size_t>(i)];
		peaks[i] = p_window.peaks[static_cast<size_t>(i)];
		samples[i] = p_window.sample_frames[static_cast<size_t>(i)];
	}
}

void FrameStatsWindow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_frames"), &FrameStatsWindow::get_frames);
	ClassDB::bind_method(D_METHOD("get_sums"), &FrameStatsWindow::get_sums);
	ClassDB::bind_method(D_METHOD("get_peaks"), &FrameStatsWindow::get_peaks);
	ClassDB::bind_method(D_METHOD("get_sample_frames"), &FrameStatsWindow::get_sample_frames);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frames"), "", "get_frames");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "sums"), "", "get_sums");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "peaks"), "", "get_peaks");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "sample_frames"), "", "get_sample_frames");
}

uint64_t FrameStats::frame_index() {
	return Engine::get_singleton()->get_process_frames();
}

void FrameStats::set_capture_active(bool p_active) {
	board_.set_capture_active(p_active, frame_index());
	sync_capture_signal();
}

void FrameStats::add(int p_slot, int64_t p_amount) {
	board_.add(static_cast<opennova::devtools::Slot>(p_slot), p_amount, frame_index());
}

Ref<FrameStatsWindow> FrameStats::drain() {
	Ref<FrameStatsWindow> window;
	window.instantiate();
	window->assign(board_.drain(frame_index()));
	return window;
}

String FrameStats::slot_name(int p_slot) {
	const char *name = opennova::devtools::slot_name(static_cast<opennova::devtools::Slot>(p_slot));
	return name == nullptr ? String() : String::utf8(name);
}

String FrameStats::slot_description(int p_slot) {
	return String::utf8(opennova::devtools::slot_description(static_cast<opennova::devtools::Slot>(p_slot)));
}

void FrameStats::sync_capture_signal() {
	const bool active = board_.is_capture_active();
	if (active == last_signaled_) {
		return;
	}
	last_signaled_ = active;
	emit_signal("capture_changed", active);
}

void FrameStats::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_capture_active", "active"), &FrameStats::set_capture_active);
	ClassDB::bind_method(D_METHOD("is_capture_active"), &FrameStats::is_capture_active);
	ClassDB::bind_method(D_METHOD("add", "slot", "amount"), &FrameStats::add);
	ClassDB::bind_method(D_METHOD("drain"), &FrameStats::drain);
	ClassDB::bind_static_method("FrameStats", D_METHOD("slot_count"), &FrameStats::slot_count);
	ClassDB::bind_static_method("FrameStats", D_METHOD("slot_name", "slot"), &FrameStats::slot_name);
	ClassDB::bind_static_method("FrameStats", D_METHOD("slot_description", "slot"), &FrameStats::slot_description);
	ADD_SIGNAL(MethodInfo("capture_changed", PropertyInfo(Variant::BOOL, "active")));

#define OPENNOVA_FRAME_STATS_BIND_SLOT(name, description) BIND_ENUM_CONSTANT(name);
	OPENNOVA_FRAME_STATS_SLOTS(OPENNOVA_FRAME_STATS_BIND_SLOT)
#undef OPENNOVA_FRAME_STATS_BIND_SLOT
	BIND_ENUM_CONSTANT(SLOT_COUNT);
}

} // namespace godot
