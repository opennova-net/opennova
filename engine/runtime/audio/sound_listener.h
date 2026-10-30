#pragma once

// The listener as the sound layer holds it, and what the sound calls derive from it for a
// channel: the bearing pan byte the mixer's blend table pans by and the emitter channels' doppler.
// [orig: Audio_UpdateListenerPosition @0x527960, from the view each frame before the frame's
// presentation fires its one-shots (Audio_UpdateListenerFromView @0x43B403..0x43B423); the pans of Sound_Play3DPositional @0x527DCF..0x527DF8, SoundEmitter_UpdateAndMixTop8
// @0x5289A1..0x5289ED and @0x528B00..0x528B91, Audio_Play3DPositionalSound @0x4ECC80 and
// Audio_UpdateAmbientStream @0x4EDAD8; the doppler Sound_Calculate3DAttenuation @0x527F60, a misnomer:
// it moves the play factor]. Positions are the mission frame's 16.16 units (x, y on the ground, z up),
// yaws and bearings 32-bit turns (BAM, counter-clockwise from +x as atan2 reads them).

#include <cstdint>
#include <functional>

namespace opennova::audio {

class Mixer;

// The bearing of an offset as the sound calls read it: atan2(dy, dx) times retail's negative BAM
// per radian, truncated [orig: `fild dy; fild dx; fpatan; fmul g_NegBamPerRadian` then _ftol2_sse,
// g_NegBamPerRadian @0x7C57B8 the negation of dbl_7C19D8 bit for bit; the emitter pan's
// @0x5289C2..0x5289CA in SoundEmitter_UpdateAndMixTop8].
int32_t sound_bearing(int32_t dx, int32_t dy);

// The yaw a listener facing (fx, fy) holds: the bearing a source dead ahead cancels, so the pan of
// a source ahead is 0 [orig: g_SoundListenerYaw @0x24D663C, the view's yaw (positionData[3]
// @0x5279AF), which every pan subtracts].
int32_t listener_yaw(double fx, double fy);

// A positional channel's pan byte: 0 for a source within half a unit of the listener on both
// ground axes or one the listener rides (`centered`: the local player, its parent or ground
// entity), else (0x800000 - bearing - yaw) >> 24 [orig: Sound_Play3DPositional @0x527DA7..
// 0x527DF8; SoundEmitter_UpdateAndMixTop8 @0x5289A1..0x5289ED].
uint8_t positional_pan(int32_t dx, int32_t dy, int32_t yaw, bool centered);

// The scripted voice's pan byte: 0 within |dx| + |dy| of a quarter unit, else (-yaw - bearing)
// >> 24 [orig: Audio_UpdateAmbientStream @0x4EDA90..0x4EDAD8; Audio_Play3DPositionalSound
// @0x4ECCF8..0x4ECD39].
uint8_t voice_pan(int32_t dx, int32_t dy, int32_t yaw);

// The speed of sound the doppler reads, 330 units a second [orig: g_SoundSpeedFixed @0x24D6660,
// 0x14A0000 by DialogSystem_Init @0x5275FC].
inline constexpr int32_t kSoundSpeedQ16 = 0x14A0000;

// An emitter channel's play factor through the doppler [orig: Sound_Calculate3DAttenuation
// @0x527F60, called @0x52896A on each of the loudest eight]: with either velocity nonzero, the
// distance now less the distance one velocity step on (each 16.16 unit over 65535), over the
// divisor where it is 2 or more, is the closing rate R; at R of half the speed of sound a tick or
// more the factor doubles, at -half or less it halves, else it scales by (C + R) / C, C the speed
// a tick. The velocities are the step back to the previous tick's place (the listener's and the
// ground vehicles' lanes', SoundListener and world::SoundEmitterEvent::velocity), so a source
// drawing away plays sharp.
uint32_t doppler_play_word(uint32_t play_q16, const int32_t listener[3],
		const int32_t listener_velocity[3], const int32_t source[3], const int32_t source_velocity[3],
		uint8_t divisor, int32_t sound_speed_q16 = kSoundSpeedQ16);

class SoundListener {
public:
	// A view's frame of `ticks` logic ticks: none leaves the listener where it was; else its
	// place and yaw, and the step back to the last place over the ticks, zeroed where its x
	// passes 105703 (a jump) [orig: Audio_UpdateListenerPosition @0x527960..0x527A3E].
	void update(const int32_t position[3], int32_t yaw, uint32_t ticks);
	// The first place, no velocity (a mission's start, a cut).
	void place(const int32_t position[3], int32_t yaw);

	const int32_t *position() const { return position_; }
	int32_t yaw() const { return yaw_; }
	const int32_t *velocity() const { return velocity_; }

	// The positional pan byte of a source at (x, y) (16.16 ground units) heard from here
	// (positional_pan), `centered` for one the listener rides.
	uint8_t pan_of(int32_t x, int32_t y, bool centered) const;
	// The scripted voice's pan byte of a speaker at (x, y) heard from here (voice_pan).
	uint8_t voice_pan_of(int32_t x, int32_t y) const;

private:
	int32_t position_[3] = { 0, 0, 0 };
	int32_t yaw_ = 0;
	int32_t velocity_[3] = { 0, 0, 0 };
};

// Where the entity a channel's id names stands (its 16.16 place), when it is one the re-pan
// follows: an entity of an item type that is not the local player.
using SoundSourceAt = std::function<bool(uint32_t id, int32_t position[3])>;

// The frame's re-pan of every channel whose id names such an entity and that stands more than half
// a unit off the listener on a ground axis: its pan byte anew, its volume and play factor as they
// are [orig: SoundEmitter_UpdateAndMixTop8 @0x528AF1..0x528BA4 -> AudioChannel_SetAndPlay].
void repan_entity_channels(Mixer &mixer, const SoundListener &listener, const SoundSourceAt &at);

} // namespace opennova::audio
