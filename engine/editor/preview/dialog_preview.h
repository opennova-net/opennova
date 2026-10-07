#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/rtxt/rtxt.h>

namespace opennova {
class FileSource;
}

namespace opennova::editor {

// A mission's dialog played as the game plays it (ADR 0046 DI-32), the portable half of the one preview
// sound player: the dialog the game finds by name in the dialog bank, its lines in order, each the wave of
// its name in the bank's sounds at that wave's dialog volume, one after another on the one dialog channel
// (docs/audio/lwf-dbf-sound-re.md "The dialog banks"; runtime/audio/dialog_queue, the same lookups the
// game's port makes). The Shell plays the waves at the starts this plans.

// What a dialog plays from: the dialog bank, its sounds (<bank>.lwf, else <bank>.pwf) and the mission's text
// (<mission>.bin, else medmssn.bin), each as the project holds it, an open document standing in (FileSource).
struct DialogSources {
	std::string bank_name;   // the dialog bank's file name
	dbf::File bank;
	bool bank_read = false;
	std::string sounds_name; // the sounds the game opens beside it ("" none)
	lwf::File sounds;
	bool sounds_read = false;
	std::string text_name;   // the mission text the subtitles read ("" none)
	rtxt::File text;
	bool text_read = false;
};
// Reads the dialog bank `bank_name` and what the game opens with it: its sounds, the .lwf of its own name and
// else the .pwf [orig: DialogManager_LoadFromFile @ 0x44e7d4..0x44e807], and the mission's text `text_name` and
// else medmssn.bin [orig: TextResource_LoadMissionTextBin @ 0x51ed90]. False, with `error`, where the bank
// does not read.
bool read_dialog_sources(const FileSource &files, const std::string &bank_name, const std::string &text_name,
                         DialogSources &out, std::string &error);

// One line of a dialog as it plays: its index, the wave it names, the file the bank's sounds give that wave
// ("" none: the game says "EX Cannot load audio" and plays nothing), the line's volume (the wave's dialog
// volume, 0 playing at full), its wait after the line before (its delay, from the second line on), when it
// starts after the dialog does and how long it lasts (the wave's length; 12 ticks where nothing plays [orig:
// Dialog_LoadAudioClip @ 0x44dd7c]), and its subtitle.
struct DialogPlayLine {
	int index = 0;
	std::string wave;
	std::string file;
	int volume = 255;
	double wait_s = 0.0;
	double start_s = 0.0;
	double seconds = 0.0;
	std::string text;
};
// What a dialog play comes to: the dialog and its bank (none: `found` false, `words` why), its sounds, the
// lines, and what it plays in words ("dlg001 in 01TR.dbf: Z01R100 (Z01R100.WAV) 'Take up ...'").
struct DialogPlay {
	bool found = false;
	std::string dialog;
	std::string bank;
	std::string sounds;
	std::vector<DialogPlayLine> lines;
	std::string words;
};

// The dialog `dialog` (its name, or a number: dlg%03i of it [orig: Dialog_PlayByIndex @ 0x527ae0]) found as the
// game finds it, the first of the name matched exactly [orig: Dialog_PlayByName @ 0x44d9f0], its lines (the one
// at `line` alone where it is 0 or more) each with its wave, volume and subtitle as the game's port resolves
// them (runtime/audio/dialog_queue), timed one after another: a line starts once the one before it has ended,
// after its own wait [orig: Dialog_UpdatePlayback @ 0x44e470, the channel gate @ 0x44e53a, the wait @ 0x44e585;
// the game's countdown beside the gate, D-SND-4]. `seconds_of` gives a wave file's length (0 where unknown).
DialogPlay plan_dialog_play(const DialogSources &sources, const std::string &dialog, int line,
                            const std::function<double(const std::string &file)> &seconds_of);

// The dialog name a value gives: a name as written, a whole number its dlg%03i ("12" is dlg012).
std::string dialog_name_given(const std::string &value);

} // namespace opennova::editor
