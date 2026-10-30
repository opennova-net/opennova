// opennova::audio::EffectsVolumePark (runtime/audio/effects_volume_park.h): the
// single-player round end's park of the sound-effects option, moved out of the Godot
// mission audio (ADR 0077 R2). A park saves the outputs' levels only when nothing is
// saved, the outputs hold the zeroed option's level while it stands, and the restore
// hands the saved levels back once [orig: Sound_ParkEffectsVolume @ 0x5280b0;
// Sound_RestoreEffectsVolume @ 0x5280d0].
#include <runtime/audio/effects_volume_park.h>

#include "common/test_expect.h"

using opennova::audio::EffectsVolumePark;

int main() {
	EffectsVolumePark park;
	TEST_EXPECT(!park.parked());
	TEST_EXPECT(EffectsVolumePark::parked_db() == opennova::audio::kVolumeSilentDb);

	// Nothing to restore before a park.
	EffectsVolumePark::Levels out{ 1.0, 2.0 };
	TEST_EXPECT(!park.restore(out));
	TEST_EXPECT(out[0] == 1.0 && out[1] == 2.0);

	// The park saves the levels it finds.
	TEST_EXPECT(park.park({ -6.0, -3.0 }));
	TEST_EXPECT(park.parked());
	// A repeat of the park saves nothing over them (the outputs then read the
	// parked level, which must not become the saved one).
	TEST_EXPECT(!park.park({ EffectsVolumePark::parked_db(), EffectsVolumePark::parked_db() }));

	TEST_EXPECT(park.restore(out));
	TEST_EXPECT(out[EffectsVolumePark::kEffectsOutput] == -6.0);
	TEST_EXPECT(out[EffectsVolumePark::kAmbientOutput] == -3.0);
	TEST_EXPECT(!park.parked());
	// The restore clears the park: a second one writes nothing.
	out = { 0.0, 0.0 };
	TEST_EXPECT(!park.restore(out));
	TEST_EXPECT(out[0] == 0.0 && out[1] == 0.0);

	// A new park after the restore saves the levels then.
	TEST_EXPECT(park.park({ -1.0, -2.0 }));
	TEST_EXPECT(park.restore(out) && out[0] == -1.0 && out[1] == -2.0);
	return 0;
}
