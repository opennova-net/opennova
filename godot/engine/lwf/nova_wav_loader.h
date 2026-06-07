#ifndef NOVA_WAV_LOADER_H
#define NOVA_WAV_LOADER_H

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

// Decodes raw RIFF/WAVE bytes (as served by the VFS / NovaResourceRoot) into a
// Godot AudioStreamWAV, always emitting signed 16-bit (FORMAT_16_BITS):
//   - PCM 8-bit UNSIGNED (ambient, e.g. game.lwf): upconvert (u-128)<<8.
//   - PCM 16-bit signed LE: copied verbatim.
//   - IMA-ADPCM (audioFormat 0x11; NovaLogic voice / zone / dialog audio, 4-bit
//     block-based): decoded to 16-bit via the standard step/index tables.
// Other formats (e.g. MS-ADPCM 0x02) are unsupported and return a null Ref.
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
