#pragma once

// The music stream over the mixer: the pump's device (MusicStreamDevice) that streams the selected
// SBF entry a block at a time onto mixer channels 1 and 2, and the music's volumes [orig:
// AudioVM_SelectStream @0x671BC0; AudioStream_Update @0x671C60, the block leg @0x671CA4..0x671DB3;
// Audio_SubmitStereoSampleSplit @0x7BD205 (the byte-paired form); Audio_PlayStereoSample @0x7BCF95
// (the IMA ADPCM form); AudioVM_OpenMusicContext @0x6722A0; AudioVM_StopMusicContext @0x671E00].
// The music's level is the global volume (the cfg's music_volume [orig: AudioVM_SetGlobalVolume
// @0x671F20, called @0x5255A4 / @0x56AA83 / @0x55ADEB]) times the stream's (GSV / GSDV, which a
// track ramps toward its target over its blocks), each block's two buffers queued at it.
// Witness record: docs/audio/mus-sbf-re.md, "The music stream over the mixer".

#include <runtime/audio/music_stream_pump.h>

#include <cstddef>
#include <cstdint>

namespace opennova::sbf {
struct SbfArchive;
}

namespace opennova::audio {

class Mixer;

// The buffers the stream keeps queued: an update streams while fewer are pending [orig:
// AudioStream_Update @0x671C95, `cmp eax, 7; jb`].
inline constexpr uint32_t kMusicQueueFull = 7;
// The chunk buffers' pitch: the eight static AUD1 buffers' +8, never written, 22050 Hz [orig:
// g_MusicStreamLeftBuffers / g_MusicStreamRightBuffers's buffers @0x79CDF0.., +8 0x8000].
inline constexpr uint32_t kMusicChunkPitchQ16 = 0x8000;

class MusicStream final : public MusicStreamDevice {
public:
	explicit MusicStream(Mixer &mixer) : mixer_(mixer) {}

	// The context's bank (its bytes held by the caller while bound) and the global volume: no track
	// selected, the volumes full, the music channels flushed [orig: AudioVM_OpenMusicContext
	// @0x6722DB..0x67230C], then the cfg's music_volume [orig: AudioVM_SetGlobalVolume @0x671F20].
	void open(const sbf::SbfArchive *archive, const uint8_t *bytes, size_t size, int32_t global_volume);
	// The context stopped: the music channels flushed [orig: AudioVM_StopMusicContext @0x671E19 ->
	// Audio_FlushChannels @0x7BCF70]; the bank unbound.
	void close();
	// [orig: AudioVM_SetGlobalVolume @0x671F20: clamped to 0..255, 16.16]
	void set_global_volume(int32_t volume);

	// The volume each block's buffers are queued at, g_MusicStreamVolume [orig: dword_85A5FC].
	uint8_t stream_volume() const { return volume_; }

	void start_track(uint32_t entry) override;
	void flush() override { bytes_left_ = 0; }
	bool streaming() const override { return bytes_left_ > 0; }
	bool queue_full() const override;
	void stream_block() override;
	void volume_changed(int32_t current_16_16, int32_t target_16_16) override;

private:
	void split_byte_paired(const uint8_t *block, size_t size);
	void split_adpcm(const uint8_t *block, size_t size, uint32_t count);

	Mixer &mixer_;
	const sbf::SbfArchive *archive_ = nullptr;
	const uint8_t *bytes_ = nullptr;
	size_t size_ = 0;
	size_t cursor_ = 0;           // the file pointer, dword_31C37D4
	int32_t bytes_left_ = 0;      // g_AudioStreamBytesLeft @0x31C37EC
	uint32_t block_size_ = 0;     // g_AudioStreamBlockSize @0x31C37E8
	uint32_t samples_left_ = 0;   // dword_31C37F0, the IMA ADPCM form's frames left
	int32_t global_ = 0xFF0000;   // unk_31C37FC
	int32_t current_ = 0xFF0000;  // unk_31C3800
	int32_t target_ = 0xFF0000;   // dword_31C3804
	int32_t ramp_ = 0;            // dword_31C3808, a block's step toward the target
	uint8_t volume_ = 0;          // g_MusicStreamVolume
};

} // namespace opennova::audio
