#pragma once

#include <cstdint>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Reads one named begin/end RenderingDevice timestamp pair out of the
// completed frame's captures and reports the GPU span in microseconds.
// RenderingDevice publishes captures with its frame delay, so the span
// describes the previous completed frame; returns false when either endpoint
// is missing (timing just enabled, or the pass did not run that frame).
// capture_timestamp() splits the RD graph with a full barrier, so callers
// gate the whole mechanism on the F3 Stats capture being active.
inline bool rd_timestamp_span_us(RenderingDevice *p_rd,
		const String &p_begin_name, const String &p_end_name,
		std::uint64_t &r_span_us) {
	r_span_us = 0;
	if (p_rd == nullptr)
		return false;
	const int64_t count = p_rd->get_captured_timestamps_count();
	std::uint64_t begin_gpu = 0;
	std::uint64_t end_gpu = 0;
	bool have_begin = false;
	bool have_end = false;
	for (int64_t i = 0; i < count; ++i) {
		const String name = p_rd->get_captured_timestamp_name(i);
		if (name == p_begin_name) {
			begin_gpu = static_cast<std::uint64_t>(
					p_rd->get_captured_timestamp_gpu_time(i));
			have_begin = true;
		} else if (name == p_end_name) {
			end_gpu = static_cast<std::uint64_t>(
					p_rd->get_captured_timestamp_gpu_time(i));
			have_end = true;
		}
	}
	if (!have_begin || !have_end || end_gpu < begin_gpu)
		return false;
	// get_captured_timestamp_gpu_time reports nanoseconds (measured against
	// the known ~1.6 ms Q3 pass on the dev box); the stats slots carry µs.
	r_span_us = (end_gpu - begin_gpu) / 1000u;
	return true;
}

} // namespace godot
