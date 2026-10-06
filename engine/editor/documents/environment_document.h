#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/env/env.h>

namespace opennova::editor {

// An environment (the deep-integration plan's DI-19a; CONTEXT.md "Environment document"): a `.env`,
// a mission's sky, light, fog and water, read and written through the engine's own reader and
// writer (env::load_env, env::save_env: the writer from scratch, ADR 0003) over the engine's own
// record, env::Config (docs/env/env-tod-re.md). One row, the environment, whose fields are the
// keywords the game reads in the units the file writes them (a time as HHMM, a fog distance in
// whole metres, a water height in half metres, a colour as its three bytes), with the colours the
// game runs on where no keyframe is; and its list of time-of-day keyframes, at most 16, each its
// time and its twelve colours. The sky's cloud layers name textures and the sun, moon, glare and
// star name models (the asset graph's edges, each a Go to). What the reader leaves out, or reads in
// a way the record cannot hold, is a source finding of its line (environment_source_issues).

enum class EnvironmentKind : NodeKind { Environment = 0, Keyframe = 1 };
constexpr NodeKind node_kind(EnvironmentKind kind) { return static_cast<NodeKind>(kind); }

// The environment row: the engine's record and the sky height's latent value (its keyword reads
// whole metres; the engine's default, 200/65536 m, has no file form, so a file that leaves the
// keyword out keeps the default and the value it would write is kept apart: Clear and Write).
struct EnvironmentRow : TableRow {
	env::Config config;
	int sky_height_latent = 0;

	EnvironmentRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<EnvironmentRow>(*this); }
	std::string name() const override { return "Environment"; }
	RecordHandle record() const override;
	size_t footprint() const override;
	// Whether the file writes sky_height (the record holds a value other than the engine's default).
	bool sky_height_written() const;
};

// The environment's table (environment_document.cpp): the environment row's and a keyframe's kind,
// their fields in the game's words and units, cited, and the keyframes' list.
const RecordTable &environment_table();

// The lines of an environment's text the game's reader reads otherwise than the record holds
// (each a SourceIssue on its line): a line it skips (a keyword it has no arm for; a terrain keyword
// is read by the terrain's reader, whose hook stays on for the .env, so that one blocks), a
// keyword written again (the last line wins), a colour line short of its three values (the blue
// an earlier line's), a time that reads as another, a tod_begin past the 16th, and an envscale
// that follows a colour it does not scale the way the record would (the record scales every colour
// by the last envscale: it blocks).
void environment_source_issues(const std::string &text, std::vector<SourceIssue> &issues);

class EnvironmentDocument : public TableDocument {
public:
	const RecordTable &table() const override { return environment_table(); }
	// A kind's fields without a document (DocumentType::fields).
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return environment_table().fields(kind); }
	// A keyframe by its time ("Keyframe 12:00"), the row as the environment.
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<EnvironmentDocument>(*this); }
	// What a save writes beyond the edits: the editor's layout (env::save_env), the file's comments and
	// the lines the game skips left out.
	std::string save_words() const override;

	const EnvironmentRow *environment_row() const;
	// The environment as the engine holds it (null before a load).
	const env::Config *config() const;

protected:
	// A cloud layer's name loads the PCX the game's parser makes of it, as a sky cloud layer
	// (texture_roles.h SkyCloud, kTextureArgPcx).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// An environment keeps its one row.
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
};

bool is_environment_kind(AssetKind kind);

// The environment type's validator (DocumentType::validate_file), an open document standing in for
// its file: its source findings (environment.invalid_input, environment.ignored_input), and a sky
// height the file leaves out (environment.sky_height_default: the dome at the engine's default
// height, 200/65536 m).
std::vector<Diagnostic> validate_environment_file(const DocumentBase &document);

// The environment type's own finding codes (DocumentType::findings), in the order of its table
// (environment_document.cpp, static_asserted): input the record cannot hold (the file does not
// serialize), input the game reads otherwise than as written or skips (a Rewrite writes what it
// reads), the sky height left to the engine's default.
enum class EnvironmentFinding { InvalidInput, IgnoredInput, SkyHeightDefault, kCount };
const FindingCodeRow &finding_code(EnvironmentFinding code);
FindingTable environment_finding_codes();

} // namespace opennova::editor
