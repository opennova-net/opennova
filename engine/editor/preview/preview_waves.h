#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <vector>

#include <editor/session/view/workspace_view.h>
#include <formats/lwf/wav_pcm.h>

namespace opennova::editor {

// The project's waves as the preview decodes them, each once: decoded as the game decodes it
// (lwf::wav_decode_pcm16) on a worker, read from the file at its full path, and kept until that file
// changes. What the Shell's clip-sound and Listen players share (godot/src/authoring/preview_sound_player).
class PreviewWaves {
public:
	enum class State : uint8_t { Decoding, Decoded, Failed };
	struct Wave {
		State state = State::Decoding;
		// The decoded wave (null unless Decoded), and its decode's serial: a wave decoded anew (its file
		// changed) has another.
		const lwf::WavPcm *pcm = nullptr;
		uint64_t serial = 0;
		std::string error; // why it failed
	};

	PreviewWaves() = default;
	PreviewWaves(const PreviewWaves &) = delete;
	PreviewWaves &operator=(const PreviewWaves &) = delete;
	// A decode in flight is let finish (its future's end waits for it).
	~PreviewWaves() = default;

	// The wave at the full path `file`, its decode begun on a worker the first time it is asked.
	Wave wave(const std::string &file);
	// The project's files moved (a rescan, a document edited): each wave whose file changed on disk since its
	// decode began (its size or its last write), or is gone, or came to be, dropped and decoded anew when next
	// asked; the rest kept as they stand, a decode in flight among them. The files dropped, in order.
	std::vector<std::string> refresh();
	// Every wave dropped.
	void forget() { waves_.clear(); }
	// How many decode still.
	int decoding() const;

private:
	struct Decode {
		bool decoded = false;
		std::string error;
		lwf::WavPcm pcm;
	};
	// A file as its decode found it: its size and last write, or none.
	struct DiskStamp {
		bool exists = false;
		uint64_t size = 0;
		int64_t written = 0;
		bool operator==(const DiskStamp &other) const {
			return exists == other.exists && size == other.size && written == other.written;
		}
	};
	struct Entry {
		DiskStamp stamp;
		std::future<Decode> job; // valid while the decode runs
		Decode done;
		bool finished = false;
		uint64_t serial = 0;
	};
	static DiskStamp stamp_of(const std::string &file);

	std::map<std::string, Entry> waves_; // by the wave's full path
	uint64_t serials_ = 0;
};

// The clip sounds' voices waiting on their waves (DI-04's player): each sound's voices, as the session fired them,
// wait until their waves decode, then start in the order they fired. The project's files moving keeps them waiting
// (a sound fired in the frame a save or an edit lands still plays); one whose wave's file changed or went is dropped
// with that wave, as is one whose wave does not decode.
class PreviewVoiceQueue {
public:
	struct Ready {
		std::string file; // the wave's full path
		WorkspaceView::Voice voice;
	};

	explicit PreviewVoiceQueue(PreviewWaves &waves) : waves_(waves) {}

	// `voices`' waves (project-relative paths under `root`), each decode begun.
	void add(const std::string &root, const std::vector<WorkspaceView::Voice> &voices);
	// The voices whose waves are decoded, in the order they fired, taken; those whose wave failed dropped; the rest
	// wait on.
	std::vector<Ready> take_ready();
	// The project's files moved: the waves whose files changed or went dropped (PreviewWaves::refresh) and the voices
	// waiting on them with them; the rest wait on, their decodes in flight. The files dropped.
	std::vector<std::string> refresh();
	// Every voice waiting dropped (the project closing); the waves kept.
	void clear() { waiting_.clear(); }
	size_t waiting() const { return waiting_.size(); }

private:
	PreviewWaves &waves_;
	std::vector<Ready> waiting_;
};

} // namespace opennova::editor
