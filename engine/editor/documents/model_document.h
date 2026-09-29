#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/model/document.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_schema.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class AssetGraph;

// A model (ADR 0046 S10): a `.3di`, whose engine features the editor changes through
// the format's property table (formats/threedi/threedi_schema.h) over the engine's own
// records (ADR 0027). The parsed file is an immutable base (`assets::Model`): geometry,
// skinning and the collision pools stay there, authored in Blender. Two rows:
//   the model row: the model's name; its LODs (fixed; each holds its part animations,
//     PANM being per LOD); its materials (each holds its texture rows); its lights, user
//     points, CTRL registers and MTRX frames;
//   the collision row: the collision sections, volumes and bullet faces, and the
//     occlusion records (all fixed: their geometry is the base's; their types and flags
//     are edited).
// Everything that names another table's entry by index sits in the model row, so a
// structural edit renumbers within one row. serialize() composes the base and the rows
// into the struct the writer takes (strips renumbered to the materials' new order) and
// writes it from scratch (ADR 0003); an untouched model writes its own bytes (ctest
// threedi_retail_rewrite).

enum class ModelKind : NodeKind {
	Model = 0, Collision, Lod, PartAnimation, Material, Texture, Light, UserPoint, Register, Frame, Section, Volume,
	Face, Occlusion,
};
constexpr NodeKind node_kind(ModelKind kind) { return static_cast<NodeKind>(kind); }

// Where an identity sits in its row: the row's collection (the model row's 0..5, or its
// part animations and texture rows by their owner), and the index there.
struct ModelPlace {
	uint8_t collection = 0;
	uint32_t owner = 0;
	uint32_t index = 0;
};
using ModelPlaces = std::unordered_map<NodeId, ModelPlace>;

struct ModelLod {
	threedi::ThreediLod lod;                          // the base's LOD (its geometry pointers the base's)
	std::vector<threedi::ThreediPartAnimation> panm;  // its PANM rows, in part order
	std::vector<NodeId> panm_ids;
};

struct ModelMaterial {
	threedi::ThreediMaterial material;
	int source = -1;               // its index in the base's MTRL table (-1: added here, drawn by no strip)
	std::vector<NodeId> texture_ids; // one per texture row in use
};

struct ModelRow : Node {
	assets::Model base;
	threedi::ThreediHeader header;
	std::vector<ModelLod> lods;
	std::vector<ModelMaterial> materials;
	std::vector<threedi::ThreediLight> lights;
	std::vector<threedi::ThreediUserPoint> user_points;
	std::vector<threedi::ThreediControlRegister> registers;
	std::vector<threedi::ThreediMatrix4x4> frames;
	// collections: 0 LODs, 1 materials, 2 lights, 3 user points, 4 registers, 5 frames.
	// Every identity's place, built on first use and shared by a clone; a structural
	// edit of a clone forgets it (a committed row never changes).
	mutable std::shared_ptr<const ModelPlaces> places;

	ModelRow();
	std::shared_ptr<Node> clone() const override;
	std::string name() const override { return header.name; }
	void for_each_identity(const std::function<void(NodeId &)> &fn) override;
};

struct CollisionRow : Node {
	std::vector<threedi::ThreediCollisionObject> sections;
	std::vector<threedi::ThreediBoundingVolume> volumes;
	std::vector<threedi::ThreediCollisionFace> faces;
	std::vector<threedi::ThreediOcclusionObject> occlusion;
	// collections: 0 sections, 1 volumes, 2 faces, 3 occlusion records.
	mutable std::shared_ptr<const ModelPlaces> places;

	CollisionRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<CollisionRow>(*this); }
	std::string name() const override { return "Collision"; }
	void for_each_identity(const std::function<void(NodeId &)> &fn) override;
};

// The model as the writer takes it, composed from a document's rows: `model` points into
// the vectors here and into the base, which this keeps alive.
struct ComposedModel {
	threedi::Threedi3di3 model{};
	assets::Model base;
	std::vector<threedi::ThreediLod> lods;
	std::vector<std::vector<threedi::ThreediTriangleStrip>> strips;
	std::vector<std::vector<threedi::ThreediPartAnimation>> panm;
	std::vector<threedi::ThreediMaterial> materials;
	std::vector<threedi::ThreediLight> lights;
	std::vector<threedi::ThreediUserPoint> user_points;
	std::vector<threedi::ThreediControlRegister> registers;
	std::vector<threedi::ThreediMatrix4x4> frames;
	threedi::ThreediCollisionModel collision{};
	std::vector<threedi::ThreediBoundingVolume> volumes;
	std::vector<threedi::ThreediCollisionFace> faces;
	std::vector<threedi::ThreediOcclusionObject> occlusion;
	ComposedModel() = default;
	ComposedModel(const ComposedModel &) = delete;
	ComposedModel &operator=(const ComposedModel &) = delete;
};

class ModelDocument : public Document {
public:
	const char *kind_label(NodeKind kind) const override;
	NodeKind kind_from_name(const std::string &name) const override;
	bool is_top_kind(NodeKind kind) const override;
	std::vector<KindSpec> top_kinds() const override { return {}; }
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// Whether the game reads the field on this record (a generator's parameter as a
	// register, a track by its flags, a spot light's axis), and what it names there.
	FieldSchema field_on(const NodeAddress &address, const FieldSchema &field) const override;
	// A user point past the first 16 is inert.
	void refine_symbol(const NodeAddress &address, GraphSymbol &symbol) const override;
	SerializeResult serialize() const override;

	const ModelRow *model_row() const;
	const CollisionRow *collision_row() const;
	// The rows as the writer takes them.
	void compose(ComposedModel &out) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, std::string &error) override;
	// A field through the property table. A material's shader also sets the words the
	// shader decides (glass, reflection, emissive: threedi_build_material_surface), and
	// is refused when it would read tangents the model's vertices lack or move a drawn
	// material to another draw pass (both are geometry: re-export from Blender).
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	// Materials, texture rows, lights and user points: add, duplicate, remove, move (a
	// material a strip draws with is not removed). CTRL registers and MTRX frames: added at
	// the end, the last removed while nothing names it (an index is what names them). Part
	// animations: the next part's inert row added at the end, the last removed. LODs and
	// the collision records are fixed.
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	bool set_file_value(std::shared_ptr<const FileState> &, const Edit &, Diagnostic &error) override;
	// A model keeps its two rows.
	bool accept_change(const Change &change, std::string &error) const override;
};

bool is_model_kind(AssetKind kind);

// The model document type's validator (document_types): every model in the project,
// open documents standing in for their files. More than 8 `sitex` seats, a register or
// frame named that the model lacks, a light on a part LOD 0 lacks are errors; duplicate
// user point names, a shader or register name the engine does not know, LOD thresholds
// that do not descend are warnings; more than 16 user points and a material no strip
// draws with are notes. Textures are the asset graph's.
std::vector<Diagnostic> validate_models(const ValidationInput &input, const AssetGraph &graph);

} // namespace opennova::editor
