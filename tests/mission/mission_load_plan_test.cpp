// Pins the mission load plan (runtime/mission/mission_load_plan.h): the stage
// order, the witnessed progress anchors (each a value from Game_StartMission's
// schedule), their monotonic climb, and the out-of-range rule.

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

	check(kMissionLoadStageCount == 7, "seven stages");
	// The plan rows sit in enum order so the stage IS the row index.
	for (int i = 0; i < kMissionLoadStageCount; ++i)
		check(static_cast<int>(kMissionLoadPlan[i].stage) == i, "row order = stage order");

	// The witnessed anchors, in load order.
	const int expected[] = { 2, 6, 26, 41, 70, 90, 95 };
	const char *names[] = { "environment", "terrain", "objects", "runtime", "audio",
		"effects", "finish" };
	for (int i = 0; i < kMissionLoadStageCount; ++i) {
		check(kMissionLoadPlan[i].progress_percent == expected[i], "anchor value");
		check(std::strcmp(kMissionLoadPlan[i].name, names[i]) == 0, "stage name");
		check(mission_load_progress_percent(static_cast<MissionLoadStage>(i)) == expected[i],
				"lookup = row");
	}
	// Strictly climbing, and every stage sits below the completion value.
	for (int i = 1; i < kMissionLoadStageCount; ++i)
		check(kMissionLoadPlan[i].progress_percent > kMissionLoadPlan[i - 1].progress_percent,
				"monotonic anchors");
	check(kMissionLoadPlan[kMissionLoadStageCount - 1].progress_percent < kMissionLoadProgressComplete,
			"last stage below complete");
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
