#include <editor/session/sound_play.h>

#include <optional>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/sound_bank_document.h>
#include <editor/import/wave_source.h>
#include <editor/preview/dialog_preview.h>
#include <editor/assets/project_asset_source.h>
#include <runtime/mission/mission_sidecars.h>
#include <editor/documents/sound_profile_document.h>
#include <editor/model/finding_code_row.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

const std::string &value_of(const EditorRequest &request, const char *token) {
	static const std::string none;
	for (const auto &[key, value] : request.values)
		if (strutil::iequals(key, token)) return value;
	return none;
}

bool has_value(const EditorRequest &request, const char *token) {
	for (const auto &entry : request.values)
		if (strutil::iequals(entry.first, token)) return true;
	return false;
}

void refuse(SessionCore &core, const std::string &message, const std::string &asset = std::string()) {
	core.refuse_request(CoreFinding::WorkspaceRefused, message, asset);
}

// The sound the Shell plays from now on: `voices`, each a wave of the project, in place of any.
void start(SessionCore &core, std::vector<WorkspaceView::Voice> voices, const std::string &set, const std::string &bank,
           const std::string &words) {
	WorkspaceView::Sound &sound = core.view().workspace.sound;
	sound.path = voices.empty() ? std::string() : voices.front().path;
	sound.voices = std::move(voices);
	sound.set = set;
	sound.bank = bank;
	sound.words = words;
	++sound.serial;
	sound.state = WorkspaceView::SoundState::Starting;
	sound.error.clear();
	core.touch(ViewConcern::Workspace);
	if (!words.empty()) {
		// A set's words name the set first ("Playing FSP_DIRT_L in game.lwf: ..."); a slot's are a sentence of
		// their own ("default's SSLFootGND plays FSP_DIRT_L in game.lwf: ...").
		const bool set_first = !set.empty() && words.compare(0, set.size(), set) == 0;
		core.view().activity.status = set_first ? "Playing " + words : words;
		core.touch(ViewConcern::Output);
	}
}

// A planned play's voices as project waves; a voice whose file the project lacks, or that is past what a
// card reads, left out and said.
void start_play(SessionCore &core, const PreviewPlay &play) {
	const SessionView &view = core.view();
	if (!play.found) return refuse(core, play.words, play.bank_path);
	std::vector<WorkspaceView::Voice> voices;
	std::string missing;
	for (const PreviewVoice &voice : play.voices) {
		const AssetEntry *entry = voice.file.empty() ? nullptr : view.project.scan->find(voice.file);
		if (!entry || entry->kind != AssetKind::Wave || entry->size_bytes > kWaveCardBytes) {
			missing += (missing.empty() ? "" : ", ") + (voice.file.empty() ? voice.wave : voice.file);
			continue;
		}
		voices.push_back({entry->relative_path, voice.pitch_q16, voice.volume});
	}
	if (voices.empty())
		return refuse(core,
		              play.voices.empty() ? play.words
		                                  : play.words + " The project has no wave it plays (" + missing + ").",
		              play.bank_path);
	start(core, std::move(voices), play.set, play.bank,
	      missing.empty() ? play.words : play.words + " (the project lacks " + missing + ")");
}

// values {frame}: what the clip the animation document at `path` (the active one when left out) plays at
// that frame, as a press of its mark on the timeline asks (DI-04: ModelViewport::press_event, under its
// model viewport's sound options), every sound of the event at once.
void play_clip_event(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	const std::optional<int> frame = strutil::parse_int(value_of(request, "frame"));
	if (!frame || *frame < 0) return refuse(core, "frame is a clip's frame, a whole number from 0.");
	std::string error;
	auto *clip = dynamic_cast<ModelViewport *>(
			core.viewports().resolve(view, request.path, ViewportKind::Model, error));
	if (!clip || !clip->animating())
		return refuse(core, !error.empty() ? error : "No clip plays in the model preview of " + request.path + ".",
		              request.path);
	std::vector<ClipSoundFired> fired;
	if (!clip->press_event(*frame, view.project.scan.get(), core.sound_selector(), fired, error))
		return refuse(core, error, clip->path());
	std::vector<WorkspaceView::Voice> voices;
	std::string words;
	for (const ClipSoundFired &one : fired) {
		words += (words.empty() ? "" : " ") + one.words;
		for (const ClipSoundFired::Voice &voice : one.voices)
			if (!voice.path.empty()) voices.push_back({voice.path, voice.pitch_q16, voice.volume});
	}
	if (voices.empty()) return refuse(core, words, clip->path());
	start(core, std::move(voices), fired.front().set, fired.front().bank, words);
}

