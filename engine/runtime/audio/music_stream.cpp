// The music stream over the mixer -- see music_stream.h.
#include <runtime/audio/music_stream.h>

#include <runtime/audio/mixer.h>

#include <base/io/le.h>
#include <formats/sbf/sbf.h>

#include <algorithm>
#include <vector>

namespace opennova::audio {

namespace {

// The stereo split's frame bounds: a chunk of 3 frames or fewer, or past 2048, queues nothing
// [orig: Audio_SubmitStereoSampleSplit @0x7BD222..0x7BD233].
constexpr uint32_t kSplitLeast = 3;
constexpr uint32_t kSplitMost = 0x800;
// The split's pad: the last frame four times more [orig: @0x7BD268..0x7BD283]; the IMA ADPCM
// form's buffers hold the decoder's own samples past the count (one past it is all the 22050 Hz
// step's upper taps reach), ours the last sample repeated likewise.
constexpr size_t kSplitPad = 4;
// The pans the two channels are queued at, which their fixed coefficients override [orig:
// @0x7BD2A9 / @0x7BD30B; AudioChannel_ComputeMixCoefficients @0x7BD553..0x7BD574].
constexpr uint8_t kMusicLeftPan = 0x40;
constexpr uint8_t kMusicRightPan = 0xC0;

ChannelParams music_params(MixWaveRef wave, uint8_t pan, uint8_t volume) {
	// The split's scratch [orig: @0x7BD29A..0x7BD2DA]: play 0x10000, no spread or ambience, the dry
	// and send bytes the music's volume, the bus-2 bytes 0. Its +8 is never written: whatever the
	// scratch last held, which no music reader reads.
	ChannelParams params;
	params.wave = std::move(wave);
	params.play_q16 = 0x10000;
	params.volume = volume;
	params.send = volume;
	params.pan = pan;
	return params;
}

} // namespace

void MusicStream::open(const sbf::SbfArchive *archive, const uint8_t *bytes, size_t size,
		int32_t global_volume) {
	archive_ = archive;
	bytes_ = bytes;
	size_ = size;
	cursor_ = 0;
	bytes_left_ = 0;
	samples_left_ = 0;
	global_ = 0xFF0000;
	current_ = 0xFF0000;
	target_ = 0xFF0000;
	ramp_ = 0;
	// The fade word (dword_31C380C) is zeroed here and set by no live code [orig: its setter
	// @0x671F50 has no reference]: ours has none.
	mixer_.flush_music();
	set_global_volume(global_volume);
}

void MusicStream::close() {
	mixer_.flush_music();
	archive_ = nullptr;
	bytes_ = nullptr;
	size_ = 0;
	bytes_left_ = 0;
}

void MusicStream::set_global_volume(int32_t volume) {
	global_ = volume < 0 ? 0 : (volume > 255 ? 255 : volume) << 16;
}

void MusicStream::volume_changed(int32_t current_16_16, int32_t target_16_16) {
	// GSV sets both, GSDV the target alone (its current -1) [orig: AudioVM_Intrinsic_GSV @0x6720E0;
	// AudioVM_Intrinsic_GSDV @0x672120].
	if (current_16_16 >= 0) current_ = current_16_16;
	target_ = target_16_16;
}

void MusicStream::start_track(uint32_t entry) {
	if (archive_ == nullptr || archive_->entries == nullptr || archive_->header.entry_count == 0) {
		bytes_left_ = 0;
		return;
	}
	// [orig: AudioVM_SelectStream @0x671BC0: an entry past the count reads entry 0]
	const uint32_t index = entry >= archive_->header.entry_count ? 0 : entry;
	const sbf::SbfRawEntry &raw = archive_->entries[index];
	block_size_ = raw.block_size;
	bytes_left_ = static_cast<int32_t>(raw.total_size);
	samples_left_ = raw.sample_length_hint;
	// A block size past 16 bits streams nothing [orig: @0x671BFE..0x671C0A].
	if ((block_size_ & 0xFFFF0000u) != 0) {
		bytes_left_ = 0;
		samples_left_ = 0;
		return;
	}
	// The stream volume's step a block toward its target over the track [orig: @0x671C1A..
	// 0x671C3B]; retail divides by zero on a track of no whole block, ours steps none.
	const int32_t blocks = block_size_ != 0 ? bytes_left_ / static_cast<int32_t>(block_size_) : 0;
	ramp_ = blocks != 0 ? (target_ - current_) / blocks : 0;
	cursor_ = raw.data_offset;
}

bool MusicStream::queue_full() const {
	// Channel 1's queue [orig: Audio_GetPendingBufferCount @0x7BD357].
	return mixer_.pending(kMusicLeftChannel) >= kMusicQueueFull;
}

void MusicStream::stream_block() {
	if (archive_ == nullptr || bytes_ == nullptr) {
		bytes_left_ = 0;
		return;
	}
	// The read [orig: AudioStream_Update @0x671CA4..0x671CBC]: a read past the bank ends the track
	// (retail's read of no bytes replays its buffer).
	const size_t available = cursor_ < size_ ? std::min(size_ - cursor_, size_t(block_size_)) : 0;
	if (available == 0) {
		bytes_left_ = 0;
		return;
	}
	const uint8_t *block = bytes_ + cursor_;
	cursor_ += available;
	// The block's volume, the global and stream words' high halves multiplied [orig: @0x671CDA..
	// 0x671CEF, @0x671D1D..0x671D44; Audio_SetGlobalVolume @0x766D00, past 255 held at 255].
	const int32_t product =
			(int32_t(int16_t(uint32_t(global_) >> 16)) * int32_t(int16_t(uint32_t(current_) >> 16))) >> 8;
	volume_ = static_cast<uint8_t>(uint32_t(product) > 255u ? 255u : uint32_t(product));
	if (archive_->header.flags == sbf::SBF_FLAGS_BYTE_PAIRED_STEREO) {
		split_byte_paired(block, available);
	} else {
		// [orig: @0x671CF8..0x671D0F: the frames left, at most 4068 a block]
		split_adpcm(block, available, std::min<uint32_t>(sbf::SBF_ADPCM_FRAMES, samples_left_));
	}
	// [orig: @0x671D61..0x671D98]
	bytes_left_ = static_cast<int32_t>(uint32_t(bytes_left_) - block_size_);
	samples_left_ -= sbf::SBF_ADPCM_FRAMES;
	const int32_t next = static_cast<int32_t>(uint32_t(current_) + uint32_t(ramp_));
	current_ = next < 0 ? 0 : std::min<int32_t>(next, 0xFF0000);
}

void MusicStream::split_byte_paired(const uint8_t *block, size_t size) {
	if (size < sbf::SBF_CHUNK_HEADER) return;
	// The chunk's byte count and its two shifts [orig: AudioStream_Update @0x671D29..0x671D56 ->
	// Audio_SubmitStereoSampleSplit(ecx = +0, bx = +4, esi = +8)].
	const uint32_t frames = io::read_u32_le(block) >> 1;
	if (frames <= kSplitLeast || frames > kSplitMost) return;
	const uint8_t *pairs = block + sbf::SBF_CHUNK_HEADER;
	const size_t present = (size - sbf::SBF_CHUNK_HEADER) / 2;
	std::vector<int16_t> left(size_t(frames) + kSplitPad, 0);
	std::vector<int16_t> right(size_t(frames) + kSplitPad, 0);
	// Each byte less 0x80, an 8-bit buffer's sample [orig: @0x7BD252..0x7BD266].
	for (size_t i = 0; i < frames && i < present; ++i) {
		left[i] = static_cast<int16_t>(int32_t(int8_t(pairs[2 * i] ^ 0x80u)) * 256);
		right[i] = static_cast<int16_t>(int32_t(int8_t(pairs[2 * i + 1] ^ 0x80u)) * 256);
	}
	for (size_t i = 0; i < kSplitPad; ++i) {
		left[frames + i] = left[frames - 1];
		right[frames + i] = right[frames - 1];
	}
	// Width 1, the shifts at +0x0D [orig: @0x7BD23E..0x7BD24D].
	mixer_.queue(kMusicLeftChannel,
			music_params(make_mix_wave(frames, kMusicChunkPitchQ16, 1, block[4], std::move(left)),
					kMusicLeftPan, volume_));
	mixer_.queue(kMusicRightChannel,
			music_params(make_mix_wave(frames, kMusicChunkPitchQ16, 1, block[5], std::move(right)),
					kMusicRightPan, volume_));
}

void MusicStream::split_adpcm(const uint8_t *block, size_t size, uint32_t count) {
	// [orig: Audio_PlayStereoSample @0x7BCFB2..0x7BCFC1: 1..4068 frames]
	if (count < 1 || count > sbf::SBF_ADPCM_FRAMES) return;
	std::vector<int16_t> frames;
	sbf::sbf_decode_adpcm_chunk(block, size, count, frames);
	if (frames.size() < size_t(count) * 2) return;
	std::vector<int16_t> left(size_t(count) + kSplitPad, 0);
	std::vector<int16_t> right(size_t(count) + kSplitPad, 0);
	for (size_t i = 0; i < count; ++i) {
		left[i] = frames[2 * i];
		right[i] = frames[2 * i + 1];
	}
	for (size_t i = 0; i < kSplitPad; ++i) {
		left[count + i] = left[count - 1];
		right[count + i] = right[count - 1];
	}
	// Width 2, no shift [orig: @0x7BCFC7..0x7BCFDD]; queued as the split queues [orig:
	// @0x7BD13B..0x7BD1F6].
	mixer_.queue(kMusicLeftChannel,
			music_params(make_mix_wave(count, kMusicChunkPitchQ16, 2, 0, std::move(left)),
					kMusicLeftPan, volume_));
	mixer_.queue(kMusicRightChannel,
			music_params(make_mix_wave(count, kMusicChunkPitchQ16, 2, 0, std::move(right)),
					kMusicRightPan, volume_));
}

} // namespace opennova::audio
