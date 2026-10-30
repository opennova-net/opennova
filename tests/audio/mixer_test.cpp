// The software mixer (runtime/audio/mixer.h) and the sound layer's pans and doppler
// (runtime/audio/sound_listener.h), each against values worked by hand from the witnessed
// arithmetic: the stereo blend table [orig: AudioMixer_InitChannelBlendTables @0x766107..0x766225],
// the coefficients [orig: AudioChannel_ComputeMixCoefficients @0x7BD4B0], the step and the four
// taps [orig: @0x7BD5F6..0x7BD653], the coefficient shift [orig: @0x7BD5E6..0x7BD5F3], the channel
// calls and the allocator [orig: AudioChannel_Open @0x7668F0; Audio_FindAndOpenChannel @0x766E80],
// the buses' tail and the stereo stage [orig: the mix loop @0x7BEC79..0x7BEE2C;
// AudioMixer_OutputStereo @0x7BD71A], the channel-zero meter [orig: @0x7BDE22..0x7BDE2C,
// @0x7BEF16..0x7BEF55], the bearing pans [orig: Sound_Play3DPositional @0x527DA7..0x527DF8;
// Audio_UpdateAmbientStream @0x4EDA90..0x4EDAD8] and the doppler [orig:
// Sound_ApplyDopplerShift @0x527F60]; and a mixed-buffer golden of the minted
// fixtures/lwf/tone.wav (tests/fixtures/minimal_lwf_gen.cpp).
#include <runtime/audio/mixer.h>
#include <runtime/audio/music_stream.h>
#include <runtime/audio/script_voice_player.h>
#include <runtime/audio/sound_listener.h>

#include <base/io/sha256.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/sbf/sbf.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::audio;
using test_expect::expect;

