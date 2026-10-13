#include <editor/session/terrain_stroke.h>

#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_add_map.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_stroke_planes.h>

namespace opennova::editor {

namespace {

void refuse(SessionCore &core, const std::string &why, const std::string &at) {
	core.refuse_request(CoreFinding::TerrainStroke, why, at, DiagnosticSeverity::Error);
}

// Whether the gesture `token` is open in a document (a stroke's sample goes on with one).
bool gesture_open(const SessionView &view, uint64_t token) {
	for (const OpenGesture &gesture : view.documents.gestures)
		if (gesture.token == token) return true;
	return false;
}

} // namespace

void serve_terrain_stroke(SessionCore &core, const EditorRequest &request) {
	if (!core.view().project.open) return;
	DocumentSet &documents = core.documents();
	const TerrainStrokeAsk &ask = request.stroke;
	core.outcome().gesture = ask.gesture;
	DocumentBase *view = documents.document_for(request.path);
	if (!view) return refuse(core, "Open the mission or the terrain the stroke is in first.", request.path);
	// A cancel drops what the gesture did wherever it went (Esc mid-stroke).
	if (ask.cancel) {
		if (!ask.gesture) return refuse(core, "A cancel names the gesture it drops (stroke.gesture).", view->path());
		documents.end_edit(std::string(), ask.gesture, true);
		return;
	}
	TerrainFamily family;
	if (!resolve_terrain_family(core.view(), *view, family)) return refuse(core, family.error, view->path());
	if (ask.add) {
		add_terrain_map(core, *view, family, ask.plane);
		return;
	}
	const TerrainFamilyFile &file = family.plane(ask.plane);
	if (!file.refusal.empty()) return refuse(core, file.refusal, view->path());
	if (file.path.empty())
		return refuse(core, std::string("No file holds ") + terrain_plane_words(ask.plane) + " of " + basename_of(view->path()) + ".",
		              view->path());
	const std::string view_path = view->path();
	// A gesture goes on with a token open. A wire stroke's nonzero token must be one (the outcome of its first sample
	// named it; one whose samples lapsed, or that the session never handed out, is refused: gesture 0 begins one), so
	// two strokes never share a link. A canvas's own token begins its gesture on its first sample (next_edit_gesture's,
	// which no other gesture holds); 0 begins one with the session's.
	const bool open = ask.gesture && gesture_open(core.view(), ask.gesture);
	if (ask.wire && ask.gesture && !open)
		return refuse(core, "No open gesture " + std::to_string(ask.gesture) +
		                            ": a stroke goes on under the gesture its first sample was given, while its samples come "
		                            "within 10 s; begin again with gesture 0.",
		              view_path);
	const bool first = !open;
	const uint64_t token = ask.gesture ? ask.gesture : next_edit_gesture();
	const bool was_open = documents.document_for(file.path) != nullptr;
	DocumentBase *target = documents.open_quietly(file.path, "the brush in " + basename_of(view_path) + " edits it");
	if (!target) return refuse(core, "Could not open " + file.path + " to brush it.", view_path);
	TerrainStrokeContext context{core, *documents.document_for(view_path), family, ask.plane, file, *target,
	                             terrain_stroke_brush(core.view().workspace.terrain_brush, ask), ask.points, token, first};
	TerrainStrokePlan plan;
	std::string error;
	const bool planned = ask.plane == TerrainPlane::Heights ? plan_heights_stroke(context, plan, error)
	                                                        : plan_paint_stroke(context, plan, error);
	if (!planned) {
		// A file opened for a stroke that did not go is not left open behind it.
		if (!was_open) documents.close_document(file.path);
		return refuse(core, error, file.path);
	}
	for (Edit &edit : plan.edits) edit.gesture = token;
	if (!plan.edits.empty() && !documents.apply_terrain_edits(*target, plan.edits, view_path, plan.words)) return;
	core.outcome().gesture = token;
	core.outcome().stroke.changed = std::move(plan.changed);
	core.outcome().stroke.bake = std::move(plan.bake);
	if (!was_open) core.outcome().stroke.opened.push_back(target->path());
	// The gesture ends with this sample, or stays open for the next. The view holds it too. A wire stroke's keeps its
	// last sample's time: another request on the view ends it (SessionCore::request_arrives) and 10 s with no sample
	// lapse it, by its token in every file it changed. A canvas's keeps none: it lives until its release ends it (a
	// bracket key or a still pointer mid-press never splits it).
	if (ask.end) {
		documents.end_edit(std::string(), token, false);
	} else {
		const int64_t now = ask.wire ? core.platform().now_ms() : -1;
		documents.open_gesture(target->path(), token, now);
		documents.open_gesture(view_path, token, now);
	}
}

} // namespace opennova::editor
