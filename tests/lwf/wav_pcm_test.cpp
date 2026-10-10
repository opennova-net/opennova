// lwf::wav_decode_pcm16: the game's wave loader's walk and its refusals
// (D-SND-33 / D-SND-43), the PCM8/PCM16 normalizations, the loader's own AUD1
// buffer (D-SND-44) and its own IMA ADPCM decode (D-SND-45, each leg
// hand-computed from the witnessed nibble step), moved from the shell
// adapter's WavLoader; and the sample count and pitch ratio the game's wave
// loader records (a dialog line's hold reads them).
#include <formats/lwf/wav_pcm.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

std::vector<int16_t> samples_of(const opennova::lwf::WavPcm &pcm) {
	std::vector<int16_t> out(pcm.pcm16.size() / 2);
	for (size_t i = 0; i < out.size(); ++i) out[i] = sample_at(pcm, i);
	return out;
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

	// IMA ADPCM, the loader's own decode [orig: Audio_LoadWavFileFromArchive @ 0x766480, the 4-bit
	// leg; Audio_AdpcmDecodeNibbleStep @ 0x7bf250]: exactly the fact chunk's count of samples, each
	// block its predictor and then, per byte, its low and its high nibble, each nibble moving the
	// sample by (step * scale[nibble]) >> 16, floored. One 8-byte block {pred 100, index 0, pad,
	// 0x40 0x00 0x00 0x00} with a fact count of 5: the predictor 100; nibble 0 at step 7 adds
	// 7 * 0x2000 >> 16 = 0 (index 0); nibble 4 adds 7 * 0x12000 >> 16 = 7 (index 2); nibble 0 at
	// step 9 adds 1 (index 1); nibble 0 at step 8 adds 1; then the count is spent, mid-block.
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
		// The loader's record: the fact chunk's count; 8000 Hz is
		// (524288000 + 22050) / 44100 = 11889 [orig: @ 0x76678a..0x7667ba, @ 0x7667e1].
		const std::vector<uint8_t> facted_wav = with_fact(wav, 5);
		WavPcm out;
		if (!expect(wav_decode_pcm16(facted_wav.data(), facted_wav.size(), out, error),
				"ADPCM decodes")) return 1;
		if (!expect(samples_of(out) == std::vector<int16_t>({100, 100, 107, 108, 109}),
				"the fact count's samples, stopped mid-block")) return 1;
		if (!expect(out.loader_samples == 5 && out.loader_pitch_q16 == 11889,
				"ADPCM loader samples are the fact count")) return 1;
		// The stop lands on the predictor, a low nibble or a high one
		// [orig: @ 0x766823, @ 0x766866, @ 0x766896].
		const std::vector<std::vector<int16_t>> stops = {{100}, {100, 100}, {100, 100, 107}};
		for (uint32_t count = 1; count <= 3; ++count) {
			const std::vector<uint8_t> stopped = with_fact(wav, count);
			if (!expect(wav_decode_pcm16(stopped.data(), stopped.size(), out, error) &&
					samples_of(out) == stops[count - 1], "the count stops the decode at once")) return 1;
		}
		// A count of 0 or less decodes none [orig: @ 0x7667fb]; the record keeps it as it says.
		for (const uint32_t count : {0u, 0xFFFFFFFFu, 0x80000000u}) {
			const std::vector<uint8_t> none = with_fact(wav, count);
			if (!expect(wav_decode_pcm16(none.data(), none.size(), out, error) && out.pcm16.empty() &&
					out.loader_samples == count, "a count of 0 or less decodes no sample")) return 1;
		}
		// 4-bit samples with no fact chunk are refused [orig: @ 0x766772].
		WavPcm unfacted;
		if (!expect(!wav_decode_pcm16(wav.data(), wav.size(), unfacted, error) &&
				unfacted.pcm16.empty() && !error.empty(),
				"ADPCM with no fact chunk is refused")) return 1;
		// Past the data the decode reads on through the file, a chunk after it included (the
		// loader's read is the count's, never the data chunk's size): a 4-byte data chunk and a LIST
		// after it, its id's 'L' (0x4C) read as nibbles 12 and 4 (at step 7 floor(-63 / 8) = -8,
		// then at step 9 floor(81 / 8) = 10). Where the file's bytes end the decode stops (a bound of ours: the loader reads on
		// past them).
		const std::vector<uint8_t> header(data.begin(), data.begin() + 4);
		std::vector<uint8_t> listed = with_fact(make_wav(0x11, 1, 8000, 8, 4, header), 3);
		listed.insert(listed.end(), {'L', 'I', 'S', 'T', 0, 0, 0, 0});
		if (!expect(wav_decode_pcm16(listed.data(), listed.size(), out, error) &&
				samples_of(out) == std::vector<int16_t>({100, 92, 102}),
				"the decode reads past the data chunk")) return 1;
		const std::vector<uint8_t> past = with_fact(wav, 100);
		if (!expect(wav_decode_pcm16(past.data(), past.size(), out, error) &&
				out.pcm16.size() == 9 * 2 && out.loader_samples == 100,
				"a count past the file's bytes plays the samples there")) return 1;
	}

	// The nibble step [orig: Audio_AdpcmDecodeNibbleStep @ 0x7bf250]: the step read at the index
	// before it moves (word_7BF300 @ 0x7bf256), diff = (step * scale[nibble]) >> 16 over the scale
	// table +/-0x2000 * (2n + 1) (dword_7BF2C0 @ 0x7bf265; imul, shrd 16), the sample held to int16
	// (@ 0x7bf274..0x7bf289), the index moved by dword_7BF3B2 (@ 0x7bf25e) and held to 0..88
	// (@ 0x7bf28e..0x7bf29a). Each leg: a 5-byte block {pred, index, pad, byte}, the fact count 3.
	{
		const auto decode = [&](int16_t pred, uint8_t index, uint8_t byte, std::vector<int16_t> &r) {
			std::vector<uint8_t> block;
			push_u16(block, static_cast<uint16_t>(pred));
			block.push_back(index);
			block.push_back(0);
			block.push_back(byte);
			const std::vector<uint8_t> wav = with_fact(make_wav(0x11, 1, 8000, 5, 4, block), 3);
			WavPcm out;
			if (!wav_decode_pcm16(wav.data(), wav.size(), out, error)) return false;
			r = samples_of(out);
			return r.size() == 3;
		};
		struct Leg {
			int16_t pred;
			uint8_t index;
			uint8_t byte;   // low nibble first, then high
			int16_t first;  // after the low nibble
			int16_t second; // after the high nibble
			const char *what;
		};
		const Leg legs[] = {
			// Step 7: nibble 7 adds floor(7 * 15 / 8) = 13 (the shifted steps add 11); then at the
			// index 8 (step 16) nibble 0 adds 2.
			{0, 0, 0x07, 13, 15, "step 7, nibble 7 adds 13"},
			// Step 7: nibble 8 takes floor(-7 / 8) = -1 (the shifted steps 0); the index held at 0,
			// nibble 0 adds floor(7 / 8) = 0.
			{0, 0, 0x08, -1, -1, "step 7, nibble 8 takes 1"},
			// Step 7: nibble 15 takes floor(-105 / 8) = -14; then step 16, nibble 1 adds 6.
			{0, 0, 0x1F, -14, -8, "step 7, nibble 15 takes 14"},
			// Step 876 (index 50): nibble 3 adds floor(876 * 7 / 8) = 766; at index 49 (step 796)
			// nibble 11 takes floor(-796 * 7 / 8) = -697 (the shifted steps 696).
			{0, 50, 0xB3, 766, 69, "step 876 nibble 3 adds 766, step 796 nibble 11 takes 697"},
			// Step 19 (index 10): nibble 4 adds 21; at index 12 (step 23) nibble 5 adds
			// floor(23 * 11 / 8) = 31 (the shifted steps 30).
			{1000, 10, 0x54, 1021, 1052, "step 19 nibble 4, then step 23 nibble 5"},
			// Step 32767 (index 88): nibble 7 adds floor(32767 * 15 / 8) = 61438, held at 32767;
			// nibble 15 takes 61439, held at -32768.
			{0, 88, 0x77, 32767, 32767, "the sample is held at 32767"},
			{0, 88, 0xFF, -32768, -32768, "the sample is held at -32768"},
			// The index held at 88: nibble 7 from -32768 adds 61438 (28670), the index 96 held at
			// 88, so nibble 0 adds floor(32767 / 8) = 4095.
			{-32768, 88, 0x07, 28670, 32765, "the index is held at 88"},
			// The index held at 0: nibble 0 at index 0 leaves it 0, nibble 7 adds 13.
			{500, 0, 0x70, 500, 513, "the index is held at 0"},
		};
		for (const Leg &leg : legs) {
			std::vector<int16_t> got;
			const bool ok = decode(leg.pred, leg.index, leg.byte, got);
			if (!expect(ok && got[0] == leg.pred && got[1] == leg.first && got[2] == leg.second,
					leg.what)) {
				if (got.size() == 3) std::fprintf(stderr, "  got %d %d %d\n", got[0], got[1], got[2]);
				return 1;
			}
		}
		// The block's step index is a signed byte [orig: @ 0x766804]: 0xF7 is -9, which nibble 7
		// moves to -1 and the step holds at 0, so the next nibble steps at 7 (nibble 9 takes
		// floor(-21 / 8) = -3); read unsigned it would be 247, held at 88 (a step of 32767). The
		// first nibble's own step, read at -9 past the table's start, is D-SND-47's.
		std::vector<int16_t> got;
		if (!expect(decode(0, 0xF7, 0x97, got) && got[2] - got[1] == -3,
				"the step index is a signed byte")) return 1;
	}

	// block_align is read signed, its 4-byte header taken off [orig: @ 0x766786..0x76679e]: at 4 or
	// under (0, 2, 0x8000, 0xFFFF among them) a block is its header alone, each 4 bytes one sample
	// [orig: @ 0x766834]; at 5 a header and one byte, three samples.
	{
		std::vector<uint8_t> headers;
		for (const int16_t pred : {int16_t(11), int16_t(-22), int16_t(33)}) {
			push_u16(headers, static_cast<uint16_t>(pred));
			headers.push_back(0);
			headers.push_back(0);
		}
		for (const uint16_t align : {uint16_t(0), uint16_t(2), uint16_t(4), uint16_t(0x8000), uint16_t(0xFFFF)}) {
			const std::vector<uint8_t> wav = with_fact(make_wav(0x11, 1, 8000, align, 4, headers), 3);
			WavPcm out;
			if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error) &&
					samples_of(out) == std::vector<int16_t>({11, -22, 33}),
					"a block_align of 4 or under (signed) is a header alone")) return 1;
		}
		std::vector<uint8_t> blocks;
		for (const int16_t pred : {int16_t(100), int16_t(-100)}) {
			push_u16(blocks, static_cast<uint16_t>(pred));
			blocks.push_back(0);
			blocks.push_back(0);
			blocks.push_back(0x07);
		}
		const std::vector<uint8_t> wav = with_fact(make_wav(0x11, 1, 8000, 5, 4, blocks), 6);
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error) &&
				samples_of(out) == std::vector<int16_t>({100, 113, 115, -100, -87, -85}),
				"a block_align of 5 is a header and one byte")) return 1;
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

	// The loader's own buffer, AUD1 (bytes 41 55 44 31), copied unchecked
	// [orig: Audio_LoadWavFileFromArchive @ 0x7664e2, @ 0x7664e9..0x766511], read
	// as the mixer reads it: the count at +4 (sub_7BD671 @ 0x7bd67f), the Q16
	// pitch at +8 (AudioChannel_ComputeMixCoefficients @ 0x7bd603), signed
	// samples from +16, 16-bit where +12 is 2 and 8-bit for any other byte there
	// (sub_7BD671 @ 0x7bd692). An AOA1 buffer falls to the RIFF compare and is
	// refused (@ 0x766523), as is either form under the strict and the lenient decode.
	for (const uint8_t width : {uint8_t(1), uint8_t(2)}) {
		std::vector<uint8_t> aud;
		push_tag(aud, "AUD1"); push_u32(aud, 3); push_u32(aud, 32768);
		push_u32(aud, width);
		if (width == 1) aud.insert(aud.end(), {0x80, 0x00, 0x7F});
		else { push_u16(aud, 0x8000); push_u16(aud, 0); push_u16(aud, 0x7F00); }
		const size_t payload_end = aud.size();
		aud.insert(aud.end(), 8, 0x55);
		WavPcm out;
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error),
				"AUD1 signed PCM decodes")) return 1;
		if (!expect(out.channels == 1 && out.sample_rate == 22050 && out.pcm16.size() == 6,
				"AUD1 mono count/rate excludes the bytes past its samples")) return 1;
		if (!expect(sample_at(out, 0) == -32768 && sample_at(out, 1) == 0 &&
				sample_at(out, 2) == 32512, "AUD1 signed sample extrema")) return 1;
		if (!expect(out.loader_samples == 3 && out.loader_pitch_q16 == 32768,
				"AUD1 loader samples/pitch are its header's")) return 1;
		WavPcm lenient;
		if (!expect(opennova::lwf::wav_decode_pcm16_lenient(aud.data(), aud.size(), lenient, error) &&
				lenient.pcm16 == out.pcm16 && lenient.loader_samples == 3,
				"the lenient decode reads AUD1 as the strict one does")) return 1;
		std::vector<uint8_t> aoa = aud;
		aoa[1] = 'O';
		if (!expect(!wav_decode_pcm16(aoa.data(), aoa.size(), out, error) && out.pcm16.empty() &&
				!opennova::lwf::wav_decode_pcm16_lenient(aoa.data(), aoa.size(), lenient, error),
				"an AOA1 buffer is refused at the RIFF compare")) return 1;
		aud.resize(payload_end);
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error),
				"on-disk AUD1 needs no bytes past its samples")) return 1;
		// The copy reads what follows the bytes: a buffer ending inside its header is
		// refused (a bound of ours); samples past the end play those present, the
		// count as it says.
		for (const size_t truncated : {size_t(4), size_t(15)}) {
			if (!expect(!wav_decode_pcm16(aud.data(), truncated, out, error) &&
					out.pcm16.empty() && !error.empty(), "AUD1 ending inside its header is refused")) return 1;
		}
		if (!expect(wav_decode_pcm16(aud.data(), payload_end - width, out, error) &&
				out.pcm16.size() == 4 && out.loader_samples == 3,
				"AUD1 samples past the bytes play those present")) return 1;
		aud[4] = 0xFF; aud[5] = 0xFF; aud[6] = 0xFF; aud[7] = 0xFF;
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error) && out.pcm16.size() == 6 &&
				out.loader_samples == 0xFFFFFFFFu, "AUD1 count past the bytes plays the bytes")) return 1;
		aud[4] = 0; aud[5] = aud[6] = aud[7] = 0;
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error) && out.pcm16.empty() &&
				out.loader_samples == 0, "AUD1 count 0 plays no sample")) return 1;
		// A pitch of 0 is taken as it is, the rate it rounds to 0.
		aud[4] = 3;
		aud[8] = aud[9] = aud[10] = aud[11] = 0;
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error) && out.loader_pitch_q16 == 0 &&
				out.sample_rate == 0, "AUD1 pitch 0 is taken")) return 1;
	}
	// The width byte: 2 is 16-bit, any other 8-bit [orig: sub_7BD671 @ 0x7bd692].
	for (const uint8_t width : {uint8_t(0), uint8_t(3), uint8_t(0xFF)}) {
		std::vector<uint8_t> aud;
		push_tag(aud, "AUD1"); push_u32(aud, 2); push_u32(aud, 16384);
		push_u32(aud, width);
		aud.insert(aud.end(), {0x81, 0x7F});
		WavPcm out;
		if (!expect(wav_decode_pcm16(aud.data(), aud.size(), out, error) && out.pcm16.size() == 4 &&
				sample_at(out, 0) == -32512 && sample_at(out, 1) == 32512 && out.sample_rate == 11025,
				"AUD1 width byte other than 2 is 8-bit")) return 1;
	}
	// The loader's pad goes by the raw width byte [orig: Audio_LoadWavFileFromArchive
	// @ 0x76668a..0x7668da]: at width 0 it lands on +16, the mixer's first eight samples, and
	// zeroes them where the word there is under 0x800 in size (else copies them onto themselves).
	{
		const auto width0 = [&](uint8_t lo, uint8_t hi, std::vector<int16_t> &r) {
			std::vector<uint8_t> aud;
			push_tag(aud, "AUD1"); push_u32(aud, 10); push_u32(aud, 32768);
			push_u32(aud, 0);
			aud.insert(aud.end(), {lo, hi, 3, 4, 5, 6, 7, 8, 9, 10});
			WavPcm out;
			if (!wav_decode_pcm16(aud.data(), aud.size(), out, error)) return false;
			r = samples_of(out);
			return r.size() == 10;
		};
		const std::vector<int16_t> tail = {9 << 8, 10 << 8};
		std::vector<int16_t> got;
		for (const auto &word : std::vector<std::pair<uint8_t, uint8_t>>{{0x10, 0x00}, {0xFF, 0x07}, {0x01, 0xF8}}) {
			if (!expect(width0(word.first, word.second, got) &&
					std::vector<int16_t>(got.begin(), got.begin() + 8) == std::vector<int16_t>(8, 0) &&
					std::vector<int16_t>(got.begin() + 8, got.end()) == tail,
					"AUD1 width 0: a first word under 0x800 in size zeroes the first eight samples")) return 1;
		}
		for (const auto &word : std::vector<std::pair<uint8_t, uint8_t>>{{0x00, 0x08}, {0x00, 0xF8}}) {
			if (!expect(width0(word.first, word.second, got) &&
					got[0] == static_cast<int16_t>(word.first << 8) &&
					got[1] == static_cast<int16_t>(static_cast<int8_t>(word.second) * 256) &&
					got[2] == (3 << 8) && got[9] == (10 << 8),
					"AUD1 width 0: a first word of 0x800 or more in size leaves them")) return 1;
		}
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
