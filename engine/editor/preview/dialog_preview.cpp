// The dialog preview (dialog_preview.h, ADR 0046 DI-32).
#include <editor/preview/dialog_preview.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <base/vfs/file_source.h>
#include <formats/lwf/wav_pcm.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>

namespace opennova::editor {

namespace {

// The most ticks a dialog's play is run for (an hour of the clock): a guard, no game rule.
constexpr int32_t kDialogTicksMost = int32_t(io::kTickHz * 3600.0);

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

DialogWave dialog_wave(const std::vector<uint8_t> &bytes) {
	DialogWave out;
	lwf::WavPcm pcm;
	std::string error;
	if (bytes.empty() || !lwf::wav_decode_pcm16(bytes.data(), bytes.size(), pcm, error)) return out;
	out.loaded = true;
	out.samples = pcm.loader_samples;
	out.pitch_q16 = pcm.loader_pitch_q16;
	const size_t frames = pcm.channels ? pcm.pcm16.size() / (size_t(2) * pcm.channels) : 0;
	out.seconds = pcm.sample_rate ? double(frames) / double(pcm.sample_rate) : 0.0;
	return out;
}

// --- DialogChannel ----------------------------------------------------------------------------------

bool DialogChannel::enqueue(const std::string &dialog, std::vector<audio::DialogLineRef> lines) {
	return queue_.enqueue(dialog, std::move(lines));
}

void DialogChannel::tick(int32_t tick, const WaveOf &wave_of, std::vector<Loaded> &out) {
	// The device's half of Dialog_LoadAudioClip: the line's wave loaded and its voice started on the channel, a
	// wave that does not load no voice (audio::DialogQueue::LoadLine).
	const auto load = [&](const audio::DialogLineRef &line) {
		Loaded loaded;
		loaded.tick = tick;
		loaded.line = line;
		if (!line.file.empty() && wave_of) loaded.wave = wave_of(io::utf8_file_name(line.file));
		audio::DialogClip clip;
		clip.loaded = loaded.wave.loaded;
		clip.samples = loaded.wave.samples;
		clip.pitch_q16 = loaded.wave.pitch_q16;
		if (clip.loaded) {
			clip.voice = next_voice_++;
			const int32_t length = std::max(1, int32_t(std::ceil(loaded.wave.seconds * io::kTickHz)));
			voices_.push_back({clip.voice, tick + length});
		}
		loaded.hold = audio::dialog_clip_hold(clip);
		out.push_back(std::move(loaded));
		return clip;
	};
	// AudioChannel_ValidateHandle: the voice plays until its wave's length has run.
	const auto playing = [&](uint64_t voice) {
		for (const auto &[held, ends] : voices_)
			if (held == voice) return tick < ends;
		return false;
	};
	// A frame drawn before every tick: a fresh hold counts from the tick after it loaded.
	queue_.tick(true, load, playing);
	const std::vector<uint64_t> &kept = queue_.voices();
	voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
	                             [&](const std::pair<uint64_t, int32_t> &voice) {
		                             return std::find(kept.begin(), kept.end(), voice.first) == kept.end();
	                             }),
	              voices_.end());
}

void DialogChannel::clear() {
	queue_.clear();
	voices_.clear();
}

DialogPlay plan_dialog_play(const DialogSources &sources, const std::string &dialog, int line,
                            const DialogChannel::WaveOf &wave_of) {
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
	// Its lines as the game's register takes them, each with the wave the bank's sounds give it (the one line alone
	// where one is asked); the game's queue run on an idle channel until the dialog leaves its slot.
	std::vector<audio::DialogLineRef> refs;
	for (size_t i = 0; i < group->lines.size(); ++i) {
		if (line >= 0 && size_t(line) != i) continue;
		const dbf::Line &entry = group->lines[i];
		audio::DialogLineRef ref;
		ref.wave = entry.def_id_name;
		ref.delay = entry.delay;
		ref.dialog_name = group->group_name;
		ref.line = int(i);
		if (const lwf::Single *wave = sounds ? audio::find_dialog_wave(*sounds, entry.def_id_name) : nullptr) {
			ref.file = wave->path;
			ref.volume = audio::dialog_wave_volume(*wave);
		}
		refs.push_back(std::move(ref));
	}
	DialogChannel channel;
	channel.enqueue(group->group_name, refs);
	std::vector<DialogChannel::Loaded> loaded;
	for (int32_t tick = 0; tick < kDialogTicksMost && !channel.idle(); ++tick) channel.tick(tick, wave_of, loaded);
	std::string said;
	for (const DialogChannel::Loaded &each : loaded) {
		const dbf::Line &entry = group->lines[size_t(each.line.line)];
		DialogPlayLine played;
		played.index = each.line.line;
		played.wave = each.line.wave;
		played.file = each.line.file;
		played.volume = each.line.volume;
		played.text = audio::dialog_line_text(text, entry);
		played.wait_s = (line < 0 && each.line.line > 0) ? double(each.line.delay) / 10.0 : 0.0;
		played.start_tick = each.tick;
		played.start_s = double(each.tick) / io::kTickHz;
		played.hold_ticks = each.hold;
		played.seconds = each.wave.loaded ? each.wave.seconds : 0.0;
		std::string words = played.wave.empty() ? std::string("(no wave)") : played.wave;
		if (played.file.empty()) words += " (the sounds have no wave of the name: \"EX Cannot load audio\")";
		else words += " (" + io::utf8_file_name(played.file) + (played.volume != 255 ? ", volume " + std::to_string(played.volume) : "") + ")";
		if (played.start_tick > 0) words += " at " + seconds_words(played.start_s);
		if (played.wait_s > 0.0) words += " (after " + seconds_words(played.wait_s) + ")";
		if (!played.text.empty()) words += " \"" + played.text + "\"";
		said += (said.empty() ? "" : "; ") + words;
		out.lines.push_back(std::move(played));
	}
	out.words = out.dialog + (line >= 0 ? " line " + std::to_string(line) : std::string()) + " in " + sources.bank_name +
	            (out.sounds.empty() ? std::string(", which has no sounds beside it") : std::string()) + ": " + said;
	return out;
}

} // namespace opennova::editor
