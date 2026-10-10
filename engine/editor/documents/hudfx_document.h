#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/def/def_hudfx.h>

namespace opennova::editor {

// The HUD effects (`hudfx.def`, ADR 0046 S23 B; docs/interface/hud-re.md "hudfx.def"): the HUD's 3D models, each
// line a tag (3DIHud, 3DIPower1..8) and a model's name, read at each mission's HUD init [orig: HUD_InitOverlaySystem
// @ 0x5A4620 -> HUD_CacheModelNameByTag @ 0x58F970]. The game reads the file's first tagged line alone (a tag's
// callback ends the walk); its model is loaded into the tag's slot and drawn: the HUD model over the first-person
// view, a power slot's while its ammo pool holds any [orig: HUD_RenderAllOverlays @ 0x5A8070]. The rows are the
// file's tagged lines in its order; the document reads and writes the file through formats/def/def_hudfx over the
// file's modeled layout. JO ships none.

enum class HudFxKind : NodeKind { Line = 0 };
constexpr NodeKind node_kind(HudFxKind kind) { return static_cast<NodeKind>(kind); }

struct HudFxRow : TableRow {
	def::HudFxLine line;

	HudFxRow() { kind = node_kind(HudFxKind::Line); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<HudFxRow>(*this); }
	std::string name() const override;
	RecordHandle record() const override { return RecordHandle{kind, const_cast<def::HudFxLine *>(&line)}; }
	size_t footprint() const override { return sizeof(HudFxRow) + footprint_of(line.model) + ids_footprint(); }
};

const RecordTable &hudfx_table();

class HudFxDocument : public TableDocument {
public:
	const RecordTable &table() const override { return hudfx_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return hudfx_table().fields(kind); }
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<HudFxDocument>(*this); }
	// The file as the rows hold it.
	def::HudFxFile file() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new line: the HUD model's tag, no model yet.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A copy names none of the file's layout (written in the writer's form).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A line after the first is never read: its model is none the game loads.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
};

bool is_hudfx_kind(AssetKind kind);

// The HUD effects' validator: a line after the first (never read), a first line of no model, a first model's name
// running into the next slots, a first line that is a power slot's (loaded, never drawn), and a first line that is
// the HUD model's (the power slots' draw reads a slot with no model: the game fails, which the build gates on).
std::vector<Diagnostic> validate_hudfx_file(const DocumentBase &document);

enum class HudFxFinding { NeverRead, NoModel, NameRuns, PowerUnseen, PowerSlotsEmpty, kCount };
const FindingCodeRow &finding_code(HudFxFinding code);
FindingTable hudfx_finding_codes();

} // namespace opennova::editor
