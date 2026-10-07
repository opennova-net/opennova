#include <runtime/audio/dialog_queue.h>

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

// [orig: Dialog_Register @ 0x44d980 queues the line behind whatever plays]
void DialogQueue::enqueue(const std::vector<DialogLineRef> &lines) {
	for (const DialogLineRef &line : lines) {
		pending_.push_back(line);
	}
}

// [orig: Dialog_UpdatePlayback @ 0x44e470 only loads the next clip once the
//  active channel frees]
bool DialogQueue::take_next(DialogLineRef &r_line) {
	if (line_active_ || pending_.empty()) {
		return false;
	}
	r_line = std::move(pending_.front());
	pending_.pop_front();
	return true;
}

void DialogQueue::line_started() {
	line_active_ = true;
}

void DialogQueue::line_finished() {
	line_active_ = false;
}

void DialogQueue::clear() {
	pending_.clear();
	line_active_ = false;
}

} // namespace opennova::audio