namespace {

// A looping buffer of one value: its last sample past 0x800 in size, so the loader's pad copies
// its first four (16-bit), and every tap reads the value.
MixWaveRef constant_wave(int16_t value, uint32_t count = 64, uint32_t pitch = 0x8000) {
	return make_mix_wave(count, pitch, 2, 0, std::vector<int16_t>(count + 4, value));
}

void blend_table() {
	const Mixer mixer;
	using Entry = std::array<int16_t, 8>;
	// Ahead (0): left (48) and right (208) at (48 / 96)^0.75 of a third of 0x7FFF each.
	expect(mixer.pan_entry(0) == Entry{ 6494, 6494, 0, 0x2800, 0, 0, 0, 0 }, "pan 0: both sides");
	// Hard left: the left speaker's whole weight.
	expect(mixer.pan_entry(48) == Entry{ 10922, 0, 0, 0x2800, 0, 0, 0, 0 }, "pan 48: hard left");
	// Between left and behind: the behind weight's 80/128 on the delayed lanes, positive on the
	// left (the side the dry weights favour).
	expect(mixer.pan_entry(88) == Entry{ 6494, 0, 0, 0x2800, 4058, -4058, 0, 0 }, "pan 88");
	// Behind: no dry weight, the delayed pair in opposite phase.
	expect(mixer.pan_entry(128) == Entry{ 0, 0, 0, 0x2800, 6826, -6826, 0, 0 }, "pan 128: behind");
	expect(mixer.pan_entry(208) == Entry{ 0, 10922, 0, 0x2800, 0, 0, 0, 0 }, "pan 208: hard right");
}

void coefficients_and_step() {
	Mixer mixer;
	const MixWaveRef wave = constant_wave(16384);
	mixer.open_looping(12, voice_params(wave, 48, 255, 0x10000));
	int16_t out[2];
	mixer.render(out, 1);
	// Three entries (no spread) summed: 32766 and 0x7800; the ambience blend's keep 0x7FFF halves
	// and the paddsw doubles; the volume byte's level 32766: 32762 dry, 30716 on the send.
	using Lanes = std::array<int16_t, 4>;
	expect(mixer.dry_lanes(12).w == Lanes{ 32762, 0, 0, 30716 }, "the full-volume hard-left dry lanes");
	expect(mixer.delayed_lanes(12).w == Lanes{ 0, 0, 0, 0 }, "no delayed lanes at hard left");
	// 22050 Hz (pitch 0x8000) at play 0x10000: step 256 (half a sample a frame).
	expect(mixer.step(12) == 256, "a 22050 Hz wave steps 256");
	// Half volume: the byte's level is its word squared, 8256 at 128.
	mixer.set_and_play(mixer.handle_of(12), 48, 128, 0x10000);
	expect(mixer.dry_lanes(12).w == Lanes{ 8254, 0, 0, 7738 }, "volume 128 is a squared level");
	// Behind: the delayed pair, the negative lane rounding a unit further.
	mixer.set_and_play(mixer.handle_of(12), 128, 255, 0x10000);
	expect(mixer.delayed_lanes(12).w == Lanes{ 20474, -20478, 0, 0 }, "pan 128's delayed lanes");
	// A pitch of 0 steps the least step, 1 [orig: @0x7BD619..0x7BD61D].
	mixer.open_looping(13, voice_params(constant_wave(100, 64, 0), 0, 255, 0x10000));
	mixer.render(out, 1);
	expect(mixer.step(13) == 1, "a step of 0 is forced to 1");
	// The music's channels take fixed lanes whatever the pan [orig: @0x7BD553..0x7BD574].
	mixer.queue(kMusicLeftChannel, voice_params(wave, 0x40, 255, 0x10000));
	mixer.render(out, 1);
	expect(mixer.dry_lanes(kMusicLeftChannel).w[0] > 30000 && mixer.dry_lanes(kMusicLeftChannel).w[1] == 0,
			"music channel 1 plays left alone");
}

void coefficient_shift() {
	Mixer mixer;
	const auto shifted = [](uint8_t shift) {
		return make_mix_wave(64, 0x8000, 2, shift, std::vector<int16_t>(68, 16384));
	};
	mixer.open_looping(12, voice_params(shifted(1), 128, 255, 0x10000));
	mixer.open_looping(13, voice_params(shifted(15), 128, 255, 0x10000));
	int16_t out[2];
	mixer.render(out, 1);
	// psraw halves each lane at 1; at 15 the positive lanes go to 0 and the negative to -1, a
	// delayed tap of -1 a sample (D-SND-51's facet, now retail's own).
	using Lanes = std::array<int16_t, 4>;
	expect(mixer.delayed_lanes(12).w == Lanes{ 10237, -10239, 0, 0 }, "shift 1 halves the lanes, floored");
	expect(mixer.delayed_lanes(13).w == Lanes{ 0, -1, 0, 0 }, "shift 15 leaves the negative lanes at -1");
}

void box_resampler() {
	// A ramp of 0, 256, 512, ...: the four taps at the position and h/2, h and 3h/2 on, h =
	// max(step >> 1, 0x100), averaged.
	const auto ramp = [](uint32_t pitch) {
		std::vector<int16_t> samples(68, 0);
		for (int i = 0; i < 64; ++i) samples[size_t(i)] = int16_t(i * 256);
		return make_mix_wave(64, pitch, 2, 0, samples);
	};
	const auto first_two = [&](uint32_t pitch, int16_t &a, int16_t &b) {
		Mixer mixer;
		mixer.open(12, voice_params(ramp(pitch), 48, 255, 0x10000));
		int16_t out[2];
		mixer.render(out, 1);
		a = mixer.last_sample(12);
		mixer.render(out, 1);
		b = mixer.last_sample(12);
		return mixer.step(12);
	};
	int16_t a = 0, b = 0;
	// A sample a frame (44100 Hz, step 512, h 256): every tap inside the one sample.
	expect(first_two(0x10000, a, b) == 512 && a == 0 && b == 256, "step 512: the sample itself");
	// Two samples a frame (step 1024, h 512): the sample and the next, twice each.
	expect(first_two(0x20000, a, b) == 1024 && a == 128 && b == 640, "step 1024: the box over two samples");
	// Half a sample a frame (22050 Hz, step 256, h held at 256): the box across the half step.
	expect(first_two(0x8000, a, b) == 256 && a == 0 && b == 128, "step 256: the box across half a sample");
	// 8-bit samples, the four bytes' sum shifted left 6: at the 16-bit scale the same average.
	Mixer bytes;
	bytes.open(12, voice_params(make_mix_wave(4, 0x10000, 1, 0, std::vector<int16_t>(12, int16_t(-128 * 256))),
			48, 255, 0x10000));
	int16_t out[2];
	bytes.render(out, 1);
	expect(bytes.last_sample(12) == -32768, "an 8-bit -128 reads -32768");
}

void channel_calls_and_allocator() {
	Mixer mixer;
	std::vector<MixWaveRef> waves;
	for (int i = 0; i < 16; ++i) waves.push_back(constant_wave(1000));
	// 12..25 in order while each idle channel scores 0.
	for (int ch = 12; ch < 26; ++ch) {
		const uint32_t handle = mixer.find_and_open(waves[size_t(ch - 12)], 0, 200, 0x10000, 0);
		expect((handle & 0x3F) == uint32_t(ch), "the allocator fills 12..25 in order");
	}
	// Each channel's six volume bytes sum 1200: exactly half a 100's floor of 1200 steals none,
	// 101's floor of 1212 takes the first.
	expect(mixer.find_and_open(waves[14], 0, 100, 0x10000, 0) == 0, "exactly half cannot steal");
	expect((mixer.find_and_open(waves[14], 0, 101, 0x10000, 0) & 0x3F) == 12, "louder than half can");
	expect(mixer.find_and_open(waves[14], 0, 0, 0x10000, 0) == 0, "volume 0 opens nothing");
	// A reset channel scores 0: the quietest open takes it.
	mixer.reset(mixer.handle_of(20));
	expect(mixer.validate(mixer.handle_of(20)) != 0 && mixer.channel(20).volume == 0, "the reset channel is silent");
	expect((mixer.find_and_open(waves[15], 0, 1, 0x10000, 0) & 0x3F) == 20, "an idle channel outranks a live one");

	// The own-channel key: the channel playing the wave for the id scores 0.
	Mixer keyed;
	const MixWaveRef wave = constant_wave(500);
	expect((keyed.find_and_open(wave, 0, 250, 0x10000, 9) & 0x3F) == 12, "the first open takes 12");
	expect((keyed.find_and_open(wave, 0, 1, 0x10000, 9) & 0x3F) == 12, "the same wave and id retake it");
	expect((keyed.find_and_open(wave, 0, 1, 0x10000, 10) & 0x3F) == 13, "another id opens anew");

	// A one-shot plays its buffer then the silent one: the handle stops validating.
	Mixer once;
	const uint32_t handle = once.open(12, voice_params(constant_wave(1000, 4, 0x10000), 0, 255, 0x10000));
	int16_t out[2 * 16];
	once.render(out, 2);
	expect(once.validate(handle) == handle, "a one-shot validates while it plays");
	once.render(out, 16);
	expect(once.validate(handle) == 0, "a one-shot ends into the silent buffer");
	// A looping channel replays its buffer.
	const uint32_t loop = once.open_looping(13, voice_params(constant_wave(1000, 4, 0x10000), 0, 255, 0x10000));
	once.render(out, 16);
	expect(once.validate(loop) == loop, "a looping channel keeps its buffer");
	// The queue: two buffers, one after the other.
	once.queue(1, voice_params(constant_wave(1000, 8, 0x10000), 0, 255, 0x10000));
	once.queue(1, voice_params(constant_wave(1000, 8, 0x10000), 0, 255, 0x10000));
	expect(once.pending(1) == 2, "two queued");
	once.render(out, 1);
	expect(once.pending(1) == 1, "the first plays");
	once.render(out, 8);
	expect(once.pending(1) == 0, "the second plays after it");
}

void tail_and_stereo_stage() {
	// A hard-left constant 16384: bus 0 takes [8190, 0, 0, 7679]. The first frame's high band is
	// the input 128 frames back (none) less the 256-frame average ((x + 128) >> 8), and the stereo
	// stage adds bus 0's word-3 average to both sides.
	Mixer mixer;
	mixer.open_looping(12, voice_params(constant_wave(16384), 48, 255, 0x10000));
	std::vector<int16_t> out(2 * 600);
	mixer.render(out.data(), 1);
	// Frame 0 binds the buffer and mixes its first sample.
	const int32_t avg0 = (8190 + 128) >> 8;
	const int32_t avg3 = (7679 + 128) >> 8;
	expect(out[0] == -avg0 + avg3 && out[1] == avg3, "frame 0: the high band's dip and the bass on both sides");
	mixer.render(out.data(), 600);
	// Steady: the ring holds 256 of the input, so the high band is 0 and both sides play the
	// bass lane alone: DC from a hard-left channel sounds on both sides.
	expect(out[2 * 599] == 7679 && out[2 * 599 + 1] == 7679, "steady DC plays as the bass send on both sides");

	// Behind: the delayed lanes reach the output 256 frames after the high band, each side its
	// phase.
	Mixer behind;
	behind.open_looping(12, voice_params(constant_wave(16384), 128, 255, 0x10000));
	std::vector<int16_t> back(2 * 400);
	behind.render(back.data(), 400);
	// The two history taps are 256 and 255 frames back, each halved: frame 254 carries none,
	// frame 255 the first's half.
	expect(back[2 * 254] == back[2 * 254 + 1], "no rear term before 255 frames");
	expect(back[2 * 255] != back[2 * 255 + 1], "frame 255 carries frame 0's halved");
	expect(back[2 * 300] != back[2 * 300 + 1], "the rear term arrives out of phase");
}

void meter() {
	// Channel 0 at a constant 16384: the follower steps 16 a frame while the sample stands above
	// it; frame 0 stages a block of 16 (level 0) and restarts it; frame 1024 stages 16 * 1024 =
	// 16384, (16384^2 >> 15) - 0x400 clamped to 0x3FF, halved into stage 1 and again into the level.
	Mixer mixer;
	mixer.open_looping(kVoiceChannel, voice_params(constant_wave(16384), 0, 0, 0x10000));
	std::vector<int16_t> out(2 * 1025);
	mixer.render(out.data(), 1024);
	expect(mixer.meter().stage1() == 0 && mixer.meter().level() == 0, "frame 0's block reads 0");
	mixer.render(out.data(), 1);
	expect(mixer.meter().stage1() == 0x1FF && mixer.meter().level() == 0xFF,
			"frame 1024's block halves into stage 1 and the level");
	// The follower reads the raw sample, whatever the channel's volume (0 here).
}

void pans() {
	// A source 10 units along +y, the listener facing +x (yaw 0): 90 degrees counter-clockwise,
	// left of ahead, the bearing -2^30 and the rounding 0x800000: pan 64, between left and behind.
	expect(sound_bearing(0, 10 << 16) == -0x40000000, "the bearing of +y");
	expect(positional_pan(0, 10 << 16, 0, false) == 64, "a source to the left pans 64");
	// Within half a unit on both ground axes, or ridden: centred.
	expect(positional_pan(0x8000, -0x8000, 0, false) == 0, "within half a unit: 0");
	expect(positional_pan(0, 10 << 16, 0, true) == 0, "a source the listener rides: 0");
	// The listener facing +y: the source ahead pans 0.
	expect(listener_yaw(0.0, 1.0) == 0x40000000, "the yaw of +y");
	expect(positional_pan(0, 10 << 16, listener_yaw(0.0, 1.0), false) == 0, "dead ahead: 0");
	// Behind: 128.
	expect(positional_pan(-(10 << 16), 0, 0, false) == 128, "behind: 128");
	// The voice: no rounding, its near test |dx| + |dy| under a quarter unit.
	expect(voice_pan(0, 1 << 16, 0) == 64, "the voice's left");
	expect(voice_pan(0x1000, 0x1000, 0) == 0, "the voice near the listener: 0");
}

void doppler() {
	const int32_t origin[3] = { 0, 0, 0 };
	const int32_t still[3] = { 0, 0, 0 };
	const int32_t source[3] = { 100 << 16, 0, 0 };
	// No velocity: the factor as it is.
	expect(doppler_play_word(0x10000, origin, still, source, still, 0) == 0x10000, "no velocity, no change");
	// A source 100 units off drawing away a unit a tick (its step back -1 unit): the distance now
	// less the stepped distance is +1 unit, C = 330 / 62 units a tick: 0x10000 * (C + 1) / C.
	const int32_t away[3] = { -(1 << 16), 0, 0 };
	expect(doppler_play_word(0x10000, origin, still, source, away, 0) == 77848, "drawing away plays sharp");
	const int32_t closing[3] = { 1 << 16, 0, 0 };
	expect(doppler_play_word(0x10000, origin, still, source, closing, 0) == 53223, "closing plays flat");
	// Half the speed of sound a tick or more: doubled; -half or less: halved.
	const int32_t fast_away[3] = { -(3 << 16), 0, 0 };
	expect(doppler_play_word(0x10000, origin, still, source, fast_away, 0) == 0x20000, "past half: doubled");
	const int32_t fast_closing[3] = { 3 << 16, 0, 0 };
	expect(doppler_play_word(0x10000, origin, still, source, fast_closing, 0) == 0x8000, "past -half: halved");
	// The divisor: 2 halves the rate.
	expect(doppler_play_word(0x10000, origin, still, source, fast_away, 2) != 0x20000, "the divisor 2 halves the rate");

	// The listener's step back is over the frame's ticks, none leaving it in place.
	SoundListener listener;
	const int32_t start[3] = { 10 << 16, 0, 0 };
	listener.place(start, 0);
	const int32_t moved[3] = { 12 << 16, 0, 0 };
	listener.update(moved, 0, 0);
	expect(listener.position()[0] == (10 << 16), "a frame of no ticks leaves the listener");
	listener.update(moved, 0, 2);
	expect(listener.position()[0] == (12 << 16) && listener.velocity()[0] == -(1 << 16),
			"the step back over two ticks");
	const int32_t jump[3] = { 30 << 16, 0, 0 };
	listener.update(jump, 0, 1);
	expect(listener.velocity()[0] == 0, "a jump zeroes the velocity");
}

void music_stream() {
	// A byte-paired bank of one stream, two chunks of 8 frames at shifts 1 / 2 and 0 / 0, the
	// stereo split's buffers queued on channels 1 and 2 [orig: Audio_SubmitStereoSampleSplit
	// @0x7BD205]: 8-bit, each byte less 0x80, the last frame four times more, the shifts at +0x0D,
	// pitch 0x8000, pans 0x40 / 0xC0, at the music's volume, the global and the stream words'
	// high halves multiplied, (255 * 255) >> 8 = 254 at full.
	sbf::SbfFile bank;
	sbf::SbfStream stream;
	stream.name = "t";
	stream.block_size = sbf::SBF_CHUNK_TOTAL;
	for (int c = 0; c < 2; ++c) {
		sbf::SbfChunk chunk;
		chunk.scale_a = c == 0 ? 1 : 0;
		chunk.scale_b = c == 0 ? 2 : 0;
		for (int i = 0; i < 8; ++i) {
			chunk.audio.push_back(uint8_t(0x80 + 16 * i));
			chunk.audio.push_back(uint8_t(0x80 - 16 * i));
		}
		stream.chunks.push_back(chunk);
	}
	bank.streams.push_back(stream);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!expect(sbf::sbf_write_bank(bank, bytes, error), "the bank writes")) return;
	sbf::SbfArchive archive{};
	if (!expect(sbf::sbf_open_memory(&archive, bytes.data(), bytes.size()) == 0, "the bank opens")) return;
	Mixer mixer;
	MusicStream music(mixer);
	music.open(&archive, bytes.data(), bytes.size(), 255);
	// The open's flush queues the silent buffer on both channels [orig: Audio_FlushChannels
	// @0x7BCF70 -> AudioChannel_OpenSilent], which the next frame loads.
	expect(mixer.pending(kMusicLeftChannel) == 1, "the flush queues the silent buffer");
	std::vector<int16_t> out(2 * 5122);
	mixer.render(out.data(), 1);
	music.start_track(0);
	expect(music.streaming() && !music.queue_full(), "a track selected streams");
	music.stream_block();
	expect(mixer.pending(kMusicLeftChannel) == 1 && mixer.pending(kMusicRightChannel) == 1, "one block, one buffer each");
	expect(music.stream_volume() == 254, "the music's volume at full");
	music.stream_block();
	expect(!music.streaming(), "two blocks stream the track out");
	// A buffer queued on a channel playing the silent one starts at the next frame [orig:
	// AudioChannel_QueueEvent @0x766A7B..0x766A8A, the position -1].
	mixer.render(out.data(), 1);
	const ChannelParams &left = mixer.channel(kMusicLeftChannel);
	const ChannelParams &right = mixer.channel(kMusicRightChannel);
	expect(left.wave && left.wave->count == 8 && left.wave->width == 1 && left.wave->shift == 1 &&
					left.wave->pitch_q16 == 0x8000 && left.pan == 0x40 && left.volume == 254,
			"the left buffer as the split queues it");
	expect(right.wave && right.wave->shift == 2 && right.pan == 0xC0, "the right buffer");
	expect(left.wave && left.wave->samples.size() == 12 && left.wave->samples[1] == 16 * 256 &&
					left.wave->samples[8] == left.wave->samples[7] && left.wave->samples[11] == left.wave->samples[7],
			"each byte less 0x80 at the 16-bit scale, the last frame padded four times");
	// GSDV's target ramps the stream volume over the next track's blocks (2 here: half a step each).
	music.volume_changed(-1, 0);
	music.start_track(0);
	music.stream_block();
	music.stream_block();
	music.start_track(0);
	music.stream_block();
	expect(music.stream_volume() == 0, "the ramp reaches the target over the track");
	music.close();
	sbf::sbf_close(&archive);
}

