// Pins base/os_random: buffers of many sizes filled to the byte (the guard bytes either side
// untouched), two draws that differ, no all-zero fill, every byte value over a megabyte, the
// nonzero draw never 0, make_uuid_v4's spelling with no repeat, the OsRandom generator under the
// <random> distributions, and the case std::random_device fails
// where libstdc++ serves it from RDSEED on a CPU with AMD's RDSEED erratum: many threads drawing
// 32-bit values at the same moment, which must show no zero runs and no more repeats than the
// birthday bound allows.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <base/io/log.h>
#include <base/os_random/os_random.h>

#include "common/test_expect.h"

using namespace opennova;

namespace {

bool all_zero(const uint8_t *bytes, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		if (bytes[i] != 0) return false;
	}
	return true;
}

// Every size fills exactly its bytes: the guards before and after keep their pattern.
int test_fills_each_size() {
	static constexpr size_t kSizes[] = {0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 255, 256,
	                                    257, 1000, 4096, 65537};
	static constexpr size_t kGuard = 32;
	static constexpr uint8_t kPattern = 0xA5;
	os_random_bytes(nullptr, 0); // the empty fill touches nothing
	for (const size_t size : kSizes) {
		std::vector<uint8_t> first(size + 2 * kGuard, kPattern);
		std::vector<uint8_t> second(size + 2 * kGuard, kPattern);
		os_random_bytes(first.data() + kGuard, size);
		os_random_bytes(second.data() + kGuard, size);
		for (size_t i = 0; i < kGuard; ++i) {
			TEST_EXPECT(first[i] == kPattern && first[kGuard + size + i] == kPattern);
			TEST_EXPECT(second[i] == kPattern && second[kGuard + size + i] == kPattern);
		}
		if (size >= 16) {
			// 2^-128 apiece: an all-zero or repeated 16-byte fill is a broken source.
			TEST_EXPECT(!all_zero(first.data() + kGuard, size));
			TEST_EXPECT(!all_zero(second.data() + kGuard, size));
			TEST_EXPECT(!std::equal(first.begin() + kGuard, first.begin() + kGuard + size,
			                        second.begin() + kGuard));
		}
	}
	return 0;
}

int test_draws_differ() {
	TEST_EXPECT(os_random_u64() != os_random_u64());
	const uint32_t first = os_random_u32();
	bool any_differs = false;
	for (int i = 0; i < 8 && !any_differs; ++i) any_differs = os_random_u32() != first;
	TEST_EXPECT(any_differs);
	return 0;
}

// The nonzero draw: never 0, and not stuck on one value. A raw 0 is a 1-in-2^32 event, so this
// pins the contract a caller sees rather than exercising the redraw itself.
int test_nonzero_draws() {
	std::set<uint32_t> values;
	for (int i = 0; i < 4096; ++i) {
		const uint32_t v = os_random_nonzero_u32();
		TEST_EXPECT(v != 0);
		values.insert(v);
	}
	TEST_EXPECT(values.size() > 4000); // about 0.002 repeats expected
	return 0;
}

