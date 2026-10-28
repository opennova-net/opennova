// The main loop's clock both hosts drive (engine inmatch/main_loop_clock.h): the frame's
// elapsed time since its stamp, the frame lock's stamp handoff, the time since the last
// render, the restart across a load, and a session driven by it as the game's frame and
// opennova-serve's loop drive theirs.
// [orig: Game_MainLoop @0x52B630 - the elapsed read and the stamp @0x52B798..0x52B7B4, the
//  lock word @0x52B86C, Sleep(1) and its restamps @0x52B8B2..0x52B8DB, the render's re-base
//  read @0x52BAC8..0x52BAD2, the initialize's read @0x52B766]
//
// The inputs are minted: a fake clock whose every sleep advances it 1 ms, no wall time read.
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/main_loop_clock.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <cstdio>

namespace {

using namespace opennova;
using namespace opennova::inmatch;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

struct FakeClock final : world::FrameLockClock {
	uint32_t now = 1000;
	int sleeps = 0;
	void sleep_one_ms() override {
		++now;
		++sleeps;
	}
	uint32_t now_ms() override { return now; }
};

// A Serve Only role over a bare kernel that counts its ticks.
struct CountingRole final : Role {
	mission::MissionKernel kernel;
	int ticks = 0;
	CountingRole() { bind(kernel); }
	RoleKind kind() const override { return RoleKind::DedicatedHost; }
	void run_tick(const TickInput &) override {
		++ticks;
		++kernel.world.logic_tick;
	}
	void close() override {}
};

void elapsed_stamps_and_restarts() {
	FakeClock fake;
	MainLoopClock clock(&fake);
	CHECK(clock.frame_stamp_ms() == 0);
	// The first frame only stamps.
	CHECK(clock.take_frame_elapsed_seconds() == 0.0);
	CHECK(clock.frame_stamp_ms() == 1000);
	fake.now += 16;
	CHECK(clock.take_frame_elapsed_seconds() == 0.016);
	CHECK(clock.frame_stamp_ms() == 1016);
	// The lock's last read replaces the stamp; a frame that did not sleep leaves it.
	clock.end_frame(0);
	CHECK(clock.frame_stamp_ms() == 1016);
	clock.end_frame(1030);
	CHECK(clock.frame_stamp_ms() == 1030);
	fake.now = 1046;
	CHECK(clock.take_frame_elapsed_seconds() == 0.016);
	// A load the embedder runs outside its frames: the next frame only stamps again.
	clock.restart();
	fake.now += 3000;
	CHECK(clock.take_frame_elapsed_seconds() == 0.0);
	std::printf("  the elapsed time since the stamp, the lock's restamp, the restart\n");
}

void frame_input_takes_the_lock_word_and_the_render() {
	FakeClock fake;
	MainLoopClock clock(&fake);
	gamecfg::GameCfg cfg = gamecfg::defaults();
	// The shipped default locks (Config_SetDefaults @0x54D12F).
	CHECK(main_loop_frame_lock(&cfg));
	CHECK(!main_loop_frame_lock(nullptr));
	FrameInput first = clock.begin_frame(&cfg);
	CHECK(first.delta_seconds == 0.0);
	CHECK(first.frame_lock);
	CHECK(first.frame_stamp_ms == 1000);
	CHECK(first.since_render_seconds < 0.0);
	clock.mark_rendered();
	fake.now += 5;
	CHECK(clock.seconds_since_render() == 0.005);
	cfg.lock_framerate = 0;
	fake.now += 11;
	const FrameInput second = clock.begin_frame(&cfg);
	CHECK(second.delta_seconds == 0.016);
	CHECK(!second.frame_lock);
	CHECK(second.frame_stamp_ms == 1016);
	CHECK(second.since_render_seconds == 0.016);
	std::printf("  the frame input: the stamp, game.cfg's lock word, the time since the render\n");
}

void a_locked_session_frame_sleeps_and_hands_its_stamp_back() {
	FakeClock fake;
	MainLoopClock clock(&fake);
	gamecfg::GameCfg cfg = gamecfg::defaults();
	CountingRole role;
	Session session(role);
	session.set_frame_lock_clock(&fake);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	// The first frame stamps; the lock sleeps it to 16 ms banked (the fresh bank's 1 ms and
	// 15 sleeps), and its four quanta run one update.
	FrameOutcome outcome = session.advance(clock.begin_frame(&cfg));
	clock.end_frame(outcome.frame_stamp_ms);
	CHECK(outcome.perf.lock_sleeps == 15);
	CHECK(role.ticks == 1);
	CHECK(clock.frame_stamp_ms() == 1015);
	// The next frame measures from the lock's last read, not from its own start.
	fake.now += 2;
	const FrameInput next = clock.begin_frame(&cfg);
	CHECK(next.delta_seconds == 0.002);
	std::printf("  a locked frame sleeps to 16 ms, runs one update and restamps the clock\n");
}

void a_load_inside_a_frame_is_never_banked() {
	// A headless host's map change runs inside one frame: the session completes the next
	// map's load there, and the render after it is the stamp the next frame re-bases to, so
	// the frame banks none of the three-second load (the initialize's read, @0x52B766).
	FakeClock fake;
	MainLoopClock clock(&fake);
	gamecfg::GameCfg cfg = gamecfg::defaults();
	cfg.lock_framerate = 0;
	CountingRole role;
	Session session(role);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	(void)session.advance(clock.begin_frame(&cfg));
	clock.mark_rendered();
	fake.now += 16;
	FrameOutcome outcome = session.advance(clock.begin_frame(&cfg));
	clock.end_frame(outcome.frame_stamp_ms);
	clock.mark_rendered();
	const int before = role.ticks;
	// The map change: the load runs, then the render callback returns.
	CHECK(session.fail({SessionErrorCode::SessionLost, "map change"}).applied());
	CHECK(session.begin_load().applied());
	fake.now += 3000;
	CHECK(session.complete_load().applied());
	clock.mark_rendered();
	fake.now += 16;
	outcome = session.advance(clock.begin_frame(&cfg));
	CHECK(role.ticks - before <= 1);
	// A direct frame of the same delta (the old loop's, with no render stamp) banks the load
	// up to the bank's 500 ms clamp: 31 updates at once.
	CountingRole direct_role;
	Session direct(direct_role);
	CHECK(direct.begin_load().applied() && direct.complete_load().applied());
	FrameInput late;
	late.delta_seconds = 3.016;
	(void)direct.advance(late);
	CHECK(direct_role.ticks >= 31);
	std::printf("  a load inside a frame is never banked; a direct delta would bank 500 ms\n");
}

} // namespace

int main() {
	std::printf("main_loop_clock:\n");
	elapsed_stamps_and_restarts();
	frame_input_takes_the_lock_word_and_the_render();
	a_locked_session_frame_sleeps_and_hands_its_stamp_back();
	a_load_inside_a_frame_is_never_banked();
	if (failures != 0) {
		std::printf("main_loop_clock: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("main_loop_clock: OK\n");
	return 0;
}