// The listener's pans of a place: the offset from its own 16.16 place, wrapped as retail wraps it.
void listener_pans() {
	SoundListener listener;
	const int32_t at[3] = { 0x10000 * 10, 0x10000 * 10, 0 };
	listener.place(at, 0);
	// Ten units further along +x: the offset's positional pan.
	expect(listener.pan_of(0x10000 * 20, 0x10000 * 10, false) ==
					positional_pan(0x10000 * 10, 0, 0, false), "pan_of is the offset's positional pan");
	expect(listener.pan_of(0x10000 * 10, 0x10000 * 10, false) == 0, "a source on the listener plays centred");
	expect(listener.voice_pan_of(0x10000 * 10, 0x10000 * 20) == voice_pan(0, 0x10000 * 10, 0),
			"voice_pan_of is the offset's voice pan");
}

// The scripted voice on channel zero: a start opens its clip at the frame's gain and its speaker's
// pan, the scene frame re-places it, a new clip opens anew, and a clip played out lapses.
void script_voice() {
	Mixer mixer;
	SoundListener listener;
	const int32_t at[3] = { 0, 0, 0 };
	listener.place(at, 0);
	auto clip = std::make_shared<lwf::WavPcm>();
	clip->channels = 1;
	clip->loader_samples = 64;
	clip->loader_pitch_q16 = 0x10000;
	clip->pcm16.assign(128, 0x40);
	ScriptVoicePlayer voice;
	ScriptVoicePlayer::Frame frame;
	frame.serial = 1;
	frame.state.clip = clip;
	frame.local = true;
	frame.volume = 207;
	voice.follow(mixer, listener, frame);
	expect(voice.handle() != 0 && (voice.handle() & 0x3F) == kVoiceChannel, "the clip opens on channel zero");
	expect(mixer.channel(kVoiceChannel).volume == 207 && mixer.channel(kVoiceChannel).pan == 0,
			"at the frame's gain, the local voice centred");
	const uint32_t first = voice.handle();
	voice.follow(mixer, listener, frame);
	expect(voice.handle() == first, "the same clip leaves the channel be");
	frame.local = false;
	frame.position.y = 10.0f;
	frame.volume = 100;
	voice.place(mixer, listener, frame);
	expect(mixer.channel(kVoiceChannel).volume == 100 &&
					mixer.channel(kVoiceChannel).pan == voice_pan(0, 0x10000 * 10, 0),
			"the scene frame re-places the channel: its gain and the speaker's bearing");
	expect(!voice.lapsed(mixer), "the clip still plays");
	ScriptVoicePlayer::Frame next = frame;
	next.serial = 2;
	next.state.clip = std::make_shared<lwf::WavPcm>(*clip);
	voice.follow(mixer, listener, next);
	expect(voice.handle() != 0 && voice.handle() != first && mixer.validate(first) == 0,
			"a new clip stops the old voice and opens anew");
	std::vector<int16_t> out(2 * 256);
	mixer.render(out.data(), 256);
	expect(voice.lapsed(mixer), "a clip played out lapses");
	voice.stop(mixer);
	expect(voice.handle() == 0 && !voice.frame().state.clip, "the stop forgets the clip");
}

