#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

class Viewports;
class ViewportModel;
struct SessionView;
struct ViewportHit;

// A viewport on the wire (ADR 0046 S13 V5, V7), one envelope for every kind: {kind, path, as_saved,
// status ("ready", "empty", "failed"), reason (the kind's token: no_menu, unserializable, no_rig,
// ...; "ready"), message, detail, revision (the document's now, 0 none), shown_revision (the one
// its picture shows), current, builds (how many times its device was told to make its picture),
// units ("design": a menu's 800 x 600; "pixels": the picture's), device {attached, width, height,
// canvas_sized} (the size its device draws at: a canvas's own where one sizes it, canvas_sized,
// else the viewport's state's), options (the kind's), camera (the kind's, null for none), clock
// {playing, rate, time_ms, ticks} (the preview clock), body (the kind's: a menu's screen {id, name},
// missing[] and unreadable[]; a model's lod, sphere, registers and animation), items (a page of the
// kind's items, the menu's widgets or the model's markers, `count` their whole number, set_page's)
// and notes (by the same page, the menu's compiler notes, `note_count` theirs)}. The items and the
// notes only while the picture is current. The viewport query (session/editor_queries) answers it
// and stamps its view_revision. `model` null writes the kind's empty viewport: what a viewport of
// `kind` over no document of it says (no project, nothing of the kind open, a menu with no screen
// selected).
io::JsonValue viewport_to_json(const SessionView &view, const ViewportModel *model, ViewportKind kind,
		const JsonPage &page);
// A page of a viewport's items, or of its notes (the viewport query's items and notes): {kind, path,
// status, reason, current, shown_revision, items or notes (the page), count (the list's whole
// length), offset, next_offset}.
io::JsonValue viewport_items_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page);
io::JsonValue viewport_notes_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page);
// What lies under a point of the picture, in the viewport's units: {kind (the item's: a window's
// type, a marker's kind), index, id, name, current}, index -1 for none.
io::JsonValue viewport_hit_to_json(const ViewportHit &hit);

} // namespace opennova::editor
