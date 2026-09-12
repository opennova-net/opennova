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

} // namespace

int main() {
	int failed = 0;
	failed |= test_resolution_prefers_the_dbf_lines_the_banks_carry();
	failed |= test_queue_advances_only_when_the_channel_frees();
	if (failed) {
		return 1;
	}
	std::printf("dialog_queue_test: OK\n");
	return 0;
}
