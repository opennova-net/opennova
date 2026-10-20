// The minted IMA ADPCM waves (fixtures/lwf/ima_adpcm.wav, fixtures/lwf/ima_adpcm_512.wav; written by
// tests/fixtures/minimal_lwf_gen.cpp) and the integer signal each encodes, shared with the tests that
// check a decode against it. The two forms are the two every shipped IMA ADPCM wave takes (JO:CA's 1907,
// D-SND-45): mono 4-bit tag 0x11, a 20-byte fmt (cbSize 2, then the samples a block holds), a fact chunk
// before the data, whole blocks, the first block's step index 0 and each header's fourth byte 0, the
// byte rate rate * block_align / samples_per_block floored; 11025 Hz in 256-byte blocks of 505 samples
// (1868 of them) or 22050 Hz in 512-byte blocks of 1017 (39); the fact count short of the blocks'
// capacity, the last block's bytes past it zero (1895), or the capacity exactly (12).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ima_fixture {

struct Form {
	const char *file;
	uint32_t rate;
	uint16_t block_align;
	uint16_t samples_per_block;  // 1 + 2 * (block_align - 4): the header's sample, two per byte
	uint32_t byte_rate;          // rate * block_align / samples_per_block, floored
	uint32_t samples;            // the fact count
	uint32_t blocks;
};

inline constexpr Form kForms[] = {
	// The fact count short of the three blocks' 1515: the last block holds 290 samples, its 107 bytes
	// past them zero.
	{"ima_adpcm.wav", 11025, 256, 505, 5588, 1300, 3},
	// The count the two blocks' capacity.
	{"ima_adpcm_512.wav", 22050, 512, 1017, 11100, 2034, 2},
};
inline constexpr size_t kFormCount = sizeof(kForms) / sizeof(kForms[0]);

// A 441 Hz triangle of amplitude 9000 plus a 7-sample one of 1500, integers alone (every platform
// mints the same bytes): sample `i` of a wave at `rate`.
inline int16_t source_sample(uint32_t i, uint32_t rate) {
	const auto triangle = [](uint32_t at, uint32_t period, int amplitude) {
		const int phase = static_cast<int>(at % period);
		const int half = static_cast<int>(period / 2);
		return phase <= half ? -amplitude + (2 * amplitude * phase) / half
		                     : amplitude - (2 * amplitude * (phase - half)) / (static_cast<int>(period) - half);
	};
	return static_cast<int16_t>(triangle(i, rate / 441, 9000) + triangle(i, 7, 1500));
}

} // namespace ima_fixture
