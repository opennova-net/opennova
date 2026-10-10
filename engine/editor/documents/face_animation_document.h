#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/graph_edge.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/grm/grm.h>

namespace opennova::editor {

// A face animation (ADR 0046, round S23 lane A): a `.grm`, the face a person's model animates, read and written
// through the engine's own reader and writer (grm::parse, grm::write: the writer from scratch, ADR 0003) over the
// engine's own record, grm::File (docs/world/world-wac-ai-re.md section 33.30). The game opens `<model>.GRM` for a
// person (an item of type 3) through the archives when the shadow quality is above 0 [orig: Entity_InitFromModel @
// 0x40E211..0x40E236 -> sub_57FDF0 @ 0x57FDF0 -> sub_57FCE0 @ 0x57FCE0 (the model's name, its path and extension
// stripped, .GRM added) -> sub_5891E0 @ 0x5891E0 -> FaceAnimConfig_LoadFile @ 0x588BE0]: no file names one.
//
// One row, the face: its base texture and its two eye textures (each name made .TGA, the base's .MDT twin too, and
// loaded by stage [orig: Shadow_DecalLoadTextures @ 0x588040]), its eyes' size, centres and limits, and the
// original tool's save stamp; its lists: the base mesh's vertices (a texture position and the group a gesture
// moves it by), its triangles (three vertices each, by their index: a Record reference, so a triangle follows its
// vertices), and its gestures (an expression by name, each with at most 32 parameters, a group's offset). The
// game plays the nine expressions by name [orig: AnimState_FindByName @ 0x5800B0, the table @ 0x7D7960], the last
// gesture of a name [orig: sub_588FE0 @ 0x588FE0], each vertex moved by the first parameter of its group [orig:
// sub_589090 @ 0x589090; sub_5890F0 @ 0x5890F0], the group "xxx" never [orig: Model_CollectUniqueMaterialNames @
// 0x588D90].
enum class FaceAnimationKind : NodeKind { Face = 0, Vertex = 1, Triangle = 2, Gesture = 3, Parameter = 4 };
constexpr NodeKind node_kind(FaceAnimationKind kind) { return static_cast<NodeKind>(kind); }

// The face row: the engine's record, its names in the game's code page (Windows-1252) as the reader keeps them.
struct FaceAnimationRow : TableRow {
	grm::File file;

	FaceAnimationRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<FaceAnimationRow>(*this); }
	std::string name() const override { return "Face"; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

// The face animation's table (face_animation_document.cpp): the face's, a vertex's, a triangle's, a gesture's and
// a parameter's kinds, their fields in the file's units, cited, and the lists.
const RecordTable &face_animation_table();

class FaceAnimationDocument : public TableDocument {
public:
	const RecordTable &table() const override { return face_animation_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return face_animation_table().fields(kind); }
	// A vertex by its index and group ("Vertex 3: brow"), a triangle by its corners ("Triangle 2: 0, 1, 3"), a
	// gesture by its name, a parameter by its group.
	std::string record_title(const NodeAddress &address) const override;
	// The face through the engine's writer (grm::write): the original tool's layout, four decimals.
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<FaceAnimationDocument>(*this); }
	std::string save_words() const override;

	const FaceAnimationRow *face_row() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A face keeps its one row.
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
	// A gesture holds 32 parameters at the most: an Add, a Duplicate or a Move into a full one is refused.
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// Each texture name made .TGA as the stage loader opens it (kTextureArgFaceTga, the face texture role).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	// A triangle's corner names a vertex by its index: an edit that moves the vertices moves each corner with its
	// vertex, and one removing a vertex a triangle uses is refused.
	bool renumber_references(const StagedRows &rows, const RecordShift &shift, std::vector<Edit> &sites,
	                         std::string &error) const override;
};

bool is_face_animation_kind(AssetKind kind);

// The face animation type's references that no field's value is (DocumentType::record_references): the base
// texture's .MDT twin, which the stage loader opens beside its .TGA [orig: Shadow_DecalLoadTextures @ 0x588117].
void face_animation_references(const Document &document, Extracted &out);

// The face animation type's validator (DocumentType::validate_file): its source findings (input the record cannot
// hold, a layout the writer writes otherwise), an eye texture named without the other, a triangle naming a vertex
// the face lacks (the save refuses it), a gesture of no expression's name or of one an earlier gesture has, a
// parameter whose group no vertex has or an earlier parameter of its gesture has.
std::vector<Diagnostic> validate_face_animation_file(const DocumentBase &document);

enum class FaceAnimationFinding {
	InvalidInput,
	IgnoredInput,
	EyeTextureAlone,
	Unserializable,
	GestureUnplayed,
	GestureRepeated,
	ParameterUnmatched,
	ParameterRepeated,
	kCount
};
const FindingCodeRow &finding_code(FaceAnimationFinding code);
FindingTable face_animation_finding_codes();

} // namespace opennova::editor
