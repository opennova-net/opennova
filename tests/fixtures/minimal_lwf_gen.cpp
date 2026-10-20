// Generator + guard for the synthetic sound-profile fixtures under
// fixtures/lwf/ (fixtures/README.md): menu.lwf, a three-set LWF our own
// writer mints (the two universal menu triggers MOUSE_OVER / CLICK_SELECT
// the .mnu authors reference and the V_TRUCK_ILP vehicle set the mission
// audio tests stage), and tone.wav, an 8320-sample 16-bit mono 22050 Hz
// triangle wave (integer-generated, so every platform mints the same
// bytes) the sets' members point at; stale.lwf, a bank our writer writes and
// then fills where the authoring tools leave stale memory, as the shipped
// banks hold it; and ima_adpcm.wav + ima_adpcm_512.wav, the two forms every
// shipped IMA ADPCM wave takes (tests/fixtures/ima_adpcm_source.h), encoded
// here from an integer signal by an IMA ADPCM encoder that tracks the game
// loader's own nibble step, so the loader's decode of each is the encoder's
// reconstruction sample for sample. None carries retail bytes.
//
// Default: rebuild every file in memory and byte-compare the committed files.
// `--write` (re)writes them.
#include <formats/lwf/lwf.h>

#include "common/test_paths.h"
#include "fixtures/ima_adpcm_source.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

#include "common/file_io.h"

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

// --- stale.lwf ---------------------------------------------------------------------

// A bank as the authoring tools leave one: the records our writer writes from the fields, then the
// bytes the tools leave holding their own stale memory, the four places JO:CA's banks hold it (18 of
// its 22: a single's name past its terminator in 11, a set's offset slots past its count in 15, a
// layer's +12 scratch dword where no flag 0x100 makes it the anchor in 12, a layer's member slots
// past its count in 12; the text of the tools' buffers, the scratch dword's low half alone), each
// filled here from a text of our own. No shipped bank holds stale bytes anywhere else, and none of
// another magic, trigger records or a name or path as wide as its field (docs/audio/lwf-dbf-sound-re.md).
// The fields vary as the shipped banks' do: a single's volume byte (+33, every shipped single's
// set), a set's range and pitch jitter, layers of one to three members, a set of two layers.
lwf::File make_stale_lwf() {
	lwf::File f;
	f.header.header_size = 28;
	f.header.magic = lwf::kMagic;
	f.multi_header.header_size = 20;
	// A mission's bank: its dialog lines' waves first (the seven fixtures/dbf/stale_bank.dbf's lines
	// name, as a mission's .DBF names its .LWF's [orig: Dialog_LoadAudioClip @ 0x44dcf7 ->
	// SoundBank_FindEntryByName @ 0x75bba0]), then its effects'.
	const char *const singles[] = {"Z90R100", "Z90R101", "Z90R102",   "Z90R103",   "Z90R104",
	                               "Z90R105", "Z90R106", "ENG_IDLE1", "ENG_IDLE2", "GUN_SHOT"};
	for (size_t i = 0; i < 10; ++i) {
		lwf::Single single;
		single.name = singles[i];
		single.volume = static_cast<uint8_t>(i == 9 ? 0 : 20 * (i + 1));
		single.path = std::string("SFX\\STALE\\") + singles[i] + ".wav";
		f.singles.push_back(single);
	}
	const auto member = [&f](uint32_t single, uint32_t volume, uint32_t clamp, uint32_t pitch) {
		lwf::Sndparm sp;
		sp.single_index = single;
		sp.pitch_scaled = pitch;
		sp.volume = volume;
		sp.clamp_volume = clamp;
		f.sndparms.push_back(sp);
		return static_cast<uint32_t>(f.sndparms.size() - 1);
	};
	const auto layer = [&f](uint16_t falloff, uint16_t min_distance, uint32_t flags, std::vector<uint32_t> members) {
		lwf::Playlist pl;
		pl.falloff_radius = falloff;
		pl.min_distance = min_distance;
		pl.flags = flags;
		pl.sndparm_indices = std::move(members);
		f.playlists.push_back(pl);
		return static_cast<uint32_t>(f.playlists.size() - 1);
	};
	const auto set = [&f](const char *name, uint32_t range, uint32_t jitter, std::vector<uint32_t> layers) {
		lwf::Multi m;
		m.name = name;
		m.pitch_base = lwf::kAuthoredSetPitchBase;
		m.pitch_random_range = jitter;
		m.target_id = range;
		m.playlist_indices = std::move(layers);
		f.multis.push_back(m);
	};
	set("DLG001", 0, 0, {layer(0, 0, lwf::kFlagInternal | lwf::kFlagExternal, {member(0, 255, 255, 0xFFFF)})});
	set("DLG002", 0, 0, {layer(0, 0, lwf::kFlagInternal | lwf::kFlagExternal, {member(1, 230, 255, 0xFFFF)})});
	set("ENG_IDLE", 400, 4096,
	    {layer(400, 25, lwf::kFlagExternal | lwf::kFlagLooping | lwf::kFlagRandom,
	           {member(7, 200, 255, 0xFFFF), member(8, 180, 240, 0xF000)}),
	     layer(150, 0, lwf::kFlagInternal | lwf::kFlagLooping, {member(7, 160, 200, 0xFFFF)})});
	set("GUN_SHOT", 10000, 0,
	    {layer(990, 0, 0xF, {member(9, 255, 255, 0xFFFF), member(9, 250, 255, 0x10800), member(9, 245, 255, 0xF800)})});
	return f;
}

