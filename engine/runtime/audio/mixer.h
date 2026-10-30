#pragma once

// The software mixer (ADR 0077 d4): retail's 26 channels mixed into one 16-bit stereo stream at the
// device rate, which a backend's audio device only plays. A structural translation of the
// self-patching MMX mixer [orig: AudioMixer_Init @0x7BD370; AudioChannel_ComputeMixCoefficients
// @0x7BD4B0; AudioChannel_BindSampleBuffer @0x7BD671; the mix loop @0x7BD82C..0x7BEF93;
// AudioChannel_LoadNextFrame @0x7BF0F9; the stereo output stage AudioMixer_OutputStereo @0x7BD71A],
// its channel calls [orig: AudioChannel_Open @0x7668F0; AudioChannel_QueueEvent @0x766A00;
// AudioChannel_OpenDirect @0x766AA0; AudioChannel_Submit @0x766B70; AudioChannel_SetAndPlay
// @0x766C40; AudioChannel_ValidateHandle @0x766D50; Audio_FindAndOpenChannel @0x766E80;
// AudioChannel_ResetByHandle @0x767160] and its stereo blend table [orig:
// AudioMixer_InitChannelBlendTables @0x765940]. docs/audio/lwf-dbf-sound-re.md, "The mixer".
//
// A frame of the device: each channel steps its position by its step (1/512 of a sample), reads
// four taps across the step and averages them (the box resampler), and adds that sample times its
// three coefficient lanes into three buses; the buses then pass a 256-frame box low-pass whose
// complement, the input 128 frames late, is their high band. Stereo output is bus 0's high band
// (its words 0 and 1: left and right), bus 1's high band 256 frames later (two adjacent frames,
// each halved: the rear cue the pan table puts on bus 1 out of phase) and bus 0's word-3 low band
// on both sides (the bass every channel sends).

#include <base/platform/audio_device.h>
#include <runtime/audio/audio_out_meter.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace opennova::lwf {
struct WavPcm;
}

