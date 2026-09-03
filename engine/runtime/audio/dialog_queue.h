// The serialized mission-dialog queue, the PlayWavList dialog-id resolution
// and the WAC scripted-voice channel, pushed down from the Godot mission
// audio (godot/src/audio/mission_audio, the former mission_audio.gd). The
// engine plays one dialog audio channel at a time (Dialog_Register queues,
// Dialog_UpdatePlayback only loads the next clip once the active channel
// frees -- the witnesses sit on the two DialogQueue legs in dialog_queue.cpp),
// so resolved line set-names queue and play one after another instead of
// every PlayWavList firing at once. The shell spawns the voices and reports
// their channel edges here.
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include <formats/dbf/dbf.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::audio {

// Resolve a PlayWavList dialog id (param1) to the ordered set name(s) it
// should play. Faithful first: dialog id "dlg%03d" -> the co-named .DBF ->
// the group's def_id set name(s) (played in sequence, one per dialog "line"
// as the engine advances entry index in Dialog_UpdatePlayback), each kept
// only when the loaded banks carry it; without a .DBF (or with no carried
// line), the first direct set-name form the banks contain: "DLG%03d",
// "dlg%03d", then the bare number. Empty when nothing resolves.
// [orig: Dialog_PlayByIndex @ 0x527ae0 -> Dialog_PlayByName @ 0x44d9f0 ->
//  Dialog_Register @ 0x44d980 queue; Dialog_UpdatePlayback @ 0x44e470
//  advances only when the active channel frees]
std::vector<std::string> resolve_dialog_sets(const dbf::File *dialog_bank,
		const SoundSetIndex &sets, int wav_id);

// The one-channel dialog playback state machine: a FIFO of resolved line
// set-names behind the line the shell's channel is playing.
class DialogQueue {
public:
	// Queue resolved line set-names behind whatever plays (the Dialog_Register
	// leg).
	void enqueue(const std::vector<std::string> &set_names);
	// The Dialog_UpdatePlayback advance: while no line is active, hand out the
	// next queued name for the shell to start (a line that fails to spawn is
	// simply skipped by asking again, so the queue never stalls). False when a
	// line is still playing or nothing is queued.
	bool take_next(std::string &r_set_name);
	// The shell's channel took the line it was handed.
	void line_started();
	// The active channel freed.
	void line_finished();
	bool line_active() const { return line_active_; }
	size_t pending() const { return pending_.size(); }
	// Mission teardown / repeated setup: nothing queued or active crosses
	// missions.
	void clear();

private:
	std::deque<std::string> pending_;
	bool line_active_ = false;
};

// WAC wave/pwave scripted voice: a single dedicated channel the engine RESETS
// before each play (a new wave interrupts the previous one), independent of
// the .DBF dialog queue [orig: wave/pwave @ 0x4ed610, channel dword_C6EC30].
class WacVoiceChannel {
public:
	// Play a scripted voice .wav by filename [orig: wave/pwave @ 0x4ed610] on
	// the one channel, REPLACING any currently-playing wave (the engine resets
	// the channel before each play [orig: AudioChannel_ResetByHandle(dword_C6EC30)
	// @ 0x4ed625]), so a new scripted line interrupts the previous one.
	// Returns true when a previous wave was interrupted.
	bool play(const std::string &filename);
	void stop();
	bool playing() const { return playing_; }
	const std::string &current() const { return current_; }

private:
	std::string current_;
	bool playing_ = false;
};

} // namespace opennova::audio
