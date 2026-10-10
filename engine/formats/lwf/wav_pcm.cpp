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

// The loader's own form, an AUD1 buffer (bytes 41 55 44 31), which it copies as it is, unchecked
// [orig: Audio_LoadWavFileFromArchive @ 0x766480, the magic @ 0x7664e2, the copy @ 0x7664e9..0x766511]:
// the header its RIFF decodes write (@ 0x766603..0x76663c, @ 0x766708..0x766745,
// @ 0x7667b4..0x7667f0), read as the mixer's channel set-up reads it, the sample count at +4
// [orig: sub_7BD671 @ 0x7bd67f], the pitch ratio to the 44100 Hz device in Q16 at +8
// [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd603] and the samples from +16, signed (the
// RIFF form's 8-bit bias already taken off), 16-bit where the byte at +12 is 2 and 8-bit for any
// other byte there [orig: sub_7BD671 @ 0x7bd692]. The byte at +13, which the mixer shifts the
// channel's volume by, is not read (D-SND-46). The copy is the bytes' own size; where they end inside
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
	// The rate the ratio is nearest, for the shell's player (the mixer steps by the ratio itself): the
	// inverse the dialog line's hold takes, (pitch * 44100 + 0x8000) >> 16 [orig: Dialog_LoadAudioClip
	// @ 0x44dd8e..0x44dd9f, a signed imul; ours unsigned, the same below a pitch of 2^31].
	out.sample_rate = static_cast<uint32_t>((uint64_t(pitch_q16) * 44100 + 0x8000) >> 16);
	out.channels = 1;
	out.loader_samples = samples;
	out.loader_pitch_q16 = pitch_q16;
	return true;
}

// --- WAV IMA-ADPCM (audioFormat 0x11) decode to signed 16-bit PCM ---
// NovaLogic stores voice/zone audio as 4-bit IMA-ADPCM (mono, block-based). This
// is the standard Microsoft/IMA scheme: each block begins with a per-channel
// header (int16 predictor + uint8 step index), then 4-bit nibbles decoded via the
// step/index tables. Stereo (defensive) interleaves 4-byte (8-nibble) words per
// channel after the headers.
const int IMA_STEP_TABLE[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
const int IMA_INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

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

// Returns interleaved signed-16-bit-LE PCM, or empty on a malformed stream.
std::vector<uint8_t> decode_ima_adpcm(const uint8_t *data, uint32_t size,
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
		r_error = std::string("the game's wave loader refuses it: ") + refusal_words(walk.refusal);
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
		r_out.pcm16 = decode_ima_adpcm(src, data_size, 1, walk.block_align);
		if (r_out.pcm16.empty()) {
			r_error = "failed to decode IMA-ADPCM (block_align " +
					std::to_string(walk.block_align) + ")";
			return false;
		}
		r_out.loader_samples = walk.fact + 12 <= size ? io::read_u32_le(bytes + walk.fact + 8) : 0;
	}
	r_out.sample_rate = walk.rate;
	r_out.channels = 1;
	r_out.loader_pitch_q16 = loader_pitch_q16(walk.rate);
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
		r_out.pcm16 = decode_ima_adpcm(src, data_size, channels, block_align);
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

bool wav_write_pcm_mono(const uint8_t *data, size_t size, uint32_t rate, uint16_t bits,
		std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	if (bits != 8 && bits != 16) { error = "a wave is written 8 or 16 bits a sample"; return false; }
	if (size == 0 || (bits == 16 && (size & 1) != 0)) { error = "a wave holds at least one whole sample"; return false; }
	if (rate == 0) { error = "a wave's rate is above 0"; return false; }
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