namespace opennova::audio {

// The device the mixer mixes for: the config's audio_rate, 44100 by default [orig:
// Config_SetDefaults @0x54D15B; Game_InitSubsystems @0x4A727E], and its speaker configuration's
// 2 output channels, stereo, the default [orig: Config_SetDefaults @0x54D151 (audioChannels 2);
// AudioMixer_InitChannelBlendTables @0x766045]. 16-bit PCM [orig:
// DirectSoundStream_InitPlaybackBuffer @0x768D1B]. The Options' WDM_RATE radio and the speaker
// rows are unserviced (D-MNU-21): the device is this one.
inline constexpr uint32_t kMixDeviceRate = 44100;
inline constexpr int kMixOutputChannels = 2;
// The frames the stream is kept filled ahead of the play cursor: a fifth of a second [orig:
// AudioStream_FillBuffer @0x76910C, `rate / 5` frames; the ring twice it @0x768D15].
inline constexpr uint32_t kMixFillFrames = kMixDeviceRate / 5;

// The channels and who owns them [orig: the 26 of AudioMixer_Init @0x7BD49F]: 0 the scripted
// voice (the WAC voice, the radio, the entity voice) [orig: Audio_Play3DPositionalSound
// @0x4ECD79], 1 and 2 the music's left and right [orig: Audio_SubmitStereoSampleSplit
// @0x7BD2E6 / @0x7BD348], 3 the dialog line [orig: Dialog_LoadAudioClip @0x44DE8B], 4..11 the
// eight emitter channels [orig: SoundEmitter_UpdateAndMixTop8 @0x528ADB, `desc_ptr + 4`], 12..25
// the one-shots [orig: Audio_FindAndOpenChannel @0x766F17, the scan from 12].
inline constexpr size_t kAudioChannelCount = 26;
inline constexpr int kVoiceChannel = 0;
inline constexpr int kMusicLeftChannel = 1;
inline constexpr int kMusicRightChannel = 2;
inline constexpr int kDialogChannel = 3;
inline constexpr int kFirstAmbientChannel = 4;
inline constexpr size_t kFirstOneshotChannel = 12;
// A channel's queue of buffers to play next [orig: the 16-entry ring g_AudioQueuedParameters
// @0x33430A0, its indices masked `and edx, 0Fh` @0x7BF12A].
inline constexpr uint32_t kChannelQueueDepth = 16;

// An AUD1 buffer as the mixer plays it [orig: the header the loader writes,
// Audio_LoadWavFileFromArchive @0x766603..0x76663C]: its sample count (+4), its pitch ratio to the
// 44100 Hz device in Q16 (+8), its width byte (+12: 16-bit samples where it is 2, 8-bit for any
// other [orig: AudioChannel_BindSampleBuffer @0x7BD692]) and its shift byte (+13), the samples at
// the 16-bit scale (an 8-bit one shifted up 8, which the mixer's `shl ebx, 6` of the four bytes'
// sum makes exact) and after them the pad the mixer's upper taps read (the loader's eight bytes,
// mix_wave_from_pcm). `id` stands for the buffer's address, whose bits above the sixth a channel
// handle carries [orig: AudioChannel_Open @0x7668FE].
struct MixWave {
	uint32_t id = 0;
	uint32_t count = 0;
	uint32_t pitch_q16 = 0;
	uint8_t width = 2;
	uint8_t shift = 0;
	std::vector<int16_t> samples;
};
using MixWaveRef = std::shared_ptr<const MixWave>;

// A buffer from its fields, `samples` the count's and its pad; a fresh id.
MixWaveRef make_mix_wave(uint32_t count, uint32_t pitch_q16, uint8_t width, uint8_t shift,
		std::vector<int16_t> samples);
// A decoded wave's buffer, the loader's pad after its samples: eight bytes, zeros where the last
// sample is under 8 (a byte) or 0x800 (a word) in size, else the first eight sample bytes (eight
// 8-bit samples, four 16-bit ones) [orig: Audio_LoadWavFileFromArchive @0x76668A..0x7668DA]. Null
// for a wave of no samples. A stereo decode (the tooling's) gives its `channel`'s samples.
MixWaveRef mix_wave_from_pcm(const lwf::WavPcm &pcm, size_t channel = 0);
// The silent buffer every idle channel plays at volume 0 [orig: g_AudioSilentAud1Header @0x85A600:
// 10 8-bit samples at pitch 0x400, shift 8]. Retail's samples share bytes with the stream's ring
// size (dword_85A618), which only the channel-zero follower reads; ours are zeros.
const MixWaveRef &silent_mix_wave();

// A channel's 24-byte descriptor [orig: AudioChannelParameters, g_AudioChannelBackupParameters
// @0x3345B88]: the buffer, the play factor (+4, Q16), the owner's id (+8: a one-shot's source,
// which the allocator matches), the dry volume (+0x0C), the word-3 send level (+0x0D), the pan
// byte (+0x0E, a 0..255 turn: 0 ahead, 48 hard left, 128 behind, 208 hard right), the spread
// (+0x0F, the pan entries either side the coefficients add), the ambience blend (+0x10, from the
// pan's coefficients toward the mode's fixed pair) and the four bus-2 levels (+0x11..+0x14, which
// the stereo output never reads).
struct ChannelParams {
	MixWaveRef wave;
	uint32_t play_q16 = 0;
	uint32_t id = 0;
	uint8_t volume = 0;
	uint8_t send = 0;
	uint8_t pan = 0;
	uint8_t spread = 0;
	uint8_t ambience = 0;
	std::array<uint8_t, 4> aux{};
};

// The scratch the channel calls fill [orig: g_AudioChannelScratch @0x3345890, as
// Audio_FindAndOpenChannel @0x766EA6..0x766EFC, AudioChannel_PlaySound @0x766FFC..0x76704A and
// AudioChannel_OpenSlotChecked @0x76708C..0x7670DA fill it]: all six volume bytes the one volume,
// no spread, the ambience blend g_AudioChannelAmbienceBlend, which nothing writes (0).
ChannelParams voice_params(MixWaveRef wave, uint8_t pan, uint8_t volume, uint32_t play_q16,
		uint32_t id = 0);

class Mixer {
public:
	// The stereo blend table, the device factor and every channel opened on the silent buffer and
	// bound [orig: Audio_InitSubsystems @0x7670F0 -> AudioMixer_InitChannelBlendTables,
	// AudioMixer_Init @0x7BD370 (AudioChannel_InitAll @0x7BD49A, the binds @0x7BD49F..0x7BD4A4)].
	Mixer();

	// A channel opened on `params`, played from its start at the next frame and then the silent
	// buffer [orig: AudioChannel_Open @0x7668F0]; its handle, 0 for no buffer or channel.
	uint32_t open(int channel, const ChannelParams &params);
	// Opened looping: `params` replays whenever the queue is empty [orig: AudioChannel_OpenDirect
	// @0x766AA0].
	uint32_t open_looping(int channel, const ChannelParams &params);
	// `params` queued to play after what the channel plays [orig: AudioChannel_QueueEvent
	// @0x766A00].
	void queue(int channel, const ChannelParams &params);
	// The channel's descriptor replaced at once, its loop's too while it loops, the coefficients
	// and step recomputed, the position kept [orig: AudioChannel_Submit @0x766B70].
	uint32_t submit(int channel, const ChannelParams &params);
	// The playing buffer re-panned and re-levelled: its pan, all six volume bytes `volume`, its
	// play factor, its spread and ambience kept [orig: AudioChannel_SetAndPlay @0x766C40]; 0 for a
	// handle the channel no longer plays.
	uint32_t set_and_play(uint32_t handle, uint8_t pan, uint8_t volume, uint32_t play_q16);
	// The channel opened on the silent buffer [orig: AudioChannel_ResetByHandle @0x767160, a
	// handle it no longer plays left alone; AudioChannel_OpenSilent @0x767140].
	uint32_t reset(uint32_t handle);
	void open_silent(int channel);
	// Every channel opened on the silent buffer [orig: AudioChannel_InitAll @0x7671B0].
	void init_all();
	// The music's two channels silenced [orig: Audio_FlushChannels @0x7BCF70].
	void flush_music();
	// `handle` while its channel still plays its buffer, else 0 [orig: AudioChannel_ValidateHandle
	// @0x766D50].
	uint32_t validate(uint32_t handle) const;
	// A one-shot channel: of 12..25 the one whose six volume bytes sum least, a channel already
	// playing `wave` for a nonzero `id` scoring 0, and only under 12 times `volume`; 0 when none
	// [orig: Audio_FindAndOpenChannel @0x766E80].
	uint32_t find_and_open(const MixWaveRef &wave, uint8_t pan, uint8_t volume, uint32_t play_q16,
			uint32_t id);
	// The buffers queued on a channel [orig: Audio_GetPendingBufferCount @0x7BD357, channel 1's].
	uint32_t pending(int channel) const;
	const ChannelParams &channel(int channel) const { return channels_[size_t(channel)].current; }
	uint32_t handle_of(int channel) const;