// values {leg}: the set the weapon action playing the row of a first-person map plays as it begins (begin:
// its soundset) or finishes (end: its soundsetend), as a press of the leg's mark asks (DI-13:
// ModelViewport::press_leg).
void play_action_leg(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	const std::string &leg = value_of(request, "leg");
	if (!strutil::iequals(leg, "begin") && !strutil::iequals(leg, "end"))
		return refuse(core, "leg is begin (the action's soundset) or end (its soundsetend).");
	std::string error;
	auto *clip = dynamic_cast<ModelViewport *>(
			core.viewports().resolve(view, request.path, ViewportKind::Model, error));
	if (!clip || !clip->animating())
		return refuse(core, !error.empty() ? error : "No clip plays in the model preview of " + request.path + ".",
		              request.path);
	std::vector<ClipSoundFired> fired;
	if (!clip->press_leg(strutil::iequals(leg, "end"), core.sound_selector(), view.project.scan.get(), fired, error))
		return refuse(core, error, clip->path());
	std::vector<WorkspaceView::Voice> voices;
	for (const ClipSoundFired::Voice &voice : fired.front().voices)
		if (!voice.path.empty()) voices.push_back({voice.path, voice.pitch_q16, voice.volume});
	if (voices.empty()) return refuse(core, fired.front().words, clip->path());
	start(core, std::move(voices), fired.front().set, fired.front().bank, fired.front().words);
}

// values {dialog, line?}: a dialog of the dialog bank `path` names, or of the bank the mission `path` loads, its
// lines one after another as the game plays them (DI-32, preview/dialog_preview), every line a voice starting where
// the one before it has ended, after its wait.
void play_dialog(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	const std::shared_ptr<const ProjectAssetSource> files = view.findings.assets;
	if (!files) return refuse(core, "No project is open.");
	int line = -1;
	if (has_value(request, "line")) {
		const std::optional<int> number = strutil::parse_int(value_of(request, "line"));
		if (!number || *number < 0) return refuse(core, "line is a dialog's line by its index, a whole number from 0.");
		line = *number;
	}
	const AssetEntry *entry = request.path.empty() ? nullptr : view.project.scan->named(request.path);
	if (!entry) return refuse(core, "A dialog plays from the dialog bank or the mission path names: " +
	                                        (request.path.empty() ? std::string("none is named.") : "the project has no " + request.path + "."));
	// The bank and the mission text the subtitles read: a dialog bank's own name's, or the mission's.
	std::string bank, text;
	if (entry->kind == AssetKind::DialogBank) {
		bank = entry->logical_name;
		text = mission::mission_base_name(bank) + ".bin";
	} else if (entry->kind == AssetKind::Mission) {
		std::unique_ptr<MissionDocument> loaded;
		const auto *mission = dynamic_cast<const MissionDocument *>(open_at(view, entry->relative_path));
		if (!mission && view.project.document) {
			loaded = std::make_unique<MissionDocument>();
			Diagnostic error;
			if (loaded->load(join_path(view.project.root, entry->relative_path), entry->relative_path, entry->kind,
			                 view.project.document->target_game, error))
				mission = loaded.get();
		}
		if (!mission) return refuse(core, entry->logical_name + " could not be read.", entry->relative_path);
		bank = mission_dialog_bank(*mission);
		text = mission::mission_base_name(entry->logical_name) + ".bin";
	} else {
		return refuse(core, entry->logical_name + " is no dialog bank or mission: a dialog plays from one.", entry->relative_path);
	}
	DialogSources sources;
	std::string error;
	if (!read_dialog_sources(*files, bank, text, sources, error)) return refuse(core, error, entry->relative_path);
	// A wave's length as the game decodes it, its file the project's.
	const auto seconds_of = [&](const std::string &file) {
		std::vector<uint8_t> bytes;
		return files->read(file, bytes) ? wave_seconds(bytes) : 0.0;
	};
	const DialogPlay play = plan_dialog_play(sources, value_of(request, "dialog"), line, seconds_of);
	if (!play.found) return refuse(core, play.words, entry->relative_path);
	std::vector<WorkspaceView::Voice> voices;
	std::string missing;
	for (const DialogPlayLine &played : play.lines) {
		if (played.file.empty()) continue;
		const AssetEntry *wave = view.project.scan->find(io::utf8_file_name(played.file));
		if (!wave || wave->kind != AssetKind::Wave || wave->size_bytes > kWaveCardBytes) {
			missing += (missing.empty() ? "" : ", ") + io::utf8_file_name(played.file);
			continue;
		}
		voices.push_back({wave->relative_path, 0x10000u, played.volume, int32_t(played.start_s * 1000.0 + 0.5)});
	}
	if (voices.empty())
		return refuse(core, play.words + (missing.empty() ? std::string(" No line plays a wave.")
		                                                  : " The project has no wave it plays (" + missing + ")."),
		              entry->relative_path);
	start(core, std::move(voices), play.dialog, play.bank,
	      missing.empty() ? play.words : play.words + " (the project lacks " + missing + ")");
}

