// Generator + guard for the synthetic sound-profile fixtures under
// fixtures/lwf/ (fixtures/README.md): menu.lwf, a three-set LWF our own
// writer mints (the two universal menu triggers MOUSE_OVER / CLICK_SELECT
// the .mnu authors reference and the V_TRUCK_ILP vehicle set the mission
// audio tests stage), and tone.wav, an 8320-sample 16-bit mono 22050 Hz
// triangle wave (integer-generated, so every platform mints the same
// bytes) the sets' members point at. Neither carries retail bytes; the
// retail profiles are swept by tests/lwf/lwf_jo_assets_sweep_test.cpp
// behind OPENNOVA_JO_ASSETS.
//
// Default: rebuild both in memory and byte-compare the committed files.
// `--write` (re)writes them.
#include <formats/lwf/lwf.h>

#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

constexpr uint32_t kSampleRate = 22050;
constexpr uint32_t kToneSamples = 8320;
constexpr int kTonePeriod = 50;      // 441 Hz at 22050 Hz
constexpr int kToneAmplitude = 12000;

// --- menu.lwf --------------------------------------------------------------------

struct SetSpec {
	const char *set_name;
	const char *single_name;
	const char *wav_path;
	uint16_t falloff_radius;
};

// Each set owns one layer with one member over its own single. The menu
// sets carry the retail wav basenames the menu_lwf test resolves by prefix
// (MSOVR*, SELECT*); the vehicle set names the minted tone.wav.
const SetSpec kSets[] = {
    {"MOUSE_OVER", "MSOVR_2", "SFX\\MENU\\MSOVR_2.wav", 0},
    {"CLICK_SELECT", "SELECTA1", "SFX\\MENU\\SELECTA1.wav", 0},
    {"V_TRUCK_ILP", "TONE", "tone.wav", 2000},
};
constexpr size_t kSetCount = sizeof(kSets) / sizeof(kSets[0]);

lwf::File make_menu_lwf() {
	lwf::File f;
	f.header.header_size = 28;
	f.header.magic = lwf::kMagic;
	f.multi_header.header_size = 20;
	for (size_t i = 0; i < kSetCount; ++i) {
		lwf::Single single;
		single.name = kSets[i].single_name;
		single.path = kSets[i].wav_path;
		f.singles.push_back(single);

		lwf::Sndparm member;
		member.single_index = static_cast<uint32_t>(i);
		member.pitch_scaled = lwf::kPitchUnityQ16;
		member.random_pitch_scaled = 0;
		member.volume = 255;
		member.clamp_volume = 255;
		f.sndparms.push_back(member);

		lwf::Playlist layer;
		layer.falloff_radius = kSets[i].falloff_radius;
		layer.min_distance = 0;
		layer.flags = lwf::kFlagInternal | lwf::kFlagExternal;
		layer.sndparm_indices.push_back(static_cast<uint32_t>(i));
		f.playlists.push_back(layer);

		lwf::Multi set;
		set.name = kSets[i].set_name;
		set.pitch_base = lwf::kAuthoredSetPitchBase;
		set.pitch_random_range = 0;
		set.playlist_indices.push_back(static_cast<uint32_t>(i));
		f.multis.push_back(set);
	}
	return f;
}

// --- tone.wav ----------------------------------------------------------------------

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u16(std::vector<uint8_t> &out, uint16_t v) {
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}

std::vector<uint8_t> make_tone_wav() {
	std::vector<uint8_t> out;
	const uint32_t data_bytes = kToneSamples * 2;
	out.insert(out.end(), {'R', 'I', 'F', 'F'});
	put_u32(out, 36 + data_bytes);
	out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	put_u32(out, 16);          // fmt chunk size
	put_u16(out, 1);           // PCM
	put_u16(out, 1);           // mono
	put_u32(out, kSampleRate);
	put_u32(out, kSampleRate * 2);  // byte rate
	put_u16(out, 2);           // block align
	put_u16(out, 16);          // bits per sample
	out.insert(out.end(), {'d', 'a', 't', 'a'});
	put_u32(out, data_bytes);
	const int half = kTonePeriod / 2;
	for (uint32_t i = 0; i < kToneSamples; ++i) {
		const int phase = static_cast<int>(i % kTonePeriod);
		const int value = phase <= half
				? -kToneAmplitude + (2 * kToneAmplitude * phase) / half
				: kToneAmplitude - (2 * kToneAmplitude * (phase - half)) / half;
		put_u16(out, static_cast<uint16_t>(static_cast<int16_t>(value)));
	}
	return out;
}

// --- guard -----------------------------------------------------------------------------

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

int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), (path + " missing; run with --write").c_str())) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, (path + " differs from the generator output; regenerate with --write").c_str()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/lwf";

	std::vector<uint8_t> lwf_bytes;
	std::string err;
	if (!expect(lwf::encode_lwf(make_menu_lwf(), lwf_bytes, err), ("encode_lwf: " + err).c_str())) return 1;
	// The writer's output parses back to the authored shape and re-encodes byte-stable.
	lwf::File parsed;
	if (!expect(lwf::parse_lwf_buffer(lwf_bytes.data(), lwf_bytes.size(), parsed, err), ("parse_lwf: " + err).c_str())) return 1;
	if (!expect(parsed.singles.size() == kSetCount && parsed.multis.size() == kSetCount &&
	                    parsed.playlists.size() == kSetCount && parsed.sndparms.size() == kSetCount,
	            "menu.lwf carries three singles/sets/layers/members"))
		return 1;
	std::vector<uint8_t> again;
	if (!expect(lwf::encode_lwf(parsed, again, err) && again == lwf_bytes, "menu.lwf is byte-stable across parse->encode")) return 1;

	int failures = guard(dir + "/menu.lwf", lwf_bytes, write_mode);
	failures += guard(dir + "/tone.wav", make_tone_wav(), write_mode);
	if (failures == 0 && !write_mode)
		std::printf("OK: fixtures/lwf/menu.lwf + tone.wav byte-reproducible (%zu + %zu bytes)\n",
		            lwf_bytes.size(), make_tone_wav().size());
	return failures == 0 ? 0 : 1;
}
