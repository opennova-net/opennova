// runtime/audio/dialog_queue: the PlayWavList dialog-number resolution (the
// dialog bank's dialog "dlg%03i", each line the wave of its name in the dialog
// bank's sounds), the one-channel serialized dialog queue and the joiner's line,
// over synthetic dbf/lwf documents.

#include <runtime/audio/dialog_queue.h>

#include "common/test_expect.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::audio;

// A dialog bank's sounds: one wave a name, its file and its byte +33 (the high
// byte of value_hi); a set of each name too, which a dialog line never plays.
lwf::File sounds_with(const std::vector<std::pair<std::string, int>> &waves) {
	lwf::File file;
	for (const auto &[name, volume] : waves) {
		lwf::Single single;
		single.name = name;
		single.path = name + ".wav";
		single.value_hi = uint16_t(volume << 8);
		file.singles.push_back(single);
		lwf::Multi set;
		set.name = "SET_" + name;
		file.multis.push_back(set);
	}
	return file;
}

dbf::File dialog_bank() {
	dbf::File file;
	dbf::Group dlg001;
	dlg001.group_name = "dlg001";
	for (const char *def : { "SynR100", "MISSING", "SynR101" }) {
		dbf::Line line;
		line.def_id_name = def;
		dlg001.lines.push_back(line);
	}
	file.groups.push_back(dlg001);
	dbf::Group dlg002;
	dlg002.group_name = "dlg002";
	dbf::Line unresolved;
	unresolved.def_id_name = "MISSING";
	dlg002.lines.push_back(unresolved);
	file.groups.push_back(dlg002);
	// A later dialog of the same name is never found, and one of another case is no dlg003.
	dbf::Group later = dlg002;
	later.group_name = "dlg001";
	file.groups.push_back(later);
	dbf::Group upper = dlg002;
	upper.group_name = "DLG003";
	file.groups.push_back(upper);
	return file;
}

// The lines' files, "" for a line whose wave the sounds lack.
std::vector<std::string> files(const std::vector<DialogLineRef> &lines) {
	std::vector<std::string> out;
	for (const DialogLineRef &line : lines) out.push_back(line.file);
	return out;
}

DialogLineRef ref(const char *file, const char *dialog = "", int line = -1) {
	DialogLineRef out;
	out.file = file;
	out.dialog_name = dialog;
	out.line = line;
	return out;
}

int test_a_number_plays_its_dialog_s_waves() {
	const lwf::File sounds = sounds_with({ { "SynR100", 210 }, { "synr101", 0 } });
	const dbf::File dialogs = dialog_bank();
	// [orig: Dialog_PlayByIndex @0x527ae0: "dlg%03i", 0 none]
	TEST_EXPECT(dialog_name_of(1) == "dlg001" && dialog_name_of(12) == "dlg012" && dialog_name_of(1234) == "dlg1234");
	TEST_EXPECT(dialog_name_of(0).empty());

	// Every line of the first dialog of the name, in order, with its index for the wire; the
	// wave is found without case, the line whose wave the sounds lack keeps its place without
	// a clip.
	const std::vector<DialogLineRef> first = resolve_dialog_lines(&dialogs, &sounds, 1);
	const std::vector<std::string> expect_first = { "SynR100.wav", "", "synr101.wav" };
	TEST_EXPECT(files(first) == expect_first);
	TEST_EXPECT(first.size() == 3 && first[0].dialog_name == "dlg001" && first[0].line == 0 &&
			first[1].line == 1 && first[2].line == 2 && first[1].wave == "MISSING");
	// The wave's byte +33 is the line's volume, 0 playing at full [orig: sub_527560 @0x527598].
	TEST_EXPECT(first.size() == 3 && first[0].volume == 210 && first[2].volume == 255);
	// A dialog whose line names no wave: the line, no clip; no sound set stands in.
	const std::vector<DialogLineRef> second = resolve_dialog_lines(&dialogs, &sounds, 2);
	TEST_EXPECT(second.size() == 1 && second[0].file.empty() && second[0].wave == "MISSING");
	// The name is matched exactly: DLG003 is no dlg003; a number no dialog has, 0, and no bank
	// play nothing.
	TEST_EXPECT(resolve_dialog_lines(&dialogs, &sounds, 3).empty());
	TEST_EXPECT(resolve_dialog_lines(&dialogs, &sounds, 5).empty());
	TEST_EXPECT(resolve_dialog_lines(&dialogs, &sounds, 0).empty());
	TEST_EXPECT(resolve_dialog_lines(nullptr, &sounds, 1).empty());
	// No sounds: the lines without a clip.
	const std::vector<DialogLineRef> silent = resolve_dialog_lines(&dialogs, nullptr, 1);
	TEST_EXPECT(silent.size() == 3 && silent[0].file.empty());
	return 0;
}

// Another magic's bank finds a wave only where its dword +28 is set, and the first of the name
// decides [orig: SoundBank_FindEntryByName @0x75bba0, @0x75bbdf].
int test_a_wave_found_as_the_bank_finds_it() {
	lwf::File sounds = sounds_with({ { "Z01R100", 250 } });
	lwf::Single second = sounds.singles[0];
	second.path = "second.wav";
	sounds.singles.push_back(second);
	TEST_EXPECT(find_dialog_wave(sounds, "z01r100") == &sounds.singles[0]);
	sounds.header.magic = 0;
	TEST_EXPECT(find_dialog_wave(sounds, "Z01R100") == nullptr);
	const uint32_t word = 1;
	std::memcpy(sounds.singles[0].raw_name.data() + 28, &word, sizeof(word));
	TEST_EXPECT(find_dialog_wave(sounds, "Z01R100") == &sounds.singles[0]);
	return 0;
}