// The tail and the stereo output stage against retail's own: the oracle that runs the original's
// tail and output stage under emulation with controlled bus words (scripts/oracles/reverb_preset.py:
// bus 0 (12000, -10000, 4000, 2000), bus 1 (3000, 5000, -2000, 1000) on the first frame, zero
// after, the rings zeroed, 1024 frames) produced output bytes of this SHA-256 for every reverb
// preset [orig: @0x7BEC79..0x7BEF93; AudioMixer_OutputStereo @0x7BD71A] (docs/audio/
// lwf-dbf-sound-re.md, "Reverb selection and the mixer preset boundary"; the validation report of
// 2026-09-18).
void tail_against_retail() {
	Mixer mixer;
	std::vector<int16_t> out(2 * 1024);
	const Mixer::Lanes silent;
	Mixer::Lanes bus0;
	Mixer::Lanes bus1;
	bus0.w = { 12000, -10000, 4000, 2000 };
	bus1.w = { 3000, 5000, -2000, 1000 };
	mixer.render_buses(bus0, bus1, out.data());
	for (size_t f = 1; f < 1024; ++f) mixer.render_buses(silent, silent, out.data() + 2 * f);
	const std::string hash = io::sha256_hex(out.data(), out.size() * sizeof(int16_t));
	expect(hash == "c51d1e7766544e5394f9ca3e415440f722392b575934f22bf2dc1c2f3fdcca03",
			"the tail's 1024 frames are retail's, byte for byte");
	if (hash != "c51d1e7766544e5394f9ca3e415440f722392b575934f22bf2dc1c2f3fdcca03")
		std::printf("audio_mixer: tail sha256 %s\n", hash.c_str());
}

