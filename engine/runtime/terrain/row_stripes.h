#pragma once

// Row-stripe threading for the CPU page rasters (the tile composer and the
// static-shadow alpha pass). A raster's rows are cut into fixed stripes dealt
// round-robin to lanes; each lane runs the raster's whole ordered draw list
// over only the rows it owns. Every pixel therefore sees its writes in the
// serial order, so the bytes are identical for every thread count. This is
// host scheduling around retail's GPU passes, not a port of anything retail
// ran on the CPU.

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace opennova::terrain {

inline constexpr int kRowStripeRows = 8;

// Calls lane(index) for every index in [0, lanes), on up to `lanes` threads
// including the caller. A lane whose thread cannot start runs on the caller.
template <typename Lane>
void run_row_stripe_lanes(std::size_t lanes, const Lane &lane) noexcept {
	lanes = std::max<std::size_t>(lanes, 1);
	std::vector<std::thread> helpers;
	std::size_t started = 1;
	try {
		helpers.reserve(lanes - 1);
		for (; started < lanes; ++started) {
			helpers.emplace_back([&lane, started]() { lane(started); });
		}
	} catch (...) {
	}
	lane(0);
	for (std::size_t index = started; index < lanes; ++index) lane(index);
	for (std::thread &helper : helpers) helper.join();
}

// Visits, as [begin, end) row ranges, the part of rows [row_begin, row_end)
// that `lane` of `lanes` owns.
template <typename Rows>
void for_lane_stripes(std::size_t lane, std::size_t lanes,
		int row_begin, int row_end, const Rows &rows) {
	if (row_begin >= row_end) return;
	if (lanes <= 1) {
		rows(row_begin, row_end);
		return;
	}
	const int first = row_begin / kRowStripeRows;
	const int last = (row_end - 1) / kRowStripeRows;
	const int skew = static_cast<int>(
			(lane + lanes - static_cast<std::size_t>(first) % lanes) % lanes);
	for (int stripe = first + skew; stripe <= last;
			stripe += static_cast<int>(lanes)) {
		rows(std::max(row_begin, stripe * kRowStripeRows),
				std::min(row_end, stripe * kRowStripeRows + kRowStripeRows));
	}
}

} // namespace opennova::terrain
