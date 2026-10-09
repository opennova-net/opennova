// runtime/audio/dialog_queue: the PlayWavList dialog-number resolution (the
// dialog bank's dialog "dlg%03i", each line the wave of its name in the dialog
// bank's sounds), the dialog slots' timers on the one dialog channel and the
// joiner's line, over synthetic dbf/lwf documents and a scripted device.

#include <runtime/audio/dialog_queue.h>

#include "common/test_expect.h"

#include <cstring>
#include <initializer_list>
#include <set>
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
	const uint8_t delays[] = { 3, 0, 7 };
	int at = 0;
	for (const char *def : { "SynR100", "MISSING", "SynR101" }) {
		dbf::Line line;
		line.def_id_name = def;
		line.delay = delays[at++];
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

DialogLineRef ref(const char *file, const char *dialog = "", int line = -1, uint8_t delay = 0) {
	DialogLineRef out;
	out.file = file;
	out.dialog_name = dialog;
	out.line = line;
	out.delay = delay;
	return out;
}

// The shell's half, scripted: a line with a file loads on a new voice that
// plays until the test stops it, its wave tone.wav's (8320 samples at 22050
// Hz, a hold of 48 ticks); one without loads nothing. Every load is logged as
// "<dialog>:<line>", the co-op report's order.
struct Device {
	std::vector<std::string> loads;
	std::set<uint64_t> live;
	uint64_t next_voice = 1;

	DialogQueue::LoadLine load() {
		return [this](const DialogLineRef &line) {
			loads.push_back(line.dialog_name + ":" + std::to_string(line.line));
			DialogClip clip;
			if (line.file.empty()) return clip;
			clip.loaded = true;
			clip.samples = 8320;
			clip.pitch_q16 = 32768;
			clip.voice = next_voice++;
			live.insert(clip.voice);
			return clip;
		};
	}
	DialogQueue::VoicePlaying playing() {
		return [this](uint64_t voice) { return live.count(voice) != 0; };
	}
	void stop(uint64_t voice) { live.erase(voice); }
};

// `ticks` playback ticks, a frame rendered before each unless told otherwise.
void run(DialogQueue &queue, Device &device, int ticks, bool rendered = true) {
	for (int i = 0; i < ticks; ++i) queue.tick(rendered, device.load(), device.playing());
}

std::vector<std::string> names(std::initializer_list<const char *> list) {
	std::vector<std::string> out;
	for (const char *each : list) out.emplace_back(each);
	return out;
}

int test_a_number_plays_its_dialog_s_waves() {
	const lwf::File sounds = sounds_with({ { "SynR100", 210 }, { "synr101", 0 } });
	const dbf::File dialogs = dialog_bank();
	// [orig: Dialog_PlayByIndex @0x527ae0: "dlg%03i", 0 none]
	TEST_EXPECT(dialog_name_of(1) == "dlg001" && dialog_name_of(12) == "dlg012" && dialog_name_of(1234) == "dlg1234");
	TEST_EXPECT(dialog_name_of(0).empty());
	// Its inverse: the number whose "dlg%03i" is the name, matched exactly; none for a name no number
	// forms.
	TEST_EXPECT(dialog_index_of("dlg012") == 12 && dialog_index_of("dlg1234") == 1234 && dialog_index_of("dlg000") == 0 &&
	            dialog_index_of("dlg12") == -1 && dialog_index_of("dlg0012") == -1 && dialog_index_of("DLG012") == -1 &&
	            dialog_index_of("intro") == -1 && dialog_index_of("dlg01x") == -1);
	for (const int32_t n : {1, 12, 999, 1000, 65535}) TEST_EXPECT(dialog_index_of(dialog_name_of(n)) == n);

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
	// Each line carries its DELAY byte (dbf line +0x35) [orig: Dialog_UpdatePlayback @0x44e563].
	TEST_EXPECT(first.size() == 3 && first[0].delay == 3 && first[1].delay == 0 && first[2].delay == 7);
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

// The hold: 2 * ((62 * samples + rate) / rate), the rate rounded back from the
// pitch ratio; 12 for no wave or a rate of 0 [orig: Dialog_LoadAudioClip
// @0x44dd7c, @0x44dd87..0x44ddbf].
int test_a_line_holds_about_twice_its_wave() {
	DialogClip clip;
	TEST_EXPECT(dialog_clip_hold(clip) == 12);
	clip.loaded = true;
	clip.samples = 8320;
	clip.pitch_q16 = 32768;  // tone.wav: 22050 Hz, 0.377 s, 23.4 ticks long
	TEST_EXPECT(dialog_clip_hold(clip) == 48);
	clip.samples = 44100;
	clip.pitch_q16 = 65536;  // one second at 44100 Hz
	TEST_EXPECT(dialog_clip_hold(clip) == 126);
	clip.samples = 8000;
	clip.pitch_q16 = 11889;  // one second at 8000 Hz: the ratio's rate rounds back to 8000
	TEST_EXPECT(dialog_clip_hold(clip) == 126);
	clip.samples = 1;
	clip.pitch_q16 = 32768;
	TEST_EXPECT(dialog_clip_hold(clip) == 2);
	clip.pitch_q16 = 0;
	TEST_EXPECT(dialog_clip_hold(clip) == 12);
	return 0;
}

// A dialog's first line loads on the first tick; the next waits out the hold
// (counting from the tick after a rendered frame), then the free channel, then
// its own delay, 62 * 12 / 10 = 74 ticks [orig: Dialog_UpdatePlayback
// @0x44e470: @0x44e5f1..0x44e60e, @0x44e525..0x44e544, @0x44e585].
int test_the_next_line_waits_hold_channel_delay() {
	DialogQueue queue;
	Device device;
	TEST_EXPECT(queue.enqueue("dlg002", { ref("a.wav", "dlg002", 0, 5), ref("b.wav", "dlg002", 1, 12) }));
	TEST_EXPECT(device.loads.empty());
	run(queue, device, 1);
	// The first line's own delay is never waited: the slot's wait flag is clear.
	TEST_EXPECT(device.loads == names({ "dlg002:0" }) && queue.voices().size() == 1);
	TEST_EXPECT(queue.slots().size() == 1 && queue.slots()[0].timer == 48 && !queue.slots()[0].counting);
	run(queue, device, 1);  // tick 2: the countdown starts
	TEST_EXPECT(queue.slots()[0].timer == 48 && queue.slots()[0].counting);
	run(queue, device, 48);  // ticks 3..50
	TEST_EXPECT(queue.slots()[0].timer == 0);
	run(queue, device, 9);  // ticks 51..59: the line's voice still plays
	TEST_EXPECT(device.loads.size() == 1 && queue.slots()[0].timer == 0);
	device.stop(1);
	run(queue, device, 1);  // tick 60: the channel is free, the delay starts
	TEST_EXPECT(queue.slots()[0].timer == (0x80000000u | 74u) && queue.voices().empty());
	run(queue, device, 74);  // ticks 61..134
	TEST_EXPECT(device.loads.size() == 1 && queue.slots()[0].timer == 0x80000000u);
	run(queue, device, 1);  // tick 135
	TEST_EXPECT(device.loads == names({ "dlg002:0", "dlg002:1" }));
	return 0;
}

// The hold counts only once a frame has rendered after the line loaded; the
// countdown itself needs no frame [orig: @0x44e5fe..0x44e60e; the flag
// GameLoop_RenderFrame sets @0x521cff].
int test_the_hold_starts_after_a_rendered_frame() {
	DialogQueue queue;
	Device device;
	queue.enqueue("dlg001", { ref("a.wav", "dlg001", 0), ref("b.wav", "dlg001", 1) });
	run(queue, device, 1, false);
	run(queue, device, 9, false);  // ticks 2..10, no frame
	TEST_EXPECT(queue.slots()[0].timer == 48 && !queue.slots()[0].counting);
	run(queue, device, 1);  // tick 11
	run(queue, device, 47, false);  // ticks 12..58
	TEST_EXPECT(queue.slots()[0].timer == 1);
	device.stop(1);
	run(queue, device, 1, false);  // tick 59: the hold runs out
	TEST_EXPECT(device.loads.size() == 1);
	run(queue, device, 1, false);  // tick 60: a delay of 0 loads at once
	TEST_EXPECT(device.loads == names({ "dlg001:0", "dlg001:1" }));
	return 0;
}

// A line without a wave still loads (the co-op report) and holds 12 ticks; it
// starts no voice, so the channel stays free [orig: Dialog_LoadAudioClip
// @0x44dd74..0x44dd7c].
int test_a_missing_wave_holds_twelve_ticks() {
	DialogQueue queue;
	Device device;
	queue.enqueue("dlg002", { ref("", "dlg002", 0), ref("b.wav", "dlg002", 1) });
	run(queue, device, 1);
	TEST_EXPECT(device.loads == names({ "dlg002:0" }) && queue.voices().empty());
	TEST_EXPECT(queue.slots()[0].timer == 12);
	run(queue, device, 13);  // ticks 2..14
	TEST_EXPECT(device.loads.size() == 1 && queue.slots()[0].timer == 0);
	run(queue, device, 1);  // tick 15
	TEST_EXPECT(device.loads == names({ "dlg002:0", "dlg002:1" }));
	return 0;
}

// Separate dialogs share only the channel: B's line takes the channel once A's
// first wave ends, A's second waits for it, so they run A0, B0, A1 [orig:
// Dialog_UpdatePlayback @0x44e4d8..0x44e634, the slots walked in order].
int test_dialogs_interleave_in_slot_order() {
	DialogQueue queue;
	Device device;
	queue.enqueue("dlgA", { ref("a0.wav", "dlgA", 0), ref("a1.wav", "dlgA", 1) });
	queue.enqueue("dlgB", { ref("b0.wav", "dlgB", 0) });
	run(queue, device, 1);
	TEST_EXPECT(device.loads == names({ "dlgA:0" }));
	run(queue, device, 19);  // ticks 2..20: A0 plays
	TEST_EXPECT(device.loads.size() == 1);
	device.stop(1);
	run(queue, device, 1);  // tick 21: A0's wave ended, A still holds
	TEST_EXPECT(device.loads == names({ "dlgA:0", "dlgB:0" }));
	run(queue, device, 49);  // ticks 22..70: A's hold ran out at 50; B0 plays
	TEST_EXPECT(device.loads.size() == 2);
	device.stop(2);
	run(queue, device, 1);  // tick 71
	TEST_EXPECT(device.loads == names({ "dlgA:0", "dlgB:0", "dlgA:1" }));
	// B held its last line and waits for A1's channel before it leaves its slot.
	TEST_EXPECT(queue.slots().size() == 2 && queue.slots()[1].dialog == "dlgB");
	device.stop(3);
	run(queue, device, 1);
	TEST_EXPECT(queue.slots().size() == 1 && queue.slots()[0].dialog == "dlgA");
	return 0;
}

// A run-out delay loads even while another dialog's voice plays, so two voices
// overlap; the channel test reads only the last voice on the stack, which drops
// stopped voices by moving the last into their place [orig: @0x44e52e..0x44e530;
// AudioChannel_CleanupInvalid @0x44d840].
int test_the_delay_ignores_the_channel() {
	DialogQueue queue;
	Device device;
	queue.enqueue("dlgA", { ref("a0.wav", "dlgA", 0), ref("a1.wav", "dlgA", 1, 10) });
	run(queue, device, 10);
	device.stop(1);
	run(queue, device, 41);  // ticks 11..51: the hold ran out at 50, the delay (62) started at 51
	TEST_EXPECT(queue.slots()[0].timer == (0x80000000u | 62u));
	queue.enqueue("dlgB", { ref("b0.wav", "dlgB", 0) });
	run(queue, device, 1);  // tick 52: B0 on the free channel
	TEST_EXPECT(device.loads == names({ "dlgA:0", "dlgB:0" }));
	run(queue, device, 62);  // ticks 53..114: A1 loads at 114 over B0
	TEST_EXPECT(device.loads == names({ "dlgA:0", "dlgB:0", "dlgA:1" }));
	TEST_EXPECT(queue.voices() == std::vector<uint64_t>({ 2, 3 }));
	queue.enqueue("dlgC", { ref("c0.wav", "dlgC", 0) });
	run(queue, device, 1);
	TEST_EXPECT(device.loads.size() == 3);  // A1, the last voice, plays
	device.stop(3);
	run(queue, device, 1);  // B0 still plays, but the last voice stopped
	TEST_EXPECT(device.loads.back() == "dlgC:0");
	TEST_EXPECT(queue.voices() == std::vector<uint64_t>({ 2, 4 }));
	return 0;
}

// Sixteen dialogs at once; a seventeenth is dropped [orig: Dialog_Register
// @0x44d9c3]. A dialog leaves its slot once its last line has held and the
// channel is free, and the slots behind it close up [orig: Dialog_FreeByName
// @0x44db40 -> sub_44DAF0 @0x44daf0].
int test_sixteen_slots_and_the_free() {
	DialogQueue queue;
	Device device;
	for (int i = 0; i < 16; ++i) {
		const std::string dialog = "dlg" + std::to_string(i);
		TEST_EXPECT(queue.enqueue(dialog, { ref("", dialog.c_str(), 0) }));
	}
	TEST_EXPECT(!queue.enqueue("dlgX", { ref("x.wav") }));
	TEST_EXPECT(queue.slots().size() == 16);
	// Each missing line holds 12 (counting from tick 2), so the dialogs leave on tick 15.
	run(queue, device, 14);
	TEST_EXPECT(queue.slots().size() == 16 && device.loads.size() == 16);
	run(queue, device, 1);
	TEST_EXPECT(queue.slots().empty());
	TEST_EXPECT(queue.enqueue("dlgY", { ref("y.wav", "dlgY", 0) }));
	run(queue, device, 1);
	TEST_EXPECT(device.loads.back() == "dlgY:0");
	return 0;
}

// Dialog_ResetAll empties the slots and leaves the voices playing, so a dialog
// played after it waits for the channel; a teardown forgets both [orig:
// Dialog_ResetAll @0x44dc90].
int test_reset_and_clear() {
	DialogQueue queue;
	Device device;
	queue.enqueue("dlgA", { ref("a0.wav", "dlgA", 0), ref("a1.wav", "dlgA", 1) });
	run(queue, device, 1);
	queue.discard_pending();
	TEST_EXPECT(queue.slots().empty() && queue.voices().size() == 1);
	queue.enqueue("dlgB", { ref("b0.wav", "dlgB", 0) });
	run(queue, device, 5);
	TEST_EXPECT(device.loads == names({ "dlgA:0" }));
	device.stop(1);
	run(queue, device, 1);
	TEST_EXPECT(device.loads == names({ "dlgA:0", "dlgB:0" }));
	queue.clear();
	TEST_EXPECT(queue.slots().empty() && queue.voices().empty());
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
	failed |= test_a_line_holds_about_twice_its_wave();
	failed |= test_the_next_line_waits_hold_channel_delay();
	failed |= test_the_hold_starts_after_a_rendered_frame();
	failed |= test_a_missing_wave_holds_twelve_ticks();
	failed |= test_dialogs_interleave_in_slot_order();
	failed |= test_the_delay_ignores_the_channel();
	failed |= test_sixteen_slots_and_the_free();
	failed |= test_reset_and_clear();
	failed |= test_client_line_resolution();
	if (failed) {
		return 1;
	}
	std::printf("dialog_queue_test: OK\n");
	return 0;
}
