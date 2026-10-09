// The mission-dialog playback and the PlayWavList dialog-id resolution. The
// world owns the one dialog table (world::ScriptState::dialog): the BMS
// PlayWavList action registers a dialog in it, the PLYRDIALOG triggers read it,
// and the round resets clear it. Each played dialog takes one of 16 slots,
// walked once a 62 Hz tick: a slot loads its next line once the line before it
// has held for about twice its wave's length, the dialog channel is free and
// the line's own delay has run out, so separate dialogs interleave on the one
// channel in slot order (Dialog_Register / Dialog_UpdatePlayback /
// Dialog_LoadAudioClip; the witnesses sit on DialogQueue's legs in
// dialog_queue.cpp). The shell is the device: it runs the tick, plays the wave
// a line loads, says how long it is and whether its voice still plays.
//
// A dialog line names a WAVE of the dialog bank's sounds (<bank>.lwf, else
// <bank>.pwf), an entry of the bank's singles found by its name, never a sound
// set: Dialog_LoadAudioClip hands the line's name to SoundBank_FindEntryByName
// over the dialog bank alone, plays that entry's wave and takes its byte +33 as
// the line's volume (docs/audio/lwf-dbf-sound-re.md "The dialog banks").
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <base/io/tick_rate.h>
#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::audio {

// The dialog a PlayWavList number plays: "dlg%03i" of it, 0 playing none
// [orig: Dialog_PlayByIndex @ 0x527ae0, the zero test @ 0x527af4, the sprintf @ 0x527b01].
// "" for 0. (The PLYRDIALOG triggers form "dlg%.3d" of any number instead,
// which differs for -99..-1: DialogQueue::active / finished.)
std::string dialog_name_of(int32_t dialog_index);
// The number whose "dlg%03i" forms `name` (dlg012: 12, dlg1234: 1234), the
// inverse of the formation above, matched exactly; -1 where no number forms the
// name (dlg12, dlg0012, DLG012, intro), which no number's lookup finds.
int64_t dialog_index_of(const std::string &name);
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

// One dialog line: the wave it names, the file the dialog bank's sounds give
// that wave (empty when they have no wave of the name: the game shows "EX
// Cannot load audio" and plays nothing), its volume, the line's DELAY byte
// (how long it waits, in tenths of a second, once the line before it has held
// and the channel is free: 62 * DELAY / 10 ticks), and its dialog's name and
// its index in the dialog, which the authority reports on the wire as the
// line loads.
struct DialogLineRef {
	std::string wave;
	std::string file;
	int volume = 255;
	uint8_t delay = 0;
	std::string dialog_name;
	int line = -1;
};

// The lines a PlayWavList dialog number plays: every line of the dialog
// "dlg%03i" names in the mission's dialog bank, in order, each with the wave
// the bank's sounds hold for it. Empty when the number is 0, the mission has no
// dialog bank or no dialog of the name.
// [orig: Dialog_PlayByIndex @ 0x527ae0 -> Dialog_PlayByName @ 0x44d9f0 ->
//  Dialog_Register @ 0x44d980; Dialog_UpdatePlayback @ 0x44e470 reports each
//  line it loads (Server_SendEntityStateToAll @0x44e5a5) and reads its delay
//  (line byte +0x35 @ 0x44e563); Dialog_LoadAudioClip @ 0x44dcc0 the line's
//  wave @ 0x44dcf7..0x44dd15]
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

// The dialog clock: Dialog_UpdatePlayback runs once a logic tick and its rate
// word is 62 [orig: Game_ProcessMainFrame @ 0x5265af; DialogSystem_Init
// @ 0x52765e..0x527660 -> sub_44DC60 @ 0x44dc60 into dword_A8A238].
inline constexpr uint32_t kDialogTickRate = static_cast<uint32_t>(io::kTicksPerSecondInt);
// The hold of a line whose wave did not load, or whose rate is 0 [orig:
// Dialog_LoadAudioClip @ 0x44dd7c, @ 0x44ddbf].
inline constexpr uint32_t kDialogMissingHold = 12;
// The dialogs that play at once: Dialog_Register drops a seventeenth [orig:
// Dialog_Register @ 0x44d9c3].
inline constexpr size_t kDialogSlots = 16;
// The registered-dialog history: 256 entries whose count saturates at 255, so
// the 256th registration and every one after it overwrite entry 255, which no
// scan reaches [orig: dword_A89600 / count dword_A895F8; Dialog_Register
// @ 0x44d98d, @ 0x44d99c..0x44d9a3; its only reader sub_44E220 @ 0x44e253].
inline constexpr size_t kDialogHistory = 256;

