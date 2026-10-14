#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/terrain_tools_view.h>

namespace opennova::editor {

class SessionCore;

// The terrain stroke's server (ADR 0046 S25, the shared seams; the terrain_stroke request): the stroke's view
// document (a mission or a terrain, the active one when no path is named), its family resolved as the game resolves
// it for that view (session/terrain_family), the plane's file opened quietly (DocumentSet::open_quietly), refused
// where its route refuses it, then planned by the plane's planner (session/terrain_stroke_planes.h) and applied to
// that file as a step of the stroke's gesture, the file noted as edited from the view (its undo, save and close
// scope). A gesture's samples are consecutive strokes of one plane naming its token (0 begins one, the outcome
// naming it). Over the wire a token must be open (one that lapsed, or the session never handed out, is refused); a
// canvas's own token begins its gesture on its first sample and lives until its release ends it;
// `end` ends it with the sample, `cancel` drops what it did; `add` makes the plane's map where the terrain has none its
// brush paints (session/terrain_add_map.h). Refused (terrain.stroke), nothing changed: a view with no terrain, a plane
// whose file does not resolve or is refused, a plan refused.
void serve_terrain_stroke(SessionCore &core, const EditorRequest &request);
// A stroke's refusal (terrain.stroke, an error) at the view `at`: what the terrain's files and tools refuse.
void refuse_stroke(SessionCore &core, const std::string &why, const std::string &at);

// The stroke on the wire (the request's `stroke` field, session_json's one delegate): {plane, brush?, points,
// gesture, end, cancel, add}; read back (TerrainStrokeAsk::wire set; each point within kTerrainPointMost), false with
// why. And what a stroke came to (its outcome's `stroke`).
io::JsonValue terrain_stroke_to_json(const TerrainStrokeAsk &ask);
bool terrain_stroke_from_json(const io::JsonValue &json, TerrainStrokeAsk &ask, std::string &error);
io::JsonValue terrain_stroke_answer_to_json(const TerrainStrokeAnswer &answer);

} // namespace opennova::editor
