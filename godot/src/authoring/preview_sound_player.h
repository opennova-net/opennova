#pragma once

#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <editor/preview/preview_waves.h>
#include <editor/session/view/workspace_view.h>
#include <formats/lwf/wav_pcm.h>

namespace godot {

// The editor's one preview sound player (the sound lane, DI-02): the device half of the workspace's sound
// (WorkspaceView::Sound, the session's play_sound). Each voice is a project wave decoded as the game decodes
// it (lwf::wav_decode_pcm16, boxed by WavLoader) on a worker, then played at once with the others at the
// pitch and volume the session's pick gave it (preview/sound_preview.h: the set's and the member's pitch
// composed, the member's volume): the picks are the session's, the player only sounds them. A voice that
// starts later (a dialog's next line, DI-32: WorkspaceView::Voice::start_ms) starts that long after the
// others. A play in place of one playing stops it. Every later preview (a clip's footsteps, a menu's sounds)
// plays through it.
class PreviewSoundPlayer {
public:
	enum class State { Idle, Decoding, Playing, Ended, Failed };

	explicit PreviewSoundPlayer(Node *p_parent) : parent_(p_parent) {}
	~PreviewSoundPlayer();
	PreviewSoundPlayer(const PreviewSoundPlayer &) = delete;
	PreviewSoundPlayer &operator=(const PreviewSoundPlayer &) = delete;

	// `p_voices`' waves (project-relative paths under `p_root`) decoded off the frame, in place of any play.
	void play(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices);
	void stop();
	// Starts the voices once every decode is done; how the play stands (`r_error` why it failed: every
	// voice failed to decode). Ended once every voice played through.
	State pump(std::string &r_error);
	State state() const { return state_; }
	// The first voice's wave ("" idle).
	const std::string &path() const { return path_; }

private:
	struct Decode {
		bool decoded = false;
		std::string error;
		opennova::lwf::WavPcm pcm;
	};
	struct Voice {
		opennova::editor::WorkspaceView::Voice voice;
		std::future<Decode> job;
		AudioStreamPlayer *player = nullptr;
		bool started = false;
	};
	Node *parent_ = nullptr;
	std::vector<Voice> voices_;
	State state_ = State::Idle;
	std::string path_;
	uint64_t playing_since_msec_ = 0; // when the decodes were done and the first voices started
};

// The project's waves as the preview plays them: each decoded once as the game decodes it (editor::PreviewWaves, on
// a worker, kept until its file changes) and boxed as a stream (WavLoader). What the clip sounds' and the Listen's
// players share.
class PreviewWaveStreams {
public:
	// The wave at the full path `p_file`: its stream once decoded, null meanwhile (its decode begun on a worker the
	// first time it is asked) and for one that did not decode (`r_failed`).
	Ref<AudioStreamWAV> stream(const std::string &p_file, bool &r_failed);
	// The streams of `p_files` dropped (their waves decoded anew: their files changed).
	void drop(const std::vector<std::string> &p_files);
	void forget() {
		waves_.forget();
		streams_.clear();
	}
	// How many decode still.
	int decoding() const { return waves_.decoding(); }
	opennova::editor::PreviewWaves &waves() { return waves_; }

private:
	struct Boxed {
		uint64_t serial = 0; // the decode it boxes
		Ref<AudioStreamWAV> stream;
	};
	opennova::editor::PreviewWaves waves_;
	std::map<std::string, Boxed> streams_; // by the wave's full path
};

// The clip sounds' player (DI-04): the device half of the sounds a clip's events fire in the model preview
// (ProjectSession::clip_sounds_since). Each sound's voices start together beside those still playing, a
// player each, freed as it ends, once their waves decode (editor::PreviewVoiceQueue: a wave decoded as the
// game decodes it once, kept until its file changes). The picks, pitches and volumes are the session's;
// this only sounds them.
class PreviewSoundVoices {
public:
	explicit PreviewSoundVoices(Node *p_parent) : parent_(p_parent) {}
	~PreviewSoundVoices();
	PreviewSoundVoices(const PreviewSoundVoices &) = delete;
	PreviewSoundVoices &operator=(const PreviewSoundVoices &) = delete;

	// `p_voices`' waves (project-relative paths under `p_root`) started as soon as each is decoded.
	void add(const std::string &p_root, const std::vector<opennova::editor::WorkspaceView::Voice> &p_voices);
	// Starts the voices decoded since, and frees the players that ended.
	void pump();
	// Every voice stopped and dropped (the project closing); the decoded waves kept.
	void stop();
	// The project's files moved (a rescan): the waves whose files changed or went decoded anew and the voices
	// waiting on them dropped; the rest wait on, their decodes in flight (PreviewVoiceQueue::refresh).
	void refresh();
	// How many voices it started in all, and how many play now (the tests' measure).
	uint64_t started() const { return started_; }
	int playing() const;

private:
	// The players kept at most: the oldest stopped for a new one past it.
	static constexpr size_t kMaxPlayers = 24;
	Node *parent_ = nullptr;
	PreviewWaveStreams streams_;
	opennova::editor::PreviewVoiceQueue queue_{ streams_.waves() }; // after streams_: it reads its waves
	std::vector<AudioStreamPlayer *> players_;
	uint64_t started_ = 0;
};

// The Listen's channels (DI-36): the device half of a mission view's MissionListen. Each channel its mix binds plays its
// wave looping on an AudioStreamPlayer3D of its own at the channel's place in the device's world, whose camera is the
// listener, so Godot's panner pans it as the game's own ambient channels are panned (D-SND-8), unattenuated (the volume
// is the mix's, through the engine's volume law, times the master); its pitch the member's times the registration's.
// A channel another candidate took (its `started` moved) starts its wave again from its beginning, as the game's
// channel opened anew does (D-SND-6); a free one stops. The picks, the places and the volumes are the session's; this
// only sounds them.
class PreviewSoundLoops {
public:
	struct Channel {
		uint64_t started = 0;
		std::string path; // the project-relative wave ("" a free channel)
		int32_t volume = 0;
		uint32_t pitch_q16 = 0x10000;
		float at[3] = { 0.0f, 0.0f, 0.0f }; // the presentation frame
	};

	explicit PreviewSoundLoops(Node3D *p_parent) : parent_(p_parent) {}
	~PreviewSoundLoops();
	PreviewSoundLoops(const PreviewSoundLoops &) = delete;
	PreviewSoundLoops &operator=(const PreviewSoundLoops &) = delete;

	// The channels as they stand now (channel i the i-th) of the project under `p_root`, at the master `p_volume`
	// (0..1); `p_audible` false holds each voice paused where it stands (the picture not drawn).
	void follow(const std::string &p_root, const std::vector<Channel> &p_channels, float p_volume, bool p_audible);
	// Every voice stopped and freed; the decoded waves kept.
	void stop();
	// The decoded waves dropped (the project's files moved).
	void forget() { waves_.forget(); }
	// The voices it holds, those playing (started and not held), those whose wave still decodes, those whose wave did
	// not decode; the voices it started in all.
	int voices() const;
	int playing() const;
	int decoding() const;
	int failed() const;
	uint64_t started() const { return started_; }

private:
	struct Voice {
		uint64_t started = 0;
		std::string file;
		AudioStreamPlayer3D *player = nullptr;
		bool failed = false;
		bool paused = false; // held (the picture not drawn)
	};
	Node3D *parent_ = nullptr;
	PreviewWaveStreams waves_;
	std::vector<Voice> voices_;
	uint64_t started_ = 0;
};

} // namespace godot
