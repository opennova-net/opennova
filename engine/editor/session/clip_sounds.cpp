#include <editor/session/clip_sounds.h>

#include <algorithm>

#include <editor/preview/definition_viewport.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The Preview's viewport of `kind`, followed now where no device follows it (a headless editor's, a test's),
// as a read of it would, so what it fires from is what the selection shows; null for none.
ViewportModel *previewed(SessionCore &core, ViewportKind kind) {
	const SessionView &view = core.view();
	Viewports &viewports = core.viewports();
	const std::string &path = view.documents.previews[kind].path;
	if (path.empty()) return nullptr;
	ViewportModel *model = viewports.find(path, kind);
	if (model && !model->attached()) model = viewports.follow_one(view, path, kind);
	return model;
}

// The sounds a viewport keeps of those it fired (a model's, a definition's), null for a kind that fires none.
const std::vector<ClipSoundFired> *fired_by(const ViewportModel &model) {
	if (const auto *clip = dynamic_cast<const ModelViewport *>(&model)) return &clip->sounds_fired();
	if (const auto *definition = dynamic_cast<const DefinitionViewport *>(&model)) return &definition->sounds_fired();
	if (const auto *mission = dynamic_cast<const MissionViewport *>(&model)) return &mission->sounds_fired();
	return nullptr;
}

} // namespace

void fire_clip_sounds(SessionCore &core) {
	const SessionView &view = core.view();
	Viewports &viewports = core.viewports();
	if (!view.project.open || !viewports.clock().playing()) return;
	bool fired = false;
	// A clip's events (DI-04), or a model's death as its damage state plays it (DI-10).
	if (auto *clip = dynamic_cast<ModelViewport *>(previewed(core, ViewportKind::Model)))
		fired = !(clip->animating()
		                  ? clip->fire_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(),
		                                      viewports.clip_sound_seq())
		                  : clip->fire_damage_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(),
		                                             viewports.clip_sound_seq()))
		                 .empty();
	// A definition's death as its state plays it (DI-21).
	if (auto *definition = dynamic_cast<DefinitionViewport *>(previewed(core, ViewportKind::Definition)))
		fired = !definition->fire_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(),
		                                 viewports.clip_sound_seq())
		                 .empty() ||
		        fired;
	// A mission's Shoot tool's shots (DI-23), each mission viewport's.
	std::vector<std::string> missions;
	for (size_t i = 0; i < viewports.size(); ++i)
		if (viewports.at(i).kind() == ViewportKind::Mission) missions.push_back(viewports.at(i).path());
	for (const std::string &mission_path : missions)
		if (auto *mission = dynamic_cast<MissionViewport *>(viewports.find(mission_path, ViewportKind::Mission)))
			fired = !mission->fire_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(),
			                              viewports.clip_sound_seq())
			                 .empty() ||
			        fired;
	if (fired) core.touch(ViewConcern::Viewports);
}

std::vector<ClipSoundPlay> clip_sounds_since(SessionCore &core, uint64_t after) {
	std::vector<ClipSoundPlay> out;
	Viewports &viewports = core.viewports();
	for (size_t i = 0; i < viewports.size(); ++i) {
		const std::vector<ClipSoundFired> *sounds = fired_by(viewports.at(i));
		if (!sounds) continue;
		for (const ClipSoundFired &fired : *sounds) {
			if (fired.seq <= after || fired.state != "played") continue;
			ClipSoundPlay play;
			play.seq = fired.seq;
			for (const ClipSoundFired::Voice &voice : fired.voices)
				if (!voice.path.empty()) play.voices.push_back({voice.path, voice.pitch_q16, voice.volume});
			if (!play.voices.empty()) out.push_back(std::move(play));
		}
	}
	std::sort(out.begin(), out.end(), [](const ClipSoundPlay &a, const ClipSoundPlay &b) { return a.seq < b.seq; });
	return out;
}

} // namespace opennova::editor
