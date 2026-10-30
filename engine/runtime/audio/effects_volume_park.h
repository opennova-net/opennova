#pragma once

// The sound-effects option's park: the single-player round end saves the option
// and zeroes it, unless one is saved already, so every world, ambient and
// interface sound falls silent through the end screens; the cine release puts
// the saved option back and clears the park [orig: Sound_ParkEffectsVolume
// @ 0x5280b0, called by Server_ProcessRoundEnd @ 0x516958; Sound_RestoreEffectsVolume
// @ 0x5280d0, called by Cine_ReleaseAll @ 0x577940]. The world says when it
// stands (world::EpilogCine::world_sound_parked); this keeps the save. The
// option drives the device's effects outputs (the port's SFX and Ambient buses):
// the device hands their levels in to save, writes the parked level while the
// park stands, and writes the saved levels back on the restore.

#include <runtime/audio/volume_law.h>

#include <array>

namespace opennova::audio {

class EffectsVolumePark {
public:
	// The outputs the sound-effects option drives on the device.
	enum Output { kEffectsOutput = 0, kAmbientOutput = 1, kOutputCount = 2 };
	using Levels = std::array<double, kOutputCount>;

	// The zeroed option's level, which each output holds while the park stands.
	static double parked_db() { return volume_db_from_byte(0); }

	bool parked() const { return parked_; }

	// The park: with nothing saved, `levels` (the outputs' levels now) are kept and
	// true says the device writes parked_db(); a park already standing saves
	// nothing over them and answers false.
	bool park(const Levels &levels) {
		if (parked_) return false;
		saved_ = levels;
		parked_ = true;
		return true;
	}

	// The restore: a standing park hands back the saved levels in `out` and
	// clears; with none standing, false and nothing to write.
	bool restore(Levels &out) {
		if (!parked_) return false;
		out = saved_;
		parked_ = false;
		return true;
	}

private:
	bool parked_ = false;
	Levels saved_{};
};

} // namespace opennova::audio
