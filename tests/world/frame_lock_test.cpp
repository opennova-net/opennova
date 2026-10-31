// The main loop's frame lock (game.cfg lock_framerate, the config's default 1): a frame that
// banked under 16 ms after the smoothing sleeps 1 ms at a time, each sleep adding the
// milliseconds since the last stamp, until 16 ms stand banked or 500 sleeps ran. The
// smoothing history keeps the value the EMA stored ahead of the lock; the FR counter and the
// drain take the locked bank; the lock's last read is the stamp the next frame measures from.
// [orig: Config_SetDefaults @0x54D12F (lock_framerate = 1); Game_MainLoop - the word read
//  @0x52B860, the history's store @0x52B866, the unsigned test @0x52B8A4..0x52B8A6, the 500 cap
//  @0x52B8A8..0x52B8AE, Sleep(1) @0x52B8B2, the add @0x52B8B8..0x52B8D0, the restamps
//  @0x52B8D7 / @0x52B8DB, the loop test @0x52B8DF, the FR sum @0x52B8F4, the drain
//  @0x52BA08..0x52BA6B]
//
// The oracles are retail's arithmetic in its 1/16 ms units over a fake clock that advances
// 1 ms per sleep (no wall time is read): a fresh bank of 16 units plus a 2 ms frame (32) is
// 48, which the lock lifts by 16 per sleep to 256 in 13 sleeps, and the drain's four 4 ms
// quanta run one logic update; the next 2 ms frame smooths (7 * 48 + 32 + 4) >> 3 = 46
// against the PRE-lock history and sleeps 14 times to 270, leaving 14 units; the third
// smooths (7 * 46 + 46 + 4) >> 3 = 46 again (a post-lock history of 256 would give 242 and
// one sleep). Unlocked, the same first frame runs no update.
#include <base/io/perf_clock.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/tick_accumulator.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include "../common/title_fixture.h"

