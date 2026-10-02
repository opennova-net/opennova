#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/model/node.h>
#include <editor/preview/model_preview_camera.h>
#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

class ModelDocument;

// What the model preview marks over the picture (ADR 0046 S10p4), each where the game
// puts it on the posed model.
enum class ModelOverlayKind : uint8_t {
	UserPoint, // a user point: its place and its Z axis
	Light,     // a light: its place, its reach, a spot's axis and cone
	Pivot,     // a part's pivot on the drawn level
};
const char *model_overlay_kind_token(ModelOverlayKind kind);

struct ModelOverlay {
	ModelOverlayKind kind = ModelOverlayKind::UserPoint;
	int index = 0;              // the record's index in its table (a pivot: the part)
	PreviewVec3 at;             // the preview's space
	bool has_direction = false; // direction: unit, the preview's space
	PreviewVec3 direction;
	float radius = 0.0f;        // a light's reach (its attenuation end)
	float cone = 0.0f;          // a spot light's half-angle in radians (0: omni)
	uint32_t color = 0;         // a light's start colour, 0xRRGGBB
	int part = -1;              // the part it rides (-1: the model)
	std::string name;
};

// Which kinds the preview marks.
struct ModelOverlayOptions {
	bool user_points = true;
	bool lights = true;
	bool pivots = false;
	bool operator==(const ModelOverlayOptions &o) const {
		return user_points == o.user_points && lights == o.lights && pivots == o.pivots;
	}
	bool operator!=(const ModelOverlayOptions &o) const { return !(*this == o); }
};

// The 96-slot CTRL register bus with the held registers written by name (an unknown name
// is dropped, as the game's catalog drops it).
void model_preview_ctrl_bus(const std::map<std::string, int64_t> &held, int32_t bus[96]);

// Every marker of `model` posed at `time_ms` with the register bus: a user point rides its
// part's PANM matrix of the first level while that level animates, else it is where it is
// authored (the attachment resolve [orig: the record transform @ 0x56c4f2..0x56c513, as
// world::EntityPoseProvider ports it]); a light rides its part on the drawn level `lod`
// when its part is not 0 (the unattached sentinel, ObjectModel::get_model_light_world_
// position's rule); a pivot is its part's origin on level `lod`, posed. A part's matrix
// turns about its pivot, so it is the identity at rest: model-space points go through it
// as they are.
std::vector<ModelOverlay> model_overlays(const threedi::Threedi3di3 &model, int lod, uint32_t time_ms,
                                         const int32_t bus[96], const ModelOverlayOptions &options);

// The part matrix a marker rides at `time_ms` (model_overlays' rule: a user point the
// first level's, a light or a pivot the drawn level's), row-vector (p' = p * M); the
// identity when it rides none.
threedi::ThreediMatrix4x4 model_overlay_pose(const threedi::Threedi3di3 &model, const ModelOverlay &overlay, int lod,
                                             uint32_t time_ms, const int32_t bus[96]);

// The marker a device point picks: the front-most within `slop` pixels of it; -1 none.
int pick_model_overlay(const std::vector<ModelOverlay> &overlays, const OrbitCamera &camera, int width, int height,
                       float x, float y, float slop = 8.0f);

// The record a marker is, in the model document it shows (current revision only): a user
// point, a light, or the part's PANM row on its level; none when it has no record.
NodeAddress model_overlay_record(const ModelDocument &document, const ModelOverlay &overlay, int lod);
// The marker a record is (the inverse): false for a record no marker shows.
bool model_overlay_of(const ModelDocument &document, const NodeAddress &record, ModelOverlayKind &kind, int &index);

} // namespace opennova::editor
