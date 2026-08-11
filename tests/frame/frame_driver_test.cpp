// frame::FrameDriver — the engine-owned main-loop shape (ADR 0033 R1): the
// bank/catch-up/present-once batch, the per-tick leg order, the zero-tick
// rows-only present, and the fixed post-batch frame-leg order the shell used
// to hand-sequence in GDScript.
#include <frame/frame_driver.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

struct Trace {
	std::vector<std::string> calls;
	int32_t tick = 100;
	bool step_result = true;

	opennova::frame::FrameHooks hooks() {
		opennova::frame::FrameHooks h;
		h.terrain = [this] { calls.push_back("terrain"); };
		h.foliage = [this] { calls.push_back("foliage"); };
		h.stamp_listener = [this] { calls.push_back("listener"); };
		h.step = [this] {
			calls.push_back("step");
			if (step_result) {
				++tick;
			}
			return step_result;
		};
		h.logic_tick = [this] { return tick; };
		h.begin_effect_tick = [this](int32_t t) {
			calls.push_back("effect_tick:" + std::to_string(t));
		};
		h.sync_fixed_effects = [this] { calls.push_back("sync_fixed"); };
		h.drain_effects = [this] { calls.push_back("drain"); };
		h.fixed_tick_completed = [this](int32_t t) {
			calls.push_back("fixed_done:" + std::to_string(t));
		};
		h.present_rows = [this] { calls.push_back("present_rows"); };
		h.present_frame = [this] { calls.push_back("present_frame"); };
		h.present_local_view = [this] { calls.push_back("local_view"); };
		h.net_drive = [this] { calls.push_back("net_drive"); };
		h.weather = [this] { calls.push_back("weather"); };
		h.blink_gates = [this] { calls.push_back("blink"); };
		h.occlusion_frame = [this] { calls.push_back("occlusion"); };
		h.materials = [this] { calls.push_back("materials"); };
		h.particles = [this] { calls.push_back("particles"); };
		h.iris_samples = [this] { calls.push_back("iris"); };
		h.audio = [this](int32_t n) {
			calls.push_back("audio:" + std::to_string(n));
		};
		return h;
	}
};

std::string joined(const std::vector<std::string> &v) {
	std::string out;
	for (const std::string &s : v) {
		if (!out.empty()) out += " ";
		out += s;
	}
	return out;
}

}  // namespace

int main() {
	using opennova::frame::FrameDriver;
	using opennova::world::TickAccumulator;

	// One 16 ms frame: exactly one tick, the per-tick legs in order, present
	// ONCE after the batch, then the frame legs in the fixed order.
	{
		FrameDriver driver;
		Trace t;
		const int32_t ran = driver.run_frame(TickAccumulator::kTickDt, t.hooks());
		if (!expect(ran == 1, "one quantum runs one tick")) return 1;
		const std::string want =
				"terrain foliage listener step effect_tick:101 sync_fixed drain "
				"fixed_done:101 present_frame local_view net_drive weather blink "
				"occlusion materials particles iris audio:1";
		if (!expect(joined(t.calls) == want, "the frame order is fixed")) {
			std::fprintf(stderr, "  got:  %s\n  want: %s\n",
					joined(t.calls).c_str(), want.c_str());
			return 1;
		}
	}

	// A zero-tick frame presents the rows alone (submission re-evaluates every
	// render frame), skips blink, and still runs the other frame legs.
	{
		FrameDriver driver;
		Trace t;
		const int32_t ran = driver.run_frame(TickAccumulator::kTickDt / 2.0, t.hooks());
		if (!expect(ran == 0, "half a quantum runs nothing")) return 1;
		const std::string want =
				"terrain foliage listener present_rows local_view net_drive weather "
				"occlusion materials particles iris audio:0";
		if (!expect(joined(t.calls) == want,
				"the zero-tick frame presents rows only and skips blink")) {
			std::fprintf(stderr, "  got:  %s\n  want: %s\n",
					joined(t.calls).c_str(), want.c_str());
			return 1;
		}
		// The banked remainder crosses the quantum on the next half-frame.
		t.calls.clear();
		if (!expect(driver.run_frame(TickAccumulator::kTickDt / 2.0, t.hooks()) == 1,
				"the banked remainder fires on the next frame")) return 1;
	}

	// A catch-up batch runs N ticks and still presents exactly once; the perf
	// record carries the count.
	{
		FrameDriver driver;
		Trace t;
		const int32_t ran = driver.run_frame(0.1, t.hooks());
		if (!expect(ran == 6, "0.1 s banks 6 fixed-step ticks")) return 1;
		int presents = 0;
		int steps = 0;
		for (const std::string &c : t.calls) {
			if (c == "present_frame") ++presents;
			if (c == "step") ++steps;
		}
		if (!expect(presents == 1 && steps == 6,
				"6 steps, one present after the batch")) return 1;
		if (!expect(driver.perf().ticks == 6 && driver.perf().did_tick,
				"perf carries the batch tick count")) return 1;
	}

	// The catch-up clamp: a long stall runs kMaxCatchupTicks and drops the
	// backlog rather than carrying it.
	{
		FrameDriver driver;
		Trace t;
		if (!expect(driver.run_frame(1.0, t.hooks()) ==
						TickAccumulator::kMaxCatchupTicks,
				"a long stall clamps to the catch-up cap")) return 1;
		t.calls.clear();
		if (!expect(driver.run_frame(0.001, t.hooks()) == 0,
				"the clamped backlog was dropped, not carried")) return 1;
	}

	// A declined step (the sim's own gates) runs no per-tick legs but the
	// batch still full-presents only when a tick RAN; here nothing ran, and
	// the accumulator consumed the quantum regardless.
	{
		FrameDriver driver;
		Trace t;
		t.step_result = false;
		const int32_t ran = driver.run_frame(TickAccumulator::kTickDt, t.hooks());
		if (!expect(ran == 0, "a declined step counts zero ticks")) return 1;
		const std::string want =
				"terrain foliage listener step present_frame local_view net_drive "
				"weather occlusion materials particles iris audio:0";
		if (!expect(joined(t.calls) == want,
				"declined step: no per-tick legs, no blink")) {
			std::fprintf(stderr, "  got:  %s\n  want: %s\n",
					joined(t.calls).c_str(), want.c_str());
			return 1;
		}
	}

	// The single-step primitive: listener + one step + per-tick legs + full
	// present, no frame legs, no accumulator interaction.
	{
		FrameDriver driver;
		Trace t;
		if (!expect(driver.run_single(t.hooks()), "single step runs the tick")) return 1;
		const std::string want =
				"listener step effect_tick:101 sync_fixed drain fixed_done:101 "
				"present_frame local_view";
		if (!expect(joined(t.calls) == want, "single-step order")) {
			std::fprintf(stderr, "  got:  %s\n  want: %s\n",
					joined(t.calls).c_str(), want.c_str());
			return 1;
		}
		if (!expect(driver.accumulator().banked() == 0.0,
				"single step never touches the bank")) return 1;
	}

	// reset_bank discards a paused frame's backlog (Play must not burst).
	{
		FrameDriver driver;
		Trace t;
		(void)driver.run_frame(TickAccumulator::kTickDt / 2.0, t.hooks());
		driver.reset_bank();
		t.calls.clear();
		if (!expect(driver.run_frame(TickAccumulator::kTickDt / 2.0, t.hooks()) == 0,
				"reset_bank dropped the banked remainder")) return 1;
	}

	std::printf("OK: frame_driver order/batch/clamp/single/reset\n");
	return 0;
}
