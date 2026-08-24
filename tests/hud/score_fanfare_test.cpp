// The S2C 0x81 hit-confirm tone ladder (hud/score_fanfare.h): the exact
// selection order of NapiNPClientMsg_ScoreDeltaSound @0x42a0b0 over the
// EXP_FANFARE lo/hi bytes, and the three registry trigger-set names.
#include <cstdio>
#include <string>

#include <hud/score_fanfare.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int main() {
	constexpr uint16_t kFanfare = 0x1405; // lo 5 (kill), hi 20 (headshot)
	// Disarmed: lo == 0, or lo >= hi [orig: @0x42a0e0..0x42a0f6].
	CHECK(score_delta_tone(7, 0x1400) == ScoreTone::None);
	CHECK(score_delta_tone(7, 0x0505) == ScoreTone::None);
	CHECK(score_delta_tone(7, 0x0510) == ScoreTone::None);
	// Non-positive deltas never play [orig: @0x42a0d4].
	CHECK(score_delta_tone(0, kFanfare) == ScoreTone::None);
	CHECK(score_delta_tone(-3, kFanfare) == ScoreTone::None);
	// The ladder: below lo -> HIT, [lo, hi) -> KILL, >= hi -> HEADSHOT.
	CHECK(score_delta_tone(1, kFanfare) == ScoreTone::Hit);
	CHECK(score_delta_tone(4, kFanfare) == ScoreTone::Hit);
	CHECK(score_delta_tone(5, kFanfare) == ScoreTone::Kill);
	CHECK(score_delta_tone(19, kFanfare) == ScoreTone::Kill);
	CHECK(score_delta_tone(20, kFanfare) == ScoreTone::Headshot);
	CHECK(score_delta_tone(1000, kFanfare) == ScoreTone::Headshot);
	// The registry names [orig: rows 81..83 @0x82f590].
	CHECK(std::string(score_tone_set_name(ScoreTone::Hit)) == "HITTONE");
	CHECK(std::string(score_tone_set_name(ScoreTone::Kill)) == "KILLTONE");
	CHECK(std::string(score_tone_set_name(ScoreTone::Headshot)) == "HEADSHOTTONE");
	CHECK(std::string(score_tone_set_name(ScoreTone::None)).empty());
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("score_fanfare_test OK\n");
	return 0;
}