// The tools' stale memory, one text of ours repeated: the bytes it fills look as the shipped ones do
// (text the tools' buffers held), none of them a retail byte.
constexpr char kStaleText[] = "OPENNOVA MINTED STALE TOOL MEMORY; ";

void fill_stale(std::vector<uint8_t> &bytes, size_t from, size_t to, size_t &cursor) {
	for (size_t at = from; at < to && at < bytes.size(); ++at)
		bytes[at] = static_cast<uint8_t>(kStaleText[cursor++ % (sizeof(kStaleText) - 1)]);
}

uint32_t read_u32(const std::vector<uint8_t> &bytes, size_t at) {
	uint32_t v = 0;
	std::memcpy(&v, bytes.data() + at, 4);
	return v;
}

// The four stale places over the writer's bytes of `f`: every single's name past its terminator, every
// set's slots past its count, every layer's +12 (its low half) and its member slots past its count.
std::vector<uint8_t> with_stale_bytes(const lwf::File &f, std::vector<uint8_t> bytes) {
	size_t cursor = 0;
	for (size_t i = 0; i < f.singles.size(); ++i) {
		const size_t at = 28 + i * 52;
		fill_stale(bytes, at + f.singles[i].name.size() + 1, at + 32, cursor);
	}
	const uint32_t multi_table = read_u32(bytes, read_u32(bytes, 16) + 8);
	for (size_t i = 0; i < f.multis.size(); ++i) {
		const size_t at = multi_table + i * 80;
		fill_stale(bytes, at + 40 + f.multis[i].playlist_indices.size() * 4, at + 72, cursor);
	}
	const size_t layers = multi_table + f.multis.size() * 80;
	for (size_t i = 0; i < f.playlists.size(); ++i) {
		const size_t at = layers + i * 48;
		fill_stale(bytes, at + 12, at + 14, cursor);
		fill_stale(bytes, at + 16 + f.playlists[i].sndparm_indices.size() * 4, at + 48, cursor);
	}
	return bytes;
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

// --- ima_adpcm.wav, ima_adpcm_512.wav ------------------------------------------------------------

// The IMA step table and a nibble's index move (the standard IMA tables the game's loader holds), and
// the loader's step of a nibble n: the sample moved by (step * 0x2000 * (2 (n & 7) + 1)) >> 16, floored,
// negative for n & 8, held to int16; the index moved and held to 0..88 [orig:
// Audio_AdpcmDecodeNibbleStep @ 0x7bf250]. The encoder picks, sample by sample, the nibble whose step
// lands nearest the source (the lower nibble on a tie), so the loader's decode of the bytes is the
// encoder's own track of them.
constexpr int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
constexpr int kImaIndexMove[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

void ima_step(int nibble, int &sample, int &index) {
	const int64_t scale = int64_t(0x2000) * (2 * (nibble & 7) + 1) * ((nibble & 8) ? -1 : 1);
	// The 64-bit product shifted right, floored, as the loader's shrd.
	const int64_t product = int64_t(kImaStep[index]) * scale;
	const int64_t moved = product >= 0 ? product / 65536 : -((-product + 65535) / 65536);
	sample = int(std::max<int64_t>(-32768, std::min<int64_t>(32767, sample + moved)));
	index = std::max(0, std::min(88, index + kImaIndexMove[nibble & 7]));
}

// A form's RIFF WAVE: RIFF, a 20-byte fmt (tag 0x11, mono, the rate, the byte rate, the block, 4 bits,
// cbSize 2 and the samples a block holds), a fact of the count, the data's whole blocks. Each block a
// header (its first sample whole, the running step index, a zero byte), then its nibbles, low first;
// the first block's index 0; the bytes past the count zero.
std::vector<uint8_t> make_ima_wav(const ima_fixture::Form &form) {
	std::vector<uint8_t> data;
	int index = 0;
	uint32_t i = 0;
	for (uint32_t block = 0; block < form.blocks; ++block) {
		const size_t start = data.size();
		data.resize(start + form.block_align, 0);
		if (i >= form.samples) continue;
		int sample = ima_fixture::source_sample(i++, form.rate);
		data[start] = static_cast<uint8_t>(sample);
		data[start + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8);
		data[start + 2] = static_cast<uint8_t>(index);
		for (uint32_t k = 0; k + 1 < form.samples_per_block && i < form.samples; ++k, ++i) {
			const int target = ima_fixture::source_sample(i, form.rate);
			int best = 0, best_error = 1 << 30;
			for (int n = 0; n < 16; ++n) {
				int s = sample, x = index;
				ima_step(n, s, x);
				const int error = s > target ? s - target : target - s;
				if (error < best_error) {
					best = n;
					best_error = error;
				}
			}
			ima_step(best, sample, index);
			data[start + 4 + k / 2] |= static_cast<uint8_t>(best << (4 * (k % 2)));
		}
	}
	std::vector<uint8_t> out;
	const auto text = [&out](const char *t) { out.insert(out.end(), t, t + 4); };
	text("RIFF");
	put_u32(out, static_cast<uint32_t>(4 + (8 + 20) + (8 + 4) + 8 + data.size()));
	text("WAVE");
	text("fmt ");
	put_u32(out, 20);
	put_u16(out, 0x11);  // IMA ADPCM
	put_u16(out, 1);     // mono
	put_u32(out, form.rate);
	put_u32(out, form.byte_rate);
	put_u16(out, form.block_align);
	put_u16(out, 4);     // bits per sample
	put_u16(out, 2);     // cbSize
	put_u16(out, form.samples_per_block);
	text("fact");
	put_u32(out, 4);
	put_u32(out, form.samples);
	text("data");
	put_u32(out, static_cast<uint32_t>(data.size()));
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

// --- guard -----------------------------------------------------------------------------

using test_io::read_file;

int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), (path + " missing; run with --write").c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
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

	// stale.lwf: the writer's bytes of the bank, then the tools' stale memory; read, it is the bank, and
	// written, the writer's bytes again.
	const lwf::File stale_bank = make_stale_lwf();
	std::vector<uint8_t> stale_clean;
	if (!expect(lwf::encode_lwf(stale_bank, stale_clean, err), ("encode_lwf stale: " + err).c_str())) return 1;
	const std::vector<uint8_t> stale = with_stale_bytes(stale_bank, stale_clean);
	lwf::File stale_read;
	std::vector<uint8_t> stale_written;
	if (!expect(stale != stale_clean && lwf::parse_lwf_buffer(stale.data(), stale.size(), stale_read, err) &&
	                    lwf::encode_lwf(stale_read, stale_written, err) && stale_written == stale_clean,
	            "stale.lwf reads as its bank and writes back as the writer's bytes"))
		return 1;

	int failures = guard(dir + "/menu.lwf", lwf_bytes, write_mode);
	failures += guard(dir + "/tone.wav", make_tone_wav(), write_mode);
	failures += guard(dir + "/stale.lwf", stale, write_mode);
	for (const ima_fixture::Form &form : ima_fixture::kForms) {
		const std::vector<uint8_t> wave = make_ima_wav(form);
		if (!expect(wave.size() == 60 + size_t(form.blocks) * form.block_align &&
		                    uint32_t(form.samples_per_block) == 1 + 2 * (uint32_t(form.block_align) - 4) &&
		                    form.byte_rate == form.rate * form.block_align / form.samples_per_block &&
		                    form.samples <= form.blocks * uint32_t(form.samples_per_block) &&
		                    form.samples > (form.blocks - 1) * uint32_t(form.samples_per_block),
		            (std::string(form.file) + ": whole blocks, the count inside the last").c_str()))
			return 1;
		failures += guard(dir + "/" + form.file, wave, write_mode);
	}
	if (failures == 0 && !write_mode)
		std::printf("OK: fixtures/lwf/menu.lwf, tone.wav, stale.lwf and the IMA ADPCM waves byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
