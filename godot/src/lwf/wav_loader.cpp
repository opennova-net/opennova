#include "lwf/wav_loader.h"
#include "lwf/wave_stream.h"
#include "util/string_convert.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/lwf/wav_pcm.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace godot {

void WavLoader::_bind_methods() {
	ClassDB::bind_static_method("WavLoader", D_METHOD("from_bytes", "bytes"), &WavLoader::from_bytes);
	ClassDB::bind_static_method("WavLoader", D_METHOD("pitch_scale_for", "stream", "pitch_scale"),
			&WavLoader::pitch_scale_for);
}

Ref<AudioStreamWAV> WavLoader::from_bytes(const PackedByteArray &p_bytes) {
	// The RIFF walk and the IMA-ADPCM/PCM normalizations are
	// lwf::wav_decode_pcm16's (engine/formats/lwf); this boxes the decoded
	// PCM into Godot's AudioStreamWAV.
	opennova::lwf::WavPcm decoded;
	std::string error;
	if (!opennova::lwf::wav_decode_pcm16(p_bytes.ptr(),
				static_cast<size_t>(p_bytes.size()), decoded, error)) {
		UtilityFunctions::push_warning("WavLoader: ", opennova::to_gd(error));
		return Ref<AudioStreamWAV>();
	}
	return from_pcm(decoded);
}

Ref<AudioStreamWAV> WavLoader::from_pcm(const opennova::lwf::WavPcm &decoded) {
	PackedByteArray pcm;
	pcm.resize(static_cast<int64_t>(decoded.pcm16.size()));
	if (!decoded.pcm16.empty()) {
		std::memcpy(pcm.ptrw(), decoded.pcm16.data(), decoded.pcm16.size());
	}
	Ref<WaveStream> stream;
	stream.instantiate();
	stream->set_format(AudioStreamWAV::FORMAT_16_BITS); // always signed 16-bit out
	// The mix rate is a whole int32: a rate past INT32_MAX is boxed there and the stream keeps its own,
	// which pitch_scale_for scales the box back up to.
	stream->set_mix_rate(static_cast<int32_t>(std::min<uint32_t>(decoded.sample_rate, INT32_MAX)));
	stream->set_wave_rate(static_cast<int64_t>(decoded.sample_rate));
	stream->set_stereo(decoded.channels == 2);
	stream->set_loop_mode(AudioStreamWAV::LOOP_DISABLED);
	stream->set_data(pcm);
	stream->set_loader_pitch_q16(static_cast<int64_t>(decoded.loader_pitch_q16));
	return stream;
}

double WavLoader::pitch_scale_for(const Ref<AudioStream> &p_stream, double p_pitch_scale) {
	// A stream not decoded here carries no word and keeps the composed scale; a WaveStream made
	// elsewhere, with no rate of its own, is its mix rate.
	const WaveStream *wave = Object::cast_to<WaveStream>(p_stream.ptr());
	if (wave == nullptr) return p_pitch_scale;
	const int64_t mix_rate = wave->get_mix_rate();
	const int64_t wave_rate = wave->get_wave_rate() > 0 ? wave->get_wave_rate() : mix_rate;
	return opennova::lwf::wave_pitch_scale(static_cast<uint32_t>(wave->get_loader_pitch_q16()),
			static_cast<uint32_t>(wave_rate), static_cast<uint32_t>(mix_rate > 0 ? mix_rate : 0), p_pitch_scale);
}

} // namespace godot
