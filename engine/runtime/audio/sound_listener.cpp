// The sound layer's listener, pans and doppler -- see sound_listener.h.
#include <runtime/audio/sound_listener.h>

#include <runtime/audio/mixer.h>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>

#include <cmath>

namespace opennova::audio {

namespace {

// `cdq; xor; sub`: a wrapping absolute value, read signed.
int32_t abs32(int32_t v) { return v < 0 ? static_cast<int32_t>(0u - uint32_t(v)) : v; }

// The doppler's constants [orig: dbl_7D0968 (1 / 65535), dbl_7C3618 (0.5), dbl_7D0960 (1 / 62),
// dbl_7D0958 (65535.0)].
constexpr double kUnitPerQ16 = 1.5259021896696422e-05;
constexpr double kHalf = 0.5;
constexpr double kPerTick = 0.016129032258064516;
constexpr double kQ16Back = 65535.0;

// A listener jump: a step back past this on x zeroes the velocity [orig: `cmp eax, 19CE7h`
// @0x527A30].
constexpr int32_t kListenerJump = 105703;

} // namespace

int32_t sound_bearing(int32_t dx, int32_t dy) {
	return io::retail_ftol_sse2(std::atan2(double(dy), double(dx)) * -io::kBamPerRadian);
}

int32_t listener_yaw(double fx, double fy) {
	return io::retail_ftol_sse2(std::atan2(fy, fx) * io::kBamPerRadian);
}

uint8_t positional_pan(int32_t dx, int32_t dy, int32_t yaw, bool centered) {
	if ((abs32(dx) <= 0x8000 && abs32(dy) <= 0x8000) || centered) return 0;
	const uint32_t turn = 0x800000u - uint32_t(sound_bearing(dx, dy)) - uint32_t(yaw);
	return static_cast<uint8_t>(turn >> 24);
}

uint8_t voice_pan(int32_t dx, int32_t dy, int32_t yaw) {
	if (int32_t(uint32_t(abs32(dx)) + uint32_t(abs32(dy))) < 0x4000) return 0;
	const uint32_t turn = 0u - uint32_t(yaw) - uint32_t(sound_bearing(dx, dy));
	return static_cast<uint8_t>(turn >> 24);
}

uint32_t doppler_play_word(uint32_t play_q16, const int32_t listener[3],
		const int32_t listener_velocity[3], const int32_t source[3], const int32_t source_velocity[3],
		uint8_t divisor, int32_t sound_speed_q16) {
	// [orig: Sound_Calculate3DAttenuation @0x527F61..0x527F8E: no velocity, no change]
	if (listener_velocity[0] == 0 && listener_velocity[1] == 0 && listener_velocity[2] == 0 &&
			source_velocity[0] == 0 && source_velocity[1] == 0 && source_velocity[2] == 0)
		return play_q16;
	const double k = kUnitPerQ16;
	// The distance now [orig: @0x527F94..0x527FC8].
	const double dx = (double(listener[0]) - double(source[0])) * k;
	const double dy = (double(listener[1]) - double(source[1])) * k;
	const double dz = (double(listener[2]) - double(source[2])) * k;
	const double now = std::sqrt((dz * dz + dy * dy) + dx * dx);
	// One velocity step on [orig: @0x527FCA..0x528004].
	const double x = dx + (double(listener_velocity[0]) - double(source_velocity[0])) * k;
	const double y = dy + (double(listener_velocity[1]) - double(source_velocity[1])) * k;
	const double z = dz + (double(listener_velocity[2]) - double(source_velocity[2])) * k;
	const double stepped = std::sqrt(x * x + (y * y + z * z));
	// `fsubp st(2), st` (DE EA): the distance now less the stepped one [orig: @0x528006].
	double rate = now - stepped;
	// [orig: @0x52800E..0x528023: 2 halves it, 3 and up divide by the byte]
	if (divisor == 2)
		rate *= kHalf;
	else if (divisor > 2)
		rate /= double(divisor);
	// A rate of 0 changes nothing [orig: `fldz; fucomp st(3); test ah, 44h; jnp` @0x528025..
	// 0x52802E].
	if (rate == 0.0) return play_q16;
	const double speed = (double(sound_speed_q16) * kPerTick) * k;
	const double half = kHalf * speed;
	// [orig: @0x528042..0x52805B: at half the speed or more the factor doubles]
	if (!(rate < half)) return uint32_t(play_q16) * 2u;
	// [orig: @0x528061..0x528081: at -half or less it halves, toward 0]
	if (!(std::fabs(rate) < half)) return static_cast<uint32_t>(int32_t(play_q16) / 2);
	// [orig: @0x528085..0x52809C]
	const double ratio = (rate + speed) / speed;
	return static_cast<uint32_t>(
			io::retail_ftol_sse2(((k * double(int32_t(play_q16))) * ratio) * kQ16Back));
}

uint8_t SoundListener::pan_of(int32_t x, int32_t y, bool centered) const {
	// The offset wraps as retail's 32-bit subtraction does [orig: Sound_Play3DPositional
	// @0x527DA7..0x527DB7].
	return positional_pan(int32_t(uint32_t(x) - uint32_t(position_[0])),
			int32_t(uint32_t(y) - uint32_t(position_[1])), yaw_, centered);
}

uint8_t SoundListener::voice_pan_of(int32_t x, int32_t y) const {
	return voice_pan(int32_t(uint32_t(x) - uint32_t(position_[0])), int32_t(uint32_t(y) - uint32_t(position_[1])),
			yaw_);
}

void SoundListener::update(const int32_t position[3], int32_t yaw, uint32_t ticks) {
	if (ticks == 0) return;
	int32_t back[3];
	for (int i = 0; i < 3; ++i) back[i] = int32_t(uint32_t(position_[i]) - uint32_t(position[i]));
	for (int i = 0; i < 3; ++i) position_[i] = position[i];
	yaw_ = yaw;
	for (int i = 0; i < 3; ++i) velocity_[i] = ticks == 1 ? back[i] : back[i] / int32_t(ticks);
	if (abs32(velocity_[0]) > kListenerJump) {
		for (int32_t &v : velocity_) v = 0;
	}
}

void SoundListener::place(const int32_t position[3], int32_t yaw) {
	for (int i = 0; i < 3; ++i) {
		position_[i] = position[i];
		velocity_[i] = 0;
	}
	yaw_ = yaw;
}

void repan_entity_channels(Mixer &mixer, const SoundListener &listener, const SoundSourceAt &at) {
	if (!at) return;
	for (size_t ch = 0; ch < kAudioChannelCount; ++ch) {
		const ChannelParams &params = mixer.channel(int(ch));
		if (params.id == 0) continue;
		int32_t place[3] = { 0, 0, 0 };
		if (!at(params.id, place)) continue;
		const int32_t dx = int32_t(uint32_t(place[0]) - uint32_t(listener.position()[0]));
		const int32_t dy = int32_t(uint32_t(place[1]) - uint32_t(listener.position()[1]));
		if (abs32(dx) <= 0x8000 && abs32(dy) <= 0x8000) continue;
		const uint8_t pan = positional_pan(dx, dy, listener.yaw(), false);
		mixer.set_and_play(mixer.handle_of(int(ch)), pan, params.volume, params.play_q16);
	}
}

} // namespace opennova::audio
