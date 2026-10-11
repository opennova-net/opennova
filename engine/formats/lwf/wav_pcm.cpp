#include <formats/lwf/wav_pcm.h>
#include <base/io/le.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace opennova {
namespace lwf {

namespace {

// The loader's limits ahead of the data [orig: Audio_LoadWavFileFromArchive @ 0x766480]: a chunk past
// this size refused (@ 0x7665a3), and the walk given up at this many chunks (@ 0x7665b2).
constexpr uint32_t kChunkMost = 0x800;
constexpr int kChunksMost = 12;
constexpr uint16_t kTagImaAdpcm = 0x11;
// The least rate whose pitch ratio, ((rate << 16) + 22050) / 44100, the loader's division cannot hold
// in 32 bits [orig: Audio_LoadWavFileFromArchive @ 0x76662b, @ 0x766730, @ 0x7667dc]: 44100 << 16.
constexpr uint32_t kRatioFaultRate = 44100u << 16;

bool tag_eq(const uint8_t *p, const char *tag) {
	return p[0] == static_cast<uint8_t>(tag[0]) && p[1] == static_cast<uint8_t>(tag[1]) &&
			p[2] == static_cast<uint8_t>(tag[2]) && p[3] == static_cast<uint8_t>(tag[3]);
}

const char *refusal_words(WaveRefusal refusal) {
	switch (refusal) {
		case WaveRefusal::None: return "";
		case WaveRefusal::NotRiffWave: return "not a RIFF/WAVE file";
		case WaveRefusal::EndsBeforeData: return "the file ends before its data chunk";
		case WaveRefusal::SecondFmt: return "a second fmt chunk ahead of the data";
		case WaveRefusal::ChunkPastMost: return "a chunk past 0x800 bytes ahead of the data";
		case WaveRefusal::TwelveChunks: return "twelve chunks ahead of the data";
		case WaveRefusal::NoFmt: return "no fmt chunk ahead of the data";
		case WaveRefusal::Bits: return "samples of other than 8, 16 or 4 bits";
		case WaveRefusal::Channels: return "more than one channel";
		case WaveRefusal::NoFact: return "4-bit samples with no fact chunk";
		case WaveRefusal::NotImaAdpcm: return "4-bit samples not IMA ADPCM";
		case WaveRefusal::RatioFaults: return "a rate from 2890137600 Hz puts its pitch ratio past 32 bits";
	}
	return "";
}

// WAV PCM8 is UNSIGNED (128 = center): upconverted to signed 16-bit LE.
void pcm8_to_pcm16(const uint8_t *src, uint32_t size, std::vector<uint8_t> &out) {
	out.resize(static_cast<size_t>(size) * 2);
	for (uint32_t i = 0; i < size; ++i) {
		const int16_t s = static_cast<int16_t>((static_cast<int>(src[i]) - 128) * 256);
		out[i * 2] = static_cast<uint8_t>(s & 0xFF);
		out[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
	}
}

// The loader's pitch ratio, ((rate << 16) + 22050) / 44100
// [orig: Audio_LoadWavFileFromArchive @ 0x766612..0x76662d / @ 0x766717..0x766735].
uint32_t loader_pitch_q16(uint32_t rate) {
	return static_cast<uint32_t>(((static_cast<uint64_t>(rate) << 16) + 22050u) / 44100u);
}

// The rate a pitch of 0 plays at. The mixer steps a channel each device frame by
// (((play * factor) >> 16) * pitch + 0x400000) >> 23, the factor (44100 << 16) / device, and forces a
// step of 0 to 1 [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd5f6..0x7bd60e, @ 0x7bd619..0x7bd61d;
// AudioMixer_Init @ 0x7bd381..0x7bd393], a step counted in 1/512 of a sample: the position starts at
// -(count << 9) - step [orig: sub_7BD671 @ 0x7bd684..0x7bd689], the mix loop adds the step to it each
// device frame (channel 0's self-patched `add eax, step` @ 0x7bdd9e, the step stored through
// off_79CBFC at 0x7bdd9f) and reads the sample at the position >> 9 (`sar eax, 9` @ 0x7bddc6). A
// pitch of 0 plays 1/512 of a sample a device frame, whatever the play factor. The device runs at
// the config's audio_rate, 44100 by default [orig: Config_SetDefaults @ 0x54d15b;
// Game_InitSubsystems @ 0x4a727e] (the Options' WDM_RATE radio, which can pick 22050, is not
// serviced, D-MNU-21): 44100 / 512, 86.13 Hz, which the decode hands the shell's stream as its whole
// mix rate, 86, every player's scale making up the rest (kLeastStepRate, wave_pitch_scale). Every other
// pitch hands the shell its own rate, not the step's 1/512 quantum (D-SND-50), past INT32_MAX from an
// AUD1 pitch of 0xBE37C63A or a RIFF rate of 0x80000000, which the shell's player boxes
// (wave_pitch_scale).
constexpr uint32_t kPitchZeroRate = 44100u / 512u;
// The least step as a rate, 1/512 of a sample a frame of the 44100 Hz device: 86.1328125 Hz, exact in
// a player's scale. A wave of pitch 0 and a play factor of 0 step alike at it.
constexpr double kLeastStepRate = 44100.0 / 512.0;

// The mixer's device factor, (44100 << 16) / device [orig: AudioMixer_Init @ 0x7bd381..0x7bd393]:
// 0x10000 on the config's default 44100 Hz device (the Options' WDM_RATE radio is unserviced, D-MNU-21).
constexpr uint32_t kDeviceFactor = 0x10000u;

// The loader's own form, an AUD1 buffer (bytes 41 55 44 31), which it copies as it is, unchecked
// [orig: Audio_LoadWavFileFromArchive @ 0x766480, the magic @ 0x7664e2, the copy @ 0x7664e9..0x766511]:
// the header its RIFF decodes write (@ 0x766603..0x76663c, @ 0x766708..0x766745,
// @ 0x7667b4..0x7667f0), read as the mixer's channel set-up reads it, the sample count at +4
// [orig: sub_7BD671 @ 0x7bd67f], the pitch ratio to the 44100 Hz device in Q16 at +8
// [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd603] and the samples from +16, signed (the
// RIFF form's 8-bit bias already taken off), 16-bit where the byte at +12 is 2 and 8-bit for any
// other byte there [orig: sub_7BD671 @ 0x7bd692], their volume shifted down by the byte at +13 [orig:
// AudioChannel_ComputeMixCoefficients @ 0x7bd5e6]. The copy is the bytes' own size; where they end inside
// the header or before the count's samples the loader's pad and the mixer read on past them
// (unwitnessed): ours refuses the first and plays the samples present of the second, the count
// recorded as it says.
bool decode_aud1(const uint8_t *bytes, size_t size, WavPcm &out, std::string &error) {
	if (size < 16) {
		error = "the AUD1 buffer ends inside its 16-byte header";
		return false;
	}
	const uint32_t samples = io::read_u32_le(bytes + 4);
	const uint32_t pitch_q16 = io::read_u32_le(bytes + 8);
	const size_t width = bytes[12] == 2 ? 2 : 1;
	const size_t present = std::min<size_t>(samples, (size - 16) / width);
	out.pcm16.resize(present * 2);
	if (width == 2) {
		if (present != 0) std::memcpy(out.pcm16.data(), bytes + 16, present * 2);
	} else {
		for (size_t i = 0; i < present; ++i) {
			out.pcm16[2 * i] = 0;
			out.pcm16[2 * i + 1] = bytes[16 + i];
		}
	}
	// The loader's pad for the mixer, past the last sample by the raw width byte [orig:
	// Audio_LoadWavFileFromArchive @ 0x76668a..0x7668da]: the last sample at +16 + (count - 1) * width
	// (@ 0x76668a..0x76669c), a byte where the width is 1 (@ 0x76669f) and a word otherwise
	// (@ 0x7668af); eight bytes at +16 + count * width, zeroed where it is under 8 (a byte) or 0x800 (a
	// word) in size, else the first eight sample bytes copied there (@ 0x7666b9..0x7666c7,
	// @ 0x7668bc..0x7668da). At widths 1 and 2 that is past the count's samples; at 3 or more past
	// them too, past the buffer unless it holds width * count bytes (D-SND-48); at 0 it is +16, the
	// mixer's first eight samples, zeroed where the word there is under 0x800 in size (where that word
	// runs past the bytes, unwitnessed, ours leaves them).
	if (bytes[12] == 0 && size >= 18) {
		const int16_t first = static_cast<int16_t>(io::read_u16_le(bytes + 16));
		if (first > -0x800 && first < 0x800)
			std::fill(out.pcm16.begin(), out.pcm16.begin() + std::min<size_t>(8, present) * 2, uint8_t(0));
	}
	// The byte at +13: the mixer shifts the channel's three mix coefficient quadwords right by it,
	// arithmetically (movzx ecx, byte [edi+0Dh]; movd mm4, ecx; psraw mm5 / mm6 / mm7, mm4 [orig:
	// AudioChannel_ComputeMixCoefficients @ 0x7bd5e6..0x7bd5f3]), each unit halving the channel's
	// volume; a count of 15 or more leaves no coefficient above 0 (each at most 0x7FFF; psraw past 15
	// fills a word with its sign), so the channel plays silent for its length. Ours shifts the samples
	// alike, s >> n, silence from 15 on. Retail scales each sample by the shifted coefficient,
	// (s * (c >> n)) >> 16, where the shell's mixer scales by its volume (D-SND-8), so the low bits
	// round apart, and mm6's negative words (the 2-channel blend table's +/-(80 * w) >> 7 pair and
	// its 0xD500 constant, channels 1 and 2's fixed quadwords in the 4- to 8-channel modes) shifted
	// 15 or more stay -1, a delayed tap of -1 for each sample above 0 (D-SND-51).
	const uint8_t shift = bytes[13];
	if (shift != 0) {
		for (size_t i = 0; i < present; ++i) {
			const int16_t s = static_cast<int16_t>(io::read_u16_le(out.pcm16.data() + 2 * i));
			const int16_t shifted = shift >= 15 ? int16_t(0) : static_cast<int16_t>(s >> shift);
			out.pcm16[2 * i] = static_cast<uint8_t>(uint16_t(shifted) & 0xFF);
			out.pcm16[2 * i + 1] = static_cast<uint8_t>(uint16_t(shifted) >> 8);
		}
	}
	// The rate the ratio is nearest, for the shell's player (the mixer steps by the ratio itself): the
	// inverse the dialog line's hold takes, (pitch * 44100 + 0x8000) >> 16 [orig: Dialog_LoadAudioClip
	// @ 0x44dd8e..0x44dd9f, a signed imul; ours unsigned, the same below a pitch of 2^31], past
	// INT32_MAX from a pitch of 0xBE37C63A, which the mixer steps all the same, at (pitch + 64) >> 7
	// exactly at a play factor of 1 on the 44100 Hz device, whose factor is 0x10000 [orig:
	// AudioChannel_ComputeMixCoefficients @ 0x7bd5f6..0x7bd60e; AudioMixer_Init @ 0x7bd381..0x7bd393]
	// (the shell's player boxes it, wave_pitch_scale); a pitch of 0 the mixer's
	// least step [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd619..0x7bd61d].
	out.sample_rate = pitch_q16 == 0 ? kPitchZeroRate
	                                 : static_cast<uint32_t>((uint64_t(pitch_q16) * 44100 + 0x8000) >> 16);
	out.channels = 1;
	out.loader_samples = samples;
	out.loader_pitch_q16 = pitch_q16;
	return true;
}

// --- WAV IMA ADPCM (audioFormat 0x11) decode to signed 16-bit PCM ---
// NovaLogic stores voice/zone audio as 4-bit IMA ADPCM, mono, block by block: each block a header
// (int16 predictor, step index byte, a byte unread), then 4-bit nibbles stepped through the tables.
// The step at an index and the index's move by a nibble are the standard IMA tables, which the game's
// loader holds as word_7BF300 and dword_7BF3B2 [orig: Audio_AdpcmDecodeNibbleStep @ 0x7bf250,
// read @ 0x7bf256 and @ 0x7bf25e].
const int IMA_STEP_TABLE[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
const int IMA_INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
// The loader's scale of a nibble's step, in Q16: 0x2000 * (2n + 1) for its low three bits n, negative
// for its fourth (dword_7BF2C0) [orig: Audio_AdpcmDecodeNibbleStep @ 0x7bf265].
const int32_t ADPCM_SCALE_TABLE[16] = {
	0x2000, 0x6000, 0xA000, 0xE000, 0x12000, 0x16000, 0x1A000, 0x1E000,
	-0x2000, -0x6000, -0xA000, -0xE000, -0x12000, -0x16000, -0x1A000, -0x1E000
};

// The tooling's nibble step (decode_ima_adpcm_standard), not the loader's: the standard IMA sum of
// the step's shifts, each truncated.
inline int16_t ima_decode_nibble(uint8_t nib, int &predictor, int &index) {
	const int step = IMA_STEP_TABLE[index];
	int diff = step >> 3;
	if (nib & 1) {
		diff += step >> 2;
	}
	if (nib & 2) {
		diff += step >> 1;
	}
	if (nib & 4) {
		diff += step;
	}
	if (nib & 8) {
		predictor -= diff;
	} else {
		predictor += diff;
	}
	if (predictor > INT16_MAX) {
		predictor = INT16_MAX;
	} else if (predictor < INT16_MIN) {
		predictor = INT16_MIN;
	}
	index += IMA_INDEX_TABLE[nib & 0x0F];
	if (index < 0) {
		index = 0;
	} else if (index > 88) {
		index = 88;
	}
	return static_cast<int16_t>(predictor);
}

// The tooling's IMA ADPCM read (wav_decode_pcm16_lenient), not the loader's: every block the data
// holds whole or in part, each nibble by the standard steps above, mono or stereo (4-byte nibble words
// round-robin per channel after the headers). Interleaved signed 16-bit LE PCM, or empty on a
// malformed stream.
std::vector<uint8_t> decode_ima_adpcm_standard(const uint8_t *data, uint32_t size,
		int channels, uint32_t block_align) {
	const uint32_t header_bytes = static_cast<uint32_t>(4 * channels);
	if (block_align < header_bytes || channels < 1 || channels > 2) {
		return {};
	}
	std::vector<std::vector<int16_t>> ch(channels);

	uint32_t bpos = 0;
	while (bpos + header_bytes <= size) {
		const uint32_t block = (block_align < size - bpos) ? block_align : (size - bpos);
		if (block < header_bytes) {
			break;
		}
		const uint8_t *b = data + bpos;
		int pred[2] = { 0, 0 };
		int idx[2] = { 0, 0 };
		for (int c = 0; c < channels; ++c) {
			pred[c] = static_cast<int16_t>(io::read_u16_le(b + 4 * c));
			idx[c] = b[4 * c + 2];
			if (idx[c] > 88) {
				idx[c] = 88;
			}
			ch[c].push_back(static_cast<int16_t>(pred[c])); // block's first sample is the predictor
		}
		// Remaining bytes: 4-byte words, round-robin across channels (8 nibbles each).
		uint32_t off = header_bytes;
		int word_ch = 0;
		while (off + 4 <= block) {
			for (int k = 0; k < 4; ++k) {
				const uint8_t byte = b[off + k];
				ch[word_ch].push_back(ima_decode_nibble(byte & 0x0F, pred[word_ch], idx[word_ch]));
				ch[word_ch].push_back(ima_decode_nibble(byte >> 4, pred[word_ch], idx[word_ch]));
			}
			off += 4;
			word_ch = (word_ch + 1) % channels;
		}
		bpos += block_align;
	}

	// Interleave channels frame-by-frame into int16 LE bytes.
	size_t frames = ch[0].size();
	for (int c = 1; c < channels; ++c) {
		if (ch[c].size() < frames) {
			frames = ch[c].size();
		}
	}
	std::vector<uint8_t> out(frames * static_cast<size_t>(channels) * 2);
	size_t w = 0;
	for (size_t f = 0; f < frames; ++f) {
		for (int c = 0; c < channels; ++c) {
			const int16_t s = ch[c][f];
			out[w++] = static_cast<uint8_t>(s & 0xFF);
			out[w++] = static_cast<uint8_t>((s >> 8) & 0xFF);
		}
	}
	return out;
}

// One nibble of the game's loader's IMA ADPCM decode [orig: Audio_AdpcmDecodeNibbleStep @ 0x7bf250]:
// the nibble the low four bits (@ 0x7bf253), the step at the index as it stands (@ 0x7bf256), the
// index moved by the nibble (@ 0x7bf25e), the sample moved by (step * scale) >> 16, floored (imul;
// shrd 16 @ 0x7bf265..0x7bf272), and held to int16 (@ 0x7bf274..0x7bf289), the index to 0..88
// (@ 0x7bf28e..0x7bf29a). The index comes in outside 0..88 only as a block's header byte, where the
// loader reads the step past the table's ends in the image; ours reads it at the end it passed
// (D-SND-47).
void adpcm_nibble_step(uint32_t nibble, int32_t &sample, int32_t &index) {
	nibble &= 0x0F;
	const int32_t step = IMA_STEP_TABLE[index < 0 ? 0 : (index > 88 ? 88 : index)];
	index += IMA_INDEX_TABLE[nibble];
	// The 64-bit product shifted right, floored (an arithmetic shift, as shrd over imul's edx:eax).
	sample += static_cast<int32_t>((static_cast<int64_t>(step) * ADPCM_SCALE_TABLE[nibble]) >> 16);
	if (sample < INT16_MIN) sample = INT16_MIN;
	if (sample > INT16_MAX) sample = INT16_MAX;
	if (index < 0) index = 0;
	if (index > 88) index = 88;
}

// The game's loader's IMA ADPCM decode [orig: Audio_LoadWavFileFromArchive @ 0x766480, the 4-bit leg
// @ 0x766783..0x7668aa]: `count` samples, the fact chunk's (@ 0x76678a), none for a count of 0 or
// less (@ 0x7667fb), from the data's first byte. Each block its 4-byte header, the predictor a sample
// of its own and the step index a signed byte (@ 0x766801..0x76681e), then block_align - 4 bytes,
// block_align read signed (@ 0x766786..0x76679e), each byte its low nibble and then its high one
// (@ 0x766836..0x76688d); a block_align of 4 or under is a header alone (@ 0x766834). The decode stops
// the moment the count is spent, mid-block too (@ 0x766823, @ 0x766866, @ 0x766896). The loader reads
// on for the count, past the data chunk (its size unread) and past the file's end (unwitnessed):
// ours reads to the end of the bytes and stops there. Signed 16-bit LE PCM.
std::vector<uint8_t> decode_ima_adpcm_loader(const uint8_t *bytes, size_t size, size_t data,
		int32_t count, uint16_t block_align) {
	std::vector<uint8_t> out;
	const int32_t block_bytes = static_cast<int32_t>(static_cast<int16_t>(block_align)) - 4;
	if (count <= 0 || data > size) return out;
	out.reserve(2 * std::min<size_t>(static_cast<size_t>(count), 2 * (size - data) + 1));
	size_t at = data;
	for (;;) {
		if (at + 3 > size) return out;
		int32_t sample = static_cast<int16_t>(io::read_u16_le(bytes + at));
		int32_t index = static_cast<int8_t>(bytes[at + 2]);
		at += 4;
		io::append_u16_le(out, static_cast<uint16_t>(sample));
		if (--count <= 0) return out;
		for (int32_t n = 0; n < block_bytes; ++n) {
			if (at >= size) return out;
			// The byte sign-extended (movsx @ 0x766836); its high nibble shifted down (@ 0x76686f).
			const uint32_t byte = static_cast<uint32_t>(static_cast<int8_t>(bytes[at++]));
			adpcm_nibble_step(byte, sample, index);
			io::append_u16_le(out, static_cast<uint16_t>(sample));
			if (--count <= 0) return out;
			adpcm_nibble_step(byte >> 4, sample, index);
			io::append_u16_le(out, static_cast<uint16_t>(sample));
			if (--count <= 0) return out;
		}
	}
}

}  // namespace

WaveLoaderWalk wave_loader_walk(const uint8_t *bytes, size_t size) {
	WaveLoaderWalk walk;
	const auto refused = [&walk](WaveRefusal why) {
		walk.refusal = why;
		return walk;
	};
	const auto tag_at = [bytes, size](size_t at, const char *tag) {
		return bytes != nullptr && at + 4 <= size && tag_eq(bytes + at, tag);
	};
	// [orig: Audio_LoadWavFileFromArchive @ 0x76651e..0x76653b]
	if (!tag_at(0, "RIFF") || !tag_at(8, "WAVE")) return refused(WaveRefusal::NotRiffWave);
	size_t at = 12;
	bool has_fmt = false;
	for (int count = 0;;) {
		// The loader reads on past the bytes for a data chunk; the walk stops at them.
		if (at + 8 > size) return refused(WaveRefusal::EndsBeforeData);
		const uint32_t chunk_size = io::read_u32_le(bytes + at + 4);
		if (tag_at(at, "RIFF")) {
			at += 12; // a nested RIFF header [orig: @ 0x766557..0x766564]
		} else if (tag_at(at, "LIST")) {
			at += 4; // stepped into by its id alone [orig: @ 0x766569..0x766570]
			walk.after_list = true;
		} else {
			// The first data of nonzero size ends the walk [orig: @ 0x766592..0x76659b].
			if (tag_at(at, "data") && chunk_size != 0) {
				walk.data = at + 8;
				walk.data_size = chunk_size;
				break;
			}
			if (tag_at(at, "fmt ")) {
				if (has_fmt) return refused(WaveRefusal::SecondFmt); // [orig: @ 0x766589]
				has_fmt = true;
				walk.fmt = at;
			} else if (tag_at(at, "fact")) {
				walk.has_fact = true; // [orig: @ 0x76657c]
				walk.fact = at;
			}
			if (chunk_size > kChunkMost) { // [orig: @ 0x76659d..0x7665a3]
				walk.chunk = at;
				walk.chunk_size = chunk_size;
				return refused(WaveRefusal::ChunkPastMost);
			}
			walk.after_list = false;
			at += size_t(chunk_size) + 8; // unpadded [orig: @ 0x7665a5]
		}
		// [orig: @ 0x7665a9..0x7665b2]
		if (++count >= kChunksMost) return refused(WaveRefusal::TwelveChunks);
	}
	// At the data: the fmt chunk ahead of it [orig: @ 0x7665c9..0x7665cb], its fields read where
	// they sit whatever its size.
	if (!has_fmt || walk.fmt + 24 > size) return refused(WaveRefusal::NoFmt);
	walk.tag = io::read_u16_le(bytes + walk.fmt + 8);
	walk.channels = io::read_u16_le(bytes + walk.fmt + 10);
	walk.rate = io::read_u32_le(bytes + walk.fmt + 12);
	walk.block_align = io::read_u16_le(bytes + walk.fmt + 20);
	walk.bits = bytes[walk.fmt + 22]; // [orig: @ 0x7665cd]
	// By the sample width [orig: @ 0x7665d3, @ 0x7666ce, @ 0x76675d]: other widths refused
	// (@ 0x76675f); one channel at each (@ 0x7665e3, @ 0x7666db, @ 0x76676a); 4-bit samples a fact
	// chunk (@ 0x766772) and IMA ADPCM's tag (@ 0x76677d). The tag of 8-bit and 16-bit samples is
	// never read.
	if (walk.bits != 8 && walk.bits != 16 && walk.bits != 4) return refused(WaveRefusal::Bits);
	if (walk.channels != 1) return refused(WaveRefusal::Channels);
	if (walk.bits == 4 && !walk.has_fact) return refused(WaveRefusal::NoFact);
	if (walk.bits == 4 && walk.tag != kTagImaAdpcm) return refused(WaveRefusal::NotImaAdpcm);
	// Then each width's leg divides the rate into its pitch ratio, ((rate << 16) + 22050) / 44100, as
	// edx:eax by 44100 [orig: @ 0x76662b, @ 0x766730, @ 0x7667dc]: from 0xAC440000 the quotient passes
	// 32 bits and the division faults, an original bug. Ours refuses the wave there, where the game's
	// loader faults (D-SND-54).
	if (walk.rate >= kRatioFaultRate) return refused(WaveRefusal::RatioFaults);
	return walk;
}

bool wav_decode_pcm16(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error) {
	r_out = WavPcm{};
	r_error.clear();
	// The own buffer ahead of the RIFF compare [orig: Audio_LoadWavFileFromArchive @ 0x7664e2].
	if (bytes != nullptr && size >= 4 && tag_eq(bytes, "AUD1"))
		return decode_aud1(bytes, size, r_out, r_error);
	const WaveLoaderWalk walk = wave_loader_walk(bytes, size);
	if (walk.refusal != WaveRefusal::None) {
		r_error = std::string(walk.refusal == WaveRefusal::RatioFaults ? "the game's wave loader faults on it: "
		                                                                 : "the game's wave loader refuses it: ") +
				refusal_words(walk.refusal);
		return false;
	}
	// A data chunk running past the bytes plays the bytes there (the loader copies its size from
	// whatever memory follows the file).
	const uint32_t data_size =
			static_cast<uint32_t>(std::min<size_t>(walk.data_size, size - walk.data));
	const uint8_t *src = bytes + walk.data;
	// The samples by their width: 8-bit unbiased (the loader's psubb 0x80
	// [orig: @ 0x766648..0x766680]), 16-bit copied as they are (@ 0x766750), 4-bit IMA ADPCM;
	// and the loader's record, 8-bit the data chunk's size as it says, 16-bit half of it, past
	// the bytes or not, IMA ADPCM the fact chunk's count
	// [orig: @ 0x766609, @ 0x766706..0x76670e, @ 0x76678a..0x7667ba].
	if (walk.bits == 8) {
		pcm8_to_pcm16(src, data_size, r_out.pcm16);
		r_out.loader_samples = walk.data_size;
	} else if (walk.bits == 16) {
		r_out.pcm16.assign(src, src + data_size);
		r_out.loader_samples = walk.data_size >> 1;
	} else {
		// IMA ADPCM: the fact chunk's count of the loader's own nibble steps, read on from the data
		// [orig: @ 0x76678a..0x7668aa], the count its record (@ 0x7667ba).
		const uint32_t count = walk.fact + 12 <= size ? io::read_u32_le(bytes + walk.fact + 8) : 0;
		r_out.pcm16 = decode_ima_adpcm_loader(bytes, size, walk.data, static_cast<int32_t>(count),
				walk.block_align);
		r_out.loader_samples = count;
	}
	r_out.channels = 1;
	r_out.loader_pitch_q16 = loader_pitch_q16(walk.rate);
	// The wave's own rate; a rate of 0, whose ratio is 0, the mixer's least step
	// [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd619..0x7bd61d].
	r_out.sample_rate = r_out.loader_pitch_q16 == 0 ? kPitchZeroRate : walk.rate;
	return true;
}

bool wav_decode_pcm16_lenient(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error) {
	r_out = WavPcm{};
	r_error.clear();
	if (bytes != nullptr && size >= 4 && tag_eq(bytes, "AUD1"))
		return decode_aud1(bytes, size, r_out, r_error);
	if (bytes == nullptr || size < 44) {
		r_error = "buffer too small to be a WAV";
		return false;
	}
	if (!tag_eq(bytes, "RIFF") || !tag_eq(bytes + 8, "WAVE")) {
		r_error = "not a RIFF/WAVE file";
		return false;
	}

	// Walk chunks starting after the 'WAVE' tag (offset 12).
	uint16_t audio_format = 0;
	uint16_t channels = 0;
	uint32_t sample_rate = 0;
	uint16_t bits_per_sample = 0;
	uint16_t block_align = 0;
	bool have_fmt = false;
	int64_t data_off = -1;
	uint32_t data_size = 0;
	bool have_fact = false;
	uint32_t fact_samples = 0;

	const int64_t total = static_cast<int64_t>(size);
	int64_t pos = 12;
	while (pos + 8 <= total) {
		const uint8_t *chunk = bytes + pos;
		const uint32_t chunk_size = io::read_u32_le(chunk + 4);
		const int64_t body = pos + 8;
		if (body + static_cast<int64_t>(chunk_size) > total) {
			// Truncated chunk; clamp the data chunk so we still play what we have.
			if (tag_eq(chunk, "data")) {
				data_off = body;
				data_size = static_cast<uint32_t>(total - body);
			}
			break;
		}
		if (tag_eq(chunk, "fmt ") && chunk_size >= 16) {
			audio_format = io::read_u16_le(chunk + 8);
			channels = io::read_u16_le(chunk + 10);
			sample_rate = io::read_u32_le(chunk + 12);
			block_align = io::read_u16_le(chunk + 20);
			bits_per_sample = io::read_u16_le(chunk + 22);
			have_fmt = true;
		} else if (tag_eq(chunk, "fact") && chunk_size >= 4) {
			have_fact = true;
			fact_samples = io::read_u32_le(chunk + 8);
		} else if (tag_eq(chunk, "data")) {
			data_off = body;
			data_size = chunk_size;
		}
		// Chunks are word-aligned (padded to even size).
		pos = body + chunk_size + (chunk_size & 1);
	}

	if (!have_fmt || data_off < 0) {
		r_error = "missing fmt/data chunk";
		return false;
	}
	if (channels < 1 || channels > 2) {
		r_error = "unsupported channel count " + std::to_string(channels);
		return false;
	}

	const uint8_t *src = bytes + data_off;
	if (audio_format == 1) {
		if (bits_per_sample == 8) {
			pcm8_to_pcm16(src, data_size, r_out.pcm16);
		} else if (bits_per_sample == 16) {
			// PCM16 is signed LE already.
			r_out.pcm16.resize(data_size);
			if (data_size > 0) {
				std::memcpy(r_out.pcm16.data(), src, data_size);
			}
		} else {
			r_error = "unsupported PCM bit depth " + std::to_string(bits_per_sample);
			return false;
		}
	} else if (audio_format == 0x11) {
		// IMA-ADPCM (NovaLogic voice / zone audio) -> signed 16-bit PCM.
		r_out.pcm16 = decode_ima_adpcm_standard(src, data_size, channels, block_align);
		if (r_out.pcm16.empty()) {
			r_error = "failed to decode IMA-ADPCM (block_align " +
					std::to_string(block_align) + ")";
			return false;
		}
	} else {
		r_error = "unsupported WAV format " + std::to_string(audio_format);
		return false;
	}

	r_out.sample_rate = sample_rate;
	r_out.channels = channels;
	// The loader's record as wav_decode_pcm16 keeps it, of the bytes present
	// where a data chunk runs past them; IMA ADPCM with no fact chunk, which
	// the loader refuses, the decoded frames.
	if (audio_format == 1) {
		r_out.loader_samples = bits_per_sample == 8 ? data_size : (data_size >> 1);
	} else {
		r_out.loader_samples = have_fact ? fact_samples
		                                 : static_cast<uint32_t>(r_out.pcm16.size() / (2u * channels));
	}
	r_out.loader_pitch_q16 = loader_pitch_q16(sample_rate);
	return true;
}

double wave_pitch_scale(uint32_t loader_pitch_q16, uint32_t sample_rate, uint32_t mix_rate, double play_scale) {
	if (mix_rate == 0) return play_scale;
	// The play factor as the channel's +4 word holds it, Q16, rounded, and the step the mixer takes from
	// it, (((play * factor) >> 16) * pitch + 0x400000) >> 23, each product 64-bit and each shrd keeping
	// 32 bits [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd5f6..0x7bd60e]. The mixer reads the word
	// unsigned (`mul` @ 0x7bd5f9); ours takes a scale of 0 or below as the word 0 and holds one past the
	// word's range at 0xFFFFFFFF. Every scale the shell composes is a word's own (lwf::pitch_from_q16 of
	// a uint32), an emitter's among them (MissionAudio::_pitch_scale reads its word unsigned).
	const double play_q16 = play_scale * 65536.0;
	const uint32_t play = !(play_scale > 0.0) ? 0u
			: play_q16 >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(play_q16 + 0.5);
	const uint32_t scaled = static_cast<uint32_t>((static_cast<uint64_t>(play) * kDeviceFactor) >> 16);
	const uint32_t step = static_cast<uint32_t>((static_cast<uint64_t>(scaled) * loader_pitch_q16 + 0x400000u) >> 23);
	// A step of 0 is forced to 1, the least step [orig: @ 0x7bd619..0x7bd61d], whatever the wave's rate:
	// a play factor of 0, a wave of pitch 0, or any product of the two under half a step, 86.13 Hz over
	// the stream's mix rate (a wave of pitch 0's stream holds the whole 86, its scale 86.13 / 86).
	if (step == 0) return kLeastStepRate / static_cast<double>(mix_rate);
	// A rate the player's mix rate holds plays at the play factor; one boxed at INT32_MAX at the play
	// factor of the rest.
	return mix_rate == sample_rate
			? play_scale
			: play_scale * (static_cast<double>(sample_rate) / static_cast<double>(mix_rate));
}

bool wav_write_pcm_mono(const uint8_t *data, size_t size, uint32_t rate, uint16_t bits,
		std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	if (bits != 8 && bits != 16) { error = "a wave is written 8 or 16 bits a sample"; return false; }
	if (size == 0 || (bits == 16 && (size & 1) != 0)) { error = "a wave holds at least one whole sample"; return false; }
	if (rate == 0) { error = "a wave's rate is above 0"; return false; }
	if (rate >= kRatioFaultRate) {
		error = "a wave's rate is under 2890137600 Hz (the game's loader faults on its pitch ratio)";
		return false;
	}
	if (size > 0xFFFFFFFFu - 64) { error = "the wave is too long for its RIFF size"; return false; }
	const uint32_t data_size = static_cast<uint32_t>(size);
	const uint16_t block = static_cast<uint16_t>(bits / 8);
	const auto tag = [&out](const char *t) { out.insert(out.end(), t, t + 4); };
	out.reserve(44 + size + (size & 1));
	tag("RIFF");
	io::append_u32_le(out, 4 + 24 + 8 + data_size + (data_size & 1));
	tag("WAVE");
	tag("fmt ");
	io::append_u32_le(out, 16);
	io::append_u16_le(out, 1); // PCM: the tag the loader leaves unread for 8 and 16 bits
	io::append_u16_le(out, 1); // mono
	io::append_u32_le(out, rate);
	io::append_u32_le(out, rate * block);
	io::append_u16_le(out, block);
	io::append_u16_le(out, bits);
	tag("data");
	io::append_u32_le(out, data_size);
	out.insert(out.end(), data, data + size);
	if (data_size & 1) out.push_back(0);
	return true;
}

}  // namespace lwf
}  // namespace opennova
