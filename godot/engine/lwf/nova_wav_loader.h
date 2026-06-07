#ifndef NOVA_WAV_LOADER_H
#define NOVA_WAV_LOADER_H

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

// Decodes raw RIFF/WAVE bytes (as served by the VFS / NovaResourceRoot) into a
// Godot AudioStreamWAV. NovaLogic .wav samples are PCM, mono, 22050 Hz, 8-bit
// UNSIGNED (0x80 = silence). The loader UPCONVERTS 8-bit -> signed 16-bit LE
// (sample16 = (u - 128) << 8) and emits FORMAT_16_BITS, matching the path Godot's
// own .wav importer takes; 16-bit PCM is signed LE and copied verbatim. Non-PCM
// (e.g. ADPCM) is not yet supported and returns a null Ref.
class NovaWavLoader : public RefCounted {
	GDCLASS(NovaWavLoader, RefCounted);

protected:
	static void _bind_methods();

public:
	// Build an AudioStreamWAV from RIFF/WAVE bytes. Returns null on parse error
	// or unsupported encoding. loop_mode/loop points are left at defaults; the
	// caller applies looping.
	static Ref<AudioStreamWAV> from_bytes(const PackedByteArray &p_bytes);
};

} // namespace godot

#endif // NOVA_WAV_LOADER_H
