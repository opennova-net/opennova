// Pins the mission load plan (runtime/mission/mission_load_plan.h): the stage
// order, OpenNova's exact real-operation checkpoints, their monotonic climb,
// and the world-ready/completion boundaries.

#include <runtime/mission/mission_load_plan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

} // namespace

int main() {
	using namespace opennova::mission;

	check(kMissionLoadStageCount == 9, "nine real loading stages");
	// The plan rows sit in enum order so the stage IS the row index.
	for (int i = 0; i < kMissionLoadStageCount; ++i)
		check(static_cast<int>(kMissionLoadPlan[i].stage) == i, "row order = stage order");

	// Deterministic checkpoints, in actual pipeline order. Ten points remain
	// reserved for the shell's world-ready wait and final presentation edge.
	const int expected[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80 };
	const char *names[] = { "mission_setup", "environment", "terrain", "objects",
		"runtime", "audio", "effects", "effects_warm", "finish" };
	for (int i = 0; i < kMissionLoadStageCount; ++i) {
		check(kMissionLoadPlan[i].progress_percent == expected[i], "anchor value");
		check(std::strcmp(kMissionLoadPlan[i].name, names[i]) == 0, "stage name");
		check(mission_load_progress_percent(static_cast<MissionLoadStage>(i)) == expected[i],
				"lookup = row");
	}
	// Strictly climbing, and every stage sits below world-ready and completion.
	for (int i = 1; i < kMissionLoadStageCount; ++i)
		check(kMissionLoadPlan[i].progress_percent > kMissionLoadPlan[i - 1].progress_percent,
				"monotonic anchors");
	check(kMissionLoadPlan[kMissionLoadStageCount - 1].progress_percent <
			kMissionLoadProgressWorldReady, "last stage below world ready");
	check(kMissionLoadProgressWorldReady == 90, "world ready = 90");
	check(kMissionLoadProgressWorldReady < kMissionLoadProgressComplete,
			"world ready below presentation complete");
	check(kMissionLoadProgressComplete == 100, "complete = 100");

	// Out-of-range stages never present a regression.
	check(mission_load_progress_percent(MissionLoadStage::kCount) == kMissionLoadProgressComplete,
			"kCount -> complete");
	check(mission_load_progress_percent(static_cast<MissionLoadStage>(-1)) == kMissionLoadProgressComplete,
			"negative -> complete");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("mission_load_plan_test: ok");
	return EXIT_SUCCESS;
}
