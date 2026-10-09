// The dialog preview (dialog_preview.h, ADR 0046 DI-32).
#include <editor/preview/dialog_preview.h>

#include <cstdio>
#include <optional>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <base/vfs/file_source.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>

namespace opennova::editor {

namespace {

// How long a line with no wave holds the dialog: the game's 12 ticks (audio::kDialogMissingHold).
constexpr double kNoWaveSeconds = double(audio::kDialogMissingHold) / io::kTickHz;

std::string seconds_words(double seconds) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f s", seconds);
	return text;
}

} // namespace

bool read_dialog_sources(const FileSource &files, const std::string &bank_name, const std::string &mission_base,
                         DialogSources &out, std::string &error) {
	out = DialogSources();
	out.bank_name = bank_name;
	std::vector<uint8_t> bytes;
	if (!files.read(bank_name, bytes)) {
		error = "The project has no dialog bank " + bank_name + ".";
		return false;
	}
	if (!dbf::parse_dbf_memory(bytes.data(), bytes.size(), out.bank, error)) {
		error = bank_name + " could not be read: " + error + ".";
		return false;
	}
	out.bank_read = true;
	for (const bool alternate : {false, true}) {
		const std::string sounds = mission::dialog_sounds_name(bank_name, alternate);
		if (files.stamp(sounds) == 0) continue;
		out.sounds_name = sounds;
		std::vector<uint8_t> lwf_bytes;
		std::string lwf_error;
		out.sounds_read = files.read(sounds, lwf_bytes) &&
		                  lwf::parse_lwf_buffer(lwf_bytes.data(), lwf_bytes.size(), out.sounds, lwf_error);
		break;
	}
	// The mission's text as the game resolves it: <base>.bin where it is, else medmssn.bin.
	mission::BootFileSource source;
	source.has_file = [&](const std::string &name) { return files.stamp(name) != 0; };
	source.read_file = [&](const std::string &name, std::vector<uint8_t> &bytes) { return files.read(name, bytes); };
	std::vector<uint8_t> text_bytes;
	const mission::MissionTextSource text = mission::resolve_mission_text(source, mission_base, text_bytes);
	if (text != mission::MissionTextSource::kNone) {
		const mission::Sidecar &row = *mission::sidecar_for_role("text");
		out.text_name = text == mission::MissionTextSource::kMission ? mission_base + row.extension : row.fallback;
		std::string text_error;
		out.text_read = rtxt::parse(text_bytes.data(), text_bytes.size(), out.text, text_error);
	}
	return true;
}

std::string dialog_name_given(const std::string &value) {
	if (!value.empty() && strutil::all_digits(value))
		if (const std::optional<int> number = strutil::parse_int(value)) return audio::dialog_name_of(*number);
	return value;
}

DialogPlay plan_dialog_play(const DialogSources &sources, const std::string &dialog, int line,
                            const std::function<double(const std::string &file)> &seconds_of) {
	DialogPlay out;
	out.dialog = dialog_name_given(dialog);
	out.bank = sources.bank_name;
	out.sounds = sources.sounds_read ? sources.sounds_name : std::string();
	const dbf::Group *group = sources.bank_read ? audio::find_dialog(sources.bank, out.dialog) : nullptr;
	if (out.dialog.empty() || !group) {
		out.words = out.dialog.empty() ? std::string("Dialog 0 plays nothing: the game plays no dialog for it.")
		                               : sources.bank_name + " has no dialog " + out.dialog +
		                                         " (the game matches a dialog's name exactly): it plays nothing.";
		return out;
	}
	if (line >= 0 && size_t(line) >= group->lines.size()) {
		out.words = out.dialog + " has " + std::to_string(group->lines.size()) + " line" +
		            (group->lines.size() == 1 ? "" : "s") + ": it has no line " + std::to_string(line) + ".";
		return out;
	}
	out.found = true;
	const lwf::File *sounds = sources.sounds_read ? &sources.sounds : nullptr;
	const rtxt::File *text = sources.text_read ? &sources.text : nullptr;
	double at = 0.0;
	std::string said;
	for (size_t i = 0; i < group->lines.size(); ++i) {
		if (line >= 0 && size_t(line) != i) continue;
		const dbf::Line &entry = group->lines[i];
		DialogPlayLine played;
		played.index = int(i);
		played.wave = entry.def_id_name;
		if (const lwf::Single *wave = sounds ? audio::find_dialog_wave(*sounds, entry.def_id_name) : nullptr) {
			played.file = wave->path;
			played.volume = audio::dialog_wave_volume(*wave);
		}
		played.text = audio::dialog_line_text(text, entry);
		// A line after the first waits its delay once the one before has ended [orig: @ 0x44e585]; a line played
		// alone starts at once.
		played.wait_s = (line < 0 && i > 0) ? double(entry.delay) / 10.0 : 0.0;
		at += played.wait_s;
		played.start_s = at;
		const double length = played.file.empty() || !seconds_of ? 0.0 : seconds_of(io::utf8_file_name(played.file));
		played.seconds = played.file.empty() ? kNoWaveSeconds : length;
		at += played.seconds;
		std::string words = played.wave.empty() ? std::string("(no wave)") : played.wave;
		if (played.file.empty()) words += " (the sounds have no wave of the name: \"EX Cannot load audio\")";
		else words += " (" + io::utf8_file_name(played.file) + (played.volume != 255 ? ", volume " + std::to_string(played.volume) : "") + ")";
		if (played.wait_s > 0.0) words += " after " + seconds_words(played.wait_s);
		if (!played.text.empty()) words += " \"" + played.text + "\"";
		said += (said.empty() ? "" : "; ") + words;
		out.lines.push_back(std::move(played));
	}
	out.words = out.dialog + (line >= 0 ? " line " + std::to_string(line) : std::string()) + " in " + sources.bank_name +
	            (out.sounds.empty() ? std::string(", which has no sounds beside it") : std::string()) + ": " + said;
	return out;
}

} // namespace opennova::editor
