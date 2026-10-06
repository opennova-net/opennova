#pragma once

#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/node.hpp>

#include <editor/session/view/workspace_view.h>
#include <formats/lwf/wav_pcm.h>

namespace godot {

// The editor's one preview sound player (the sound lane, DI-02): the device half of the workspace's sound
// (WorkspaceView::Sound, the session's play_sound). Each voice is a project wave decoded as the game decodes
// it (lwf::wav_decode_pcm16, boxed by WavLoader) on a worker, then played at once with the others at the
// pitch and volume the session's pick gave it (preview/sound_preview.h: the set's and the member's pitch
// composed, the member's volume): the picks are the session's, the player only sounds them. A play in
// place of one playing stops it. Every later preview (a clip's footsteps, a menu's sounds) plays through it.
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
	};
	Node *parent_ = nullptr;
	std::vector<Voice> voices_;
	State state_ = State::Idle;
	std::string path_;
};

// The clip sounds' player (DI-04): the device half of the sounds a clip's events fire in the model preview
// (ProjectSession::clip_sounds_since). Each sound's voices start together beside those still playing, a
// player each, freed as it ends; a wave is decoded as the game decodes it once (lwf::wav_decode_pcm16 on a
// worker) and kept until the project's files move (forget). The picks, pitches and volumes are the
// session's; this only sounds them.
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
	// The decoded waves dropped (the project's files moved).
	void forget();
	// How many voices it started in all, and how many play now (the tests' measure).
	uint64_t started() const { return started_; }
	int playing() const;

private:
	struct Decode {
		bool decoded = false;
		std::string error;
		opennova::lwf::WavPcm pcm;
	};
	struct Wave {
		std::shared_future<Decode> job;
		Ref<AudioStreamWAV> stream;
		bool failed = false;
	};
	// The players kept at most: the oldest stopped for a new one past it.
	static constexpr size_t kMaxPlayers = 24;
	Node *parent_ = nullptr;
	std::map<std::string, Wave> waves_; // by the wave's full path
	std::vector<std::pair<std::string, opennova::editor::WorkspaceView::Voice>> pending_;
	std::vector<AudioStreamPlayer *> players_;
	uint64_t started_ = 0;
};

} // namespace godot
