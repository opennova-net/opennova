// opennova::audio::SoundOptions (runtime/audio/sound_options.h): the single-player round end's
// park of the sound-effects option word. A park saves the word and zeroes it only when nothing is
// saved, and the restore puts the saved word back once [orig: Sound_ParkEffectsVolume @ 0x5280b0;
// Sound_RestoreEffectsVolume @ 0x5280d0].
#include <runtime/audio/sound_options.h>

#include "common/test_expect.h"

using opennova::audio::SoundOptions;

int main() {
	SoundOptions options;
	TEST_EXPECT(!options.parked());
	TEST_EXPECT(options.sound_fx() == opennova::audio::kDefaultChannelVolume);
	options.set(200, 150, 100);

	// Nothing to restore before a park.
	options.restore();
	TEST_EXPECT(options.sound_fx() == 200);

	// The park saves the word and zeroes it.
	options.park();
	TEST_EXPECT(options.parked() && options.sound_fx() == 0);
	TEST_EXPECT(options.dialogue() == 150 && options.music() == 100);
	// A repeat of the park saves nothing over it (the zeroed word must not become the saved one).
	options.park();
	TEST_EXPECT(options.parked() && options.sound_fx() == 0);

	options.restore();
	TEST_EXPECT(!options.parked() && options.sound_fx() == 200);
	// The restore clears the park: a second one writes nothing.
	options.set(50, 150, 100);
	options.restore();
	TEST_EXPECT(options.sound_fx() == 50);

	// A word of 0 parks as nothing saved: retail's saved word 0 is no park.
	options.set(0, 150, 100);
	options.park();
	TEST_EXPECT(!options.parked());

	// (volume * option) >> 8
	TEST_EXPECT(opennova::audio::scaled_volume(255, 192) == 191);
	TEST_EXPECT(opennova::audio::scaled_volume(255, 0) == 0);
	return 0;
}
