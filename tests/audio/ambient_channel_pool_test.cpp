// The ambient channel pool (runtime/audio/ambient_channel_pool.h): the loudest
// rows bind in rank order up to the budget, an incumbent keeps its channel
// while its rank moves, a dropout releases its channel and a re-entrant starts
// again on a free one, an unresolvable candidate is cached out (asked once)
// and the next-ranked row takes its place, a pinned pool never grows.
#include <runtime/audio/ambient_channel_pool.h>

#include <cstdio>
#include <vector>

using namespace opennova::audio;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

AmbientCandidate row(int32_t id, int32_t vol) {
	AmbientCandidate r;
	r.candidate_id = id;
	r.vol = vol;
	return r;
}

} // namespace

int main() {
	AmbientChannelPool pool(3);
	AmbientChannelPlan plan;
	int asked = 0;
	const auto always = [&](int32_t) {
		++asked;
		return true;
	};

	// Four rows, budget three: the loudest three bind in rank order.
	pool.plan({ row(10, 200), row(11, 150), row(12, 100), row(13, 50) }, always, true, plan);
	CHECK(plan.channel_count == 3 && plan.binds.size() == 3 && plan.updates.empty() && plan.released.empty());
	CHECK(plan.binds.size() == 3 && plan.binds[0].channel == 0 && plan.binds[0].row.candidate_id == 10 &&
			plan.binds[2].channel == 2 && plan.binds[2].row.candidate_id == 12);
	CHECK(asked == 3 && pool.channel_of(13) < 0);

	// The ranks move: every incumbent keeps its channel, nothing rebinds.
	pool.plan({ row(12, 220), row(10, 210), row(11, 205), row(13, 60) }, always, true, plan);
	CHECK(plan.binds.empty() && plan.released.empty() && plan.updates.size() == 3);
	CHECK(plan.updates.size() == 3 && plan.updates[0].channel == 2 && plan.updates[0].row.candidate_id == 12 &&
			plan.updates[0].row.vol == 220);
	CHECK(asked == 3); // incumbents are never re-resolved

	// 11 drops out; 13 enters on the freed channel.
	pool.plan({ row(12, 220), row(10, 210), row(13, 90) }, always, true, plan);
	CHECK(plan.released.size() == 1 && plan.released[0] == 1);
	CHECK(plan.binds.size() == 1 && plan.binds[0].channel == 1 && plan.binds[0].row.candidate_id == 13);
	CHECK(plan.updates.size() == 2 && asked == 4);

	// An unresolvable entrant is cached out and asked once; the next rank takes the channel.
	int refused = 0;
	const auto refuse_20 = [&](int32_t id) {
		if (id == 20) {
			++refused;
			return false;
		}
		return true;
	};
	pool.plan({ row(12, 220), row(10, 210), row(20, 200), row(21, 100) }, refuse_20, true, plan);
	CHECK(plan.released.size() == 1 && plan.released[0] == 1); // 13 dropped
	CHECK(plan.binds.size() == 1 && plan.binds[0].row.candidate_id == 21 && plan.binds[0].channel == 1);
	CHECK(pool.has_failed(20) && pool.failed_count() == 1 && refused == 1);
	pool.plan({ row(20, 250), row(12, 220), row(10, 210), row(21, 100) }, refuse_20, true, plan);
	CHECK(refused == 1 && pool.channel_of(20) < 0 && plan.updates.size() == 3 && plan.binds.empty());

	// A pinned pool (no player root) never grows: the fourth-ranked row stays virtual.
	AmbientChannelPool pinned(3);
	pinned.plan({ row(1, 9) }, always, false, plan);
	CHECK(plan.channel_count == 0 && plan.binds.empty());
	pinned.plan({ row(1, 9) }, always, true, plan);
	CHECK(plan.channel_count == 1 && plan.binds.size() == 1);
	pinned.plan({ row(2, 9), row(1, 8) }, always, false, plan);
	CHECK(plan.channel_count == 1 && plan.binds.empty() && plan.updates.size() == 1); // 2 waits

	// release_all frees every channel but keeps the count and the failures; reset is cold.
	pool.release_all();
	CHECK(pool.channel_count() == 3 && pool.candidate_of(0) < 0 && pool.has_failed(20));
	pool.reset();
	CHECK(pool.channel_count() == 0 && !pool.has_failed(20));

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ambient_channel_pool_test OK\n");
	return 0;
}
