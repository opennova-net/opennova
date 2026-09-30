#pragma once

#include <base/io/json.h>
#include <editor/preview/model_preview_state.h>

namespace opennova::editor {

class ModelDocument;
class ProjectSession;
struct SessionView;

// What the model preview shows, for the MCP and the tests (ADR 0046 S10p2); and what the MCP
// asks of it (its options, its camera, a drag of a marker).
struct ModelPreviewSnapshot {
	ModelPreviewStatus status = ModelPreviewStatus::NoProject;
	const ModelDocument *document = nullptr; // the previewed model (open)
	const ModelPreviewModel *model = nullptr;
};

// The snapshot of a model; `device` false when no renderer is attached.
ModelPreviewSnapshot model_preview_snapshot(const SessionView &view, const ModelPreviewModel &model, bool device);

// {status, message, detail, path, revision, shown_revision, current, builds, device {width,
//  height}, options {lod ("auto" or a level), ctrl {name: value}, playing, overlays
//  {user_points, lights, pivots}}, clock {time_ms, playing}, lod {shown, auto, count,
//  projected_px (the projection sphere's radius in pixels), thresholds[] (each level's, in
//  pixels)}, camera {target [x, y, z], yaw, pitch, distance, fov (horizontal degrees)},
//  sphere {center [x, y, z], radius}, registers [{name, value}], overlays [{kind
//  (user_point, light, pivot), index, id (its record while current, else 0), name, part,
//  position [x, y, z], screen [x, y] or null (behind the eye), direction [x, y, z] (a user
//  point's Z axis, a spot light's), radius and cone (a light's reach, a spot's half-angle
//  in radians), color ("RRGGBB", a light's)}], animation (a clip or a table previewed,
//  else null) {table, clip (the document's clip file when it is one), model (the rig's
//  model file), source ("chosen", or the record that pairs them), rig (whether the rig
//  loads), key, variant, file (the clip playing), ticks, frame, length_ticks, loops,
//  events [{frame, tick, trigger}]}}. Positions are in the preview's space (the model's
//  axes with x mirrored, y up); the model's records only when ready.
io::JsonValue model_preview_to_json(const ModelPreviewSnapshot &snapshot);
// The marker a device point picks (pick_model_overlay): {kind, index, id, name, current},
// index -1 for none.
io::JsonValue model_preview_hit_to_json(const ModelPreviewSnapshot &snapshot, float x, float y);
// The record a marker is while the preview shows the document's revision (none otherwise).
NodeAddress model_preview_record(const ModelPreviewSnapshot &snapshot, const ModelOverlay &overlay);

// --- what the editor MCP's editor_model_preview asks of the preview ------------------------

// The options an {lod ("auto" or a level 0..255), ctrl ({register: number}, the registers
// held, a 0 not held), playing, overlays ({user_points, lights, pivots} booleans), time_ms
// (>= 0: the clock sought, past its last millisecond its last), clip_ticks (>= 0: the clip
// clock sought), rig_model (a model's
// file name, "" the paired one)} object sets on `model`, each member optional, a number's
// fraction dropped. False, `model` untouched, for another member or a value out of range or
// of another type.
bool model_preview_options_from_json(const io::JsonValue &json, ModelPreviewModel &model);
// The camera a {yaw, pitch, distance (> 0), target [x, y, z], frame (look at the whole model
// again), width, height (the device's pixels, 1..8192)} object sets on `model`, each member
// optional, the pitch kept within kOrbitPitchLimit. False, `model` untouched, as
// model_preview_options_from_json.
bool model_preview_camera_from_json(const io::JsonValue &json, ModelPreviewModel &model);

// One drag of the marker of the record `record` to device pixel (x, y) by `handle`, as the
// Preview window's model pane plans one (ModelPreviewModel::handle_edits, `snap` metres, 0
// free), sent to the session as one batch and one undo step. False when the snapshot does not
// show the model as it is now, the model cannot be edited (blocked), no marker is that
// record's, the marker has no such handle, or the session did not take the batch (its outcome
// not done: an edit the document refused).
bool model_preview_drag(ProjectSession &session, const ModelPreviewSnapshot &snapshot, NodeId record, ModelHandle handle,
                        float x, float y, float snap);

} // namespace opennova::editor
