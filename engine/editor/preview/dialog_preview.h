#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/audio/dialog_queue.h>

namespace opennova {
class FileSource;
}

namespace opennova::editor {

// A mission's dialog played as the game plays it (ADR 0046 DI-32), the portable half of the one preview
// sound player: the dialog the game finds by name in the dialog bank, its lines in order, each the wave of
// its name in the bank's sounds at that wave's dialog volume, timed on the one dialog channel by the game's
// own queue (docs/audio/lwf-dbf-sound-re.md "The dialog banks"; runtime/audio/dialog_queue, the lookups and
// the timing the game's port makes). The Shell plays the waves at the starts this plans.

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

// What the device loads of a line's wave, as the game's device loads it (Dialog_LoadAudioClip's clip): whether
// it decoded, what the game's wave loader records of it (lwf::WavPcm::loader_samples, loader_pitch_q16: the
// line's hold, audio::dialog_clip_hold) and how long its voice plays (its frames at its rate, seconds).
struct DialogWave {
	bool loaded = false;
	uint32_t samples = 0;
	uint32_t pitch_q16 = 0;
	double seconds = 0.0;
};
// A wave file's DialogWave, decoded as the game's device decodes it (lwf::wav_decode_pcm16); not loaded for
// bytes that do not decode.
DialogWave dialog_wave(const std::vector<uint8_t> &bytes);

// The game's dialog channel run on a preview's clock (DI-32): the runtime's own audio::DialogQueue (its 16
// slots walked once a logic tick, each line holding 2 * ((62 * samples + rate) / rate) ticks, then the dialog
// channel, then its delay of 62 * DELAY / 10 ticks, so dialogs interleave in slot order [orig:
// Dialog_UpdatePlayback @ 0x44e470; Dialog_LoadAudioClip @ 0x44dd87..0x44ddbf]), ticked with a frame drawn
// before every tick, its device the project's waves: a line loads its wave through `wave_of` and its voice
// plays the wave's length.
class DialogChannel {
public:
	// The wave a line's file names, by its file name (io::utf8_file_name of the bank's path).
	using WaveOf = std::function<DialogWave(const std::string &file)>;
	// A line the channel loaded: the tick, the line (audio::resolve_dialog_lines's), its wave and the ticks it
	// holds its dialog (audio::dialog_clip_hold).
	struct Loaded {
		int32_t tick = 0;
		audio::DialogLineRef line;
		DialogWave wave;
		uint32_t hold = 0;
	};
	// Dialog_Register (audio::DialogQueue::enqueue): false where all 16 slots hold a dialog.
	bool enqueue(const std::string &dialog, std::vector<audio::DialogLineRef> lines);
	// The channel's tick `tick` (each tick once, in order): the lines it loads appended to `out`.
	void tick(int32_t tick, const WaveOf &wave_of, std::vector<Loaded> &out);
	// No dialog holds a slot.
	bool idle() const { return queue_.slots().empty(); }
	const audio::DialogQueue &queue() const { return queue_; }
	// Nothing registered, queued or playing.
	void clear();

private:
	audio::DialogQueue queue_;
	std::vector<std::pair<uint64_t, int32_t>> voices_; // each voice and the tick its wave ends
	uint64_t next_voice_ = 1;
};

// One line of a dialog as it plays: its index, the wave it names, the file the bank's sounds give that wave
// ("" none: the game says "EX Cannot load audio" and plays nothing), the line's volume (the wave's dialog
// volume, 0 playing at full), its delay once the line before has held (from the second line on), the tick it
// loads after the dialog registers and its seconds, the ticks it holds the dialog (12 where nothing loads
// [orig: Dialog_LoadAudioClip @ 0x44dd7c]), how long its wave plays, and its subtitle.
struct DialogPlayLine {
	int index = 0;
	std::string wave;
	std::string file;
	int volume = 255;
	double wait_s = 0.0;
	int32_t start_tick = 0;
	double start_s = 0.0;
	uint32_t hold_ticks = 0;
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
// them (runtime/audio/dialog_queue), timed as the game's queue times them on an idle channel (DialogChannel: a
// line loads once the one before has held about twice its wave's length, the channel is free and its own delay
// has run [orig: Dialog_UpdatePlayback @ 0x44e470, the channel gate @ 0x44e53a, the delay @ 0x44e563..0x44e585]).
// `wave_of` loads a wave file (DialogWave: none loaded where the project lacks it).
DialogPlay plan_dialog_play(const DialogSources &sources, const std::string &dialog, int line,
                            const DialogChannel::WaveOf &wave_of);

// The dialog name a value gives: a name as written, a whole number its dlg%03i ("12" is dlg012).
std::string dialog_name_given(const std::string &value);

} // namespace opennova::editor
