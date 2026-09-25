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
#include <memory>

namespace opennova::terrain {

inline constexpr int kRowStripeRows = 8;

// Keeps the shared lane pool's threads alive while held (null when the pool
// could not start; lanes then run on their caller). An owner that rasters
// repeatedly holds one so each call reuses the same threads. The pool lives
// beside its one owner, the page workers (terrain_tile_composition_worker.cpp).
using RowStripePoolLease = std::shared_ptr<void>;
RowStripePoolLease retain_row_stripe_pool();

namespace detail {
// Runs lane(index) for every index in [0, lanes) on the shared lane pool: a
// fixed set of threads the whole process's page rasters share, so the page
// workers composing several pages at once never spawn threads of their own
// (the frame waits on the slowest page, and per-call threads oversubscribed
// the cores). The caller claims lanes too and returns once all have run; a
// lane may run on any thread. Lanes must not call back into the pool.
void run_lanes_on_pool(std::size_t lanes, void (*invoke)(const void *, std::size_t),
		const void *context) noexcept;
} // namespace detail

// Calls lane(index) for every index in [0, lanes) and returns when all have
// run, on up to `lanes` threads including the caller.
template <typename Lane>
void run_row_stripe_lanes(std::size_t lanes, const Lane &lane) noexcept {
	lanes = std::max<std::size_t>(lanes, 1);
	if (lanes == 1) {
		lane(0);
		return;
	}
	detail::run_lanes_on_pool(lanes,
			[](const void *context, std::size_t index) {
				(*static_cast<const Lane *>(context))(index);
			},
			&lane);
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