int test_queue_advances_only_when_the_channel_frees() {
	DialogQueue queue;
	DialogLineRef next;
	TEST_EXPECT(!queue.take_next(next));
	queue.enqueue({ ref("SynR100.wav", "dlg001", 0), ref("SynR101.wav", "dlg001", 1) });
	TEST_EXPECT(queue.pending() == 2);
	TEST_EXPECT(queue.take_next(next) && next.file == "SynR100.wav" &&
			next.dialog_name == "dlg001" && next.line == 0);
	queue.line_started();
	TEST_EXPECT(queue.line_active());
	TEST_EXPECT(!queue.take_next(next));
	TEST_EXPECT(queue.pending() == 1);
	// A second play_dialog while a line plays queues behind it.
	queue.enqueue({ ref("SynR102.wav") });
	TEST_EXPECT(queue.pending() == 2);
	queue.line_finished();
	TEST_EXPECT(queue.take_next(next) && next.file == "SynR101.wav" && next.line == 1);
	// A line that fails to spawn is skipped by asking again (no line was
	// started), so the queue never stalls.
	TEST_EXPECT(queue.take_next(next) && next.file == "SynR102.wav");
	TEST_EXPECT(!queue.take_next(next));
	queue.line_started();
	queue.enqueue({ ref("SynR103.wav") });
	queue.discard_pending();
	TEST_EXPECT(queue.line_active() && queue.pending() == 0);
	queue.enqueue({ ref("after respawn") });
	TEST_EXPECT(!queue.take_next(next));
	queue.line_finished();
	TEST_EXPECT(queue.take_next(next) && next.file == "after respawn");
	queue.clear();
	TEST_EXPECT(!queue.line_active() && queue.pending() == 0);
	TEST_EXPECT(!queue.take_next(next));
	return 0;
}

// D-NET-285, the joiner's S2C 0x28 line: the dialog by exact name, the
// class-prefixed wave before the bare one, the [Mission Dialog] subtitle by
// the wave's name, else the flat text entry the sequence suffix names.
// [orig: Dialog_PlayByNameAndSlot @0x44E3F0; Dialog_LoadAudioClipLocalized
//  @0x44DEF0]
int test_client_line_resolution() {
	const lwf::File sounds = sounds_with({ { "SynR100", 210 }, { "3SynR101", 200 }, { "SynR101", 190 } });
	dbf::File dialogs = dialog_bank();
	dialogs.groups[0].lines[0].sequence = "_00000";
	dialogs.groups[0].lines[1].sequence = "line_7";
	dialogs.groups[0].lines[2].sequence = "_00002";
	rtxt::File text;
	text.sections.push_back(rtxt::Section{"Info", 2});
	text.sections.push_back(rtxt::Section{"Mission Dialog", 1});
	rtxt::Entry e0;
	e0.key = "title";
	e0.text = "Mission title";
	rtxt::Entry e1;
	e1.key = "briefing";
	e1.text = "Briefing";
	rtxt::Entry e2;
	e2.key = "SynR101";
	e2.text = "Move out!";
	e2.section_index = 1;
	text.entries = { e0, e1, e2 };

	// Class 3: the "3SynR101" variant wins; the subtitle is the wave name's.
	DialogLinePlayback line = resolve_dialog_line(&dialogs, &sounds, &text, "dlg001", 2, 3);
	TEST_EXPECT(line.line_found && line.wave == "SynR101");
	TEST_EXPECT(line.played == "3SynR101" && line.file == "3SynR101.wav" && line.volume == 200 &&
			line.text == "Move out!");
	// Class 1 has no variant: the bare wave.
	line = resolve_dialog_line(&dialogs, &sounds, &text, "dlg001", 2, 1);
	TEST_EXPECT(line.played == "SynR101" && line.file == "SynR101.wav");
	// No [Mission Dialog] entry: the flat entry the sequence suffix names.
	line = resolve_dialog_line(&dialogs, &sounds, &text, "dlg001", 0, 1);
	TEST_EXPECT(line.file == "SynR100.wav" && line.text == "Mission title");
	// A line whose wave the sounds lack resolves its subtitle but no clip; a suffix
	// past the table names no entry.
	line = resolve_dialog_line(&dialogs, &sounds, &text, "dlg001", 1, 1);
	TEST_EXPECT(line.line_found && line.file.empty() && line.text.empty());
	// The name is matched exactly, and a line past the table resolves nothing.
	TEST_EXPECT(!resolve_dialog_line(&dialogs, &sounds, &text, "DLG001", 0, 1).line_found);
	TEST_EXPECT(!resolve_dialog_line(&dialogs, &sounds, &text, "dlg001", 3, 1).line_found);
	TEST_EXPECT(!resolve_dialog_line(nullptr, &sounds, &text, "dlg001", 0, 1).line_found);
	// No mission text: the clip alone.
	line = resolve_dialog_line(&dialogs, &sounds, nullptr, "dlg001", 2, 3);
	TEST_EXPECT(line.file == "3SynR101.wav" && line.text.empty());
	// The subtitle alone, as the line's own words say it.
	TEST_EXPECT(dialog_line_text(&text, dialogs.groups[0].lines[2]) == "Move out!");
	TEST_EXPECT(dialog_line_text(&text, dialogs.groups[0].lines[0]) == "Mission title");
	TEST_EXPECT(dialog_line_text(nullptr, dialogs.groups[0].lines[0]).empty());
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed |= test_a_number_plays_its_dialog_s_waves();
	failed |= test_a_wave_found_as_the_bank_finds_it();
	failed |= test_queue_advances_only_when_the_channel_frees();
	failed |= test_client_line_resolution();
	if (failed) {
		return 1;
	}
	std::printf("dialog_queue_test: OK\n");
	return 0;
}
