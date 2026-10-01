#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_schema.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

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
// A record is found by its path in its row, which the core's index of the row keeps
// (Document::path_in, S13 D8). A generator's, a track's or a light's register and a part
// animation's rotation frame name a CTRL register or an MTRX row of the model row by its index:
// Record references (ModelRegister, ModelFrame), which the core renumbers through
// renumber_references when a register or a frame moves. serialize() composes the base and the rows
// into the struct the writer takes (strips renumbered to the materials' new order) and
// writes it from scratch (ADR 0003); an untouched model writes its own bytes (ctest
// threedi_retail_rewrite).

enum class ModelKind : NodeKind {
	Model = 0, Collision, Lod, PartAnimation, Material, Texture, Light, UserPoint, Register, Frame, Section, Volume,
	Face, Occlusion,
};
constexpr NodeKind node_kind(ModelKind kind) { return static_cast<NodeKind>(kind); }

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

	ModelRow();
	std::shared_ptr<Node> clone() const override;
	std::string name() const override { return header.name; }
	// The collections', then each LOD's part animations', then each material's texture rows'.
	void for_each_identity(const std::function<void(NodeId &)> &fn) override;
	// Its tables and part animations (the base is not its own).
	size_t footprint() const override;
};

struct CollisionRow : Node {
	std::vector<threedi::ThreediCollisionObject> sections;
	std::vector<threedi::ThreediBoundingVolume> volumes;
	std::vector<threedi::ThreediCollisionFace> faces;
	std::vector<threedi::ThreediOcclusionObject> occlusion;
	// collections: 0 sections, 1 volumes, 2 faces, 3 occlusion records.

	CollisionRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<CollisionRow>(*this); }
	std::string name() const override { return "Collision"; }
	size_t footprint() const override;
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
	// The model's and its collision's rows (the file's two, never added), then every record
	// kind they hold (model_document_detail::kKinds).
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	// One pass over a row's tables (a LOD's part animations after it, a material's texture rows
	// after it), what the core's index of the row is made by: never through collections(), whose
	// nested owners it finds by that index.
	void walk_records(const Node &row, const RecordVisitor &visit) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override { return schema(kind); }
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind);
	// What a part index names on this record: LOD 0's parts, with the value the table calls none
	// (any other index typed too). A register's index and a frame's row are Record references,
	// whose picker offers the model's registers and MTRX rows (S13 D8).
	bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const override;
	// A user point past the first 16 is inert.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<ModelDocument>(*this);
	}

	const ModelRow *model_row() const;
	const CollisionRow *collision_row() const;
	// The rows as the writer takes them.
	void compose(ComposedModel &out) const;

protected:
	// Whether the game reads the field on this record (a generator's parameter as a
	// register, a track by its flags, a spot light's axis), what it names there (a texture
	// row's file, by the row's type; a CTRL register or an MTRX row by its index, a Record
	// reference; a part of LOD 0: record_choices).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A field through the property table. A material's shader also sets the words the
	// shader decides (glass, reflection, emissive: threedi_build_material_surface), and
	// is refused when it would read tangents the model's vertices lack or move a drawn
	// material to another draw pass (both are geometry: re-export from Blender).
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	// Materials, texture rows, lights, user points, CTRL registers and MTRX frames: add, duplicate,
	// remove, move (a material a strip draws with is not removed), each list with its identities
	// through edit_id_list. The first register and the last are refused while the game reads a
	// material's or a track's register byte (reads_register_byte: the table they give or take
	// changes what each names). A frame byte of 0 names no row (the pose reads one only above 0),
	// so row 0 is never read; no pin keeps it first. What names a register or a frame by its index
	// the core has the type renumber (renumber_references). Part animations: the next part's inert
	// row added at the end, the last removed. LODs and the collision records are fixed.
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	// The registers or the frames moved: every field whose value names one by its index
	// (model_document_detail::named_by_index, on the record as the batch left it, read by the game
	// there or not, so a field it does not read yet keeps naming its record) set to the index its
	// register or frame stands at now, and an index past the collection moved with its count so it
	// names none still (RecordShift::now). A field the game reads refuses the edit when its record
	// was removed ("<record> <field> reads this register: point it at another first") or when it
	// cannot hold the new index (a byte past 255; a frame byte other than 1 to 127); one it does not
	// read keeps its value then. Refused too while the second RGB generator, which the editor never
	// sets, names a register the edit moves.
	bool renumber_references(const StagedRows &rows, const RecordShift &shift,
	                         std::vector<Edit> &sites, std::string &error) const override;
	// A material's or a loaded track's register byte the game reads (a generator, the second one
	// included, or a track above style 0x70, a flipbook on the register clock): the first one met in
	// `row`, its record path and field in `where` and its byte in `named`. Such a byte reads the
	// model's CTRL table while it has one and a global register while it has none.
	bool reads_register_byte(const Node &row, std::string &where, int64_t &named) const;
	// A model keeps its two rows: a step adding or removing a row is refused.
	bool accept_step(const EditStep &step, const StagedRows &rows,
	                 std::string &error) const override;
};

bool is_model_kind(AssetKind kind);

// The model document type's validator over one model (DocumentType::validate_file), an open
// document standing in for its file. More than 8 `sitex` seats, a register or
// frame named that the model lacks, a light on a part LOD 0 lacks are errors; duplicate
// user point names, a shader or register name the engine does not know, LOD thresholds
// that do not descend are warnings; more than 16 user points and a material no strip
// draws with are notes. Textures are the asset graph's.
std::vector<Diagnostic> validate_model_file(const DocumentBase &document);

// The model type's own finding codes (DocumentType::findings), each a row of its table
// (model_document_edits.cpp, beside the validator, static_asserted into this order): more seats
// than the game keeps, a user point name repeated, more user points than the game reads, a
// register or a frame the model lacks, a shader or register name the engine does not know, a
// material no strip draws with, a light on a part LOD 0 lacks, LOD thresholds that do not
// descend.
enum class ModelFinding {
	Seats,
	UserPointDuplicate,
	UserPoints,
	RegisterMissing,
	ShaderUnknown,
	MaterialUnused,
	LightPart,
	RegisterUnknown,
	LodOrder,
	FrameMissing,
	kCount
};
const FindingCodeRow &finding_code(ModelFinding code);
FindingTable model_finding_codes();

} // namespace opennova::editor
