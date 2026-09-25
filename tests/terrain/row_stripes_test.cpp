// The shared row-stripe lane pool (runtime/terrain/row_stripes.h): every lane
// of every call runs exactly once before the call returns, with or without a
// held lease, and with many callers sharing the pool at once (the page
// workers composing several pages in one frame).

#include <runtime/terrain/row_stripes.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using opennova::terrain::retain_row_stripe_pool;
using opennova::terrain::run_row_stripe_lanes;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Runs `calls` batches of `lanes` lanes; each lane bumps its own counter, and
// the caller checks every counter once the call returns.
bool run_batches(std::size_t lanes, int calls) {
	std::vector<std::atomic<int>> hits(lanes);
	for (int call = 0; call < calls; ++call) {
		for (auto &hit : hits) hit.store(0);
		run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
			hits[lane].fetch_add(1);
		});
		for (const auto &hit : hits) {
			if (hit.load() != 1) return false;
		}
	}
	return true;
}

} // namespace

int main() {
	// Without a lease a call builds and retires a pool around itself.
	if (!expect(run_batches(1, 4), "one lane runs on the caller")) return 1;
	if (!expect(run_batches(8, 16), "an unleased call runs every lane once")) return 1;

	// With a lease the pool persists across calls.
	{
		auto lease = retain_row_stripe_pool();
		if (!expect(lease != nullptr, "the pool starts")) return 1;
		if (!expect(run_batches(3, 200), "a leased pool runs every lane once")) return 1;
		if (!expect(run_batches(64, 50), "more lanes than threads still all run")) return 1;

		// Eight concurrent callers (the page workers) share the pool.
		std::atomic<bool> ok{ true };
		std::vector<std::thread> callers;
		for (int caller = 0; caller < 8; ++caller) {
			callers.emplace_back([&ok, caller]() {
				if (!run_batches(static_cast<std::size_t>(2 + caller), 300)) ok.store(false);
			});
		}
		for (std::thread &thread : callers) thread.join();
		if (!expect(ok.load(), "concurrent callers each see every lane once")) return 1;
	}

	// The lease released, a later call still works.
	if (!expect(run_batches(5, 8), "a call after the lease drops runs every lane")) return 1;
	std::printf("row_stripes_test: OK\n");
	return 0;
}
