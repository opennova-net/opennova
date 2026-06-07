#ifndef NOVA_WAV_LOADER_H
#define NOVA_WAV_LOADER_H

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

// Decodes raw RIFF/WAVE bytes (as served by the VFS / NovaResourceRoot) into a
// Godot AudioStreamWAV. NovaLogic .wav samples are PCM, mono, 22050 Hz, 8-bit
// UNSIGNED (0x80 = silence); AudioStreamWAV's FORMAT_8_BITS is SIGNED, so the
// loader converts unsigned-8 -> signed-8 (XOR 0x80). 16-bit PCM is signed LE in
// both, copied verbatim. Non-PCM (e.g. ADPCM) is not yet supported and returns
// a null Ref.
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
