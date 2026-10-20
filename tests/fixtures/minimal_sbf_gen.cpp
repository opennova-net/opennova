// Generator + guard for the synthetic SBF test bank fixtures/sbf/synth_gamemus.sbf
// (fixtures/README.md): thirteen entries our own encoder mints from integer
// waveforms, shaped like the retail JO gamemus bank without carrying a byte
// of it. SILENCE is one partial chunk of 2216 samples (the retail NULLS
// shape: total_size 0x1008, valid 0x08A8); TONE01 spans three chunks
// (4096 + 4096 + 1000); TONE02..TONE12 are one full chunk each of distinct
// integer triangle waves (no floating point, so every platform mints the
// same bytes).
//
// And fixtures/sbf/tool_bank.sbf, a bank our writer writes from the model and
// then left as the retail encoder leaves every shipped bank (each a header the
// writer's, its streams packed): one reserved pair in every chunk header, not
// the writer's (menumus.sbf's 0x1A, 0x01; one pair a bank: gamemus.sbf 0xFA, 0x00,
// the jox01 banks 0x20, 0x01 and 0xFB, 0x00), and each stream's last chunk
// partial, its audio area past the bytes it plays the encoder's reused buffer:
// the previous chunk's bytes there, a one-chunk stream's zero (every stream of
// JO:CA's six banks so; docs/audio/mus-sbf-re.md). The retail banks themselves
// are swept by tests/sbf/sbf_bank_sweep_test.cpp's forms of them.
//
// Default: rebuild the banks in memory, assert they parse with the pinned
// shape and byte-match the committed files. `--write` (re)writes them.
#include <formats/sbf/sbf.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::sbf;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

struct Tone {
	const char *name;
	size_t samples;
	int period;     // triangle period in samples (0 = silence)
	int amplitude;  // peak int16 value
};

// The bank's authored contents. Periods and amplitudes differ per entry so
// every chunk carries a distinct scale/byte pattern.
const Tone kTones[] = {
    {"SILENCE", 2216, 0, 0},
    {"TONE01", 9192, 50, 24000},
    {"TONE02", 4096, 25, 20000},
    {"TONE03", 4096, 32, 16000},
    {"TONE04", 4096, 40, 12000},
    {"TONE05", 4096, 64, 8000},
    {"TONE06", 4096, 80, 6000},
    {"TONE07", 4096, 100, 4000},
    {"TONE08", 4096, 128, 3000},
    {"TONE09", 4096, 160, 2000},
    {"TONE10", 4096, 200, 1500},
    {"TONE11", 4096, 256, 1000},
    {"TONE12", 4096, 320, 500},
};
constexpr size_t kToneCount = sizeof(kTones) / sizeof(kTones[0]);

// Integer triangle wave: rises from -amplitude to +amplitude over the first
// half period and falls back over the second.
std::vector<int16_t> waveform(const Tone &tone) {
	std::vector<int16_t> out(tone.samples, 0);
	if (tone.period <= 0 || tone.amplitude <= 0) return out;
	const int half = tone.period / 2;
	for (size_t i = 0; i < tone.samples; ++i) {
		const int phase = static_cast<int>(i % static_cast<size_t>(tone.period));
		int value;
		if (phase <= half)
			value = -tone.amplitude + (2 * tone.amplitude * phase) / half;
		else
			value = tone.amplitude - (2 * tone.amplitude * (phase - half)) / half;
		out[i] = static_cast<int16_t>(value);
	}
	return out;
}

bool build(std::vector<uint8_t> &bytes) {
	std::vector<std::vector<int16_t>> pcm;
	pcm.reserve(kToneCount);
	const char *names[kToneCount];
	const int16_t *samples[kToneCount];
	size_t counts[kToneCount];
	for (size_t i = 0; i < kToneCount; ++i) {
		pcm.push_back(waveform(kTones[i]));
		names[i] = kTones[i].name;
		samples[i] = pcm.back().data();
		counts[i] = pcm.back().size();
	}
	uint8_t *buf = nullptr;
	size_t size = 0;
	if (sbf_encode_file(names, static_cast<uint32_t>(kToneCount), samples, counts, &buf, &size) != 0)
		return false;
	bytes.assign(buf, buf + size);
	sbf_free(buf);
	return true;
}

// The shape every consumer test pins.
bool verify_shape(const std::vector<uint8_t> &bytes) {
	SbfArchive arc;
	if (!expect(sbf_open_memory(&arc, bytes.data(), bytes.size()) == 0, "the minted bank parses")) return false;
	bool ok = true;
	ok &= expect(arc.header.entry_count == kToneCount, "13 entries");
	ok &= expect(arc.header.flags == SBF_FLAGS_BYTE_PAIRED_STEREO, "byte-paired stereo flag");
	const SbfRawEntry *silence = sbf_find_by_name(&arc, "SILENCE");
	ok &= expect(silence != nullptr && silence == &arc.entries[0], "SILENCE is entry 0");
	if (silence != nullptr) {
		ok &= expect(silence->total_size == SBF_CHUNK_TOTAL, "SILENCE is one chunk (0x1008)");
		uint32_t valid = 0;
		std::memcpy(&valid, bytes.data() + silence->data_offset, sizeof(valid));
		ok &= expect(valid == 2216, "SILENCE chunk carries 2216 valid samples (0x08A8)");
	}
	const SbfRawEntry *tone01 = sbf_find_by_name(&arc, "TONE01");
	ok &= expect(tone01 != nullptr && tone01->total_size == 3 * SBF_CHUNK_TOTAL, "TONE01 spans three chunks");
	for (uint32_t i = 2; i < kToneCount; ++i)
		ok &= expect(arc.entries[i].total_size == SBF_CHUNK_TOTAL, "TONE02..TONE12 are one full chunk each");
	sbf_close(&arc);
	return ok;
}