	// `frames` device frames mixed, left then right [orig: the mix loop @0x7BDD90..0x7BEF6D].
	void render(int16_t *interleaved, uint32_t frames);
	// The output meter channel zero feeds [orig: @0x7BDE22..0x7BDE2C, @0x7BEF16..0x7BEF55].
	const AudioOutMeter &meter() const { return meter_; }

	// A coefficient lane quadword [orig: qword_7988B0 + 24 * channel: the dry, delayed and bus-2
	// quadwords]; the step and the three tap offsets [orig: the patched adds @0x7BDD9E..0x7BDDC0].
	struct Lanes {
		std::array<int16_t, 4> w{};
	};
	const Lanes &dry_lanes(int channel) const { return channels_[size_t(channel)].dry; }
	const Lanes &delayed_lanes(int channel) const { return channels_[size_t(channel)].delayed; }
	uint32_t step(int channel) const { return channels_[size_t(channel)].step; }
	// The averaged sample a channel mixed in the last frame, the word its lanes multiply (0 for a
	// channel that sat the call out) [orig: ebx @0x7BDDF6..0x7BDDFF].
	int16_t last_sample(int channel) const { return channels_[size_t(channel)].sample; }
	// One device frame of bus input through the tail and the output stage alone, the second half
	// of render's frame (what the channels' sum feeds): bus 0's and bus 1's words in, the left and
	// right samples out [orig: @0x7BEC79..0x7BEF6D]. The ctest runs it against retail's own tail
	// on the same input.
	void render_buses(const Lanes &bus0, const Lanes &bus1, int16_t out[2]);
	// The blend table's entry for a pan byte: words 0..3 the dry lanes, 4..7 the delayed.
	const std::array<int16_t, 8> &pan_entry(uint8_t pan) const { return pan_table_[pan]; }

private:
	struct Channel {
		ChannelParams current; // g_AudioChannelBackupParameters
		ChannelParams idle;    // g_AudioChannelActiveParameters: what plays when the queue runs dry
		std::array<ChannelParams, kChannelQueueDepth> queue;
		uint32_t read = 0;  // dword_3345818
		uint32_t write = 0; // dword_33457B0
		int32_t position = -1; // dword_3345B20: 1/512 samples, from -(count << 9) up to 0
		uint32_t step = 1;
		std::array<uint32_t, 3> taps{};
		Lanes dry;
		Lanes delayed;
		int16_t sample = 0;
	};

	void compute_coefficients(int channel);
	void load_next_frame(int channel);
	int16_t mix_sample(int channel);

	std::array<std::array<int16_t, 8>, 256> pan_table_{};
	std::array<Channel, kAudioChannelCount> channels_{};
	// The tail's state [orig: the rings @0x795000 / @0x796000, the sums qword_798818..798830,
	// the bus-1 history qword_798000, the frame counter dword_79CB20, the follower dword_79CB24].
	std::array<std::array<int32_t, 4>, 256> ring0_{};
	std::array<std::array<int32_t, 4>, 256> ring1_{};
	std::array<int32_t, 4> sum0_{};
	std::array<int32_t, 4> sum1_{};
	std::array<Lanes, 256> history_{};
	uint32_t frame_ = 0;
	int32_t follower_ = 0;
	AudioOutMeter meter_;
};

// The spec a platform audio device opens at to play the mixer's stream (platform::AudioDevice):
// kMixDeviceRate, kMixOutputChannels, signed 16-bit.
platform::AudioSpec mix_audio_spec();
// The device's queue kept kMixFillFrames ahead of its play, as retail's fill keeps its stream
// buffer: the frames it lacks mixed and pushed. The frames pushed (0 for a closed device or a full
// queue).
uint32_t feed_audio_device(Mixer &mixer, platform::AudioDevice &device);

} // namespace opennova::audio
