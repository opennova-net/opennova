// The main loop's clock (runtime/world/tick_count_clock.h, D-NET-NEW-LF-2): the bank and its
// frame lock read GetTickCount, which moves only on the system timer's 1/64 s interrupt, so the
// bank grows in 15 and 16 ms steps and a lock short of 16 ms after a 15 ms step sleeps on to the
// next one. [orig: Game_MainLoop @0x52B630 - GetTickCount @0x52B798 (the frame's elapsed, the add
// @0x52B7B2), the lock's read @0x52B8B8 after Sleep(1) @0x52B8B2, the drain @0x52BA08..0x52BA6B]
//
// The oracles are retail's arithmetic in its 1/16 ms units over a fake microsecond counter that
// advances 1 ms per Sleep(1) and 2 ms of work per frame, read through the timer's steps (no wall
// time is read). From the step at 1000 ms (the 64th interrupt): the first frame's elapsed reads 0
// and its bank holds the seed 16; the lock sleeps 14 times to the next interrupt (1015 ms, +15:
// 256 units) and the drain runs one update. The second frame smooths (7 * 16 + 0 + 4) >> 3 = 14,
// sleeps 14 times to 1031 (+16: 270) and runs one update, leaving 14. The third smooths 14 again,
// its next step is +15 (254, short of 256), so the lock sleeps on through 1046 to 1062 (+16: 510),
// 29 sleeps, and the drain's seven quanta run two updates, leaving 62.
#include <runtime/world/tick_accumulator.h>
#include <runtime/world/tick_count_clock.h>

#include <cstdint>
#include <cstdio>

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

// A monotonic microsecond counter read through the system timer's steps; Sleep(1) is 1 ms.
struct TimerClock final : world::FrameLockClock {
	uint64_t us = 64 * world::kSystemTimerIntervalUs; // the 64th interrupt: 1000 ms
	void sleep_one_ms() override { us += 1000; }
	uint32_t now_ms() override { return world::tick_count_ms_from_us(us); }
};

void test_the_clock_steps_on_the_system_timer() {
	// 0, 15, 31, 46, 62, ...: each interrupt's whole milliseconds, held until the next.
	CHECK(world::tick_count_ms_from_us(0) == 0);
	CHECK(world::tick_count_ms_from_us(world::kSystemTimerIntervalUs - 1) == 0);
	CHECK(world::tick_count_ms_from_us(world::kSystemTimerIntervalUs) == 15);
	CHECK(world::tick_count_ms_from_us(2 * world::kSystemTimerIntervalUs) == 31);
	CHECK(world::tick_count_ms_from_us(3 * world::kSystemTimerIntervalUs) == 46);
	CHECK(world::tick_count_ms_from_us(4 * world::kSystemTimerIntervalUs) == 62);
	CHECK(world::tick_count_ms_from_us(64 * world::kSystemTimerIntervalUs) == 1000);
	// Every step is 15 or 16 ms, and 64 of them make a second.
	uint32_t total = 0;
	bool steps_ok = true;
	for (uint64_t n = 0; n < 64; ++n) {
		const uint32_t step = world::tick_count_ms_from_us((n + 1) * world::kSystemTimerIntervalUs) -
				world::tick_count_ms_from_us(n * world::kSystemTimerIntervalUs);
		if (step != 15 && step != 16) steps_ok = false;
		total += step;
	}
	CHECK(steps_ok);
	CHECK(total == 1000);
	// The live clock never reads 0 (an embedder's "no stamp").
	CHECK(world::frame_tick_count_ms() != 0);
}

// One frame: 2 ms of work, the elapsed read from the stamp through the clock, the bank and lock.
int frame(world::TickAccumulator &bank, TimerClock &clock, uint32_t &stamp, int &sleeps) {
	clock.us += 2000;
	const uint32_t now = clock.now_ms();
	const double elapsed = static_cast<double>(now - stamp) / 1000.0;
	stamp = now;
	world::FrameLock lock;
	lock.clock = &clock;
	lock.stamp_ms = stamp;
	const int ticks = bank.bank(elapsed, &lock);
	sleeps = lock.sleeps;
	stamp = lock.stamp_ms;
	return ticks;
}

void test_the_lock_steps_with_the_timer() {
	TimerClock clock;
	world::TickAccumulator bank;
	uint32_t stamp = clock.now_ms();
	CHECK(stamp == 1000);
	int sleeps = 0;

	CHECK(frame(bank, clock, stamp, sleeps) == 1);
	CHECK(sleeps == 14);
	CHECK(stamp == 1015);
	CHECK(bank.banked() == 0.0);

	CHECK(frame(bank, clock, stamp, sleeps) == 1);
	CHECK(sleeps == 14);
	CHECK(stamp == 1031);
	CHECK(bank.banked() == 14.0 / 16000.0);

	// A 15 ms step leaves the bank at 254, short of 16 ms: the lock sleeps on to the next step.
	CHECK(frame(bank, clock, stamp, sleeps) == 2);
	CHECK(sleeps == 29);
	CHECK(stamp == 1062);
	CHECK(bank.banked() == 62.0 / 16000.0);
}

} // namespace

int main() {
	test_the_clock_steps_on_the_system_timer();
	test_the_lock_steps_with_the_timer();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tick_count_clock: ok\n");
	return 0;
}
