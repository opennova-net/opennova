#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class Viewports;
class ViewportModel;
struct SessionView;
struct ViewportHit;

// What a viewport's envelope reads of the view: the viewports' state (the Viewports concern), its
// document and the selection in it, the documents open and the active one, the project's files,
// the graph (a rig's model) and the project.
inline constexpr ConcernSet kViewportConcerns =
		concern_set({ ViewConcern::Viewports, ViewConcern::Documents, ViewConcern::Selection, ViewConcern::DocumentSet,
				ViewConcern::ActiveDocument, ViewConcern::Files, ViewConcern::Graph, ViewConcern::Project });

// A viewport on the wire (ADR 0046 S13 V5), one envelope for every kind: {kind, path, as_saved,
// status ("ready", "empty", "failed", and S13 V6's "loading": its device builds the picture over
// the frames, the last one drawn meanwhile; "failed" too when that build failed), reason (the kind's
// token: no_menu, unserializable, no_rig, ...; "ready"; "loading", "build_failed"), message, detail,
// progress (while loading, A1's operation progress: done of total in its unit, its label; null
// otherwise), revision (the document's now, 0 none), shown_revision (the one its picture shows),
// current, builds (how many times its device was told to make its picture: the newest build
// generation), units ("design": a menu's 800 x 600; "pixels": the picture's), device {attached,
// width, height, canvas_sized, build} (the size its device draws at: a canvas's own where one sizes
// it, canvas_sized, else the viewport's state's; build {generation, loading, failed, done, total,
// frames, frame_us, unit_us, total_us}: the build it builds or built and what it cost on the
// Shell's frames),
// options (the kind's), camera (the kind's, null for none), clock
// {playing, rate, time_ms, ticks} (the preview clock), body (the kind's: a menu's screen {id, name},
// missing[] and unreadable[]; a model's lod, sphere, registers and animation), items (a page of the
// kind's items, the menu's widgets or the model's markers, `count` their whole number, set_page's),
// notes (by the same page, the menu's compiler notes, `note_count` theirs) and view_revision (the
// view's clock at which what it reads last moved, kViewportConcerns' stamp: a client's `since`)}.
// The items and the notes only while the picture is current. `model` null writes the kind's empty
// viewport: what a viewport of `kind` over no document of it says (no project, nothing of the kind
// open, a menu with no screen selected).
io::JsonValue viewport_to_json(const SessionView &view, const ViewportModel *model, ViewportKind kind,
		const JsonPage &page);
// What lies under a point of the picture, in the viewport's units: {kind (the item's: a window's
// type, a marker's kind), index, id, name, current}, index -1 for none.
io::JsonValue viewport_hit_to_json(const ViewportHit &hit);

} // namespace opennova::editor
