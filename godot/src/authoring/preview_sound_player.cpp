#include "authoring/preview_sound_player.h"

#include <chrono>
#include <cstddef>

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <cmath>

#include <editor/documents/music_bank_document.h>
#include <editor/project/project_files.h>
#include <runtime/audio/volume_law.h>

#include "audio/sound_bank.h"
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
		const int32_t stream = voice.stream;
		made.job = std::async(std::launch::async, [file, stream]() {
			Decode out;
			std::vector<uint8_t> bytes;
			if (!opennova::io::read_file_bytes(file, bytes, out.error)) return out;
			// A music bank's stream (round S23 lane A), decoded as the game streams it; else a wave as the game loads it.
			out.decoded = stream >= 0 ? opennova::editor::music_stream_pcm(bytes, stream, out.pcm, out.error)
			                          : opennova::lwf::wav_decode_pcm16(bytes.data(), bytes.size(), out.pcm, out.error);
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
		// Every decode done: each voice that decoded starts, together, as the game starts a set's layers (a voice
		// with a start of its own, a dialog's later line, when its time comes, below).
		size_t made = 0;
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
			// Each its own name, in the voices' order (a dialog's lines, DI-32): PreviewSound, PreviewSound2, ...
			voice.player->set_name(made ? String("PreviewSound") + String::num_int64(int64_t(made) + 1) : String("PreviewSound"));
			voice.player->set_stream(stream);
			voice.player->set_pitch_scale(SoundBank::pitch_scale_from_q16(voice.voice.pitch_q16));
			voice.player->set_volume_db(opennova::audio::volume_db_from_byte(voice.voice.volume));
			parent_->add_child(voice.player);
			++made;
		}
		state_ = made ? State::Playing : State::Failed;
		playing_since_msec_ = Time::get_singleton()->get_ticks_msec();
	}
	if (state_ == State::Playing) {
		const uint64_t elapsed = Time::get_singleton()->get_ticks_msec() - playing_since_msec_;
		bool sounding = false;
		for (Voice &voice : voices_) {
			if (voice.player == nullptr) continue;
			if (!voice.started && elapsed >= uint64_t(voice.voice.start_ms > 0 ? voice.voice.start_ms : 0)) {
				voice.player->play();
				voice.started = true;
			}
			sounding = sounding || !voice.started || voice.player->is_playing();
		}
		if (!sounding) state_ = State::Ended;
	}
	return state_;
}

// --- PreviewWaveStreams ------------------------------------------------------------------------------

Ref<AudioStreamWAV> PreviewWaveStreams::stream(const std::string &p_file, bool &r_failed) {
	const opennova::editor::PreviewWaves::Wave wave = waves_.wave(p_file);
	r_failed = wave.state == opennova::editor::PreviewWaves::State::Failed;
	if (wave.state != opennova::editor::PreviewWaves::State::Decoded) return Ref<AudioStreamWAV>();
	Boxed &boxed = streams_[p_file];
	if (boxed.serial != wave.serial) {
		boxed.serial = wave.serial;
		boxed.stream = WavLoader::from_pcm(*wave.pcm);
	}
	r_failed = boxed.stream.is_null();
	return boxed.stream;
}

void PreviewWaveStreams::drop(const std::vector<std::string> &p_files) {
	for (const std::string &file : p_files) streams_.erase(file);
}

// --- PreviewSoundVoices ------------------------------------------------------------------------------

PreviewSoundVoices::~PreviewSoundVoices() {
	// The players are the parent's children, freed with it; a decode in flight is let finish.
	queue_.clear();
}

void PreviewSoundVoices::add(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices) {
	queue_.add(p_root, p_voices);
}

void PreviewSoundVoices::pump() {
	// The voices whose waves are decoded start, in the order they fired.
	for (const opennova::editor::PreviewVoiceQueue::Ready &ready : queue_.take_ready()) {
		bool failed = false;
		const Ref<AudioStreamWAV> stream = streams_.stream(ready.file, failed);
		if (stream.is_null()) continue;
		if (players_.size() >= kMaxPlayers) {
			players_.front()->stop();
			players_.front()->queue_free();
			players_.erase(players_.begin());
		}
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		player->set_name("ClipSound");
		player->set_stream(stream);
		player->set_pitch_scale(SoundBank::pitch_scale_from_q16(ready.voice.pitch_q16));
		player->set_volume_db(opennova::audio::volume_db_from_byte(ready.voice.volume));
		parent_->add_child(player);
		player->play();
		players_.push_back(player);
		++started_;
	}
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
	queue_.clear();
	for (AudioStreamPlayer *player : players_) {
		player->stop();
		player->queue_free();
	}
	players_.clear();
}

