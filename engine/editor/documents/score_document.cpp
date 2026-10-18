// The scoring table document (score_document.h, ADR 0046 S23 B): score.ini's version and fanfare and its GAMETYPE
// blocks as rows over formats/score, each block its row and each entry its id in its name table, written in
// ScoreConfig_SaveFile's form from them alone; the fields' words and the findings say what the game makes of the file
// (world-wac-ai-re §20.8b).
#include "score_document.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kHeader = node_kind(ScoreKind::Header);
constexpr NodeKind kBlock = node_kind(ScoreKind::Block);
constexpr NodeKind kField = node_kind(ScoreKind::Field);
constexpr NodeKind kVar = node_kind(ScoreKind::Var);

const ScoreHeader &header_of(const RecordHandle &r) { return r.as<ScoreHeader>(); }
const score::GameTypeBlock &block_of(const RecordHandle &r) { return r.as<score::GameTypeBlock>(); }
const score::Entry &entry_of(const RecordHandle &r) { return r.as<score::Entry>(); }

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) return out = *whole, true;
	return false;
}

// A table's name of an id, "" for none.
std::string name_of(const char *name) { return name ? std::string(name) : std::string(); }

// The GAMETYPE lines of a name no row has, which the reader reads for nothing with every FIELD and VAR line after
// them [orig: sub_52D850 @ 0x52D850; ScoreConfig_LoadFile @ 0x52DB7C, @ 0x52DC05], each line cut as the reader cuts
// it (a CR LF, an LF or a lone CR ends it; one whose first or second character is '/' read for nothing) and split
// into its tokens (white space between them, a quote toggling a quoted run, the quotes dropped) [orig:
// Text_ReadLine @ 0x52D110; Text_SplitIntoTokens @ 0x52D1B0]; at a version other than 40 the reader reads none.
void unknown_game_types(const std::vector<uint8_t> &bytes, int32_t version, std::vector<SourceIssue> &issues) {
	if (version != score::kVersion) return;
	size_t at = 0, line = 0;
	while (at < bytes.size() && bytes[at] != 0) {
		size_t end = at;
		while (end < bytes.size() && bytes[end] != '\r' && bytes[end] != '\n' && bytes[end] != 0) ++end;
		const std::string text(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(end));
		++line;
		if (end < bytes.size() && bytes[end] == '\r' && end + 1 < bytes.size() && bytes[end + 1] == '\n') ++end;
		at = end < bytes.size() && bytes[end] != 0 ? end + 1 : end;
		if ((!text.empty() && text[0] == '/') || (text.size() > 1 && text[1] == '/')) continue;
		std::vector<std::string> tokens;
		std::string token;
		bool quoted = false, any = false;
		for (const char c : text) {
			if (c == '"') {
				quoted = !quoted;
				any = true;
			} else if (!quoted && (c == ' ' || c == '\t')) {
				if (any) tokens.push_back(token);
				token.clear();
				any = false;
			} else {
				token += c;
				any = true;
			}
		}
		if (any) tokens.push_back(token);
		if (tokens.size() < 2 || !strutil::iequals(tokens[0], "GAMETYPE") || score::game_type_row(tokens[1]) >= 0) continue;
		SourceIssue issue;
		issue.blocks = false;
		issue.line = line;
		issue.message = "No game type is named '" + tokens[1] + "': the game finds no row for the block and reads its "
		                "lines for nothing [orig: sub_52D850 @ 0x52D850]; a save writes the table without them.";
		issues.push_back(std::move(issue));
	}
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- the file's own values ---------------------------------------------------------------------------
	TableKind header(RecordKindRow{kHeader, "header", "Score table", "", true});
	{
		// Shown, not set: the game reads the file only at 40 and writes 40 itself; a file of another version is one
		// it reads none of, whose blocks the document holds none of either (score.version says so).
		FieldSchema version = schema_of("version", FieldType::Integer, "Version",
				"The file's VERSION, the last of its lines; the game reads the file only at 40, else writes its own defaults "
				"over it [orig: ScoreConfig_LoadFile @ 0x52DA8A -> ScoreConfig_SaveFile @ 0x52DA9E].");
		version.read_only = true;
		header.field(RF{version, {[](const RecordHandle &r, Value &out) { return out = int64_t(header_of(r).version), true; }}});
		for (int i = 0; i < 2; ++i) {
			FieldSchema fanfare = schema_of(i ? "fanfare_high" : "fanfare_low", FieldType::Byte,
					i ? "Fanfare, high" : "Fanfare, low",
					"EXP_FANFARE's two bytes: the score rises a hit, kill and headshot tone play at, kept only when both are "
					"other than 0 and the high one is the greater [orig: ScoreConfig_LoadFile @ 0x52DC75..0x52DC9F; net-re "
					"0x81].");
			fanfare.ranged = true;
			fanfare.min = 0;
			fanfare.max = 255;
			header.field(RF{fanfare,
			                {[i](const RecordHandle &r, Value &out) { return out = int64_t(header_of(r).exp_fanfare[i] & 0xFF), true; },
			                 [i](const RecordHandle &r, const Value &v, std::string &e) {
				                 int64_t n = 0;
				                 if (!whole_of(v, n) || n < 0 || n > 255) return e = "A fanfare byte is from 0 to 255.", false;
				                 r.as<ScoreHeader>().exp_fanfare[i] = int32_t(n);
				                 return true;
			                 }}});
		}
	}
	// --- a block -----------------------------------------------------------------------------------------
	TableKind block(RecordKindRow{kBlock, "block", "Game type", "Add game type", true});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Game type",
				"The row the block's lines go into, by its name: the first of the twelve of the name, without case [orig: "
				"sub_52D850 @ 0x52D850]. The writer puts down the row's own name [orig: ScoreConfig_SaveFile @ 0x52CF81].");
		for (size_t row = 1; row < score::game_type_names().size(); ++row)
			name.choices.push_back({score::game_type_names()[row], int64_t(row), ""});
		block.field(RF{name,
		               {[](const RecordHandle &r, Value &out) {
			                const int row = block_of(r).row;
			                out = row >= 0 && size_t(row) < score::game_type_names().size()
			                              ? score::game_type_names()[size_t(row)] : std::string();
			                return true;
		                },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                const auto *text = std::get_if<std::string>(&v);
			                const int row = text ? score::game_type_row(*text) : -1;
			                if (row < 1) return e = "A game type is a name of the game's twelve rows.", false;
			                r.as<score::GameTypeBlock>().row = row;
			                return true;
		                }}});
		TableList fields;
		// A row holds 34 [orig: sub_52CD70 @ 0x52CD70]: the reader keeps no more, and the writer writes no more.
		fields.spec = Document::CollectionSpec{kField, "Scoreboard columns", "name", false, Applicability::Reads,
		                                       score::kMaxFields};
		fields.ops = vector_list<score::GameTypeBlock, score::Entry>(
				kField, [](score::GameTypeBlock &b) -> std::vector<score::Entry> & { return b.fields; });
		block.list(std::move(fields));
		TableList vars;
		vars.spec = Document::CollectionSpec{kVar, "Points", "name", false, Applicability::Reads, 0};
		vars.ops = vector_list<score::GameTypeBlock, score::Entry>(
				kVar, [](score::GameTypeBlock &b) -> std::vector<score::Entry> & { return b.vars; });
		block.list(std::move(vars));
	}
	// --- a FIELD line ------------------------------------------------------------------------------------
	TableKind field(RecordKindRow{kField, "field", "Scoreboard column", "", false});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Column",
				"A statistic of the FIELD table, compared without case [orig: the FIELD names @ 0x830240]: the first FIELD "
				"line of a block clears its row's list and each appends its column, 34 at most [orig: ScoreConfig_LoadFile "
				"@ 0x52DBC3; sub_52CD70 @ 0x52CD70]. The end-of-round scoreboard shows a configured column where a player "
				"has any of it (net-re, \"Configured columns and ordering\").");
		for (const score::Name &row : score::field_names()) name.choices.push_back({row.name, row.id, ""});
		field.field(RF{name,
		               {[](const RecordHandle &r, Value &out) { return out = name_of(score::field_name(entry_of(r).id)), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                const auto *text = std::get_if<std::string>(&v);
			                const int32_t id = text ? score::field_id(*text) : -1;
			                if (id < 0) return e = "A column is a name of the FIELD table.", false;
			                r.as<score::Entry>().id = id;
			                return true;
		                }}});
		FieldSchema value = schema_of("value", FieldType::Byte, "Shown",
				"The byte the row keeps beside the column's id [orig: ScoreConfig_LoadFile @ 0x52DBD9, atol cut to a byte].");
		value.ranged = true;
		value.min = 0;
		value.max = 255;
		field.field(RF{value,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(entry_of(r).value & 0xFF), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                int64_t n = 0;
			                if (!whole_of(v, n) || n < 0 || n > 255) return e = "The value is a byte, 0 to 255.", false;
			                r.as<score::Entry>().value = int32_t(n);
			                return true;
		                }}});
	}
	// --- a VAR line --------------------------------------------------------------------------------------
	TableKind var(RecordKindRow{kVar, "var", "Points", "", false});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Event",
				"An event of the VAR table, compared without case [orig: the VAR names @ 0x830348]: its points stored at the "
				"row's word for it [orig: ScoreConfig_LoadFile @ 0x52DC50, +300 + 4 x id], read by the scoring dispatch "
				"[orig: GameEvent_ProcessScoring @ 0x52F550]. A VAR the block lacks keeps the game's default.");
		for (const score::Name &row : score::var_names()) name.choices.push_back({row.name, row.id, ""});
		var.field(RF{name,
		             {[](const RecordHandle &r, Value &out) { return out = name_of(score::var_name(entry_of(r).id)), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              const auto *text = std::get_if<std::string>(&v);
			              const int32_t id = text ? score::var_id(*text) : -1;
			              if (id < 0) return e = "An event is a name of the VAR table.", false;
			              r.as<score::Entry>().id = id;
			              return true;
		              }}});
		FieldSchema value = schema_of("value", FieldType::Integer, "Points", "A whole number [orig: atol @ 0x52DC50].");
		var.field(RF{value,
		             {[](const RecordHandle &r, Value &out) { return out = int64_t(entry_of(r).value), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              int64_t n = 0;
			              if (!whole_of(v, n) || n < INT32_MIN || n > INT32_MAX) return e = "Points are a whole number.", false;
			              r.as<score::Entry>().value = int32_t(n);
			              return true;
		              }}});
	}
	return RecordTable({std::move(header), std::move(block), std::move(field), std::move(var)});
}

