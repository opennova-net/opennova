// lwf::wav_decode_pcm16: the game's wave loader's walk and its refusals
// (D-SND-33 / D-SND-43), the PCM8/PCM16 normalizations, and the IMA-ADPCM
// block decode (hand-computed against the IMA step/index tables), moved from
// the shell adapter's WavLoader; and the sample count and pitch ratio the
// game's wave loader records (a dialog line's hold reads them).
#include <formats/lwf/wav_pcm.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void push_u16(std::vector<uint8_t> &v, uint16_t x) {
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>(x >> 8));
}

void push_u32(std::vector<uint8_t> &v, uint32_t x) {
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void push_tag(std::vector<uint8_t> &v, const char *tag) {
	v.insert(v.end(), tag, tag + 4);
}

// Assemble RIFF/WAVE with one fmt chunk and one data chunk.
std::vector<uint8_t> make_wav(uint16_t format, uint16_t channels,
		uint32_t rate, uint16_t block_align, uint16_t bits,
		const std::vector<uint8_t> &data, int32_t declared_data_size = -1) {
	std::vector<uint8_t> v;
	push_tag(v, "RIFF");
	push_u32(v, 0);  // size backfilled below
	push_tag(v, "WAVE");
	push_tag(v, "fmt ");
	push_u32(v, 16);
	push_u16(v, format);
	push_u16(v, channels);
	push_u32(v, rate);
	push_u32(v, rate * block_align);  // byte rate (unread)
	push_u16(v, block_align);
	push_u16(v, bits);
	push_tag(v, "data");
	push_u32(v, declared_data_size >= 0 ? static_cast<uint32_t>(declared_data_size)
	                                    : static_cast<uint32_t>(data.size()));
	v.insert(v.end(), data.begin(), data.end());
	const uint32_t riff_size = static_cast<uint32_t>(v.size() - 8);
	v[4] = static_cast<uint8_t>(riff_size & 0xFF);
	v[5] = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
	v[6] = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
	v[7] = static_cast<uint8_t>((riff_size >> 24) & 0xFF);
	return v;
}

// The same wave with a `fact` chunk of `samples` ahead of its data (after the
// 16-byte fmt chunk).
std::vector<uint8_t> with_fact(std::vector<uint8_t> wav, uint32_t samples) {
	std::vector<uint8_t> fact;
	push_tag(fact, "fact");
	push_u32(fact, 4);
	push_u32(fact, samples);
	wav.insert(wav.begin() + 36, fact.begin(), fact.end());
	const uint32_t riff_size = static_cast<uint32_t>(wav.size() - 8);
	wav[4] = static_cast<uint8_t>(riff_size & 0xFF);
	wav[5] = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
	wav[6] = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
	wav[7] = static_cast<uint8_t>((riff_size >> 24) & 0xFF);
	return wav;
}

// The same wave with a chunk `tag` of `body` put at `at` (36: after the 16-byte
// fmt chunk; its size: the end), followed by a pad byte where `pad` and the
// body's size is odd.
std::vector<uint8_t> with_chunk(std::vector<uint8_t> wav, size_t at, const char *tag,
		const std::vector<uint8_t> &body, bool pad = false) {
	std::vector<uint8_t> chunk;
	push_tag(chunk, tag);
	push_u32(chunk, static_cast<uint32_t>(body.size()));
	chunk.insert(chunk.end(), body.begin(), body.end());
	if (pad && (body.size() & 1)) chunk.push_back(0);
	wav.insert(wav.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
	const uint32_t riff_size = static_cast<uint32_t>(wav.size() - 8);
	wav[4] = static_cast<uint8_t>(riff_size & 0xFF);
	wav[5] = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
	wav[6] = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
	wav[7] = static_cast<uint8_t>((riff_size >> 24) & 0xFF);
	return wav;
}

std::vector<uint8_t> text_body(const char *text, size_t size) {
	return std::vector<uint8_t>(text, text + size);
}

int16_t sample_at(const opennova::lwf::WavPcm &pcm, size_t frame) {
	return static_cast<int16_t>(pcm.pcm16[frame * 2] |
			(pcm.pcm16[frame * 2 + 1] << 8));
}

}  // namespace

int main() {
	using opennova::lwf::WavPcm;
	using opennova::lwf::wav_decode_pcm16;

	std::string error;

	// PCM16 passes through byte-for-byte.
	{
		std::vector<uint8_t> data;
		push_u16(data, 0x1234);
		push_u16(data, 0xFEDC);
		const std::vector<uint8_t> wav = make_wav(1, 1, 22050, 2, 16, data);
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error),
				"PCM16 decodes")) return 1;
		if (!expect(out.sample_rate == 22050 && out.channels == 1,
				"PCM16 rate/channels")) return 1;
		if (!expect(out.pcm16 == data, "PCM16 is a passthrough")) return 1;
		// The loader's record: half the data's bytes, ((22050 << 16) + 22050) / 44100
		// [orig: Audio_LoadWavFileFromArchive @ 0x76670e, @ 0x766735].
		if (!expect(out.loader_samples == 2 && out.loader_pitch_q16 == 32768,
				"PCM16 loader samples/pitch")) return 1;
	}

	// PCM8 is UNSIGNED with 128 = center; upconverts (v - 128) << 8.
	{
		const std::vector<uint8_t> data = { 128, 255, 0 };
		const std::vector<uint8_t> wav = make_wav(1, 1, 11025, 1, 8, data);
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error),
				"PCM8 decodes")) return 1;
		if (!expect(out.pcm16.size() == 6, "PCM8 doubles to 16-bit")) return 1;
		if (!expect(sample_at(out, 0) == 0, "128 maps to center 0")) return 1;
		if (!expect(sample_at(out, 1) == (255 - 128) << 8, "255 maps to +32512")) return 1;
		if (!expect(sample_at(out, 2) == -32768, "0 maps to -32768")) return 1;
		// 8-bit: the data's bytes; 11025 Hz rounds down to a quarter [orig: @ 0x766609, @ 0x76662d].
		if (!expect(out.loader_samples == 3 && out.loader_pitch_q16 == 16384,
				"PCM8 loader samples/pitch")) return 1;
	}

	// IMA-ADPCM mono, one 8-byte block: header {pred=100, idx=0, pad} + one
	// 4-byte nibble word 0x40 0x00 0x00 0x00. Hand-walk of the IMA tables:
	// the block's first frame IS the predictor (100); nibble 0 at idx 0
	// (step 7) adds step>>3 = 0 -> 100, idx clamps at 0; nibble 4 adds
	// step (7) -> 107, idx += 2; the six zero nibbles then decay idx to 0
	// adding step>>3 of steps {9, 8, 7, 7, 7, 7} -> 101 each time... step>>3
	// for steps 9/8/7 is 1/1/0, so frames run 108, 109, 109, 109, 109, 109.
	{
		std::vector<uint8_t> data;
		push_u16(data, 100);   // predictor
		data.push_back(0);     // step index
		data.push_back(0);     // pad
		data.push_back(0x40);  // nibbles: low 0, high 4
		data.push_back(0x00);
		data.push_back(0x00);
		data.push_back(0x00);
		const std::vector<uint8_t> wav = make_wav(0x11, 1, 8000, 8, 4, data);
		// IMA ADPCM: the fact chunk's count, not the block's decoded frames; 8000 Hz
		// is (524288000 + 22050) / 44100 = 11889 [orig: @ 0x76678a..0x7667ba, @ 0x7667e1].
		const std::vector<uint8_t> facted_wav = with_fact(wav, 5);
		WavPcm out;
		if (!expect(wav_decode_pcm16(facted_wav.data(), facted_wav.size(), out, error),
				"ADPCM decodes")) return 1;
		if (!expect(out.pcm16.size() == 9 * 2,
				"one block yields predictor + 8 nibble frames")) return 1;
		if (!expect(sample_at(out, 0) == 100, "frame 0 is the block predictor")) return 1;
		if (!expect(sample_at(out, 1) == 100, "nibble 0 at step 7 adds 0")) return 1;
		if (!expect(sample_at(out, 2) == 107, "nibble 4 at step 7 adds the step")) return 1;
		// Verify the tail against an independent re-walk of the tables.
		{
			int pred = 107;
			int idx = 2;
			const int kStep[] = { 7, 8, 9, 10 };
			(void)kStep;
			const int steps[] = { 9, 8, 7, 7, 7, 7 };
			for (int i = 0; i < 6; ++i) {
				pred += steps[i] >> 3;
				if (!expect(sample_at(out, static_cast<size_t>(3 + i)) == pred,
						"zero nibbles add step>>3 while the index decays")) return 1;
			}
		}
		if (!expect(out.loader_samples == 5 && out.loader_pitch_q16 == 11889 &&
				out.pcm16.size() == 9 * 2, "ADPCM loader samples are the fact count")) return 1;
		// 4-bit samples with no fact chunk are refused [orig: @ 0x766772].
		WavPcm unfacted;
		if (!expect(!wav_decode_pcm16(wav.data(), wav.size(), unfacted, error) &&
				unfacted.pcm16.empty() && !error.empty(),
				"ADPCM with no fact chunk is refused")) return 1;
	}

	// What the game's wave loader refuses the decode refuses; what it takes the
	// decode plays (D-SND-33 / D-SND-43) [orig: Audio_LoadWavFileFromArchive @ 0x766480].
	{
		std::vector<uint8_t> data;
		for (int i = 0; i < 8; ++i) push_u16(data, static_cast<uint16_t>(0x0101 * (i + 1)));
		const std::vector<uint8_t> mono = make_wav(1, 1, 22050, 2, 16, data);
		const auto plays = [&](const std::vector<uint8_t> &wav, const std::vector<uint8_t> &pcm,
				const char *what) {
			WavPcm out;
			const bool ok = wav_decode_pcm16(wav.data(), wav.size(), out, error) && out.pcm16 == pcm &&
					out.channels == 1;
			if (!ok) std::fprintf(stderr, "  (%s: %s)\n", what, error.c_str());
			return expect(ok, what);
		};
		const auto refuses = [&](const std::vector<uint8_t> &wav, const char *what) {
			WavPcm out;
			return expect(!wav_decode_pcm16(wav.data(), wav.size(), out, error) && out.pcm16.empty() &&
					!error.empty(), what);
		};
		if (!plays(mono, data, "mono PCM16 plays")) return 1;
		// One channel at every width [orig: @ 0x7665e3, @ 0x7666db, @ 0x76676a].
		if (!refuses(make_wav(1, 2, 22050, 4, 16, data), "stereo PCM16 is refused")) return 1;
		if (!refuses(make_wav(1, 2, 22050, 2, 8, data), "stereo PCM8 is refused")) return 1;
		std::vector<uint8_t> ima_block;
		for (int c = 0; c < 2; ++c) {
			push_u16(ima_block, 100);
			ima_block.push_back(0);
			ima_block.push_back(0);
		}
		ima_block.insert(ima_block.end(), 8, 0);
		if (!refuses(with_fact(make_wav(0x11, 2, 8000, 16, 4, ima_block), 9),
				"stereo IMA ADPCM is refused")) return 1;
		// A LIST ahead of the data: the walk steps into it by its id alone and reads
		// its list type as a size [orig: @ 0x766570, @ 0x7665a3]; one after the data is
		// never reached.
		const std::vector<uint8_t> info = text_body("INFOISFT\x0e\0\0\0Lavf58.29.100\0", 26);
		if (!refuses(with_chunk(mono, 36, "LIST", info), "a LIST ahead of the data is refused")) return 1;
		if (!plays(with_chunk(mono, mono.size(), "LIST", info), data,
				"a LIST after the data is never reached")) return 1;
		// A chunk ahead of the data past 0x800 bytes [orig: @ 0x7665a3].
		if (!refuses(with_chunk(mono, 36, "junk", std::vector<uint8_t>(0x802, 0)),
				"a chunk past 0x800 bytes ahead of the data is refused")) return 1;
		if (!plays(with_chunk(mono, 36, "junk", std::vector<uint8_t>(0x800, 0)), data,
				"a chunk of 0x800 bytes ahead of the data is stepped over")) return 1;
		// The twelfth chunk ahead of the data [orig: @ 0x7665b2]: fmt and ten more
		// play, fmt and eleven more do not.
		std::vector<uint8_t> eleven = mono;
		for (int i = 0; i < 10; ++i) eleven = with_chunk(eleven, 36, "junk", {});
		if (!plays(eleven, data, "eleven chunks ahead of the data play")) return 1;
		if (!refuses(with_chunk(eleven, 36, "junk", {}),
				"a twelfth chunk ahead of the data is refused")) return 1;
		// A second fmt [orig: @ 0x766589].
		if (!refuses(with_chunk(mono, 36, "fmt ", std::vector<uint8_t>(mono.begin() + 20, mono.begin() + 36)),
				"a second fmt chunk is refused")) return 1;
		// 4-bit samples of another format than IMA ADPCM [orig: @ 0x76677d]; other
		// widths [orig: @ 0x76675f].
		std::vector<uint8_t> ms_adpcm = with_fact(make_wav(0x11, 1, 8000, 8, 4, std::vector<uint8_t>(8, 0)), 9);
		ms_adpcm[20] = 2;
		if (!refuses(ms_adpcm, "4-bit samples not IMA ADPCM are refused")) return 1;
		if (!refuses(make_wav(1, 1, 22050, 3, 24, std::vector<uint8_t>(12, 0)),
				"24-bit samples are refused")) return 1;
		// The format tag of 8-bit and 16-bit samples is never read: the width
		// alone picks the form [orig: @ 0x7665d3, @ 0x7666ce].
		if (!plays(make_wav(0xFFFE, 1, 22050, 2, 16, data), data,
				"16-bit samples play whatever their tag")) return 1;
		WavPcm eight;
		const std::vector<uint8_t> tagged8 = make_wav(0x11, 1, 11025, 1, 8, {128, 255});
		if (!expect(wav_decode_pcm16(tagged8.data(), tagged8.size(), eight, error) &&
				eight.pcm16.size() == 4 && sample_at(eight, 1) == (255 - 128) << 8,
				"8-bit samples play as PCM whatever their tag")) return 1;
		// The walk steps over a chunk by its size, unpadded [orig: @ 0x7665a5]: an
		// odd chunk with no pad byte is stepped over to the data; one padded to an
		// even size, as the RIFF form lays it, leaves the walk on the pad byte, which
		// reads the data's id and size as a chunk past 0x800 bytes.
		const std::vector<uint8_t> odd = text_body("abc", 3);
		if (!plays(with_chunk(mono, 36, "junk", odd), data,
				"an odd chunk unpadded is stepped over")) return 1;
		if (!refuses(with_chunk(mono, 36, "junk", odd, true),
				"an odd chunk padded leaves the walk on its pad byte")) return 1;
		// The walk stops at the first data of nonzero size: an empty one is stepped
		// over [orig: @ 0x766592..0x76659b], a later one never reached.
		std::vector<uint8_t> later;
		push_u16(later, 0x7777);
		if (!plays(with_chunk(with_chunk(mono, 36, "data", {}), mono.size() + 8, "data", later), data,
				"the first data of nonzero size plays")) return 1;
	}

	// A truncated data chunk's samples clamp to the bytes present; the loader's
	// sample count is the chunk's size as it says, 8-bit the size, 16-bit half
	// of it [orig: Audio_LoadWavFileFromArchive @ 0x766609, @ 0x766706..0x76670e]
	// (JOX's grstn4.wav and sandhit6..8.wav run 7 bytes past their files).
	{
		std::vector<uint8_t> data;
		push_u16(data, 0x0102);
		const std::vector<uint8_t> wav =
				make_wav(1, 1, 22050, 2, 16, data, /*declared_data_size=*/64);
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error),
				"truncated data still decodes")) return 1;
		if (!expect(out.pcm16.size() == 2, "clamped to the present bytes")) return 1;
		if (!expect(out.loader_samples == 32, "16-bit: half the declared size")) return 1;
		const std::vector<uint8_t> wav8 =
				make_wav(1, 1, 11025, 1, 8, {128, 129, 130}, /*declared_data_size=*/10);
		WavPcm out8;
		if (!expect(wav_decode_pcm16(wav8.data(), wav8.size(), out8, error) &&
				out8.pcm16.size() == 6 && out8.loader_samples == 10,
				"8-bit: the declared size, the present samples")) return 1;
	}

	// AOA1's sample count excludes interpolation padding and PCM8 is SIGNED.
	for (const uint8_t width : {uint8_t(1), uint8_t(2)}) {
		std::vector<uint8_t> aoa;
		push_tag(aoa, "AOA1"); push_u32(aoa, 3); push_u32(aoa, 32768);
		push_u32(aoa, width);
		if (width == 1) aoa.insert(aoa.end(), {0x80, 0x00, 0x7F});
		else { push_u16(aoa, 0x8000); push_u16(aoa, 0); push_u16(aoa, 0x7F00); }
		const size_t payload_end = aoa.size();
		aoa.insert(aoa.end(), 8, 0x55);
		WavPcm out;
		if (!expect(wav_decode_pcm16(aoa.data(), aoa.size(), out, error),
				"AOA1 signed PCM decodes")) return 1;
		if (!expect(out.channels == 1 && out.sample_rate == 22050 && out.pcm16.size() == 6,
				"AOA1 mono count/rate excludes mixer padding")) return 1;
		if (!expect(sample_at(out, 0) == -32768 && sample_at(out, 1) == 0 &&
				sample_at(out, 2) == 32512, "AOA1 signed sample extrema")) return 1;
		if (!expect(out.loader_samples == 3 && out.loader_pitch_q16 == 32768,
				"AOA1 loader samples/pitch are its header's")) return 1;
		aoa.resize(payload_end);
		if (!expect(wav_decode_pcm16(aoa.data(), aoa.size(), out, error),
				"on-disk AOA1 does not require generated mixer padding")) return 1;
		for (const size_t truncated : {size_t(4), size_t(15), payload_end - 1}) {
			if (!expect(!wav_decode_pcm16(aoa.data(), truncated, out, error) &&
					out.pcm16.empty() && !error.empty(), "AOA1 rejects truncation")) return 1;
		}
		aoa[4] = 0xFF; aoa[5] = 0xFF; aoa[6] = 0xFF; aoa[7] = 0xFF;
		if (!expect(!wav_decode_pcm16(aoa.data(), aoa.size(), out, error),
				"AOA1 rejects oversized count before allocation")) return 1;
		aoa[4] = 3; aoa[5] = aoa[6] = aoa[7] = 0;
		aoa[12] = 3;
		if (!expect(!wav_decode_pcm16(aoa.data(), aoa.size(), out, error),
				"AOA1 rejects unsupported sample width")) return 1;
		aoa[12] = width; aoa[8] = aoa[9] = aoa[10] = aoa[11] = 0;
		if (!expect(!wav_decode_pcm16(aoa.data(), aoa.size(), out, error),
				"AOA1 rejects zero rate")) return 1;
	}

	// Malformed streams report errors.
	{
		WavPcm out;
		const std::vector<uint8_t> junk(64, 0x55);
		if (!expect(!wav_decode_pcm16(junk.data(), junk.size(), out, error) &&
						!error.empty(),
				"non-RIFF fails with an error")) return 1;
		if (!expect(!wav_decode_pcm16(nullptr, 0, out, error),
				"empty buffer fails")) return 1;
	}

	std::printf("OK: wav_pcm pcm16/pcm8/adpcm/loader walk/truncation/errors\n");
	return 0;
}
