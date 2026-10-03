#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/node.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class ModelDocument;

// The collision a model viewport shows (ADR 0046 S17, Models: "we should be able to see the
// collisions"): what the game tests a model against for rounds, contact, lines of sight and its draw,
// each as the file holds it, in the preview's space, a layer the modder toggles by name. The collision
// block is in mission axes about the model's origin (CVRT, BPLN, BVOL, COBJ, CMDL), so it goes to the
// preview through the mission-to-presentation map; section i rides part i of LOD 0 as the game poses it
// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0: callback matrix i with COBJ i, by their
// pointers' lockstep; BoneCallback_Generic @ 0x4e26d0 for the parts' PANM]. A person's sections are its
// bones' hit spheres, shown at rest. Each shape keeps the record it is (a face, a volume, a section, an
// occlusion record) so a later handle can edit it; the bounds the game derives keep none.

// What a shape is.
enum class ModelCollisionKind : uint8_t {
	Face,             // a bullet face (CFAC)
	Volume,           // a volume (BVOL): the convex solid of its planes
	Section,          // a section (COBJ): its bound sphere and box, or a person's hit sphere
	Occlusion,        // an occlusion record (OOBJ): its polygon
	Bounds,           // the collision block's box (CMDL)
	ProjectionSphere, // the sphere the game culls the model by and picks its LOD with (the CMDL box's)
	BoundRadius,      // the model's bound radius about its origin (GHDR)
	ProbeBox,         // a vehicle's two probe boxes: index 0 the box, 1 the footprint
	PartSphere,       // a part's bound sphere on the drawn LOD (ROBJ)
};
// "face", "volume", "section", "occlusion", "collision_box", "projection_sphere", "bound_radius",
// "probe_box", "part_sphere": its token on the wire.
const char *model_collision_kind_token(ModelCollisionKind kind);

// The layers the Show menu toggles, each a member of ModelOverlayOptions.
enum class ModelCollisionLayer : uint8_t { BulletFaces, Volumes, Sections, Bounds, ProbeBoxes, Occlusion, PartSpheres, kCount };
struct ModelCollisionLayerRow {
	ModelCollisionLayer layer;
	const char *token; // options.overlays' member on the wire
	const char *label; // the Show menu's words
	const char *words; // what it is in the game (model_collision_words.h)
	uint32_t rgb;      // its colour (a volume's is its family's)
};
const ModelCollisionLayerRow &model_collision_layer(ModelCollisionLayer layer);
bool model_collision_layer_on(const ModelOverlayOptions &options, ModelCollisionLayer layer);
void model_collision_layer_set(ModelOverlayOptions &options, ModelCollisionLayer layer, bool on);
// Whether a layer has anything to show on `model` (a menu entry for an empty one is disabled), and how
// many shapes it has.
size_t model_collision_layer_count(const threedi::Threedi3di3 &model, ModelCollisionLayer layer, int lod);

// A volume's colour by its type's family, and a person's hit sphere's.
uint32_t model_volume_rgb(int64_t type);
inline constexpr uint32_t kModelHitSphereRgb = 0xF06292;

struct ModelCollisionShape {
	ModelCollisionKind kind = ModelCollisionKind::Face;
	int index = 0;    // the record's index in its list (a face, a volume, a section, an occlusion record); a
	                  // part sphere's part; a probe box 0 the box, 1 the footprint; the other bounds 0
	int section = -1; // the section (the LOD 0 part) it rides; -1 the model
	uint32_t rgb = 0;
	std::string name;   // in words: "Face 12: Metal", "Volume 3: ladder (CL)"
	std::string legend; // its colour's row in the legend: "Bullet faces", "Volumes: ladder"
	// What is drawn, in the preview's space: segments (pairs), and a sphere's outline.
	std::vector<PreviewVec3> edges;
	bool sphere = false;
	PreviewVec3 center;
	float radius = 0.0f;
	// What a press takes: triangles (triples) where it has an area (a face, a volume's facets, an
	// occlusion polygon); a sphere by its disc.
	std::vector<PreviewVec3> triangles;
	bool pickable = false; // it is a record a click selects
};

// A record's shape by its kind (Face to Occlusion) and its index in its list.
struct ModelCollisionPick {
	ModelCollisionKind kind = ModelCollisionKind::Face;
	int index = -1;
	bool valid() const { return index >= 0; }
	bool operator==(const ModelCollisionPick &o) const { return kind == o.kind && index == o.index; }
};
// The shapes of the layers `options` shows on `model`, the drawn LOD `lod`'s for the part spheres, posed
// at `time_ms` with the register bus (model_overlays' rule for LOD 0's parts), in the layers' order. `also`
// adds that record's shape when its layer is off (the selection's). The model's static geometry (each
// volume's solid, each face's corners) is made once per parsed model and kept while it lives.
std::vector<ModelCollisionShape> model_collision_shapes(const assets::Model &model, int lod, uint32_t time_ms,
                                                        const int32_t bus[96], const ModelOverlayOptions &options,
                                                        ModelCollisionPick also = ModelCollisionPick());
// The legend of `shapes`: each colour drawn, with its words ("Bullet faces", "Volumes: ladder"), in the
// order first drawn.
struct ModelCollisionLegendRow {
	uint32_t rgb = 0;
	std::string words;
};
std::vector<ModelCollisionLegendRow> model_collision_legend(const std::vector<ModelCollisionShape> &shapes);

// The pickable shape under device pixel (x, y): the front-most triangle holding it, else the smallest
// sphere whose disc holds it; -1 none.
int pick_model_collision(const std::vector<ModelCollisionShape> &shapes, const OrbitCamera &camera, int width,
                         int height, float x, float y);

// A sphere around a shape (its own, else its points'): what Frame looks at.
void model_collision_bounds(const ModelCollisionShape &shape, PreviewVec3 &center, float &radius);

// The record a shape is in the model document (current revision only), none for a bound; and the shape
// a record is (false for a record no shape is).
NodeAddress model_collision_record(const ModelDocument &document, const ModelCollisionShape &shape);
bool model_collision_of(const ModelDocument &document, const NodeAddress &record, ModelCollisionPick &out);

// The convex solid of a volume's planes (n . p + d <= 0 inside, d the plane's stored radius: the game's
// test [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150, (n . p >> 14) + d - r < 0]), as polygons in
// mission axes, one per plane it has a face on: the add-on's rule (tools/blender/opennova_3di/importer.py
// volume_facets), corners kept within 1 mm of the solid. Empty when the planes bound no solid (a flat
// ladder keeps its facing's polygon).
std::vector<std::vector<threedi::ThreediBuildVec3>> model_volume_polygons(const threedi::ThreediBoundingPlane *planes,
                                                                          size_t count, bool ladder);
// The solid the game tests a volume as: its planes' solid within its stored box, the box being the quick
// test a point outside of is never inside (docs/world/world-wac-ai-re.md section 15.2). The box's planes
// join the volume's own only where those reach past it (two of the install's 14,811 volumes, I_LFP2's and
// I_LFPB's, by 8 cm), so a volume within its box keeps its own facets alone; a box stored inside out (four
// more, z above z) clips nothing: its planes' solid is shown as the author made it.
std::vector<std::vector<threedi::ThreediBuildVec3>> model_volume_solid(const threedi::ThreediBoundingVolume &volume,
                                                                       const threedi::ThreediBoundingPlane *planes);

} // namespace opennova::editor