// Whether `later`, read into the row `earlier` was read into, changes what the row holds: its FIELD lines, where it
// has any, replace the row's list [orig: ScoreConfig_LoadFile @ 0x52DBC3]; each VAR writes its value over the
// row's [orig: @ 0x52DC50].
bool changes_row(const score::GameTypeBlock &earlier, const score::GameTypeBlock &later) {
	if (!later.fields.empty()) {
		if (later.fields.size() != earlier.fields.size()) return true;
		for (size_t i = 0; i < later.fields.size(); ++i)
			if (later.fields[i].id != earlier.fields[i].id ||
			    (later.fields[i].value & 0xFF) != (earlier.fields[i].value & 0xFF))
				return true;
	}
	for (const score::Entry &var : later.vars) {
		const auto same = std::find_if(earlier.vars.begin(), earlier.vars.end(),
		                               [&](const score::Entry &e) { return e.id == var.id; });
		if (same == earlier.vars.end() || same->value != var.value) return true;
	}
	return false;
}

constexpr FindingCodeEntry<ScoreFinding> kFindingEntries[] = {
	{ ScoreFinding::Version, listed_code("score.version") },
	// A pair the game's gate refuses: the writer writes none but a kept pair or the defaults' 0 0 (score::write).
	{ ScoreFinding::FanfareUnkept, unwritable_code("score.fanfare_unkept") },
	{ ScoreFinding::UnknownGameType, listed_code("score.unknown_game_type") },
	{ ScoreFinding::GameTypeRepeated, listed_code("score.game_type_repeated") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(ScoreFinding::kCount),
		"every ScoreFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the scoring table's rows follow ScoreFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::ScoreTables);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const RecordTable &score_table() {
	static const RecordTable table = make_table();
	return table;
}

std::string ScoreBlockRow::name() const {
	return block.row >= 0 && size_t(block.row) < score::game_type_names().size() ? score::game_type_names()[size_t(block.row)]
	                                                                               : std::string();
}

size_t ScoreBlockRow::footprint() const {
	return sizeof(ScoreBlockRow) + footprint_of(block.fields) + footprint_of(block.vars) + ids_footprint();
}

score::File ScoreDocument::file() const {
	score::File out;
	for (const auto &node : rows()) {
		if (!node) continue;
		if (node->kind == kHeader) {
			const ScoreHeader &h = static_cast<const ScoreHeaderRow &>(*node).header;
			out.version = h.version;
			out.exp_fanfare[0] = h.exp_fanfare[0];
			out.exp_fanfare[1] = h.exp_fanfare[1];
		} else if (node->kind == kBlock) {
			out.blocks.push_back(static_cast<const ScoreBlockRow &>(*node).block);
		}
	}
	return out;
}

std::string ScoreDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) {
		if (node->kind == kHeader) return "Score table (version " + std::to_string(static_cast<const ScoreHeaderRow &>(*node).header.version) + ")";
		const score::GameTypeBlock &b = static_cast<const ScoreBlockRow &>(*node).block;
		return node->name() + " (" + std::to_string(b.fields.size()) + " columns, " + std::to_string(b.vars.size()) + " events)";
	}
	const RecordHandle handle = record_in(*node, address);
	if (!handle) return record_name(address);
	const score::Entry &e = entry_of(handle);
	return name_of(address.kind == kField ? score::field_name(e.id) : score::var_name(e.id)) + " " +
	       std::to_string(address.kind == kField ? (e.value & 0xFF) : e.value);
}

