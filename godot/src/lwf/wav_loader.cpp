#include "lwf/wav_loader.h"
#include "util/string_convert.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/lwf/wav_pcm.h>

#include <cstring>
#include <string>

namespace godot {

void WavLoader::_bind_methods() {
	ClassDB::bind_static_method("WavLoader", D_METHOD("from_bytes", "bytes"), &WavLoader::from_bytes);
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
	Ref<AudioStreamWAV> stream;
	stream.instantiate();
	stream->set_format(AudioStreamWAV::FORMAT_16_BITS); // always signed 16-bit out
	stream->set_mix_rate(static_cast<int32_t>(decoded.sample_rate));
	stream->set_stereo(decoded.channels == 2);
	stream->set_loop_mode(AudioStreamWAV::LOOP_DISABLED);
	stream->set_data(pcm);
	return stream;
}

} // namespace godot
