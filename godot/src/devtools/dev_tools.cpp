#include "devtools/dev_tools.h"

#if OPENNOVA_DEVTOOLS
#include <runtime/devtools/stats_window.h>
#endif

namespace godot {

void DevTools::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_open"), &DevTools::is_open);
	ClassDB::bind_method(D_METHOD("set_open", "open"), &DevTools::set_open);
	ClassDB::bind_method(D_METHOD("toggle"), &DevTools::toggle);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "stats"), &DevTools::set_frame_stats);
	ClassDB::bind_method(D_METHOD("get_frame_stats"), &DevTools::get_frame_stats);
	ClassDB::bind_method(D_METHOD("feed_stats_window", "frames", "sums", "peaks", "sample_frames"),
			&DevTools::feed_stats_window);
	ClassDB::bind_method(D_METHOD("stats_reading_frames"), &DevTools::stats_reading_frames);
	ClassDB::bind_method(D_METHOD("stats_row_ids"), &DevTools::stats_row_ids);
	ClassDB::bind_method(D_METHOD("stats_row_average", "row_id"), &DevTools::stats_row_average);
	ClassDB::bind_method(D_METHOD("stats_row_peak", "row_id"), &DevTools::stats_row_peak);
	ClassDB::bind_method(D_METHOD("stats_row_info", "row_id"), &DevTools::stats_row_info);
	ADD_SIGNAL(MethodInfo("open_changed", PropertyInfo(Variant::BOOL, "open")));
}

#if OPENNOVA_DEVTOOLS

DevTools::DevTools() : tools_(std::make_unique<opennova::devtools::GameDevTools>()) {}

DevTools::~DevTools() = default;

opennova::devtools::ImGuiPass *DevTools::engine_pass() {
	return &tools_->pass();
}

void DevTools::_exit_tree() {
	tools_->pass().set_open(false);
	tools_->set_frame_stats(nullptr);
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
	ImGuiPassNode::_exit_tree();
}

void DevTools::after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) {
	(void)p_frame_index;
	if (p_drew && frame_stats_.is_valid() && frame_stats_->is_capture_active()) {
		frame_stats_->add(FrameStats::FRAME_DEBUG_REFRESH, p_layout_us);
	}
	if (open_ && !tools_->pass().is_open()) {
		// Closed from inside (Escape, the menu).
		open_ = false;
		emit_signal("open_changed", false);
	}
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
}

bool DevTools::is_open() const {
	return tools_->pass().is_open();
}

void DevTools::set_open(bool p_open) {
	if (p_open == tools_->pass().is_open()) {
		return;
	}
	tools_->pass().set_open(p_open);
	open_ = p_open;
	sync_layer_visible();
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
	emit_signal("open_changed", p_open);
}

void DevTools::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
	tools_->set_frame_stats(p_stats.is_valid() ? &p_stats->board() : nullptr);
	if (p_stats.is_valid()) {
		p_stats->sync_capture_signal();
	}
}

void DevTools::feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
		const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames) {
	opennova::devtools::CaptureWindow window;
	window.frames = p_frames > 0 ? static_cast<uint64_t>(p_frames) : 0;
	const int n = opennova::devtools::kSlotCount;
	for (int i = 0; i < n; ++i) {
		if (i < p_sums.size()) {
			window.sums[static_cast<size_t>(i)] = p_sums[i];
		}
		if (i < p_peaks.size()) {
			window.peaks[static_cast<size_t>(i)] = p_peaks[i];
		}
		if (i < p_sample_frames.size()) {
			window.sample_frames[static_cast<size_t>(i)] = p_sample_frames[i];
		}
	}
	tools_->stats_window().feed_external(window);
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
}

int64_t DevTools::stats_reading_frames() const {
	return static_cast<int64_t>(tools_->stats_window().reading_frames());
}

PackedStringArray DevTools::stats_row_ids() const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	PackedStringArray ids;
	for (int i = 0; i < stats.row_count(); ++i) {
		ids.push_back(String::utf8(stats.row_id(i)));
	}
	return ids;
}

namespace {

int stats_row_index(const opennova::devtools::StatsWindow &p_stats, const String &p_row_id) {
	const CharString id = p_row_id.utf8();
	for (int i = 0; i < p_stats.row_count(); ++i) {
		if (id == p_stats.row_id(i)) {
			return i;
		}
	}
	return -1;
}

} // namespace

String DevTools::stats_row_average(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_average(row));
}

String DevTools::stats_row_peak(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_peak(row));
}

String DevTools::stats_row_info(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_info(row));
}

#else // !OPENNOVA_DEVTOOLS — the release flavour: the class exists, nothing runs.

DevTools::DevTools() = default;

DevTools::~DevTools() = default;

opennova::devtools::ImGuiPass *DevTools::engine_pass() {
	return nullptr;
}

void DevTools::_exit_tree() {
	ImGuiPassNode::_exit_tree();
}

void DevTools::after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) {
	(void)p_frame_index;
	(void)p_drew;
	(void)p_layout_us;
}

bool DevTools::is_open() const {
	return false;
}

void DevTools::set_open(bool p_open) {
	(void)p_open;
}

void DevTools::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
}

void DevTools::feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
		const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames) {
	(void)p_frames;
	(void)p_sums;
	(void)p_peaks;
	(void)p_sample_frames;
}

int64_t DevTools::stats_reading_frames() const {
	return 0;
}

PackedStringArray DevTools::stats_row_ids() const {
	return PackedStringArray();
}

String DevTools::stats_row_average(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

String DevTools::stats_row_peak(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

String DevTools::stats_row_info(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

#endif

} // namespace godot