void PreviewSoundVoices::refresh() {
	streams_.drop(queue_.refresh());
}

int PreviewSoundVoices::playing() const {
	int count = 0;
	for (const AudioStreamPlayer *player : players_) count += player->is_playing() ? 1 : 0;
	return count;
}

// --- PreviewSoundLoops -------------------------------------------------------------------------------

PreviewSoundLoops::~PreviewSoundLoops() {
	// The players are the parent's children, freed with it; a decode in flight is let finish.
	voices_.clear();
}

void PreviewSoundLoops::follow(const std::string &p_root, const std::vector<Channel> &p_channels, float p_volume,
		bool p_audible) {
	// A voice for each channel, none past the last.
	while (voices_.size() > p_channels.size()) {
		Voice &last = voices_.back();
		if (last.player != nullptr) {
			last.player->stop();
			last.player->queue_free();
		}
		voices_.pop_back();
	}
	voices_.resize(p_channels.size());
	const double master = std::clamp(double(p_volume), 0.0, 1.0);
	for (size_t i = 0; i < p_channels.size(); ++i) {
		const Channel &channel = p_channels[i];
		Voice &voice = voices_[i];
		const std::string file = channel.path.empty() ? std::string() : opennova::editor::join_path(p_root, channel.path);
		// Another candidate took the channel (or it went free): its voice goes, a new one starts at its wave's start.
		if (voice.started != channel.started || voice.file != file) {
			if (voice.player != nullptr) {
				voice.player->stop();
				voice.player->queue_free();
			}
			voice = Voice();
			voice.started = channel.started;
			voice.file = file;
		}
		if (file.empty() || voice.failed) continue;
		if (voice.player == nullptr) {
			bool failed = false;
			const Ref<AudioStreamWAV> decoded = waves_.stream(file, failed);
			voice.failed = failed;
			if (decoded.is_null()) continue;
			AudioStreamPlayer3D *player = memnew(AudioStreamPlayer3D);
			player->set_name(String("ListenChannel") + String::num_int64(int64_t(i)));
			// Its own looping copy of the whole wave (the cached stream stays a one-shot's) [the native ambient loop,
			// D-SND-6: a channel's descriptor loops].
			player->set_stream(SoundBank::loop_copy(decoded));
			// The mix owns the volume: no attenuation on top, only the panner (the game's ambient channels, D-SND-8).
			SoundBank::configure_unattenuated_3d(player);
			parent_->add_child(player);
			voice.player = player;
			// Started once from its wave's beginning; held and let go by the pause alone after.
			player->play();
			voice.paused = false;
			++started_;
		}
		AudioStreamPlayer3D *player = voice.player;
		player->set_position(Vector3(channel.at[0], channel.at[1], channel.at[2]));
		const double linear = double(std::max(channel.volume, 0)) / double(opennova::audio::kVolumeByteMax) * master;
		player->set_volume_db(float(linear > 0.0 ? opennova::audio::volume_db_from_byte(int32_t(std::lround(linear * 255.0)))
		                                         : opennova::audio::kVolumeSilentDb));
		player->set_pitch_scale(float(SoundBank::pitch_scale_from_q16(channel.pitch_q16)));
		// Held or let go each frame: a 3D player's playback starts at its next physics step, and a pause asked of one
		// not started yet is not kept.
		player->set_stream_paused(!p_audible);
		voice.paused = !p_audible;
	}
}

void PreviewSoundLoops::stop() {
	for (Voice &voice : voices_)
		if (voice.player != nullptr) {
			voice.player->stop();
			voice.player->queue_free();
		}
	voices_.clear();
}

int PreviewSoundLoops::voices() const {
	int count = 0;
	for (const Voice &voice : voices_) count += voice.player != nullptr ? 1 : 0;
	return count;
}

int PreviewSoundLoops::playing() const {
	int count = 0;
	for (const Voice &voice : voices_) count += voice.player != nullptr && !voice.paused ? 1 : 0;
	return count;
}

int PreviewSoundLoops::decoding() const {
	return waves_.decoding();
}

int PreviewSoundLoops::failed() const {
	int count = 0;
	for (const Voice &voice : voices_) count += voice.failed ? 1 : 0;
	return count;
}

} // namespace godot
