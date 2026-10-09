#include <runtime/audio/dialog_queue.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <base/io/strutil.h>

namespace opennova::audio {

std::string dialog_name_of(int32_t dialog_index) {
	if (dialog_index == 0) return std::string();
	char name[32];
	std::snprintf(name, sizeof(name), "dlg%03i", int(dialog_index));
	return name;
}

const dbf::Group *find_dialog(const dbf::File &bank, const std::string &name) {
	for (const dbf::Group &group : bank.groups)
		if (group.group_name == name) return &group;
	return nullptr;
}

const lwf::Single *find_dialog_wave(const lwf::File &sounds, const std::string &name) {
	for (const lwf::Single &single : sounds.singles) {
		if (!strutil::iequals(single.name, name)) continue;
		// The first of the name is the one: another magic's needs its dword +28 set, else none.
		if (sounds.header.magic == lwf::kMagic) return &single;
		uint32_t word = 0;
		std::memcpy(&word, single.raw_name.data() + 28, sizeof(word));
		return word != 0 ? &single : nullptr;
	}
	return nullptr;
}

int dialog_wave_volume(const lwf::Single &wave) {
	const int byte = int(wave.value_hi >> 8);
	return byte == 0 ? 255 : byte;
}

std::vector<DialogLineRef> resolve_dialog_lines(const dbf::File *dialog_bank,
		const lwf::File *dialog_sounds, int32_t dialog_index) {
	std::vector<DialogLineRef> out;
	const std::string name = dialog_name_of(dialog_index);
	if (dialog_bank == nullptr || name.empty()) return out;
	const dbf::Group *group = find_dialog(*dialog_bank, name);
	if (group == nullptr) return out;
	for (size_t i = 0; i < group->lines.size(); ++i) {
		const dbf::Line &line = group->lines[i];
		DialogLineRef ref;
		ref.wave = line.def_id_name;
		ref.delay = line.delay;
		ref.dialog_name = group->group_name;
		ref.line = static_cast<int>(i);
		if (const lwf::Single *wave = dialog_sounds ? find_dialog_wave(*dialog_sounds, line.def_id_name) : nullptr) {
			ref.file = wave->path;
			ref.volume = dialog_wave_volume(*wave);
		}
		out.push_back(std::move(ref));
	}
	return out;
}

std::string dialog_line_text(const rtxt::File *mission_text, const dbf::Line &line) {
	if (mission_text == nullptr) return std::string();
	// [Mission Dialog] by the wave's name [orig: @0x44ddd6..0x44dde4].
	if (const rtxt::Entry *text = mission_text->find_in_section("Mission Dialog", line.def_id_name))
		return text->text;
	// Else the flat entry the sequence's suffix after its last '_' names: the walk tests the
	// character before the cursor from the string's last character back [orig: @0x44ddec..0x44de3c;
	// IniSection_GetEntryByIndex @0x75D130].
	const std::string &sequence = line.sequence;
	for (size_t cursor = sequence.size() > 0 ? sequence.size() - 1 : 0; cursor >= 1; --cursor) {
		if (sequence[cursor - 1] != '_') continue;
		const unsigned long index = std::strtoul(sequence.c_str() + cursor, nullptr, 10);
		if (index < mission_text->entries.size()) return mission_text->entries[index].text;
		break;
	}
	return std::string();
}

DialogLinePlayback resolve_dialog_line(const dbf::File *dialog_bank, const lwf::File *dialog_sounds,
		const rtxt::File *mission_text, const std::string &dialog_name, int line,
		int player_class) {
	DialogLinePlayback out;
	if (dialog_bank == nullptr) return out;
	// The loaded list in load order, the first exact (strcmp) name match.
	const dbf::Group *group = find_dialog(*dialog_bank, dialog_name);
	if (group == nullptr || line < 0 || static_cast<size_t>(line) >= group->lines.size())
		return out;
	const dbf::Line &entry = group->lines[static_cast<size_t>(line)];
	out.line_found = true;
	out.wave = entry.def_id_name;
	// sprintf("%d%s", localeId, baseFilename), then the bare name [orig: @0x44df3d..0x44df96].
	const std::string localized = std::to_string(player_class) + entry.def_id_name;
	const lwf::Single *wave = dialog_sounds ? find_dialog_wave(*dialog_sounds, localized) : nullptr;
	if (wave) out.played = localized;
	else if ((wave = dialog_sounds ? find_dialog_wave(*dialog_sounds, entry.def_id_name) : nullptr)) out.played = entry.def_id_name;
	if (wave) {
		out.file = wave->path;
		out.volume = dialog_wave_volume(*wave);
	}
	out.text = dialog_line_text(mission_text, entry);
	return out;
}

uint32_t dialog_clip_hold(const DialogClip &clip) {
	// No wave: "EX Cannot load audio", 12 ticks [orig: Dialog_LoadAudioClip @ 0x44dd74, @ 0x44dd7c].
	if (!clip.loaded) return kDialogMissingHold;
	// The rate from the pitch ratio, a signed 64-bit product taken from bit 16
	// [orig: @ 0x44dd87..0x44dda3], 12 ticks when it is 0 [orig: @ 0x44ddbf].
	const int64_t product = int64_t(int32_t(clip.pitch_q16)) * 44100 + 0x8000;
	const uint32_t rate = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 16);
	if (rate == 0) return kDialogMissingHold;
	// 32-bit unsigned throughout [orig: @ 0x44dda9..0x44ddbb].
	return 2u * ((clip.samples * kDialogTickRate + rate) / rate);
}

