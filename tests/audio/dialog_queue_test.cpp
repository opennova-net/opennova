// runtime/audio/dialog_queue: the PlayWavList dialog-id resolution (the
// co-named .DBF group's carried lines, then the direct set-name forms), the
// one-channel serialized dialog queue and the WAC scripted-voice channel's
// reset-before-play rule, over synthetic dbf/lwf documents.

#include <runtime/audio/dialog_queue.h>

#include "common/test_expect.h"

#include <string>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::audio;

lwf::File bank_with(const std::vector<std::string> &set_names) {
	lwf::File file;
	for (const std::string &name : set_names) {
		lwf::Multi m;
		m.name = name;
		file.multis.push_back(m);
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
	return file;
}

int test_resolution_prefers_the_dbf_lines_the_banks_carry() {
	const lwf::File bank = bank_with({ "SynR100", "SynR101", "DLG002", "dlg003", "4" });
	SoundSetIndex sets;
	sets.add_bank(0, bank);
	const dbf::File dialogs = dialog_bank();

	const std::vector<std::string> expect_first = { "SynR100", "SynR101" };
	TEST_EXPECT(resolve_dialog_sets(&dialogs, sets, 1) == expect_first);
	// A group whose lines the banks do not carry falls through to the direct
	// forms; "DLG002" is carried.
	const std::vector<std::string> expect_second = { "DLG002" };
	TEST_EXPECT(resolve_dialog_sets(&dialogs, sets, 2) == expect_second);
	// No .DBF: the direct forms in order -- "DLG%03d", "dlg%03d", the number.
	// The index is case-insensitive, so the upper form already matches the
	// bank's "dlg003" and is the name handed back.
	const std::vector<std::string> expect_third = { "DLG003" };
	TEST_EXPECT(resolve_dialog_sets(nullptr, sets, 3) == expect_third);
	const std::vector<std::string> expect_fourth = { "4" };
	TEST_EXPECT(resolve_dialog_sets(nullptr, sets, 4) == expect_fourth);
	TEST_EXPECT(resolve_dialog_sets(nullptr, sets, 5).empty());
	TEST_EXPECT(resolve_dialog_sets(&dialogs, sets, 5).empty());
	return 0;
}

int test_queue_advances_only_when_the_channel_frees() {
	DialogQueue queue;
	std::string next;
	TEST_EXPECT(!queue.take_next(next));
	queue.enqueue({ "SynR100", "SynR101" });
	TEST_EXPECT(queue.pending() == 2);
	TEST_EXPECT(queue.take_next(next) && next == "SynR100");
	queue.line_started();
	TEST_EXPECT(queue.line_active());
	TEST_EXPECT(!queue.take_next(next));
	TEST_EXPECT(queue.pending() == 1);
	// A second play_dialog while a line plays queues behind it.
	queue.enqueue({ "SynR102" });
	TEST_EXPECT(queue.pending() == 2);
	queue.line_finished();
	TEST_EXPECT(queue.take_next(next) && next == "SynR101");
	// A line that fails to spawn is skipped by asking again (no line was
	// started), so the queue never stalls.
	TEST_EXPECT(queue.take_next(next) && next == "SynR102");
	TEST_EXPECT(!queue.take_next(next));
	queue.line_started();
	queue.enqueue({ "SynR103" });
    queue.discard_pending();
    TEST_EXPECT(queue.line_active() && queue.pending() == 0);
    queue.enqueue({ "after respawn" });
    TEST_EXPECT(!queue.take_next(next));
    queue.line_finished();
    TEST_EXPECT(queue.take_next(next) && next == "after respawn");
	queue.clear();
	TEST_EXPECT(!queue.line_active() && queue.pending() == 0);
	TEST_EXPECT(!queue.take_next(next));
	return 0;
}

// D-NET-285, the joiner's S2C 0x28 line: the dialog by exact name, the
// class-prefixed clip before the bare one, the [Mission Dialog] subtitle by
// the def name, else the flat text entry the sequence suffix names.
// [orig: Dialog_PlayByNameAndSlot @0x44E3F0; Dialog_LoadAudioClipLocalized
//  @0x44DEF0]
int test_client_line_resolution() {
	const lwf::File bank = bank_with({ "SynR100", "3SynR101", "SynR101" });
	SoundSetIndex sets;
	sets.add_bank(0, bank);
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

	// Class 3: the "3SynR101" variant wins; the subtitle is the def name's.
	DialogLinePlayback line = resolve_dialog_line(&dialogs, sets, &text, "dlg001", 2, 3);
	TEST_EXPECT(line.line_found && line.def_id_name == "SynR101");
	TEST_EXPECT(line.set_name == "3SynR101" && line.text == "Move out!");
	// Class 1 has no variant: the bare clip.
	line = resolve_dialog_line(&dialogs, sets, &text, "dlg001", 2, 1);
	TEST_EXPECT(line.set_name == "SynR101");
	// No [Mission Dialog] entry: the flat entry the sequence suffix names.
	line = resolve_dialog_line(&dialogs, sets, &text, "dlg001", 0, 1);
	TEST_EXPECT(line.set_name == "SynR100" && line.text == "Mission title");
	// A carried-by-nobody line resolves its subtitle but no clip; a suffix
	// past the table names no entry.
	line = resolve_dialog_line(&dialogs, sets, &text, "dlg001", 1, 1);
	TEST_EXPECT(line.line_found && line.set_name.empty() && line.text.empty());
	// The name is matched exactly, and a line past the table resolves nothing.
	TEST_EXPECT(!resolve_dialog_line(&dialogs, sets, &text, "DLG001", 0, 1).line_found);
	TEST_EXPECT(!resolve_dialog_line(&dialogs, sets, &text, "dlg001", 3, 1).line_found);
	TEST_EXPECT(!resolve_dialog_line(nullptr, sets, &text, "dlg001", 0, 1).line_found);
	// No mission text: the clip alone.
	line = resolve_dialog_line(&dialogs, sets, nullptr, "dlg001", 2, 3);
	TEST_EXPECT(line.set_name == "3SynR101" && line.text.empty());
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed |= test_resolution_prefers_the_dbf_lines_the_banks_carry();
	failed |= test_queue_advances_only_when_the_channel_frees();
	failed |= test_client_line_resolution();
	if (failed) {
		return 1;
	}
	std::printf("dialog_queue_test: OK\n");
	return 0;
}
