#include <editor/session/clip_sounds.h>

#include <algorithm>

#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

void fire_clip_sounds(SessionCore &core) {
	const SessionView &view = core.view();
	Viewports &viewports = core.viewports();
	if (!view.project.open || !viewports.clock().playing()) return;
	const std::string &path = view.documents.previews[ViewportKind::Model].path;
	if (path.empty()) return;
	ViewportModel *model = viewports.find(path, ViewportKind::Model);
	if (!model) return;
	// A viewport no device follows (a headless editor's, a test's) follows now, as a read of it would, so
	// the clip it fires from is the one the selection plays.
	if (!model->attached()) model = viewports.follow_one(view, path, ViewportKind::Model);
	auto *clip = dynamic_cast<ModelViewport *>(model);
	if (!clip || !clip->animating()) return;
	const std::vector<ClipSoundFired> fired =
			clip->fire_sounds(viewports.clock(), view.project.scan.get(), core.sound_selector(), viewports.clip_sound_seq());
	if (!fired.empty()) core.touch(ViewConcern::Viewports);
}

std::vector<ClipSoundPlay> clip_sounds_since(SessionCore &core, uint64_t after) {
	std::vector<ClipSoundPlay> out;
	Viewports &viewports = core.viewports();
	for (size_t i = 0; i < viewports.size(); ++i) {
		const auto *clip = dynamic_cast<const ModelViewport *>(&viewports.at(i));
		if (!clip) continue;
		for (const ClipSoundFired &fired : clip->sounds_fired()) {
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
