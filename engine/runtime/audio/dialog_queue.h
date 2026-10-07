// The serialized mission-dialog queue and the PlayWavList dialog-id resolution,
// pushed down from the Godot mission audio (godot/src/audio/mission_audio). The
// engine plays one dialog audio channel at a time (Dialog_Register queues,
// Dialog_UpdatePlayback only loads the next clip once the active channel
// frees -- the witnesses sit on the two DialogQueue legs in dialog_queue.cpp),
// so resolved lines queue and play one after another instead of every
// PlayWavList firing at once. The shell spawns the voices and reports their
// channel edges here.
//
// A dialog line names a WAVE of the dialog bank's sounds (<bank>.lwf, else
// <bank>.pwf), an entry of the bank's singles found by its name, never a sound
// set: Dialog_LoadAudioClip hands the line's name to SoundBank_FindEntryByName
// over the dialog bank alone, plays that entry's wave and takes its byte +33 as
// the line's volume (docs/audio/lwf-dbf-sound-re.md "The dialog banks").
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::audio {

// The dialog a PlayWavList number plays: "dlg%03i" of it, 0 playing none
// [orig: Dialog_PlayByIndex @ 0x527ae0, the zero test @ 0x527af4, the sprintf @ 0x527b01];
// the PLYRDIALOG triggers form the same name from any number [orig:
// Dialog_ExistsByIndex @ 0x44e170 "dlg%.3d" @ 0x44e190]. "" for 0.
std::string dialog_name_of(int32_t dialog_index);
// The dialog of `name` in the bank, matched exactly and the first in the
// bank's order [orig: Dialog_PlayByName @ 0x44d9f0 strcmp @ 0x44da8b over the
// load-ordered list]; null for none.
const dbf::Group *find_dialog(const dbf::File &bank, const std::string &name);

// A wave of the dialog bank's sounds by its name: the first single of the name,
// without case, an 'LWF1' bank's any, another magic's only one whose dword +28
// is set [orig: SoundBank_FindEntryByName @ 0x75bba0, stricmp @ 0x75bbc2, the
// magic test @ 0x75bbdf]; null for none.
const lwf::Single *find_dialog_wave(const lwf::File &sounds, const std::string &name);
// The volume a line plays its wave at, from the wave's byte +33 (the high byte
// of lwf::Single::value_hi): a byte of 0 plays at full volume [orig:
// Dialog_LoadAudioClip @ 0x44dd15 -> sub_75BE10 @ 0x75be10; the play hook
// sub_527560 @ 0x527583..0x5275a6 scales a nonzero byte by the voice option
// and takes 255 for 0]. 0..255.
int dialog_wave_volume(const lwf::Single &wave);

// One queued dialog line: the wave it names, the file the dialog bank's sounds
// give that wave (empty when they have no wave of the name: the game shows "EX
// Cannot load audio" and plays nothing), its volume, and its dialog's name and
// its index in the dialog, which the authority reports on the wire as the line
// loads.
struct DialogLineRef {
	std::string wave;
	std::string file;
	int volume = 255;
	std::string dialog_name;
	int line = -1;
};

// The lines a PlayWavList dialog number plays: every line of the dialog
// "dlg%03i" names in the mission's dialog bank, in order, each with the wave
// the bank's sounds hold for it. Empty when the number is 0, the mission has no
// dialog bank or no dialog of the name.
// [orig: Dialog_PlayByIndex @ 0x527ae0 -> Dialog_PlayByName @ 0x44d9f0 ->
//  Dialog_Register @ 0x44d980 queue; Dialog_UpdatePlayback @ 0x44e470
//  advances only when the active channel frees, and reports each line it
//  loads (Server_SendEntityStateToAll @0x44e5a5); Dialog_LoadAudioClip
//  @ 0x44dcc0 the line's wave @ 0x44dcf7..0x44dd15]
std::vector<DialogLineRef> resolve_dialog_lines(const dbf::File *dialog_bank,
		const lwf::File *dialog_sounds, int32_t dialog_index);

// A line's subtitle: the mission text's [Mission Dialog] entry keyed by the
// line's wave name, else the text's flat entry whose index follows the last '_'
// of the line's sequence (none where it holds no '_'); "" for none.
// [orig: Dialog_LoadAudioClip @ 0x44ddcd..0x44de3c; IniSection_GetEntryByIndex
//  @ 0x75d130]
std::string dialog_line_text(const rtxt::File *mission_text, const dbf::Line &line);

// A co-op dialog line a client plays on S2C 0x28 (the effect pass's
// "dialog_line" record): the dialog by its exact name in the loaded bank's
// order, then the line's wave, its "<class><name>" variant first (the class
// is the local player's, the "%d%s" prefix) and its bare name next, and its
// subtitle (dialog_line_text). Retail plays the clip at once on a dialog
// channel of its own (no queue, nothing interrupted) and posts the subtitle
// as a system chat line (channel 1, colour 930); with no clip it posts "EX
// Cannot load audio: <name>" on chat channel 2 instead of playing. A
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
	std::string wave;        // the line's own wave name
	std::string played;      // the name the clip was found by ("<class><name>" or the name); empty for none
	std::string file;        // the clip's .wav; empty when the dialog bank's sounds hold neither name
	int volume = 255;
	std::string text;        // the subtitle; empty when none resolves
};
DialogLinePlayback resolve_dialog_line(const dbf::File *dialog_bank, const lwf::File *dialog_sounds,
		const rtxt::File *mission_text, const std::string &dialog_name, int line,
		int player_class);

// The one-channel dialog playback state machine: a FIFO of resolved lines
// behind the line the shell's channel is playing.
class DialogQueue {
public:
	// Queue resolved lines behind whatever plays (the Dialog_Register leg).
	void enqueue(const std::vector<DialogLineRef> &lines);
	// The Dialog_UpdatePlayback advance: while no line is active, hand out the
	// next queued line for the shell to start (a line without a clip, or one
	// that fails to spawn, is simply skipped by asking again, so the queue
	// never stalls). False when a line is still playing or nothing is queued.
	bool take_next(DialogLineRef &r_line);
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
	std::deque<DialogLineRef> pending_;
	bool line_active_ = false;
};

} // namespace opennova::audio
