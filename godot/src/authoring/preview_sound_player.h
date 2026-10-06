#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include <godot_cpp/classes/audio_stream_player.hpp>
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

} // namespace godot