// A device that holds what it is pushed and plays nothing until told.
class HeldDevice final : public platform::AudioDevice {
public:
	bool open(const platform::AudioSpec &spec, std::string &) override {
		spec_ = spec;
		open_ = true;
		return true;
	}
	void close() override { open_ = false; }
	bool is_open() const override { return open_; }
	bool push(const void *, uint32_t frame_count) override {
		queued_ += frame_count;
		pushed_ += frame_count;
		return open_;
	}
	uint32_t queued_frames() const override { return queued_; }
	void set_gain(float) override {}
	float take_peak() override { return 0.0f; }
	void play(uint32_t frames) { queued_ -= frames < queued_ ? frames : queued_; }
	platform::AudioSpec spec_;
	uint32_t pushed_ = 0;

private:
	bool open_ = false;
	uint32_t queued_ = 0;
};

// The mixer's stream on a platform device: opened at 44100 Hz stereo S16, kept a fifth of a
// second ahead of the device's play.
void device_feed() {
	Mixer mixer;
	HeldDevice device;
	expect(feed_audio_device(mixer, device) == 0, "a closed device takes nothing");
	std::string error;
	device.open(mix_audio_spec(), error);
	expect(device.spec_.rate == 44100 && device.spec_.channels == 2 &&
					device.spec_.format == platform::SampleFormat::S16 &&
					platform::audio_frame_bytes(device.spec_) == 4,
			"the mixer's spec");
	expect(feed_audio_device(mixer, device) == kMixFillFrames, "the first feed fills the fifth of a second");
	expect(feed_audio_device(mixer, device) == 0, "a full queue takes nothing");
	device.play(1000);
	expect(feed_audio_device(mixer, device) == 1000, "the feed tops the queue back up");
	expect(device.pushed_ == kMixFillFrames + 1000, "the frames pushed");
}

