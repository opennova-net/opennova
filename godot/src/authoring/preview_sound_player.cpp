#include "authoring/preview_sound_player.h"

#include <chrono>

#include <godot_cpp/classes/audio_stream_wav.hpp>

#include <editor/project/project_files.h>
#include <runtime/audio/volume_law.h>

#include "lwf/wav_loader.h"

namespace godot {

// The players are children of the parent, which frees them with itself: only the decodes in flight go here.
PreviewSoundPlayer::~PreviewSoundPlayer() {
	for (Voice &voice : voices_)
		if (voice.job.valid()) voice.job = std::future<Decode>();
}

void PreviewSoundPlayer::play(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices) {
	stop();
	path_ = p_voices.empty() ? std::string() : p_voices.front().path;
	for (const opennova::editor::WorkspaceView::Voice &voice : p_voices) {
		Voice made;
		made.voice = voice;
		const std::string file = opennova::editor::join_path(p_root, voice.path);
		made.job = std::async(std::launch::async, [file]() {
			Decode out;
			std::vector<uint8_t> bytes;
			if (!opennova::editor::read_file_bytes(file, bytes, out.error)) return out;
			out.decoded = opennova::lwf::wav_decode_pcm16(bytes.data(), bytes.size(), out.pcm, out.error);
			return out;
		});
		voices_.push_back(std::move(made));
	}
	state_ = voices_.empty() ? State::Idle : State::Decoding;
}

void PreviewSoundPlayer::stop() {
	// A decode in flight is let finish (its future's end waits for it) and dropped.
	for (Voice &voice : voices_) {
		if (voice.job.valid()) voice.job = std::future<Decode>();
		if (voice.player != nullptr) {
			voice.player->stop();
			voice.player->queue_free();
		}
	}
	voices_.clear();
	path_.clear();
	state_ = State::Idle;
}

PreviewSoundPlayer::State PreviewSoundPlayer::pump(std::string &r_error) {
	if (state_ == State::Decoding) {
		for (const Voice &voice : voices_)
			if (voice.job.valid() && voice.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return state_;
		// Every decode done: each voice that decoded starts, together, as the game starts a set's layers.
		size_t started = 0;
		for (Voice &voice : voices_) {
			if (!voice.job.valid()) continue;
			const Decode decoded = voice.job.get();
			if (!decoded.decoded) {
				r_error = voice.voice.path + ": " + decoded.error;
				continue;
			}
			const Ref<AudioStreamWAV> stream = WavLoader::from_pcm(decoded.pcm);
			if (stream.is_null()) continue;
			voice.player = memnew(AudioStreamPlayer);
			voice.player->set_name("PreviewSound");
			voice.player->set_stream(stream);
			const double pitch = double(voice.voice.pitch_q16) / 65536.0;
			voice.player->set_pitch_scale(pitch > 0.01 ? pitch : 1.0);
			voice.player->set_volume_db(opennova::audio::volume_db_from_byte(voice.voice.volume));
			parent_->add_child(voice.player);
			voice.player->play();
			++started;
		}
		state_ = started ? State::Playing : State::Failed;
		return state_;
	}
	if (state_ == State::Playing) {
		for (const Voice &voice : voices_)
			if (voice.player != nullptr && voice.player->is_playing()) return state_;
		state_ = State::Ended;
	}
	return state_;
}

} // namespace godot
