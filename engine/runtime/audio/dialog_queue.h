// The serialized mission-dialog queue, the PlayWavList dialog-id resolution
// and the WAC scripted-voice channel, pushed down from the Godot mission
// audio (godot/src/audio/mission_audio). The
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
#include <formats/rtxt/rtxt.h>
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

// A co-op dialog line a client plays on S2C 0x28 (the effect pass's
// "dialog_line" record): the dialog by its exact name in the loaded bank's
// order, then the line's clip, its "<class><name>" variant first (the class
// is the local player's, the "%d%s" locale prefix) and its bare def name
// next, and its subtitle: the mission text's [Mission Dialog] entry keyed by
// the def name, else the text entry whose flat index follows the last '_' of
// the line's sequence string. Retail plays the clip at once on a dialog
// channel of its own (no queue, nothing interrupted) and posts the subtitle
// as a system chat line (channel 1, colour 930); with no clip it posts "EX
// Cannot load audio: <def name>" on chat channel 2 instead of playing. A
// dialog or line the bank lacks resolves to nothing (retail reads past the
// line table there; the port refuses).
// [orig: Dialog_PlayByNameAndSlot @0x44E3F0 (the list walk @0x44e3fa..0x44e43e);
//  Dialog_LoadAudioClipLocalized @0x44DEF0 — the clip @0x44df2e..0x44df96, the
//  missing-audio line @0x44dfd1..0x44dff8, the subtitle @0x44e054..0x44e0c2, the
//  play @0x44e0d7..0x44e136, the chat @0x44e144..0x44e150; the hooks
//  DialogSystem_Init installs @0x527665 (sub_527560, the master-volume gate) /
//  @0x52766F (Chat_AddSystemMessageIfValid)]
struct DialogLinePlayback {
	bool line_found = false;
	std::string def_id_name; // the line's own sound name
	std::string set_name;    // the clip to play; empty when the banks carry neither name
	std::string text;        // the subtitle; empty when none resolves
};
DialogLinePlayback resolve_dialog_line(const dbf::File *dialog_bank, const SoundSetIndex &sets,
		const rtxt::File *mission_text, const std::string &dialog_name, int line,
		int player_class);

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
    // Dialog_ResetAll clears waiting lines; the physical voice keeps playing.
    // [orig: @0x44dc90]
    void discard_pending() { pending_.clear(); }

private:
	std::deque<std::string> pending_;
	bool line_active_ = false;
};

} // namespace opennova::audio