// make_uuid_v4: the text form, its version and variant digits; a thousand never repeat.
int test_uuid_v4() {
	std::set<std::string> seen;
	for (int n = 0; n < 1000; ++n) {
		const std::string a = make_uuid_v4();
		TEST_EXPECT(a.size() == 36);
		for (size_t i = 0; i < a.size(); ++i) {
			if (i == 8 || i == 13 || i == 18 || i == 23) {
				TEST_EXPECT(a[i] == '-');
			} else {
				TEST_EXPECT((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));
			}
		}
		TEST_EXPECT(a[14] == '4');
		TEST_EXPECT(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
		TEST_EXPECT(seen.insert(a).second);
	}
	return 0;
}

// A thousand 16-byte fills, none all zero; a megabyte holds every byte value.
int test_never_all_zero() {
	for (int i = 0; i < 1000; ++i) {
		uint8_t salt[16] = {};
		os_random_bytes(salt, sizeof(salt));
		TEST_EXPECT(!all_zero(salt, sizeof(salt)));
	}
	std::vector<uint8_t> megabyte(size_t(1) << 20);
	os_random_bytes(megabyte.data(), megabyte.size());
	bool seen[256] = {};
	for (const uint8_t b : megabyte) seen[b] = true;
	for (const bool s : seen) TEST_EXPECT(s);
	return 0;
}

// The generator under the distributions the service draws through: every outcome of a small
// inclusive range turns up, nothing lands outside it.
int test_generator() {
	static_assert((OsRandom::min)() == 0, "the generator spans the whole word");
	static_assert((OsRandom::max)() == ~uint64_t(0), "the generator spans the whole word");
	OsRandom gen;
	std::uniform_int_distribution<int> pick(0, 31);
	bool seen[32] = {};
	for (int i = 0; i < 4096; ++i) {
		const int v = pick(gen);
		TEST_EXPECT(v >= 0 && v <= 31);
		seen[v] = true;
	}
	for (const bool s : seen) TEST_EXPECT(s);
	std::uniform_int_distribution<uint32_t> prime_range(448, 547);
	for (int i = 0; i < 1000; ++i) {
		const uint32_t v = prime_range(gen);
		TEST_EXPECT(v >= 448 && v <= 547);
	}
	return 0;
}

// The RDSEED failure mode: kThreads threads released together, each drawing kDraws 32-bit
// values back to back. A sound source gives about n^2 / 2^33 repeats over n draws (2.0 here)
// and one zero in 2^32; a source that answers contention with 0 gives zero runs and thousands
// of repeats.
int test_concurrent_draws() {
	constexpr int kThreads = 32;
	constexpr size_t kDraws = 4096;
	std::vector<std::vector<uint32_t>> draws(kThreads, std::vector<uint32_t>(kDraws));
	std::atomic<int> ready{0};
	std::atomic<bool> go{false};
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			ready.fetch_add(1);
			while (!go.load()) std::this_thread::yield();
			for (uint32_t &value : draws[t]) value = os_random_u32();
		});
	}
	while (ready.load() < kThreads) std::this_thread::yield();
	go.store(true);
	for (std::thread &thread : threads) thread.join();

	size_t zeros = 0;
	size_t zero_runs = 0;
	std::vector<uint32_t> all;
	all.reserve(kThreads * kDraws);
	for (const std::vector<uint32_t> &thread_draws : draws) {
		for (size_t i = 0; i < thread_draws.size(); ++i) {
			if (thread_draws[i] != 0) continue;
			++zeros;
			if (i > 0 && thread_draws[i - 1] == 0) ++zero_runs;
		}
		all.insert(all.end(), thread_draws.begin(), thread_draws.end());
	}
	std::sort(all.begin(), all.end());
	size_t repeats = 0;
	for (size_t i = 1; i < all.size(); ++i) {
		if (all[i] == all[i - 1]) ++repeats;
	}
	const double n = static_cast<double>(all.size());
	const double expected_repeats = n * (n - 1.0) / 2.0 / 4294967296.0;
	// Poisson(2): more than 15 repeats is under a 1-in-10^9 event for a sound source.
	const size_t repeat_bound = static_cast<size_t>(expected_repeats * 4.0) + 8;
	std::printf("concurrent draws: %zu values over %d threads, %zu zeros, %zu zero runs, "
	            "%zu repeats (expected %.2f, bound %zu)\n",
	            all.size(), kThreads, zeros, zero_runs, repeats, expected_repeats, repeat_bound);
	TEST_EXPECT(zero_runs == 0);
	TEST_EXPECT(zeros <= 1); // 2 or more in 131072 draws: about 1 in 2 * 10^9
	TEST_EXPECT(repeats <= repeat_bound);
	return 0;
}

void stderr_sink(io::LogLevel, const char *message) { std::fprintf(stderr, "%s\n", message); }

} // namespace

int main() {
	io::set_log_sink(stderr_sink); // the abort path's reason, should a call fail
	int failures = 0;
	failures += test_fills_each_size();
	failures += test_draws_differ();
	failures += test_never_all_zero();
	failures += test_nonzero_draws();
	failures += test_uuid_v4();
	failures += test_generator();
	failures += test_concurrent_draws();
	if (failures == 0) std::printf("os_random_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
