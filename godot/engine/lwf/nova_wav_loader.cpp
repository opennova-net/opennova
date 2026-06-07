#include "lwf/nova_wav_loader.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdint>
#include <cstring>

namespace godot {

namespace {

// Little-endian readers over the raw buffer (bounds-checked by caller).
uint16_t rd_u16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd_u32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
bool tag_eq(const uint8_t *p, const char *tag) {
	return p[0] == static_cast<uint8_t>(tag[0]) && p[1] == static_cast<uint8_t>(tag[1]) &&
			p[2] == static_cast<uint8_t>(tag[2]) && p[3] == static_cast<uint8_t>(tag[3]);
}

} // namespace

void NovaWavLoader::_bind_methods() {
	ClassDB::bind_static_method("NovaWavLoader", D_METHOD("from_bytes", "bytes"), &NovaWavLoader::from_bytes);
}

Ref<AudioStreamWAV> NovaWavLoader::from_bytes(const PackedByteArray &p_bytes) {
	const int64_t size = p_bytes.size();
	if (size < 44) {
		UtilityFunctions::push_warning("NovaWavLoader: buffer too small to be a WAV");
		return Ref<AudioStreamWAV>();
	}
	const uint8_t *buf = p_bytes.ptr();

	if (!tag_eq(buf, "RIFF") || !tag_eq(buf + 8, "WAVE")) {
		UtilityFunctions::push_warning("NovaWavLoader: not a RIFF/WAVE file");
		return Ref<AudioStreamWAV>();
	}

	// Walk chunks starting after the 'WAVE' tag (offset 12).
	uint16_t audio_format = 0;
	uint16_t channels = 0;
	uint32_t sample_rate = 0;
	uint16_t bits_per_sample = 0;
	bool have_fmt = false;
	int64_t data_off = -1;
	uint32_t data_size = 0;

	int64_t pos = 12;
	while (pos + 8 <= size) {
		const uint8_t *chunk = buf + pos;
		const uint32_t chunk_size = rd_u32(chunk + 4);
		const int64_t body = pos + 8;
		if (body + static_cast<int64_t>(chunk_size) > size) {
			// Truncated chunk; clamp the data chunk so we still play what we have.
			if (tag_eq(chunk, "data")) {
				data_off = body;
				data_size = static_cast<uint32_t>(size - body);
			}
			break;
		}
		if (tag_eq(chunk, "fmt ") && chunk_size >= 16) {
			audio_format = rd_u16(chunk + 8);
			channels = rd_u16(chunk + 10);
			sample_rate = rd_u32(chunk + 12);
			bits_per_sample = rd_u16(chunk + 22);
			have_fmt = true;
		} else if (tag_eq(chunk, "data")) {
			data_off = body;
			data_size = chunk_size;
		}
		// Chunks are word-aligned (padded to even size).
		pos = body + chunk_size + (chunk_size & 1);
	}

	if (!have_fmt || data_off < 0) {
		UtilityFunctions::push_warning("NovaWavLoader: missing fmt/data chunk");
		return Ref<AudioStreamWAV>();
	}
	if (audio_format != 1) {
		UtilityFunctions::push_warning("NovaWavLoader: unsupported WAV format (only PCM supported), got ", audio_format);
		return Ref<AudioStreamWAV>();
	}
	if (channels < 1 || channels > 2) {
		UtilityFunctions::push_warning("NovaWavLoader: unsupported channel count ", channels);
		return Ref<AudioStreamWAV>();
	}

	PackedByteArray pcm;
	AudioStreamWAV::Format fmt;
	if (bits_per_sample == 8) {
		// WAV PCM8 is unsigned (128 = center); AudioStreamWAV FORMAT_8_BITS is
		// signed two's complement, so XOR 0x80 to convert.
		fmt = AudioStreamWAV::FORMAT_8_BITS;
		pcm.resize(data_size);
		uint8_t *dst = pcm.ptrw();
		const uint8_t *src = buf + data_off;
		for (uint32_t i = 0; i < data_size; ++i) {
			dst[i] = src[i] ^ 0x80;
		}
	} else if (bits_per_sample == 16) {
		// WAV PCM16 and AudioStreamWAV FORMAT_16_BITS are both signed LE.
		fmt = AudioStreamWAV::FORMAT_16_BITS;
		pcm.resize(data_size);
		std::memcpy(pcm.ptrw(), buf + data_off, data_size);
	} else {
		UtilityFunctions::push_warning("NovaWavLoader: unsupported bit depth ", bits_per_sample);
		return Ref<AudioStreamWAV>();
	}

	Ref<AudioStreamWAV> stream;
	stream.instantiate();
	stream->set_format(fmt);
	stream->set_mix_rate(static_cast<int32_t>(sample_rate));
	stream->set_stereo(channels == 2);
	stream->set_loop_mode(AudioStreamWAV::LOOP_DISABLED);
	stream->set_data(pcm);
	return stream;
}

} // namespace godot
