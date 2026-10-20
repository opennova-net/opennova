#include <editor/session/frame_times.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace opennova::editor {

using io::JsonValue;

const char *frame_phase_token(FramePhase phase) {
	switch (phase) {
	case FramePhase::Wall: return "wall";
	case FramePhase::Layout: return "layout";
	case FramePhase::Requests: return "requests";
	case FramePhase::Poll: return "poll";
	case FramePhase::Validation: return "validation";
	case FramePhase::Compose: return "compose";
	case FramePhase::Operation: return "operation";
	case FramePhase::Originals: return "originals";
	case FramePhase::Devices: return "devices";
	case FramePhase::kCount: break;
	}
	return "";
}

void FrameTimes::begin_frame() {
	current_.fill(0);
	open_ = true;
}

void FrameTimes::add(FramePhase phase, int64_t us) {
	if (!open_ || phase >= FramePhase::kCount) return;
	current_[size_t(phase)] += std::max<int64_t>(0, us);
}

void FrameTimes::end_frame() {
	if (!open_) return;
	open_ = false;
	ring_[next_] = current_;
	next_ = (next_ + 1) % kFrames;
	held_ = std::min(held_ + 1, kFrames);
	++total_;
	const int64_t wall = current_[size_t(FramePhase::Wall)];
	if (wall > kSlowUs) ++slow_total_;
	if (wall > kStallUs) ++stall_total_;
}

int64_t FrameTimes::percentile(FramePhase phase, double fraction) const {
	if (held_ == 0 || phase >= FramePhase::kCount) return 0;
	std::vector<int64_t> values(held_);
	for (size_t i = 0; i < held_; ++i) values[i] = ring_[i][size_t(phase)];
	// Nearest rank: the smallest value at least `fraction` of the frames are at or under.
	const double clamped = std::min(1.0, std::max(0.0, fraction));
	const size_t rank = std::max<size_t>(1, size_t(std::ceil(clamped * double(held_))));
	std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(rank - 1), values.end());
	return values[rank - 1];
}

FrameTimes::Sample FrameTimes::worst() const {
	Sample out{};
	int64_t longest = -1;
	for (size_t i = 0; i < held_; ++i)
		if (ring_[i][size_t(FramePhase::Wall)] > longest) {
			longest = ring_[i][size_t(FramePhase::Wall)];
			out = ring_[i];
		}
	return out;
}

JsonValue FrameTimes::json() const {
	JsonValue out = JsonValue::make_object();
	out.set("frames", JsonValue::make_number(double(held_)));
	out.set("total_frames", JsonValue::make_number(double(total_)));
	out.set("slow_total", JsonValue::make_number(double(slow_total_)));
	out.set("stall_total", JsonValue::make_number(double(stall_total_)));
	size_t slow = 0, stall = 0;
	for (size_t i = 0; i < held_; ++i) {
		slow += ring_[i][size_t(FramePhase::Wall)] > kSlowUs ? 1 : 0;
		stall += ring_[i][size_t(FramePhase::Wall)] > kStallUs ? 1 : 0;
	}
	out.set("slow", JsonValue::make_number(double(slow)));
	out.set("stall", JsonValue::make_number(double(stall)));
	JsonValue phases = JsonValue::make_object();
	const Sample longest = worst();
	JsonValue worst_frame = JsonValue::make_object();
	for (size_t p = 0; p < kFramePhaseCount; ++p) {
		const FramePhase phase = FramePhase(p);
		int64_t sum = 0, most = 0;
		for (size_t i = 0; i < held_; ++i) {
			sum += ring_[i][p];
			most = std::max(most, ring_[i][p]);
		}
		JsonValue row = JsonValue::make_object();
		row.set("mean", JsonValue::make_number(double(held_ ? sum / int64_t(held_) : 0)));
		row.set("p50", JsonValue::make_number(double(percentile(phase, 0.5))));
		row.set("p95", JsonValue::make_number(double(percentile(phase, 0.95))));
		row.set("max", JsonValue::make_number(double(most)));
		phases.set(frame_phase_token(phase), std::move(row));
		worst_frame.set(frame_phase_token(phase), JsonValue::make_number(double(longest[p])));
	}
	out.set("phases", std::move(phases));
	out.set("worst", std::move(worst_frame));
	return out;
}

int64_t steady_clock_us() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

} // namespace opennova::editor
