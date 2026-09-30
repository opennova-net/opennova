#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>
#include <formats/adm/adm.h>

namespace opennova::editor {

// An animation map (ADR 0046 S10): a `.adm`, the table that names which clips a rig
// plays in each of the engine's 252 animation slots. Rows are the table's rows in file
// order (a row per key; two rows of one slot join one ring, as retail registers them);
// a row holds its clips, at most 8, served last to first (docs/anim/adm-bad-format-re.md).
// A key's choices are the slot keys the engine looks up, and any other key is written as
// typed; a clip names a `.bad` beside the table (the asset graph's edge). The rows are the
// file's list: added, duplicated, removed and moved like a row's clips. A line whose input
// the table leaves out is a source finding (adm_parse_buffer's dropped lines): input the
// game ignores (a comment, a row that registers nothing) is dropped on save; a row naming
// more than 8 clips blocks the file. serialize() writes the canonical form through the
// engine's writer (adm_write_buffer), refusing a row it cannot read back
// (adm_row_problem) and a table with no anim_reset row, which the game cannot load
// [orig: AnimMap_LoadAdmFile @ 0x40cc40, @0x40ce11].

enum class AnimationMapKind : NodeKind { Row = 0, Clip = 1 };
constexpr NodeKind node_kind(AnimationMapKind kind) { return static_cast<NodeKind>(kind); }

struct AnimationMapRow : Node {
	std::string key;
	std::vector<std::string> clips; // collections[0] carries their identities

	AnimationMapRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<AnimationMapRow>(*this); }
	std::string name() const override { return key; }
	size_t footprint() const override;
};

class AnimationMapDocument : public Document {
public:
	// A row, which the outline adds, and its clips.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// A row by its slot's words (animation_key_title), a clip by its file.
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<AnimationMapDocument>(*this);
	}
	// The table as the engine reads it, rebuilt from the rows.
	std::vector<adm::AdmEntry> entries() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	// A new row: anim_reset when the table (`rows`, as its batch has left them) has none, else
	// anim_idle, with no clip yet.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	// A row's clips: add, duplicate, remove, move; at most 8.
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
};

bool is_animation_map_kind(AssetKind kind);

// A table's key in the words the editor shows: a slot's key as the slot's words, compared as
// the engine compares them ("anim_walk_forward": "walk forward"), any other key as it is.
std::string animation_key_title(const std::string &key);

// The animation map document type's validator over one table (DocumentType::validate_file), an
// open document standing in for its file. Each line the table leaves out is a finding on its
// line (and its row, where it keeps one): input the game ignores a warning
// (animation_map.ignored_input, Rewrite drops it), a row the table cannot hold an error
// (animation_map.invalid_input). A table with no anim_reset row and a row the writer
// refuses are errors; a key naming none of the engine's slots is a warning (the game skips
// the row [orig: AnimMap_ParseConfigLine @ 0x40CB60, the test @0x40CBA4]); a row naming a
// slot an earlier row names is a note (animation_map.slot_repeated: the game joins their
// clips into one ring, and keeps only the last reset clip). A clip the project lacks is the
// asset graph's.
std::vector<Diagnostic> validate_animation_map_file(const DocumentBase &document);

} // namespace opennova::editor