bool ScoreDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                          std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	score::File read;
	std::string message;
	score::parse(bytes.data(), bytes.size(), read, message);
	unknown_game_types(bytes, read.version, issues);
	auto header = std::make_shared<ScoreHeaderRow>();
	header->header.version = read.version;
	header->header.exp_fanfare[0] = read.exp_fanfare[0];
	header->header.exp_fanfare[1] = read.exp_fanfare[1];
	shape(*header);
	rows.push_back(std::move(header));
	for (score::GameTypeBlock &block : read.blocks) {
		auto row = std::make_shared<ScoreBlockRow>();
		row->block = std::move(block);
		shape(*row);
		rows.push_back(std::move(row));
	}
	return true;
}

SerializeResult ScoreDocument::serialize() const {
	SerializeResult result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!score::write(file(), bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The scoring table could not be written: " + error});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string ScoreDocument::save_words() const {
	return "Saving writes the table in the game's own writer's form (ScoreConfig_SaveFile's, the form the shipped "
	       "score.ini is in), from what the game reads of it: its version, its fanfare, then each game type's columns "
	       "and events.";
}

std::shared_ptr<Node> ScoreDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                               std::string &error) {
	if (kind != kBlock) {
		error = "The scoring table's rows are its game types (the file's own values are its first row).";
		return nullptr;
	}
	auto row = std::make_shared<ScoreBlockRow>();
	// The first game type the file has no block of, in the ladder's order from row 1; COOP (row 2) where every row
	// has one.
	row->block.row = 2;
	for (size_t index = 1; index < score::game_type_names().size(); ++index) {
		const bool taken = std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
			return other && other->kind == kBlock && static_cast<const ScoreBlockRow &>(*other).block.row == int(index);
		});
		if (!taken) {
			row->block.row = int(index);
			break;
		}
	}
	shape(*row);
	return row;
}

