#include "audio/audio_out.h"

#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>

using namespace godot;

namespace {

ObjectID g_device;
int64_t g_frames_mixed = 0;

} // namespace

void AudioOut::_bind_methods() {
	ClassDB::bind_static_method("AudioOut", D_METHOD("set_option_volumes", "sound_fx", "dialogue", "music"),
			&AudioOut::set_option_volumes);
	ClassDB::bind_static_method("AudioOut", D_METHOD("frames_mixed"), &AudioOut::frames_mixed);
	ClassDB::bind_static_method("AudioOut", D_METHOD("option_volumes"), &AudioOut::option_volumes);
	ClassDB::bind_static_method("AudioOut", D_METHOD("mix_frames", "frames"), &AudioOut::mix_frames);
	BIND_CONSTANT(VOLUME_BYTE_MAX);
	BIND_CONSTANT(DEFAULT_CHANNEL_VOLUME);
}

opennova::audio::Mixer &AudioOut::mixer() {
	static opennova::audio::Mixer mixer;
	return mixer;
}

opennova::audio::SoundOptions &AudioOut::options() {
	static opennova::audio::SoundOptions options;
	return options;
}

void AudioOut::attach(Node *p_any) {
	if (p_any == nullptr || Object::cast_to<AudioOut>(ObjectDB::get_instance(g_device)) != nullptr) return;
	SceneTree *tree = p_any->get_tree();
	Window *root = tree != nullptr ? tree->get_root() : nullptr;
	if (root == nullptr) return;
	AudioOut *device = memnew(AudioOut);
	device->set_name("AudioOut");
	g_device = ObjectID(device->get_instance_id());
	root->call_deferred("add_child", device);
}

void AudioOut::set_option_volumes(int p_sound_fx, int p_dialogue, int p_music) {
	options().set(p_sound_fx, p_dialogue, p_music);
}

PackedInt32Array AudioOut::option_volumes() {
	PackedInt32Array out;
	out.push_back(options().sound_fx());
	out.push_back(options().dialogue());
	out.push_back(options().music());
	return out;
}

int64_t AudioOut::frames_mixed() {
	return g_frames_mixed;
}

void AudioOut::mix_frames(int p_frames) {
	if (p_frames <= 0) return;
	std::vector<int16_t> pcm(size_t(p_frames) * opennova::audio::kMixOutputChannels);
	mixer().render(pcm.data(), uint32_t(p_frames));
	g_frames_mixed += p_frames;
}

void AudioOut::_ready() {
	Ref<AudioStreamGenerator> stream;
	stream.instantiate();
	stream->set_mix_rate(static_cast<float>(opennova::audio::kMixDeviceRate));
	stream->set_buffer_length(static_cast<float>(opennova::audio::kMixFillFrames) /
			static_cast<float>(opennova::audio::kMixDeviceRate));
	AudioStreamPlayer *player = memnew(AudioStreamPlayer);
	player->set_name("Stream");
	player->set_stream(stream);
	add_child(player);
	player->play();
	playback_ = player->get_stream_playback();
	pump();
}

void AudioOut::_process(double /*p_delta*/) {
	pump();
}

void AudioOut::pump() {
	if (playback_.is_null()) return;
	const int32_t free_frames = playback_->get_frames_available();
	if (free_frames <= 0) return;
	const uint32_t frames = std::min<uint32_t>(uint32_t(free_frames), opennova::audio::kMixFillFrames);
	pcm_.resize(size_t(frames) * opennova::audio::kMixOutputChannels);
	mixer().render(pcm_.data(), frames);
	g_frames_mixed += frames;
	PackedVector2Array buffer;
	buffer.resize(frames);
	Vector2 *out = buffer.ptrw();
	constexpr float kScale = 1.0f / 32768.0f;
	for (uint32_t f = 0; f < frames; ++f)
		out[f] = Vector2(float(pcm_[2 * f]) * kScale, float(pcm_[2 * f + 1]) * kScale);
	playback_->push_buffer(buffer);
}
