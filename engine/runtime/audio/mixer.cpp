// The software mixer -- see mixer.h.
#include <runtime/audio/mixer.h>

#include <formats/lwf/wav_pcm.h>

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace opennova::audio {

namespace {

// The MMX word arithmetic the mixer is written in: paddsw / psubsw saturate, pmulhw keeps the high
// word of the signed product, psraw shifts arithmetically and fills a word with its sign past 15.
int16_t sat16(int32_t v) {
	return static_cast<int16_t>(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}
int16_t adds(int16_t a, int16_t b) { return sat16(int32_t(a) + int32_t(b)); }
int16_t subs(int16_t a, int16_t b) { return sat16(int32_t(a) - int32_t(b)); }
int16_t mulhi(int16_t a, int16_t b) { return static_cast<int16_t>((int32_t(a) * int32_t(b)) >> 16); }
int16_t sra(int16_t a, uint32_t n) {
	return n > 15 ? static_cast<int16_t>(a < 0 ? -1 : 0) : static_cast<int16_t>(a >> n);
}
// A byte unpacked to a word and halved, 0..0x7FFF (`punpcklbw x, x; psrlw x, 1`).
int16_t byte_word(uint8_t b) { return static_cast<int16_t>((uint32_t(b) * 257u) >> 1); }
// A volume byte's level: its word squared and doubled (`pmulhw x, x; paddsw x, x`).
int16_t level_word(uint8_t b) {
	const int16_t v = byte_word(b);
	const int16_t sq = mulhi(v, v);
	return adds(sq, sq);
}

// The device factor, (44100 << 16) / device [orig: AudioMixer_Init @0x7BD381..0x7BD393]:
// 0x10000 on the 44100 Hz device.
constexpr uint32_t kDeviceFactor = 0xAC440000u / kMixDeviceRate;

// The stereo mode's constants [orig: AudioMixer_InitChannelBlendTables @0x766020..0x7660F5, the
// speaker configuration 2 (and 3)]: the ambience blend's dry and delayed targets,
// g_AudioAmbienceDryTarget (0x2B00 left and right, 0x7800 on word 3) and
// g_AudioAmbienceDelayedTarget (0xD500 left, 0x2B00 right), and the music channels' fixed dry
// quadwords, g_AudioMusicLeftDry (channel 1: 0x7FFF left) and g_AudioMusicRightDry (channel 2:
// 0x7FFF right), each 0x2800 on word 3, their delayed quadwords 0.
constexpr std::array<int16_t, 4> kAmbienceDry = { 0x2B00, 0x2B00, 0, 0x7800 };
constexpr std::array<int16_t, 4> kAmbienceDelayed = { int16_t(-0x2B00), 0x2B00, 0, 0 };
constexpr std::array<int16_t, 4> kMusicLeftDry = { 0x7FFF, 0, 0, 0x2800 };
constexpr std::array<int16_t, 4> kMusicRightDry = { 0, 0x7FFF, 0, 0x2800 };

// The stereo mode's three speaker positions on the pan turn: left, behind, right [orig:
// @0x76604F..0x76605F, 48 / 128 / 208].
constexpr int kStereoSpeakers[3] = { 48, 128, 208 };
// A speaker's full weight: a third of 0x7FFF, so the three entries the spread adds reach it [orig:
// flt_800A64, 10922.2998046875f, the float load @0x7661A4 / @0x7661C6]; the curve's power [orig:
// dbl_800A68, 0.75 @0x766199 / @0x7661BB, into _CIpow].
constexpr float kSpeakerWeight = 10922.2998046875f;
constexpr double kSpeakerCurve = 0.75;
// Word 3 of every pan entry: the bass every channel sends [orig: `mov word ptr [esi+4], 2800h`
// @0x76620A].
constexpr int16_t kPanSend = 0x2800;

// The 256 pan entries of the stereo mode [orig: AudioMixer_InitChannelBlendTables @0x766107..
// 0x766225]: the speakers either side of the pan (the last at or below it, the next after it,
// wrapping), each weighted by the other's share of their folded distance to the power 0.75; the
// left and right weights are the dry words 0 and 1, and the behind weight, 80/128 of it, goes to
// the delayed words 4 and 5 in opposite phase, positive on the side the dry weights favour.
std::array<std::array<int16_t, 8>, 256> stereo_pan_table() {
	std::array<std::array<int16_t, 8>, 256> table{};
	for (int pan = 0; pan < 256; ++pan) {
		int prev = 2;
		int next = 0;
		for (int i = 0; i < 3; ++i) {
			if (pan >= kStereoSpeakers[i]) {
				prev = i;
				next = i + 1;
			}
		}
		if (next >= 3) next = 0;
		const auto folded = [pan](int speaker) {
			int d = std::abs(pan - speaker);
			return d >= 128 ? 256 - d : d;
		};
		const int d_prev = folded(kStereoSpeakers[prev]);
		const int d_next = folded(kStereoSpeakers[next]);
		const int total = d_prev + d_next;
		int32_t weight[3] = { 0, 0, 0 };
		// `fild total; fidivr d_next` and `fild d_prev; fdiv total`, each through _CIpow and the
		// float weight, truncated by _ftol2_sse [orig: @0x76618D..0x7661D1].
		weight[prev] = static_cast<int32_t>(
				std::pow(static_cast<double>(d_next) / total, kSpeakerCurve) * double(kSpeakerWeight));
		weight[next] = static_cast<int32_t>(
				std::pow(static_cast<double>(d_prev) / total, kSpeakerCurve) * double(kSpeakerWeight));
		// [orig: `lea eax, [eax+eax*4]; shl eax, 4; sar eax, 7` @0x7661E1..0x7661E7, the phase
		// @0x7661EA..0x7661F4]
		int32_t behind = (weight[1] * 80) >> 7;
		int32_t delayed_left = behind;
		int32_t delayed_right = -behind;
		if (!(weight[0] >= weight[2])) {
			delayed_left = -behind;
			delayed_right = behind;
		}
		std::array<int16_t, 8> &entry = table[size_t(pan)];
		entry[0] = static_cast<int16_t>(weight[0]);
		entry[1] = static_cast<int16_t>(weight[2]);
		entry[2] = 0;
		entry[3] = kPanSend;
		entry[4] = static_cast<int16_t>(delayed_left);
		entry[5] = static_cast<int16_t>(delayed_right);
		entry[6] = 0;
		entry[7] = 0;
	}
	return table;
}

uint32_t next_wave_id() {
	// The bits above the sixth a handle carries; 0 is no buffer.
	static std::atomic<uint32_t> counter{ 0 };
	uint32_t id = 0;
	while (id == 0) id = (counter.fetch_add(1) + 1u) & 0x03FFFFFFu;
	return id;
}

uint32_t channel_handle(const ChannelParams &params, int channel) {
	return params.wave ? (params.wave->id << 6) | uint32_t(channel) : 0u;
}

int16_t wave_sample(const MixWave &wave, int32_t tap) {
	// The tap is an index back from the samples' end: the 16-bit read `movsx eax, word [eax*2 +
	// end]`, the 8-bit `movsx eax, byte [eax + end]` [orig: AudioChannel_BindSampleBuffer
	// @0x7BD6B0..0x7BD716, the patched loads @0x7BDDD2..0x7BDDEA]. Past the pad the mixer reads the
	// memory after the buffer; ours reads 0.
	const int64_t index = int64_t(wave.count) + tap;
	if (index < 0 || index >= int64_t(wave.samples.size())) return 0;
	return wave.samples[size_t(index)];
}

} // namespace

MixWaveRef make_mix_wave(uint32_t count, uint32_t pitch_q16, uint8_t width, uint8_t shift,
		std::vector<int16_t> samples) {
	auto wave = std::make_shared<MixWave>();
	wave->id = next_wave_id();
	wave->count = count;
	wave->pitch_q16 = pitch_q16;
	wave->width = width;
	wave->shift = shift;
	wave->samples = std::move(samples);
	return wave;
}

MixWaveRef mix_wave_from_pcm(const lwf::WavPcm &pcm, size_t channel) {
	const size_t step = pcm.channels == 2 ? 2 : 1;
	// The loader's count; a stereo decode records its frames.
	const uint32_t count = step == 2 ? uint32_t(pcm.pcm16.size() / 4) : pcm.loader_samples;
	if (count == 0) return nullptr;
	const size_t lane = channel < step ? channel : 0;
	const size_t present = pcm.pcm16.size() / (2 * step);
	// The pad by the width byte: eight bytes, eight 8-bit samples or four 16-bit ones; at another
	// width the loader writes it elsewhere (D-SND-48) and ours leaves none.
	const size_t pad = pcm.loader_width == 2 ? 4 : pcm.loader_width == 1 ? 8 : 0;
	std::vector<int16_t> samples(size_t(count) + pad, 0);
	for (size_t i = 0; i < present && i < count; ++i) {
		const size_t at = 2 * (step * i + lane);
		samples[i] = static_cast<int16_t>(uint16_t(pcm.pcm16[at]) | uint16_t(pcm.pcm16[at + 1]) << 8);
	}
	if (pad != 0) {
		// The last sample's size against 8 for a byte, 0x800 for a word: an 8-bit sample at the
		// 16-bit scale is under 0x800 exactly where its byte is under 8 [orig: @0x7666AD..0x7666BF,
		// @0x7668AF..0x7668C4]; the first eight sample bytes copied at or above it [orig:
		// @0x7668CC..0x7668DA].
		const int32_t last = samples[count - 1];
		if (std::abs(last) >= 0x800) {
			for (size_t i = 0; i < pad; ++i) samples[count + i] = i < count ? samples[i] : 0;
		}
	}
	return make_mix_wave(count, pcm.loader_pitch_q16, pcm.loader_width, pcm.loader_shift,
			std::move(samples));
}

const MixWaveRef &silent_mix_wave() {
	static const MixWaveRef silent = make_mix_wave(10, 0x400, 1, 8, std::vector<int16_t>(18, 0));
	return silent;
}

ChannelParams voice_params(MixWaveRef wave, uint8_t pan, uint8_t volume, uint32_t play_q16,
		uint32_t id) {
	ChannelParams params;
	params.wave = std::move(wave);
	params.play_q16 = play_q16;
	params.id = id;
	params.volume = volume;
	params.send = volume;
	params.pan = pan;
	params.spread = 0;
	params.ambience = 0; // g_AudioChannelAmbienceBlend, never written
	params.aux = { volume, volume, volume, volume };
	return params;
}

Mixer::Mixer() : pan_table_(stereo_pan_table()) {
	// Every channel opened on the silent buffer at volume 0, play 4096, then each bound from 25
	// down [orig: AudioMixer_Init @0x7BD49A..0x7BD4A4].
	init_all();
	for (int ch = int(kAudioChannelCount) - 1; ch >= 0; --ch) load_next_frame(ch);
}

void Mixer::init_all() {
	for (size_t ch = 0; ch < kAudioChannelCount; ++ch) open_silent(int(ch));
}

uint32_t Mixer::handle_of(int channel) const {
	if (channel < 0 || size_t(channel) >= kAudioChannelCount) return 0;
	return channel_handle(channels_[size_t(channel)].current, channel);
}

uint32_t Mixer::open(int channel, const ChannelParams &params) {
	if (!params.wave || channel < 0 || size_t(channel) >= kAudioChannelCount) return 0;
	Channel &c = channels_[size_t(channel)];
	// [orig: AudioChannel_Open @0x76691C..0x7669E6: the ring emptied, the loop the silent buffer
	// at volume 0 and play 256 (its +0x11..+0x14 left), the descriptor queued, the position -1 so
	// the next frame's step loads it]
	c.read = 0;
	c.write = 0;
	c.idle.wave = silent_mix_wave();
	c.idle.play_q16 = 256;
	c.idle.id = 0;
	c.idle.volume = 0;
	c.idle.send = 0;
	c.idle.pan = 0;
	c.idle.spread = 0;
	c.idle.ambience = 0;
	c.queue[0] = params;
	c.write = 1;
	c.position = -1;
	c.current = params;
	return channel_handle(c.current, channel);
}

uint32_t Mixer::open_looping(int channel, const ChannelParams &params) {
	if (!params.wave || channel < 0 || size_t(channel) >= kAudioChannelCount) return 0;
	Channel &c = channels_[size_t(channel)];
	// [orig: AudioChannel_OpenDirect @0x766AD3..0x766B58]
	c.idle = params;
	c.read = 0;
	c.write = 0;
	c.position = -1;
	c.current = params;
	return channel_handle(c.current, channel);
}

void Mixer::queue(int channel, const ChannelParams &params) {
	if (!params.wave || channel < 0 || size_t(channel) >= kAudioChannelCount) return;
	Channel &c = channels_[size_t(channel)];
	// [orig: AudioChannel_QueueEvent @0x766A2E..0x766A8A]
	c.queue[c.write] = params;
	c.write = (c.write + 1) & (kChannelQueueDepth - 1);
	if (c.current.wave == silent_mix_wave()) c.position = -1;
}

uint32_t Mixer::submit(int channel, const ChannelParams &params) {
	if (channel < 0 || size_t(channel) >= kAudioChannelCount || !params.wave) return 0;
	Channel &c = channels_[size_t(channel)];
	// [orig: AudioChannel_Submit @0x766B88..0x766C18]
	if (c.idle.wave != silent_mix_wave()) c.idle = params;
	c.current = params;
	compute_coefficients(channel);
	return channel_handle(c.current, channel);
}

uint32_t Mixer::validate(uint32_t handle) const {
	const uint32_t ch = handle & 0x3Fu;
	if (ch >= kAudioChannelCount) return 0;
	const ChannelParams &current = channels_[ch].current;
	if (!current.wave || ((handle ^ (current.wave->id << 6)) & ~0x3Fu) != 0) return 0;
	return handle;
}

uint32_t Mixer::set_and_play(uint32_t handle, uint8_t pan, uint8_t volume, uint32_t play_q16) {
	if (validate(handle) == 0) return 0;
	const int ch = int(handle & 0x3Fu);
	const ChannelParams &current = channels_[size_t(ch)].current;
	// [orig: AudioChannel_SetAndPlay @0x766C86..0x766CE9]
	ChannelParams scratch = voice_params(current.wave, pan, volume, play_q16, current.id);
	scratch.spread = current.spread;
	scratch.ambience = current.ambience;
	submit(ch, scratch);
	return handle;
}

void Mixer::open_silent(int channel) {
	open(channel, voice_params(silent_mix_wave(), 0, 0, 4096, 0));
}

uint32_t Mixer::reset(uint32_t handle) {
	if (validate(handle) == 0) return 0;
	open_silent(int(handle & 0x3Fu));
	return handle;
}

void Mixer::flush_music() {
	open_silent(kMusicLeftChannel);
	open_silent(kMusicRightChannel);
}

uint32_t Mixer::find_and_open(const MixWaveRef &wave, uint8_t pan, uint8_t volume,
		uint32_t play_q16, uint32_t id) {
	if (!wave) return 0;
	// [orig: Audio_FindAndOpenChannel @0x766ED9: a volume of 0 opens nothing; @0x766F0A the
	// floor 12 * volume a channel's score must stand under]
	if (volume == 0) return 0;
	int32_t least = 12 * int32_t(volume);
	int best = -1;
	for (size_t ch = kFirstOneshotChannel; ch < kAudioChannelCount; ++ch) {
		const ChannelParams &p = channels_[ch].current;
		// The six volume bytes, +0x0C, +0x0D and +0x11..+0x14 [orig: @0x766F1C..0x766F38]; the
		// channel playing this buffer for this owner scores 0 [orig: @0x766F3A..0x766F48].
		int32_t score = int32_t(p.volume) + p.send + p.aux[0] + p.aux[1] + p.aux[2] + p.aux[3];
		if (id != 0 && p.wave == wave && p.id == id) score = 0;
		if (score < least) {
			least = score;
			best = int(ch);
		}
	}
	if (best < 0) return 0;
	return open(best, voice_params(wave, pan, volume, play_q16, id));
}

uint32_t Mixer::pending(int channel) const {
	if (channel < 0 || size_t(channel) >= kAudioChannelCount) return 0;
	const Channel &c = channels_[size_t(channel)];
	return (c.write - c.read) & (kChannelQueueDepth - 1);
}

void Mixer::compute_coefficients(int channel) {
	Channel &c = channels_[size_t(channel)];
	const ChannelParams &p = c.current;
	const MixWave &wave = p.wave ? *p.wave : *silent_mix_wave();
	// The pan entry and the two the spread reaches either side, added [orig:
	// AudioChannel_ComputeMixCoefficients @0x7BD4BE..0x7BD501].
	const std::array<int16_t, 8> &at = pan_table_[p.pan];
	const std::array<int16_t, 8> &up = pan_table_[uint8_t(p.pan + p.spread)];
	const std::array<int16_t, 8> &down = pan_table_[uint8_t(p.pan - p.spread)];
	Lanes dry;
	Lanes delayed;
	for (size_t l = 0; l < 4; ++l) {
		dry.w[l] = adds(adds(at[l], up[l]), down[l]);
		delayed.w[l] = adds(adds(at[4 + l], up[4 + l]), down[4 + l]);
	}
	// The ambience blend toward the mode's pair, doubled [orig: @0x7BD508..0x7BD550].
	const int16_t blend = byte_word(p.ambience);
	const int16_t keep = byte_word(uint8_t(~p.ambience));
	for (size_t l = 0; l < 4; ++l) {
		dry.w[l] = adds(mulhi(dry.w[l], keep), mulhi(blend, kAmbienceDry[l]));
		delayed.w[l] = adds(mulhi(delayed.w[l], keep), mulhi(blend, kAmbienceDelayed[l]));
		dry.w[l] = adds(dry.w[l], dry.w[l]);
		delayed.w[l] = adds(delayed.w[l], delayed.w[l]);
	}
	// The music's channels take fixed quadwords [orig: @0x7BD553..0x7BD574].
	if (channel == kMusicLeftChannel || channel == kMusicRightChannel) {
		dry.w = channel == kMusicLeftChannel ? kMusicLeftDry : kMusicRightDry;
		delayed.w = { 0, 0, 0, 0 };
	}
	// The volume: the dry lanes at the volume byte, word 3 at the send byte; the delayed at the
	// volume byte; each level the byte's word squared and doubled [orig: @0x7BD57B..0x7BD5C2].
	const int16_t volume = level_word(p.volume);
	const int16_t send = level_word(p.send);
	for (size_t l = 0; l < 4; ++l) {
		const int16_t d = mulhi(dry.w[l], l == 3 ? send : volume);
		dry.w[l] = adds(d, d);
		const int16_t e = mulhi(delayed.w[l], volume);
		delayed.w[l] = adds(e, e);
	}
	// The buffer's shift byte, psraw [orig: @0x7BD5E6..0x7BD5F3].
	for (size_t l = 0; l < 4; ++l) {
		dry.w[l] = sra(dry.w[l], wave.shift);
		delayed.w[l] = sra(delayed.w[l], wave.shift);
	}
	c.dry = dry;
	c.delayed = delayed;
	// The step in 1/512 samples, the play factor through the device factor and the buffer's pitch,
	// the low 32 bits kept, 0 forced to 1 [orig: @0x7BD5F6..0x7BD622].
	const uint32_t played = static_cast<uint32_t>((uint64_t(p.play_q16) * kDeviceFactor) >> 16);
	uint32_t step = static_cast<uint32_t>((uint64_t(played) * wave.pitch_q16 + 0x400000u) >> 23);
	if (step == 0) step = 1;
	c.step = step;
	// The taps' reach h = max(step >> 1, 0x100), then h / 2 and h + h / 2 [orig: @0x7BD624..
	// 0x7BD653].
	uint32_t h = step >> 1;
	if (h < 0x100) h = 0x100;
	c.taps = { h, h >> 1, (h >> 1) + h };
}

void Mixer::load_next_frame(int channel) {
	Channel &c = channels_[size_t(channel)];
	// The next queued buffer, else the loop [orig: AudioChannel_LoadNextFrame @0x7BF0FC..0x7BF175].
	if (c.read != c.write) {
		c.current = c.queue[c.read];
		c.read = (c.read + 1) & (kChannelQueueDepth - 1);
	} else {
		c.current = c.idle;
	}
	// Bound: the coefficients, and the position a step before the first sample [orig:
	// AudioChannel_BindSampleBuffer @0x7BD67A..0x7BD68B].
	compute_coefficients(channel);
	const uint32_t count = c.current.wave ? c.current.wave->count : 0;
	c.position = static_cast<int32_t>((0u - count) * 512u - c.step);
}

int16_t Mixer::mix_sample(int channel) {
	Channel &c = channels_[size_t(channel)];
	// The step, and the next buffer when it carries past the samples' end [orig: `add eax, step;
	// jb` @0x7BDD9E..0x7BDDA3 -> @0x7BEF94, AudioChannel_LoadNextFrame and back to the add].
	// Retail loops for good on a buffer of no samples that loads itself; ours gives up the frame.
	for (int loads = 0;; ++loads) {
		const uint64_t next = uint64_t(uint32_t(c.position)) + c.step;
		if ((next >> 32) == 0) {
			c.position = static_cast<int32_t>(uint32_t(next));
			break;
		}
		if (loads > int(kChannelQueueDepth)) return 0;
		load_next_frame(channel);
	}
	const MixWave &wave = c.current.wave ? *c.current.wave : *silent_mix_wave();
	const uint32_t pos = uint32_t(c.position);
	// The four taps, the position and its three offsets, each >> 9, summed and averaged [orig:
	// @0x7BDDA9..0x7BDDFF].
	const int32_t sum = int32_t(wave_sample(wave, int32_t(pos) >> 9)) +
			wave_sample(wave, int32_t(pos + c.taps[0]) >> 9) +
			wave_sample(wave, int32_t(pos + c.taps[1]) >> 9) +
			wave_sample(wave, int32_t(pos + c.taps[2]) >> 9);
	return static_cast<int16_t>(sum >> 2);
}

void Mixer::render(int16_t *interleaved, uint32_t frames) {
	// A channel on the silent buffer with nothing queued sits out the call [orig: the prologue's
	// patches @0x7BD82C..0x7BDD0C].
	std::array<bool, kAudioChannelCount> skip{};
	for (size_t ch = 0; ch < kAudioChannelCount; ++ch) {
		const Channel &c = channels_[ch];
		skip[ch] = c.current.wave == silent_mix_wave() && c.read == c.write;
	}
	for (uint32_t f = 0; f < frames; ++f) {
		Lanes bus0;
		Lanes bus1;
		for (size_t ch = 0; ch < kAudioChannelCount; ++ch) {
			if (skip[ch]) {
				channels_[ch].sample = 0;
				continue;
			}
			const int16_t sample = mix_sample(int(ch));
			channels_[ch].sample = sample;
			// Channel zero's follower [orig: @0x7BDE22..0x7BDE2C].
			if (ch == 0 && int32_t(sample) > follower_) follower_ += 16;
			const Channel &c = channels_[ch];
			for (size_t l = 0; l < 4; ++l) {
				bus0.w[l] = adds(bus0.w[l], mulhi(c.dry.w[l], sample));
				bus1.w[l] = adds(bus1.w[l], mulhi(c.delayed.w[l], sample));
			}
		}
		// The buses' 256-frame box: the sum of the last 256 inputs (dword lanes), its average
		// rounded, and the input 128 frames late less it [orig: @0x7BEC79..0x7BEE2C; the ring mask
		// dword_798880 0xFF, the late index dword_798884 0x80, the rounding dword_798878 0x80].
		const uint32_t at = frame_ & 0xFFu;
		const uint32_t late = (frame_ + 0x80u) & 0xFFu;
		Lanes avg0;
		Lanes high0;
		Lanes high1;
		const auto stage = [&](const Lanes &bus, std::array<std::array<int32_t, 4>, 256> &ring,
				std::array<int32_t, 4> &sum, Lanes &avg, Lanes &high) {
			for (size_t l = 0; l < 4; ++l) {
				const int32_t x = bus.w[l];
				sum[l] = int32_t(uint32_t(sum[l]) - uint32_t(ring[at][l]) + uint32_t(x));
				ring[at][l] = x;
				avg.w[l] = sat16(int32_t(uint32_t(sum[l]) + 0x80u) >> 8);
				high.w[l] = subs(sat16(ring[late][l]), avg.w[l]);
			}
		};
		stage(bus0, ring0_, sum0_, avg0, high0);
		Lanes avg1;
		stage(bus1, ring1_, sum1_, avg1, high1);
		// The block stage every 0x400th frame [orig: @0x7BEF16..0x7BEF55].
		if ((frame_ & 0x3FFu) == 0) {
			meter_.stage(follower_);
			follower_ = 0;
		}
		// The stereo output stage [orig: AudioMixer_OutputStereo @0x7BD71A, the stereo mode's
		// g_AudioMixerOutputStage @0x7BD3A2]: bus 1's high band 256 frames back, two frames each
		// halved, the newest in.
		const uint32_t back = frame_ & 0xFFu;
		const uint32_t after = (frame_ + 1u) & 0xFFu;
		const Lanes oldest = history_[back];
		const Lanes next = history_[after];
		history_[back] = high1;
		for (size_t l = 0; l < 2; ++l) {
			int16_t out = adds(high0.w[l], sra(oldest.w[l], 1));
			out = adds(out, sra(next.w[l], 1));
			out = adds(out, avg0.w[3]);
			interleaved[size_t(f) * 2 + l] = out;
		}
		++frame_;
	}
}

platform::AudioSpec mix_audio_spec() {
	return { kMixDeviceRate, static_cast<uint8_t>(kMixOutputChannels), platform::SampleFormat::S16 };
}

uint32_t feed_audio_device(Mixer &mixer, platform::AudioDevice &device) {
	if (!device.is_open()) return 0;
	const uint32_t queued = device.queued_frames();
	if (queued >= kMixFillFrames) return 0;
	const uint32_t frames = kMixFillFrames - queued;
	std::vector<int16_t> pcm(size_t(frames) * kMixOutputChannels);
	mixer.render(pcm.data(), frames);
	return device.push(pcm.data(), frames) ? frames : 0;
}

} // namespace opennova::audio