// What the device loaded for a line, Dialog_LoadAudioClip's clip: whether its
// wave loaded (the bank's sounds hold one of the line's name and it decoded),
// the sample count and pitch ratio the game's wave loader records for it
// (lwf::WavPcm::loader_samples / loader_pitch_q16) and the voice it plays on,
// 0 for none (no wave, or nothing played).
struct DialogClip {
	bool loaded = false;
	uint32_t samples = 0;
	uint32_t pitch_q16 = 0;
	uint64_t voice = 0;
};

// How long a loaded line holds its dialog before the next line may load, in
// ticks: 2 * ((62 * samples + rate) / rate) with rate = (44100 * pitch + 0x8000)
// >> 16, about twice the wave's length; 12 for a wave that did not load or a
// rate of 0 [orig: Dialog_LoadAudioClip @ 0x44dd87..0x44ddbf].
uint32_t dialog_clip_hold(const DialogClip &clip);

// The dialog table and the dialog channel's voices: the registered history and
// the 16 slots (Dialog_Register, Dialog_UpdatePlayback, Dialog_ResetAll, the
// PLYRDIALOG readers) and the dialog channel stack, the shell supplying the
// device (the wave a line loads, whether a voice still plays). The world owns
// the one instance (world::ScriptState::dialog).
class DialogQueue {
public:
	// One slot's dialog: its lines, the next to load, the timer (a line's hold,
	// or with bit 31 its delay) and whether the timer counts down yet.
	struct Slot {
		std::string dialog;
		std::vector<DialogLineRef> lines;
		uint32_t index = 0;
		uint32_t timer = 0;
		bool counting = false;
	};
	// The device's half of Dialog_LoadAudioClip: start the line's wave on a new
	// voice of the dialog channel and say what loaded.
	using LoadLine = std::function<DialogClip(const DialogLineRef &)>;
	// AudioChannel_ValidateHandle: the voice still plays.
	using VoicePlaying = std::function<bool(uint64_t)>;

	// The PlayWavList play: the dialog "dlg%03i" of the number names in the
	// mission's dialog bank registers with its lines (each with the wave the
	// bank's sounds give it); nothing for 0, no bank or no dialog of the name.
	// Dialog_Register's result: false also when all 16 slots hold a dialog.
	// [orig: Dialog_PlayByIndex @ 0x527ae0 -> Dialog_PlayByName @ 0x44d9f0 ->
	//  Dialog_Register @ 0x44d980]
	bool play(const dbf::File *dialog_bank, const lwf::File *dialog_sounds,
			int32_t dialog_index);
	// Dialog_Register: the history takes the dialog, then the dialog takes the
	// next free slot, its first line due at once; false (the slot insert is
	// dropped, the history entry stays) when all 16 slots hold a dialog.
	bool enqueue(const std::string &dialog, std::vector<DialogLineRef> lines);
	// One Dialog_UpdatePlayback tick. `frame_rendered`: a frame has rendered
	// since the tick before, which starts a fresh hold's countdown.
	void tick(bool frame_rendered, const LoadLine &load, const VoicePlaying &playing);
	// The PLYRDIALOG readers, keyed by the trigger's number, whose name is
	// "dlg%.3d" of it. active: a slot holds the dialog (trigger sub 34
	// PlayerDialogDone is its negation) [orig: Dialog_ExistsByIndex
	// @ 0x44e170]. finished: the history holds it and no slot does (sub 35
	// PlayerDialogFinished) [orig: sub_44E220 @ 0x44e220].
	bool active(int32_t dialog_index) const;
	bool finished(int32_t dialog_index) const;
	const std::vector<Slot> &slots() const { return slots_; }
	// The registered dialogs the scans reach, oldest first.
	const std::vector<std::string> &history() const { return history_; }
	// The dialog channel's voices, oldest first save for the removals' swaps.
	const std::vector<uint64_t> &voices() const { return voices_; }
	// Mission teardown: nothing registered, queued or playing crosses
	// missions.
	void clear();
	// Dialog_ResetAll: every slot and the history cleared; the channel's
	// voices play on [orig: Dialog_ResetAll @ 0x44dc90, the slot memset
	// @ 0x44dc9c, the counts @ 0x44dca4 / @ 0x44dcae].
	void reset();

private:
	std::vector<Slot> slots_;
	// The history's [0, count): entry 255, the saturated writes' target, has
	// no reader, so it is not kept.
	std::vector<std::string> history_;
	std::vector<uint64_t> voices_;
};

} // namespace opennova::audio