// The tool bank's streams: a one-chunk silence of the NULLS shape (2216 bytes), and three streams of
// two to four chunks, each ending in a partial one.
const Tone kToolTones[] = {
    {"SILENCE", 2216, 0, 0},
    {"TOOL_A", 2 * 4096 + 1500, 50, 18000},
    {"TOOL_B", 4096 + 333, 36, 9000},
    {"TOOL_C", 3 * 4096 + 4000, 120, 3000},
};
constexpr size_t kToolToneCount = sizeof(kToolTones) / sizeof(kToolTones[0]);
// menumus.sbf's reserved pair.
constexpr uint8_t kToolReservedA = 0x1A;
constexpr uint8_t kToolReservedB = 0x01;

uint32_t read_u32(const std::vector<uint8_t> &bytes, size_t at) {
	uint32_t v = 0;
	std::memcpy(&v, bytes.data() + at, 4);
	return v;
}

// The model's write, then the retail encoder's leavings: its reserved pair in every chunk, each
// stream's last chunk's tail the previous chunk's bytes there (zero for a stream of one chunk).
bool build_tool_bank(std::vector<uint8_t> &bytes) {
	SbfFile bank;
	for (const Tone &tone : kToolTones) {
		const std::vector<int16_t> pcm = waveform(tone);
		bank.streams.push_back(sbf_encode_stream(tone.name, pcm.data(), pcm.size()));
	}
	std::string error;
	if (!sbf_write_bank(bank, bytes, error)) return false;
	for (size_t i = 0; i < bank.streams.size(); ++i) {
		const size_t entry = size_t(SBF_HEADER_SIZE) + i * size_t(SBF_ENTRY_SIZE);
		const uint32_t offset = read_u32(bytes, entry + 16), total = read_u32(bytes, entry + 20),
		               block = read_u32(bytes, entry + 24);
		const uint32_t chunks = total / block;
		for (uint32_t c = 0; c < chunks; ++c) {
			bytes[offset + c * block + 6] = kToolReservedA;
			bytes[offset + c * block + 7] = kToolReservedB;
		}
		const size_t last = offset + size_t(chunks - 1) * block;
		const uint32_t valid = read_u32(bytes, last);
		for (size_t k = SBF_CHUNK_HEADER + valid; k < block; ++k)
			bytes[last + k] = chunks > 1 ? bytes[last - block + k] : 0;
	}
	return true;
}

// The tool bank reads into the model as its streams, laid out as the writer lays a bank out but for
// the leavings, and its write is the writer's bytes again.
bool verify_tool_bank(const std::vector<uint8_t> &bytes) {
	SbfFile bank;
	SbfFileLayout layout;
	std::string error;
	if (!expect(sbf_read_bank(bytes.data(), bytes.size(), bank, error, &layout), "the tool bank reads")) return false;
	size_t chunks = 0;
	for (const SbfStream &stream : bank.streams) chunks += stream.chunks.size();
	bool ok = expect(bank.streams.size() == kToolToneCount && layout.packed && layout.trailing_bytes == 0 &&
	                         layout.short_chunks == 0 && layout.header_words_other == 0 &&
	                         layout.reserved_other == chunks && layout.tails_other == kToolToneCount,
	                 "the tool bank's reserved pairs and tails are the encoder's, the rest the writer's");
	std::vector<uint8_t> written, again;
	ok &= expect(sbf_write_bank(bank, written, error) && written != bytes && written.size() == bytes.size(),
	             "the tool bank's write is the writer's own bytes, its size");
	SbfFile back;
	ok &= expect(sbf_read_bank(written.data(), written.size(), back, error) && sbf_write_bank(back, again, error) &&
	                     again == written,
	             "the tool bank's write is a fixed point");
	return ok;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/sbf/synth_gamemus.sbf";

	std::vector<uint8_t> bytes;
	if (!expect(build(bytes), "sbf_encode_file")) return 1;
	if (!verify_shape(bytes)) return 1;
	const std::string tool_path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/sbf/tool_bank.sbf";
	std::vector<uint8_t> tool;
	if (!expect(build_tool_bank(tool), "sbf_write_bank (the tool bank)")) return 1;
	if (!verify_tool_bank(tool)) return 1;

	if (write_mode) {
		for (const auto &file : {std::make_pair(path, &bytes), std::make_pair(tool_path, &tool)}) {
			std::ofstream o(file.first, std::ios::binary);
			o.write(reinterpret_cast<const char *>(file.second->data()), static_cast<std::streamsize>(file.second->size()));
			std::printf("wrote %s (%zu bytes)\n", file.first.c_str(), file.second->size());
		}
		return 0;
	}
	std::vector<uint8_t> committed_tool;
	if (!expect(test_io::read_file(tool_path, committed_tool), "committed fixtures/sbf/tool_bank.sbf missing; run with --write"))
		return 1;
	if (!test_io::is_lfs_pointer(committed_tool) &&
	    !expect(committed_tool == tool, "tool_bank.sbf differs from the generator output; regenerate with --write"))
		return 1;
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), "committed fixtures/sbf/synth_gamemus.sbf missing; run with --write")) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] synth_gamemus.sbf is an unpulled LFS pointer\n");
		return 0;
	}
	if (!expect(committed == bytes, "synth_gamemus.sbf differs from the encoder output; regenerate with --write"))
		return 1;
	std::printf("OK: synth_gamemus.sbf byte-reproducible (%zu bytes, %zu entries)\n", bytes.size(), kToneCount);
	return 0;
}