// A slot by its keyword, without case, or its number; -1 for none.
int slot_named(const std::string &text) {
	if (const int slot = sound_profile_slot_of(text); slot >= 0) return slot;
	if (const std::optional<int> number = strutil::parse_int(text); number && *number >= 0 &&
	    *number < audio::kSoundProfileSlotCount)
		return *number;
	return -1;
}

} // namespace

std::vector<PreviewBank> project_banks(const SessionView &view) {
	std::vector<PreviewBank> banks;
	if (!view.project.open || !view.project.scan) return banks;
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (entry.kind != AssetKind::SoundBank) continue;
		PreviewBank bank;
		bank.name = entry.logical_name;
		bank.path = entry.relative_path;
		if (const auto *document = dynamic_cast<const SoundBankDocument *>(open_at(view, entry.relative_path))) {
			if (!document->bank(bank.file)) continue;
		} else {
			std::vector<uint8_t> bytes;
			std::string error;
			if (!io::read_file_bytes(join_path(view.project.root, entry.relative_path), bytes, error) ||
			    !lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank.file, error))
				continue;
		}
		banks.push_back(std::move(bank));
	}
	return banks;
}

std::vector<audio::SoundProfile> project_profiles(const SessionView &view) {
	if (!view.project.open || !view.project.scan || !view.project.document) return {};
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (entry.kind != AssetKind::SoundProfileDefs) continue;
		if (const auto *document = dynamic_cast<const SoundProfileDocument *>(open_at(view, entry.relative_path)))
			return document->profiles();
		SoundProfileDocument document;
		Diagnostic error;
		if (document.load(join_path(view.project.root, entry.relative_path), entry.relative_path, entry.kind,
		                  view.project.document->target_game, error))
			return document.profiles();
	}
	return {};
}

std::string project_expansion(const SessionView &view) {
	return view.project.document ? view.project.document->expansion.name : std::string();
}

void serve_sound_play(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	if (!view.project.open || !view.project.scan) return refuse(core, "No project is open.");
	if (has_value(request, "frame")) return play_clip_event(core, request);
	if (has_value(request, "leg")) return play_action_leg(core, request);
	if (has_value(request, "dialog")) return play_dialog(core, request);
	const std::string &set = value_of(request, "set");
	const bool slot_play = has_value(request, "slot") || has_value(request, "profile") || has_value(request, "surface");
	if (!set.empty()) {
		// A bank `path` names, else the chain.
		std::string only;
		if (!request.path.empty()) {
			const AssetEntry *entry = view.project.scan->named(request.path);
			if (!entry || entry->kind != AssetKind::SoundBank)
				return refuse(core, "No sound bank of the project is " + request.path + ".", request.path);
			only = entry->logical_name;
		}
		return start_play(core, plan_set_play(project_banks(view), project_expansion(view), set, only,
		                                      core.sound_selector()));
	}
	if (slot_play) {
		const std::vector<audio::SoundProfile> profiles = project_profiles(view);
		const std::string profile = has_value(request, "profile") ? value_of(request, "profile") : std::string("default");
		const std::string &slot_text = value_of(request, "slot");
		if (slot_text.empty() || strutil::iequals(slot_text, "footstep") || has_value(request, "surface")) {
			FootSurface surface = FootSurface::Ground;
			if (has_value(request, "surface") && !foot_surface_of(value_of(request, "surface"), surface))
				return refuse(core, "A surface is ground, snow, object or water.");
			const std::string &foot = value_of(request, "foot");
			if (!foot.empty() && !strutil::iequals(foot, "left") && !strutil::iequals(foot, "right"))
				return refuse(core, "A foot is left or right.");
			return start_play(core, plan_footstep_play(profiles, profile, surface, strutil::iequals(foot, "right") ? 1 : 0,
			                                           project_banks(view), project_expansion(view), core.sound_selector()));
		}
		const int slot = slot_named(slot_text);
		if (slot < 0) return refuse(core, "'" + slot_text + "' names no profile slot: give its keyword (SSLFootGND) or 0 to 50.");
		return start_play(core, plan_slot_play(profiles, profile, slot, project_banks(view), project_expansion(view),
		                                       core.sound_selector()));
	}
	// A wave of the project, as recorded (workspace_parts.h's play_sound).
	play_sound(core, request.path);
}

} // namespace opennova::editor
