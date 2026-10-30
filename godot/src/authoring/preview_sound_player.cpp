#include "authoring/preview_sound_player.h"

#include <chrono>
#include <cstddef>

#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <cmath>

#include <base/io/fixed.h>
#include <editor/documents/music_bank_document.h>
#include <editor/project/project_files.h>
#include <formats/lwf/lwf.h>
#include <runtime/audio/sound_listener.h>

#include "audio/audio_out.h"

namespace godot {

namespace {

// A preview's hard pans for a stereo decode (a music bank's stream): the stereo blend table's left
// and right speakers.
constexpr uint8_t kLeftPan = 48;
constexpr uint8_t kRightPan = 208;

} // namespace

// Only the decodes in flight go here; the channels are the process mixer's.
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

void PreviewSoundPlayer::wait() {
	for (Voice &voice : voices_)
		if (voice.job.valid()) voice.job.wait();
}

void PreviewSoundPlayer::stop() {
	for (Voice &voice : voices_) {
		if (voice.job.valid()) voice.job = std::future<Decode>();
		AudioOut::mixer().reset(voice.handle);
		AudioOut::mixer().reset(voice.right_handle);
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
			voice.wave = opennova::audio::mix_wave_from_pcm(decoded.pcm, 0);
			if (decoded.pcm.channels == 2) voice.right = opennova::audio::mix_wave_from_pcm(decoded.pcm, 1);
			if (!voice.wave) continue;
			++made;
		}
		state_ = made ? State::Playing : State::Failed;
		playing_since_msec_ = Time::get_singleton()->get_ticks_msec();
	}
	if (state_ == State::Playing) {
		opennova::audio::Mixer &mixer = AudioOut::mixer();
		AudioOut::attach(parent_);
		const uint64_t elapsed = Time::get_singleton()->get_ticks_msec() - playing_since_msec_;
		bool sounding = false;
		for (Voice &voice : voices_) {
			if (!voice.wave) continue;
			if (!voice.started && elapsed >= uint64_t(voice.voice.start_ms > 0 ? voice.voice.start_ms : 0)) {
				const uint8_t volume = static_cast<uint8_t>(voice.voice.volume);
				voice.handle = mixer.find_and_open(voice.wave, voice.right ? kLeftPan : 0, volume,
						voice.voice.pitch_q16, 0);
				if (voice.right)
					voice.right_handle = mixer.find_and_open(voice.right, kRightPan, volume, voice.voice.pitch_q16, 0);
				voice.started = true;
			}
			sounding = sounding || !voice.started || mixer.validate(voice.handle) != 0;
		}
		if (!sounding) state_ = State::Ended;
	}
	return state_;
}

// --- PreviewWaveStreams ------------------------------------------------------------------------------

opennova::audio::MixWaveRef PreviewWaveStreams::stream(const std::string &p_file, bool &r_failed) {
	const opennova::editor::PreviewWaves::Wave wave = waves_.wave(p_file);
	r_failed = wave.state == opennova::editor::PreviewWaves::State::Failed;
	if (wave.state != opennova::editor::PreviewWaves::State::Decoded) return nullptr;
	Boxed &boxed = streams_[p_file];
	if (boxed.serial != wave.serial) {
		boxed.serial = wave.serial;
		boxed.wave = opennova::audio::mix_wave_from_pcm(*wave.pcm);
	}
	r_failed = !boxed.wave;
	return boxed.wave;
}

void PreviewWaveStreams::drop(const std::vector<std::string> &p_files) {
	for (const std::string &file : p_files) streams_.erase(file);
}

// --- PreviewSoundVoices ------------------------------------------------------------------------------

PreviewSoundVoices::~PreviewSoundVoices() {
	// A decode in flight is let finish.
	queue_.clear();
}

void PreviewSoundVoices::add(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices) {
	queue_.add(p_root, p_voices);
}

void PreviewSoundVoices::pump() {
	opennova::audio::Mixer &mixer = AudioOut::mixer();
	// The voices whose waves are decoded start, in the order they fired, each on the channel the
	// allocator picks.
	for (const opennova::editor::PreviewVoiceQueue::Ready &ready : queue_.take_ready()) {
		bool failed = false;
		const opennova::audio::MixWaveRef wave = streams_.stream(ready.file, failed);
		if (!wave) continue;
		AudioOut::attach(parent_);
		if (players_.size() >= kMaxPlayers) {
			mixer.reset(players_.front());
			players_.erase(players_.begin());
		}
		players_.push_back(mixer.find_and_open(wave, 0, static_cast<uint8_t>(ready.voice.volume),
				ready.voice.pitch_q16, 0));
		++started_;
	}
	// The voices that played through, dropped.
	for (size_t i = 0; i < players_.size();) {
		if (mixer.validate(players_[i]) != 0) {
			++i;
			continue;
		}
		players_.erase(players_.begin() + std::ptrdiff_t(i));
	}
}

void PreviewSoundVoices::stop() {
	queue_.clear();
	for (const uint32_t handle : players_) AudioOut::mixer().reset(handle);
	players_.clear();
}

