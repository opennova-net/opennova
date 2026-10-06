#include <editor/session/sound_play.h>

#include <optional>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/sound_bank_document.h>
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
			if (!read_file_bytes(join_path(view.project.root, entry.relative_path), bytes, error) ||
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
