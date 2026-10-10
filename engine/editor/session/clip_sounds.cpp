#include <editor/session/clip_sounds.h>

#include <algorithm>
#include <string>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/definition_viewport.h>
#include <editor/preview/environment_viewport.h>
#include <editor/preview/menu_viewport.h>
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

// The sounds a viewport keeps of those it fired (a model's, a definition's, a mission's Shoot tool's and its Listen's,
// an environment's Listen's (S23 C), a menu's), none for a kind that fires none.
std::vector<const ClipSoundFired *> fired_by(const ViewportModel &model) {
	std::vector<const ClipSoundFired *> out;
	if (const auto *clip = dynamic_cast<const ModelViewport *>(&model))
		for (const ClipSoundFired &fired : clip->sounds_fired()) out.push_back(&fired);
	else if (const auto *definition = dynamic_cast<const DefinitionViewport *>(&model))
		for (const ClipSoundFired &fired : definition->sounds_fired()) out.push_back(&fired);
	else if (const auto *mission = dynamic_cast<const MissionViewport *>(&model)) {
		for (const ClipSoundFired &fired : mission->sounds_fired()) out.push_back(&fired);
		for (const ClipSoundFired &fired : mission->listen().sounds_fired()) out.push_back(&fired);
	} else if (const auto *environment = dynamic_cast<const EnvironmentViewport *>(&model)) {
		for (const ClipSoundFired &fired : environment->listen().sounds_fired()) out.push_back(&fired);
	} else if (const auto *menu = dynamic_cast<const MenuViewport *>(&model))
		for (const MenuSoundFired &fired : menu->sounds_fired()) out.push_back(&fired.sound);
	return out;
}

// The document open at `path`, null for none.
const DocumentBase *open_document(const SessionView &view, const std::string &path) {
	for (const auto &open : view.documents.open)
		if (open && open->path() == path) return open.get();
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
	// A mission's Shoot tool's shots (DI-23), and what a listening mission heard of its weather and its script
	// (DI-36: its device's channels play the rest), each mission viewport's.
	std::vector<std::string> missions;
	for (size_t i = 0; i < viewports.size(); ++i)
		if (viewports.at(i).kind() == ViewportKind::Mission) missions.push_back(viewports.at(i).path());
	for (const std::string &mission_path : missions)
		if (auto *mission = dynamic_cast<MissionViewport *>(viewports.find(mission_path, ViewportKind::Mission))) {
			fired = !mission->fire_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(),
			                              viewports.clip_sound_seq())
			                 .empty() ||
			        fired;
			fired = !mission->fire_listen_sounds(view.project.scan.get(), core.sound_selector(),
			                                     viewports.clip_sound_seq())
			                 .empty() ||
			        fired;
		}
	// What a listening environment heard of its weather's thunder (S23 C), each environment viewport's.
	std::vector<std::string> environments;
	for (size_t i = 0; i < viewports.size(); ++i)
		if (viewports.at(i).kind() == ViewportKind::Environment) environments.push_back(viewports.at(i).path());
	for (const std::string &environment_path : environments)
		if (auto *environment =
						dynamic_cast<EnvironmentViewport *>(viewports.find(environment_path, ViewportKind::Environment)))
			fired = !environment->fire_listen_sounds(view.project.scan.get(), core.sound_selector(),
			                                         viewports.clip_sound_seq())
			                 .empty() ||
			        fired;
	if (fired) core.touch(ViewConcern::Viewports);
}

void fire_menu_sounds(SessionCore &core, const std::string &path) {
	const SessionView &view = core.view();
	Viewports &viewports = core.viewports();
	if (!view.project.open || !view.findings.assets) return;
	// The menu viewports sampled: the one a SetViewport named, else each kept.
	std::vector<std::string> paths;
	if (!path.empty()) {
		paths.push_back(path);
	} else {
		for (size_t i = 0; i < viewports.size(); ++i)
			if (viewports.at(i).kind() == ViewportKind::Menu) paths.push_back(viewports.at(i).path());
	}
	bool fired = false;
	for (const std::string &each : paths) {
		ViewportModel *model = viewports.find(each, ViewportKind::Menu);
		// A SetViewport's viewport followed now where no device follows it (a headless editor's, a test's), so
		// what it moved is heard against the picture as it is.
		if (model && !path.empty() && !model->attached()) model = viewports.follow_one(view, each, ViewportKind::Menu);
		auto *menu = dynamic_cast<MenuViewport *>(model);
		if (!menu) continue;
		const ViewportInput input{view, viewports.clock(), open_document(view, each)};
		fired = !menu->fire_sounds(input, *view.findings.assets, view.project.scan.get(), core.sound_selector(),
		                           viewports.clip_sound_seq())
		                 .empty() ||
		        fired;
	}
	if (fired) core.touch(ViewConcern::Viewports);
}

std::vector<ClipSoundPlay> clip_sounds_since(SessionCore &core, uint64_t after) {
	std::vector<ClipSoundPlay> out;
	Viewports &viewports = core.viewports();
	for (size_t i = 0; i < viewports.size(); ++i) {
		for (const ClipSoundFired *each : fired_by(viewports.at(i))) {
			const ClipSoundFired &fired = *each;
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

std::vector<const ClipSoundFired *> clip_sounds_recent(SessionCore &core, size_t most) {
	std::vector<const ClipSoundFired *> out;
	Viewports &viewports = core.viewports();
	for (size_t i = 0; i < viewports.size(); ++i)
		for (const ClipSoundFired *fired : fired_by(viewports.at(i))) out.push_back(fired);
	std::sort(out.begin(), out.end(), [](const ClipSoundFired *a, const ClipSoundFired *b) { return a->seq < b->seq; });
	if (out.size() > most) out.erase(out.begin(), out.end() - std::ptrdiff_t(most));
	return out;
}

} // namespace opennova::editor
