// lwf::wav_decode_pcm16 — the RIFF walk, the PCM8/PCM16 normalizations, and
// the IMA-ADPCM block decode (hand-computed against the IMA step/index
// tables), moved from the shell adapter's WavLoader.
#include <lwf/wav_pcm.h>

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
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error),
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
	}

	// A truncated data chunk clamps to the bytes present.
	{
		std::vector<uint8_t> data;
		push_u16(data, 0x0102);
		const std::vector<uint8_t> wav =
				make_wav(1, 1, 22050, 2, 16, data, /*declared_data_size=*/64);
		WavPcm out;
		if (!expect(wav_decode_pcm16(wav.data(), wav.size(), out, error),
				"truncated data still decodes")) return 1;
		if (!expect(out.pcm16.size() == 2, "clamped to the present bytes")) return 1;
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

	std::printf("OK: wav_pcm pcm16/pcm8/adpcm/truncation/errors\n");
	return 0;
}