const FindingCodeRow &finding_code(ScoreFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable score_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_score_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *table = dynamic_cast<const ScoreDocument *>(&document);
	if (!table) return findings;
	const auto add = [&](const Node &node, DiagnosticSeverity severity, ScoreFinding code, const char *field,
	                     const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = node.id;
		d.record_kind = node.kind;
		d.record = node.name();
		findings.push_back(std::move(d));
	};
	std::unordered_map<int, const score::GameTypeBlock *> seen; // a row -> its first block
	for (const auto &node : table->rows()) {
		if (!node) continue;
		if (node->kind == kHeader) {
			const ScoreHeader &h = static_cast<const ScoreHeaderRow &>(*node).header;
			if (h.version != score::kVersion)
				add(*node, DiagnosticSeverity::Warning, ScoreFinding::Version, "version",
				    "The version is " + std::to_string(h.version) + ": the game reads none of the file (no block of it is read "
				    "here either) and writes its own defaults over it [orig: ScoreConfig_LoadFile @ 0x52DA8A -> "
				    "ScoreConfig_SaveFile @ 0x52DA9E]. A save keeps the file's lines as they are.");
			// A pair the game's gate refuses (the game's own 0 0 holds what it says): written, read for nothing.
			score::File f;
			f.exp_fanfare[0] = h.exp_fanfare[0];
			f.exp_fanfare[1] = h.exp_fanfare[1];
			if (!score::exp_fanfare_kept(f) && (h.exp_fanfare[0] != 0 || h.exp_fanfare[1] != 0))
				add(*node, DiagnosticSeverity::Error, ScoreFinding::FanfareUnkept, "fanfare_low",
				    "The game keeps the fanfare only when both bytes are other than 0 and the high one is the greater "
				    "[orig: ScoreConfig_LoadFile @ 0x52DC8E]: the game would read this pair for nothing and hold 0 0, so "
				    "the table is not saved until the pair is one it keeps, or 0 0.");
			continue;
		}
		if (node->kind != kBlock) continue;
		const score::GameTypeBlock &b = static_cast<const ScoreBlockRow &>(*node).block;
		const auto first = seen.find(b.row);
		// The game's own writer writes COOP twice, alike: a later block that changes nothing the row holds is no matter.
		if (first != seen.end() && changes_row(*first->second, b))
			add(*node, DiagnosticSeverity::Info, ScoreFinding::GameTypeRepeated, "name",
			    "An earlier block is " + node->name() + ": the game reads both into the same row, this block's FIELD "
			    "lines replacing the earlier's and its VARs writing over theirs [orig: ScoreConfig_LoadFile @ 0x52DB50, @ "
			    "0x52DBC3].");
		else if (first == seen.end())
			seen.emplace(b.row, &b);
	}
	// The GAMETYPE lines of a name no row has, which the read found (unknown_game_types).
	for (const SourceIssue &issue : table->issues()) {
		Diagnostic d = make_finding(ScoreFinding::UnknownGameType, DiagnosticSeverity::Warning, issue.message, document.path());
		d.line = issue.line;
		findings.push_back(std::move(d));
	}
	return findings;
}

} // namespace opennova::editor
