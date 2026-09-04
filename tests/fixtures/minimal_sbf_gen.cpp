// Generator + guard for the synthetic SBF test bank fixtures/sbf/synth_gamemus.sbf
// (fixtures/README.md): thirteen entries our own encoder mints from integer
// waveforms, shaped like the retail JO gamemus bank without carrying a byte
// of it. SILENCE is one partial chunk of 2216 samples (the retail NULLS
// shape: total_size 0x1008, valid 0x08A8); TONE01 spans three chunks
// (4096 + 4096 + 1000); TONE02..TONE12 are one full chunk each of distinct
// integer triangle waves (no floating point, so every platform mints the
// same bytes). The retail banks themselves are swept by
// tests/sbf/sbf_jo_install_sweep_test.cpp behind OPENNOVA_JO_DIR.
//
// Default: rebuild the bank in memory, assert it parses with the pinned
// shape and byte-matches the committed file. `--write` (re)writes it.
#include <formats/sbf/sbf.h>

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

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	out.resize(static_cast<size_t>(sz));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return true;
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

	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), "committed fixtures/sbf/synth_gamemus.sbf missing; run with --write")) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] synth_gamemus.sbf is an unpulled LFS pointer\n");
		return 0;
	}
	if (!expect(committed == bytes, "synth_gamemus.sbf differs from the encoder output; regenerate with --write"))
		return 1;
	std::printf("OK: synth_gamemus.sbf byte-reproducible (%zu bytes, %zu entries)\n", bytes.size(), kToneCount);
	return 0;
}