void PreviewSoundVoices::refresh() {
	streams_.drop(queue_.refresh());
}

int PreviewSoundVoices::playing() const {
	int count = 0;
	for (const uint32_t handle : players_) count += AudioOut::mixer().validate(handle) != 0 ? 1 : 0;
	return count;
}

// --- PreviewSoundLoops -------------------------------------------------------------------------------

PreviewSoundLoops::~PreviewSoundLoops() {
	stop();
}

void PreviewSoundLoops::follow(const std::string &p_root, const std::vector<Channel> &p_channels, bool p_audible,
		const Transform3D *p_listener) {
	opennova::audio::Mixer &mixer = AudioOut::mixer();
	AudioOut::attach(parent_);
	// The listener: the device's camera in the mission frame's 16.16 units (presentation (x, y, z) is mission
	// (x, -z, y)), its yaw from its facing; none, every channel centred.
	opennova::audio::SoundListener listener;
	if (p_listener != nullptr) {
		const Vector3 origin = p_listener->origin;
		const int32_t at[3] = { int32_t(origin.x * opennova::io::kFp16One), int32_t(-origin.z * opennova::io::kFp16One),
			int32_t(origin.y * opennova::io::kFp16One) };
		const Vector3 facing = -p_listener->basis.get_column(2);
		listener.place(at, opennova::audio::listener_yaw(double(facing.x), double(-facing.z)));
	}
	// A voice for each channel, the mixer's eight at most, none past the last.
	const size_t kept = std::min<size_t>(p_channels.size(), size_t(opennova::audio::kAmbientMixChannels));
	while (voices_.size() > kept) {
		mixer.reset(voices_.back().handle);
		voices_.pop_back();
	}
	voices_.resize(kept);
	for (size_t i = 0; i < kept; ++i) {
		const Channel &channel = p_channels[i];
		Voice &voice = voices_[i];
		const std::string file = channel.path.empty() ? std::string() : opennova::editor::join_path(p_root, channel.path);
		// A channel another candidate took, or a free one: its voice starts anew or stops.
		if (voice.started != channel.started || voice.file != file) {
			mixer.reset(voice.handle);
			voice = Voice();
			voice.started = channel.started;
			voice.file = file;
		}
		if (file.empty() || voice.failed) continue;
		uint8_t pan = 0;
		if (p_listener != nullptr) {
			const int32_t dx = int32_t(uint32_t(int32_t(channel.at[0] * opennova::io::kFp16One)) -
					uint32_t(listener.position()[0]));
			const int32_t dy = int32_t(uint32_t(int32_t(-channel.at[2] * opennova::io::kFp16One)) -
					uint32_t(listener.position()[1]));
			pan = opennova::audio::positional_pan(dx, dy, listener.yaw(), false);
		}
		// A held voice plays on silent where it stands (the mixer has no pause).
		const uint8_t volume = p_audible ? static_cast<uint8_t>(channel.volume) : 0;
		if (voice.handle == 0) {
			bool failed = false;
			const opennova::audio::MixWaveRef wave = waves_.stream(file, failed);
			voice.failed = failed;
			if (!wave) continue;
			voice.handle = mixer.open_looping(opennova::audio::kFirstAmbientChannel + int(i),
					opennova::audio::voice_params(wave, pan, volume, channel.pitch_q16));
			++started_;
		} else {
			mixer.set_and_play(voice.handle, pan, volume, channel.pitch_q16);
		}
		voice.paused = !p_audible;
	}
}

void PreviewSoundLoops::listen(const std::string &p_root,
		const std::vector<opennova::editor::MissionSoundChannel> &p_channels, bool p_on, uint64_t p_presented,
		const Transform3D *p_listener) {
	if (!p_on) {
		stop();
		idle_frames_ = kHeldFrames;
		return;
	}
	idle_frames_ = p_presented != presented_ ? 0 : std::min(idle_frames_ + 1, kHeldFrames);
	presented_ = p_presented;
	std::vector<Channel> channels;
	channels.reserve(p_channels.size());
	for (const opennova::editor::MissionSoundChannel &each : p_channels) {
		Channel channel;
		channel.started = each.started;
		channel.path = each.candidate >= 0 ? each.path : std::string();
		channel.volume = each.volume;
		channel.pitch_q16 = each.pitch_q16;
		channel.at[0] = each.at.x;
		channel.at[1] = each.at.y;
		channel.at[2] = each.at.z;
		channels.push_back(std::move(channel));
	}
	follow(p_root, channels, audible(), p_listener);
}

void PreviewSoundLoops::stop() {
	for (const Voice &voice : voices_) AudioOut::mixer().reset(voice.handle);
	voices_.clear();
}

int PreviewSoundLoops::voices() const {
	int count = 0;
	for (const Voice &voice : voices_) count += voice.handle != 0 ? 1 : 0;
	return count;
}

int PreviewSoundLoops::playing() const {
	int count = 0;
	for (const Voice &voice : voices_) count += voice.handle != 0 && !voice.paused ? 1 : 0;
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
