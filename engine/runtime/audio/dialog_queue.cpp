#include <runtime/audio/dialog_queue.h>

#include <cstdio>
#include <utility>

namespace opennova::audio {

std::vector<std::string> resolve_dialog_sets(const dbf::File *dialog_bank,
		const SoundSetIndex &sets, int wav_id) {
	std::vector<std::string> out;
	char dlg_id[32];
	std::snprintf(dlg_id, sizeof(dlg_id), "dlg%03d", wav_id);
	if (dialog_bank != nullptr) {
		if (const dbf::Group *group = dbf::find_group(*dialog_bank, dlg_id)) {
			for (const dbf::Line &line : group->lines) {
				if (!line.def_id_name.empty() && sets.has(line.def_id_name)) {
					out.push_back(line.def_id_name);
				}
			}
		}
	}
	if (!out.empty()) {
		return out;
	}
	char upper_id[32];
	std::snprintf(upper_id, sizeof(upper_id), "DLG%03d", wav_id);
	char bare_id[32];
	std::snprintf(bare_id, sizeof(bare_id), "%d", wav_id);
	for (const char *candidate : { upper_id, dlg_id, bare_id }) {
		if (sets.has(candidate)) {
			out.emplace_back(candidate);
			return out;
		}
	}
	return out;
}

// [orig: Dialog_Register @ 0x44d980 queues the line behind whatever plays]
void DialogQueue::enqueue(const std::vector<std::string> &set_names) {
	for (const std::string &name : set_names) {
		pending_.push_back(name);
	}
}

// [orig: Dialog_UpdatePlayback @ 0x44e470 only loads the next clip once the
//  active channel frees]
bool DialogQueue::take_next(std::string &r_set_name) {
	if (line_active_ || pending_.empty()) {
		return false;
	}
	r_set_name = std::move(pending_.front());
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
