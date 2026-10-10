#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/aip/aip.h>

namespace opennova::editor {

// An AI profile (`.aip`, ADR 0046 S23 B): the settings a unit the computer runs reads, one profile a file, read
// when a placed item names it [orig: AIProfile_LoadOrFind @ 0x45FD80 -> File_ParseASCIIFile -> AIProfile_ParseProperty
// @ 0x45DE70] (formats/aip; docs/world/world-wac-ai-re.md §17.9c, §17.9d, §23.3). The file's one row is the profile,
// its fields the reader's keys in the units the file writes them, each read only by the type's key set (HELO and
// GROUND; ORGANIC reads none), a weapon's name an ammo of the ammo table. The document reads the file through the
// game's reader with its layout modeled and writes it back through the profile's writer (aip::write_profile, over
// the layout: the file as it was but for a changed value's line).

enum class AiProfileKind : NodeKind { Profile = 0 };
constexpr NodeKind node_kind(AiProfileKind kind) { return static_cast<NodeKind>(kind); }

struct AiProfileRow : TableRow {
	aip::Profile profile;

	AiProfileRow() { kind = node_kind(AiProfileKind::Profile); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<AiProfileRow>(*this); }
	std::string name() const override { return "Profile"; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<aip::Profile *>(&profile)}; }
	size_t footprint() const override;
};

const RecordTable &ai_profile_table();

class AiProfileDocument : public TableDocument {
public:
	const RecordTable &table() const override { return ai_profile_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return ai_profile_table().fields(kind); }
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<AiProfileDocument>(*this); }
	// The profile as the rows hold it (null before a load).
	const aip::Profile *profile() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A profile's file holds one profile: no row is added.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
};

bool is_ai_profile_kind(AssetKind kind);

// The profile type's validator (DocumentType::validate_file): the lines the reader reads nothing of (listed), and a
// profile of no type (the game reads none of its keys).
std::vector<Diagnostic> validate_ai_profile_file(const DocumentBase &document);

enum class AiProfileFinding { InvalidInput, IgnoredInput, NoType, kCount };
const FindingCodeRow &finding_code(AiProfileFinding code);
FindingTable ai_profile_finding_codes();

} // namespace opennova::editor
