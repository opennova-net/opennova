#include <formats/lwf/wav_pcm.h>
#include <base/io/le.h>

#include <cstring>

namespace opennova {
namespace lwf {

namespace {

bool tag_eq(const uint8_t *p, const char *tag) {
	return p[0] == static_cast<uint8_t>(tag[0]) && p[1] == static_cast<uint8_t>(tag[1]) &&
			p[2] == static_cast<uint8_t>(tag[2]) && p[3] == static_cast<uint8_t>(tag[3]);
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

bool wav_decode_pcm16(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error) {
	r_out = WavPcm{};
	r_error.clear();
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
			// WAV PCM8 is UNSIGNED (128 = center). Upconvert to signed 16-bit LE.
			r_out.pcm16.resize(static_cast<size_t>(data_size) * 2);
			for (uint32_t i = 0; i < data_size; ++i) {
				const int16_t s = static_cast<int16_t>((static_cast<int>(src[i]) - 128) << 8);
				r_out.pcm16[i * 2] = static_cast<uint8_t>(s & 0xFF);
				r_out.pcm16[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
			}
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
	return true;
}

}  // namespace lwf
}  // namespace opennova
