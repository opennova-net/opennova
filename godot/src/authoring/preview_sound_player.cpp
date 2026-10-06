#include "authoring/preview_sound_player.h"

#include <chrono>
#include <cstddef>

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

// --- PreviewSoundVoices ------------------------------------------------------------------------------

PreviewSoundVoices::~PreviewSoundVoices() {
	// The players are the parent's children, freed with it; a decode in flight is let finish.
	pending_.clear();
	waves_.clear();
}

void PreviewSoundVoices::add(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices) {
	for (const opennova::editor::WorkspaceView::Voice &voice : p_voices) {
		const std::string file = opennova::editor::join_path(p_root, voice.path);
		Wave &wave = waves_[file];
		if (!wave.job.valid() && wave.stream.is_null() && !wave.failed)
			wave.job = std::async(std::launch::async, [file]() {
				Decode out;
				std::vector<uint8_t> bytes;
				if (!opennova::editor::read_file_bytes(file, bytes, out.error)) return out;
				out.decoded = opennova::lwf::wav_decode_pcm16(bytes.data(), bytes.size(), out.pcm, out.error);
				return out;
			}).share();
		pending_.emplace_back(file, voice);
	}
}

void PreviewSoundVoices::pump() {
	// The voices whose waves are decoded start, in the order they fired.
	std::vector<std::pair<std::string, opennova::editor::WorkspaceView::Voice>> waiting;
	for (const auto &[file, voice] : pending_) {
		Wave &wave = waves_[file];
		if (wave.stream.is_null() && !wave.failed) {
			if (!wave.job.valid() || wave.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				waiting.emplace_back(file, voice);
				continue;
			}
			const Decode &decoded = wave.job.get();
			wave.stream = decoded.decoded ? WavLoader::from_pcm(decoded.pcm) : Ref<AudioStreamWAV>();
			wave.failed = wave.stream.is_null();
			wave.job = std::shared_future<Decode>();
		}
		if (wave.failed) continue;
		if (players_.size() >= kMaxPlayers) {
			players_.front()->stop();
			players_.front()->queue_free();
			players_.erase(players_.begin());
		}
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		player->set_name("ClipSound");
		player->set_stream(wave.stream);
		const double pitch = double(voice.pitch_q16) / 65536.0;
		player->set_pitch_scale(pitch > 0.01 ? pitch : 1.0);
		player->set_volume_db(opennova::audio::volume_db_from_byte(voice.volume));
		parent_->add_child(player);
		player->play();
		players_.push_back(player);
		++started_;
	}
	pending_ = std::move(waiting);
	// The players that played through, freed.
	for (size_t i = 0; i < players_.size();) {
		if (players_[i]->is_playing()) {
			++i;
			continue;
		}
		players_[i]->queue_free();
		players_.erase(players_.begin() + std::ptrdiff_t(i));
	}
}

void PreviewSoundVoices::stop() {
	pending_.clear();
	for (AudioStreamPlayer *player : players_) {
		player->stop();
		player->queue_free();
	}
	players_.clear();
}

void PreviewSoundVoices::forget() {
	pending_.clear();
	waves_.clear();
}

int PreviewSoundVoices::playing() const {
	int count = 0;
	for (const AudioStreamPlayer *player : players_) count += player->is_playing() ? 1 : 0;
	return count;
}

} // namespace godot