// The first free of the 16 slots (the table stays packed, so the one after the
// last held), the dialog at line 0 with timer 0; its wait flag is the cleared
// slot's 0 [orig: Dialog_Register @ 0x44d980, the walk @ 0x44d9b1..0x44d9c8,
// @ 0x44d9cc..0x44d9de].
bool DialogQueue::enqueue(const std::string &dialog, std::vector<DialogLineRef> lines) {
	if (slots_.size() >= kDialogSlots) return false;
	Slot slot;
	slot.dialog = dialog;
	slot.lines = std::move(lines);
	slots_.push_back(std::move(slot));
	return true;
}

// [orig: Dialog_UpdatePlayback @ 0x44e470, the slot walk @ 0x44e4d8..0x44e634]
void DialogQueue::tick(bool frame_rendered, const LoadLine &load, const VoicePlaying &playing) {
	constexpr uint32_t kDelayBit = 0x80000000u;
	constexpr uint32_t kCount = 0x7FFFFFFFu;
	size_t s = 0;
	while (s < slots_.size()) {
		Slot &slot = slots_[s];
		if ((slot.timer & kCount) != 0) {
			// A running timer drops one a tick once it counts; a fresh hold
			// starts counting on the first tick after a frame has rendered
			// [orig: @ 0x44e5f1..0x44e5fa; @ 0x44e5fe..0x44e60e, the flag
			// GameLoop_RenderFrame sets @ 0x521cff, cleared each tick @ 0x5265c3].
			if (slot.counting) --slot.timer;
			else if (frame_rendered) slot.counting = true;
			++s;
			continue;
		}
		// At 0: a run-out delay (bit 31) loads at once; a run-out hold waits for
		// the dialog channel, the last voice on its stack [orig: @ 0x44e525..0x44e544].
		const bool delay_ran_out = (slot.timer & kDelayBit) != 0;
		if (!delay_ran_out && !voices_.empty() && playing(voices_.back())) {
			++s;
			continue;
		}
		const size_t count = slot.lines.size();
		// A counted-out hold with a line left starts that line's delay, 62 *
		// DELAY / 10 ticks; a delay of 0 loads at once [orig: @ 0x44e54a..0x44e585].
		if (slot.counting && !delay_ran_out && slot.index < count) {
			slot.timer = kDialogTickRate * uint32_t(slot.lines[slot.index].delay) / 10u + kDelayBit;
			if ((slot.timer & kCount) != 0) {
				++s;
				continue;
			}
		}
		if (slot.index >= count) {
			// The last line has held and the channel is free: the dialog goes,
			// the first slot of its name, and the table closes up behind it, so
			// this index is walked again [orig: @ 0x44e5c8..0x44e5ef ->
			// Dialog_FreeByName @ 0x44db40 -> sub_44DAF0 @ 0x44daf0].
			if ((slot.timer & kCount) == 0) {
				const std::string dialog = slot.dialog;
				for (size_t f = 0; f < slots_.size(); ++f) {
					if (slots_[f].dialog != dialog) continue;
					slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(f));
					break;
				}
				continue;
			}
			++s;
			continue;
		}
		// Load the line (its wave on a new voice of the channel, which the shell
		// reports to the co-op broadcast), then hold it [orig: @ 0x44e58b..0x44e5bf;
		// Dialog_LoadAudioClip @ 0x44dcc0, the voice onto the stack @ 0x44de90..0x44deaf].
		const DialogLineRef line = slot.lines[slot.index];
		const DialogClip clip = load(line);
		if (clip.voice != 0) voices_.push_back(clip.voice);
		Slot &loaded = slots_[s];
		loaded.index += 1;
		loaded.timer = dialog_clip_hold(clip);
		loaded.counting = false;
		++s;
	}
	// The stack drops the voices that stopped, each replaced by the last
	// [orig: AudioChannel_CleanupInvalid @ 0x44d840, @ 0x44d902..0x44d927].
	size_t v = 0;
	while (v < voices_.size()) {
		if (playing(voices_[v])) {
			++v;
			continue;
		}
		voices_[v] = voices_.back();
		voices_.pop_back();
	}
}

void DialogQueue::clear() {
	slots_.clear();
	voices_.clear();
}

} // namespace opennova::audio
