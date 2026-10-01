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
// status ("ready", "empty", "failed", and S13 V6's "loading": its device builds the picture over
// the frames, the last one drawn meanwhile; "failed" too when that build failed), reason (the kind's
// token: no_menu, unserializable, no_rig, ...; "ready"; "loading", "build_failed"), message, detail,
// progress (while loading, A1's operation progress {generation, done, total, unit, label}: the build
// generation it counts, done of total in its unit, its label; per generation, so a newer one's
// begins again at 0; null otherwise), revision (the document's now, 0 none), shown_revision (the one
// its picture shows), current, builds (how many times its device was told to make its picture: the
// newest build generation), units ("design": a menu's 800 x 600; "pixels": the picture's), device
// {attached, width, height, canvas_sized, build} (the size its device draws at: a canvas's own where
// one sizes it, canvas_sized, else the viewport's state's; build {generation, loading, failed, done,
// total, frames, frame_us, unit_us, total_us}: the build it builds or built and what it cost on the
// Shell's frames), options (the kind's), camera (the kind's, null for none), clock
// {playing, rate, time_ms, ticks} (the preview clock), body (the kind's: a menu's screen {id, name},
// missing[] and unreadable[]; a model's lod, sphere, registers and animation), items (a page of the
// kind's items, the menu's widgets or the model's markers, `count` their whole number, set_page's)
// and notes (by the same page, the menu's compiler notes, `note_count` theirs)}. The items and the
// notes only while the picture is current. The viewport query (session/editor_queries) answers it
// and stamps its view_revision; every viewport it reads is a document's (the query refuses with no
// document open), so a viewport over no document is on no wire.
io::JsonValue viewport_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page);
// A page of a viewport's items, or of its notes (the viewport query's items and notes): {kind, path,
// status, reason (the envelope's: "loading" while its device builds, S13 V6), current,
// shown_revision, items or notes (the page), count (the list's whole length), offset, next_offset}.
io::JsonValue viewport_items_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page);
io::JsonValue viewport_notes_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page);
// What lies under a point of the picture of `model`, in its units: {viewport (the viewport's kind),
// path (its document), kind (the item's: a window's type, a marker's kind), index, id, name,
// current}, index -1 for none.
io::JsonValue viewport_hit_to_json(const ViewportModel &model, const ViewportHit &hit);

} // namespace opennova::editor
