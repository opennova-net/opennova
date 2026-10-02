#include <runtime/audio/dialog_queue.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace opennova::audio {

std::vector<DialogLineRef> resolve_dialog_lines(const dbf::File *dialog_bank,
		const SoundSetIndex &sets, int wav_id) {
	std::vector<DialogLineRef> out;
	char dlg_id[32];
	std::snprintf(dlg_id, sizeof(dlg_id), "dlg%03d", wav_id);
	bool carried = false;
	if (dialog_bank != nullptr) {
		if (const dbf::Group *group = dbf::find_group(*dialog_bank, dlg_id)) {
			for (size_t i = 0; i < group->lines.size(); ++i) {
				const dbf::Line &line = group->lines[i];
				DialogLineRef ref;
				ref.dialog_name = group->group_name;
				ref.line = static_cast<int>(i);
				if (!line.def_id_name.empty() && sets.has(line.def_id_name)) {
					ref.set_name = line.def_id_name;
					carried = true;
				}
				out.push_back(std::move(ref));
			}
		}
	}
	if (carried) {
		return out;
	}
	out.clear();
	char upper_id[32];
	std::snprintf(upper_id, sizeof(upper_id), "DLG%03d", wav_id);
	char bare_id[32];
	std::snprintf(bare_id, sizeof(bare_id), "%d", wav_id);
	for (const char *candidate : { upper_id, dlg_id, bare_id }) {
		if (sets.has(candidate)) {
			DialogLineRef ref;
			ref.set_name = candidate;
			out.push_back(std::move(ref));
			return out;
		}
	}
	return out;
}

DialogLinePlayback resolve_dialog_line(const dbf::File *dialog_bank, const SoundSetIndex &sets,
		const rtxt::File *mission_text, const std::string &dialog_name, int line,
		int player_class) {
	DialogLinePlayback out;
	if (dialog_bank == nullptr) return out;
	// The loaded list in load order, the first exact (strcmp) name match.
	const dbf::Group *group = nullptr;
	for (const dbf::Group &candidate : dialog_bank->groups) {
		if (candidate.group_name == dialog_name) {
			group = &candidate;
			break;
		}
	}
	if (group == nullptr || line < 0 || static_cast<size_t>(line) >= group->lines.size())
		return out;
	const dbf::Line &entry = group->lines[static_cast<size_t>(line)];
	out.line_found = true;
	out.def_id_name = entry.def_id_name;
	// sprintf("%d%s", localeId, baseFilename), then the bare name [orig: @0x44df3d..0x44df96].
	const std::string localized = std::to_string(player_class) + entry.def_id_name;
	if (sets.has(localized))
		out.set_name = localized;
	else if (sets.has(entry.def_id_name))
		out.set_name = entry.def_id_name;
	if (mission_text == nullptr) return out;
	// [Mission Dialog] by the def name [orig: @0x44e05d..0x44e06b].
	if (const rtxt::Entry *text = mission_text->find_in_section("Mission Dialog", entry.def_id_name)) {
		out.text = text->text;
		return out;
	}
	// Else the flat entry the sequence's suffix after its last '_' names: the
	// walk tests the character before the cursor from the string's last
	// character back [orig: @0x44e072..0x44e0c2; IniSection_GetEntryByIndex @0x75D130].
	const std::string &sequence = entry.sequence;
	for (size_t cursor = sequence.size() > 0 ? sequence.size() - 1 : 0; cursor >= 1; --cursor) {
		if (sequence[cursor - 1] != '_') continue;
		const unsigned long index = std::strtoul(sequence.c_str() + cursor, nullptr, 10);
		if (index < mission_text->entries.size()) out.text = mission_text->entries[index].text;
		break;
	}
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