namespace {

using namespace opennova;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

// A clock whose every sleep advances it `step_ms` (0: a clock that never moves).
struct FakeClock final : world::FrameLockClock {
	uint32_t now = 1000;
	uint32_t step_ms = 1;
	int sleeps = 0;
	void sleep_one_ms() override {
		now += step_ms;
		++sleeps;
	}
	uint32_t now_ms() override { return now; }
};

void test_a_short_frame_sleeps_to_sixteen_ms_and_runs_one_update() {
	FakeClock clock;
	world::TickAccumulator locked;
	world::FrameLock lock;
	lock.clock = &clock;
	lock.stamp_ms = clock.now;
	CHECK(locked.bank(0.002, &lock) == 1);
	CHECK(lock.sleeps == 13);
	CHECK(lock.stamp_ms == 1013);
	// The drain took all 256 units.
	CHECK(locked.banked() == 0.0);

	// The next frame smooths against the history's pre-lock 48.
	world::FrameLock second;
	second.clock = &clock;
	second.stamp_ms = lock.stamp_ms;
	CHECK(locked.bank(0.002, &second) == 1);
	CHECK(second.sleeps == 14);
	CHECK(second.stamp_ms == 1027);
	CHECK(locked.banked() == 14.0 / 16000.0);

	world::FrameLock third;
	third.clock = &clock;
	third.stamp_ms = second.stamp_ms;
	CHECK(locked.bank(0.002, &third) == 1);
	CHECK(third.sleeps == 14);

	// Unlocked, the same first frame drains nothing.
	world::TickAccumulator unlocked;
	CHECK(unlocked.bank(0.002) == 0);
}

void test_a_frame_that_banked_sixteen_ms_does_not_sleep() {
	FakeClock clock;
	world::TickAccumulator acc;
	world::FrameLock lock;
	lock.clock = &clock;
	lock.stamp_ms = clock.now;
	// 16 + 20 ms = 336 units, no history: past the floor, no sleep; five quanta from phase 0
	// run two updates (phases 0 and 4).
	CHECK(acc.bank(0.020, &lock) == 2);
	CHECK(lock.sleeps == 0);
	CHECK(lock.stamp_ms == 1000);
	CHECK(clock.sleeps == 0);
}

void test_the_lock_stops_at_five_hundred_sleeps() {
	FakeClock clock;
	clock.step_ms = 0;
	world::TickAccumulator acc;
	world::FrameLock lock;
	lock.clock = &clock;
	lock.stamp_ms = clock.now;
	CHECK(acc.bank(0.002, &lock) == 0);
	CHECK(lock.sleeps == world::TickAccumulator::kFrameLockMaxSleeps);
	CHECK(lock.sleeps == 500);
}

// Every locked frame sums at least 256 units into the FR window, so its frame rate reads at
// most 16000 / 256 = 62; an unlocked 2 ms stream reads far past it.
void test_the_frame_rate_reads_the_locked_bank() {
	FakeClock clock;
	world::TickAccumulator locked;
	uint32_t stamp = clock.now;
	for (int i = 0; i < 200 && locked.average_fps() == 0; ++i) {
		world::FrameLock lock;
		lock.clock = &clock;
		lock.stamp_ms = stamp;
		(void)locked.bank(0.002, &lock);
		stamp = lock.stamp_ms;
	}
	CHECK(locked.average_fps() > 0);
	CHECK(locked.average_fps() <= 62);

	world::TickAccumulator unlocked;
	for (int i = 0; i < 2000 && unlocked.average_fps() == 0; ++i) (void)unlocked.bank(0.002);
	CHECK(unlocked.average_fps() > 62);
}

// The session's live frame: the lock runs when the frame asks for it and carries the
// embedder's stamp; the outcome hands back the lock's last read. A frame with no stamp (a
// test's or a probe's direct frame) never locks.
struct ProbeRole final : inmatch::Role {
	mission::MissionKernel kernel;
	bool kernel_jo = test_titles::jo_world(kernel.world);
	int ticks = 0;
	ProbeRole() { bind(kernel); }
	inmatch::RoleKind kind() const override { return inmatch::RoleKind::SinglePlayer; }
	void run_tick(const inmatch::TickInput &) override { ++ticks; }
	void close() override {}
};

void test_the_session_locks_a_live_frame_only() {
	auto role = std::make_unique<ProbeRole>();
	test_titles::jo_controls(*role);
	auto session = std::make_unique<inmatch::Session>(*role);
	CHECK(session->begin_load().applied() && session->complete_load().applied());
	FakeClock clock;
	session->set_frame_lock_clock(&clock);

	inmatch::FrameInput live;
	live.delta_seconds = 0.002;
	live.frame_lock = true;
	live.frame_stamp_ms = clock.now;
	const inmatch::FrameOutcome first = session->advance(live);
	CHECK(first.ticks_run() == 1);
	CHECK(first.frame_stamp_ms == 1013);
	CHECK(clock.sleeps == 13);
	CHECK(first.perf.lock_sleeps == 13);

	// The word clear: no lock, no stamp handed back.
	inmatch::FrameInput off = live;
	off.frame_lock = false;
	off.frame_stamp_ms = first.frame_stamp_ms;
	const inmatch::FrameOutcome second = session->advance(off);
	CHECK(second.frame_stamp_ms == 0);
	CHECK(clock.sleeps == 13);
	CHECK(second.perf.lock_sleeps == 0);

	// No stamp (not a live frame): no lock.
	inmatch::FrameInput direct = live;
	direct.frame_stamp_ms = 0;
	const inmatch::FrameOutcome third = session->advance(direct);
	CHECK(third.frame_stamp_ms == 0);
	CHECK(clock.sleeps == 13);
	CHECK(third.perf.lock_sleeps == 0);
}

// The session frame's phases, an instrument (FramePerf; the embedder's SIM_FRAME_* stats
// slots): the bank counts the lock's sleeps, the updates' span holds their ticks, and the bank,
// the updates and the legs past the drain are disjoint spans inside the frame's wall.
struct SpinRole final : inmatch::Role {
	mission::MissionKernel kernel;
	bool kernel_jo = test_titles::jo_world(kernel.world);
	SpinRole() { bind(kernel); }
	inmatch::RoleKind kind() const override { return inmatch::RoleKind::SinglePlayer; }
	void run_tick(const inmatch::TickInput &) override {
		const uint64_t start = io::perf_now_us();
		while (io::perf_now_us() - start < 1500) {
		}
	}
	void close() override {}
};

void test_the_frame_names_its_phases() {
	auto role = std::make_unique<SpinRole>();
	test_titles::jo_controls(*role);
	auto session = std::make_unique<inmatch::Session>(*role);
	CHECK(session->begin_load().applied() && session->complete_load().applied());
	FakeClock clock;
	session->set_frame_lock_clock(&clock);

	inmatch::FrameInput live;
	live.delta_seconds = 0.002;
	live.frame_lock = true;
	live.frame_stamp_ms = clock.now;
	const uint64_t start = io::perf_now_us();
	const inmatch::FrameOutcome out = session->advance(live);
	const int64_t wall = static_cast<int64_t>(io::perf_now_us() - start);
	CHECK(out.ticks_run() == 1);
	CHECK(out.perf.lock_sleeps == 13);
	CHECK(out.perf.tick_us >= 1500);
	CHECK(out.perf.bank_us >= 0);
	CHECK(out.perf.render_legs_us >= 0);
	CHECK(out.perf.bank_us + out.perf.tick_us + out.perf.render_legs_us <= wall);
	// The input frame's parts sit inside the updates' span, beside the tick.
	CHECK(out.perf.input_rows_us >= 0 && out.perf.input_drain_us >= 0 &&
			out.perf.input_apply_us >= 0);
	CHECK(out.perf.input_rows_us + out.perf.input_drain_us + out.perf.input_apply_us + 1500 <=
			out.perf.tick_us);
}

} // namespace

int main() {
	test_a_short_frame_sleeps_to_sixteen_ms_and_runs_one_update();
	test_a_frame_that_banked_sixteen_ms_does_not_sleep();
	test_the_lock_stops_at_five_hundred_sleeps();
	test_the_frame_rate_reads_the_locked_bank();
	test_the_session_locks_a_live_frame_only();
	test_the_frame_names_its_phases();
	if (failures != 0) {
		std::printf("frame_lock: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("frame_lock: ok\n");
	return 0;
}
