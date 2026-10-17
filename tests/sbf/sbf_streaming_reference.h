#pragma once

// The PCM the Godot player's streaming playback (SbfAudioStreamPlayback, deleted with C-164 when the
// player started handing the mixer a built-in AudioStreamWAV of sbf_decode_entry's PCM) produced for
// one entry, its chunk walk restated: chunk c read as one block_size block at c * block_size while that
// block lies inside the entry, decoded by sbf_decode_chunk, its frames played while both of a frame's
// samples lie inside the chunk's decoded count; the stream moved to the next chunk after a full chunk's
// SBF_CHUNK_AUDIO / 2 frames and stopped at the first frame past a shorter chunk's count, a chunk that
// did not decode, or the entry's end. The ctests hold sbf_decode_entry to it.

#include <formats/sbf/sbf.h>

#include <cstddef>
#include <cstdint>
#include <vector>

inline std::vector<int16_t> sbf_streaming_reference(const uint8_t *entry, size_t size, uint32_t block_size) {
	using namespace opennova::sbf;
	std::vector<int16_t> out;
	if (block_size == 0) return out;
	constexpr uint32_t kFramesPerChunk = SBF_CHUNK_AUDIO / 2;
	int16_t decoded[SBF_CHUNK_AUDIO];
	for (uint64_t chunk = 0;; ++chunk) {
		const uint64_t at = chunk * block_size;
		if (at >= size || size - at < block_size) return out;
		const int n = sbf_decode_chunk(entry + at, block_size, decoded, SBF_CHUNK_AUDIO);
		if (n < 0) return out;
		for (uint32_t frame = 0; frame < kFramesPerChunk; ++frame) {
			if (frame * 2u + 1u >= static_cast<uint32_t>(n)) return out;
			out.push_back(decoded[frame * 2u]);
			out.push_back(decoded[frame * 2u + 1u]);
		}
	}
}