uint64_t fnv1a(const std::vector<int16_t> &samples) {
	uint64_t h = 1469598103934665603ull;
	for (const int16_t s : samples) {
		for (int i = 0; i < 2; ++i) {
			h ^= uint8_t(uint16_t(s) >> (8 * i));
			h *= 1099511628211ull;
		}
	}
	return h;
}

int golden() {
	// The minted triangle wave (22050 Hz, 8320 16-bit samples), decoded as the game's loader does,
	// on a hard-left one-shot at full volume and a half-volume one behind, 4096 frames of the mix.
	const std::string root = test_paths_repo_root(__FILE__);
	const std::vector<uint8_t> bytes = test_io::read_file(root + "/fixtures/lwf/tone.wav");
	lwf::WavPcm pcm;
	std::string error;
	if (!expect(!bytes.empty() && lwf::wav_decode_pcm16(bytes.data(), bytes.size(), pcm, error),
				"tone.wav decodes")) return 1;
	const MixWaveRef tone = mix_wave_from_pcm(pcm);
	if (!expect(tone && tone->count == 8320 && tone->pitch_q16 == 0x8000 && tone->width == 2,
				"tone.wav as the loader leaves it")) return 1;
	Mixer mixer;
	mixer.open(12, voice_params(tone, 48, 255, 0x10000));
	mixer.open(13, voice_params(tone, 128, 128, 0x18000));
	std::vector<int16_t> out(2 * 4096);
	mixer.render(out.data(), 4096);
	const uint64_t hash = fnv1a(out);
	std::printf("audio_mixer: golden fnv1a %016llx\n", static_cast<unsigned long long>(hash));
	expect(hash == 0x47578920010bed73ull, "the mixed tone's golden");
	return 0;
}

} // namespace

int main() {
	blend_table();
	coefficients_and_step();
	coefficient_shift();
	box_resampler();
	channel_calls_and_allocator();
	tail_and_stereo_stage();
	meter();
	pans();
	doppler();
	music_stream();
	listener_pans();
	script_voice();
	device_feed();
	tail_against_retail();
	golden();
	if (test_expect::failures == 0) std::printf("audio_mixer: all passed\n");
	return test_expect::failures == 0 ? 0 : 1;
}
