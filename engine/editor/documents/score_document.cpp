// The scoring table document (score_document.h, ADR 0046 S23 B): score.ini's version and fanfare and its GAMETYPE
// blocks as rows over formats/score, read and written over the file's modeled layout; the fields' words and the
// findings say what the game makes of the file (world-wac-ai-re §20.8b).
#include "score_document.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/noted_file_state.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kHeader = node_kind(ScoreKind::Header);
constexpr NodeKind kBlock = node_kind(ScoreKind::Block);
constexpr NodeKind kField = node_kind(ScoreKind::Field);
constexpr NodeKind kVar = node_kind(ScoreKind::Var);

const ScoreHeader &header_of(const RecordHandle &r) { return r.as<ScoreHeader>(); }
const score::GameTypeBlock &block_of(const RecordHandle &r) { return r.as<score::GameTypeBlock>(); }
const score::Entry &entry_of(const RecordHandle &r) { return r.as<score::Entry>(); }

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) return out = *whole, true;
	return false;
}

// A name the reader takes as one token of its line: no quote (it would end the quoted run), no line break.
bool set_quoted_name(std::string &field, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text || text->empty()) return error = "A name is a text of one character or more.", false;
	if (text->find_first_of("\"\r\n") != std::string::npos) return error = "A name holds no quote and no line break.", false;
	field = *text;
	return true;
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
			// Left out where the file has no fanfare line (the defaults' 0 0 stand); a set of another value writes one,
			// and a set where it has one rewrites that line.
			fanfare.optional = true;
			header.field(RF{fanfare,
			                {[i](const RecordHandle &r, Value &out) { return out = int64_t(header_of(r).exp_fanfare[i] & 0xFF), true; },
			                 [i](const RecordHandle &r, const Value &v, std::string &e) {
				                 int64_t n = 0;
				                 if (!whole_of(v, n) || n < 0 || n > 255) return e = "A fanfare byte is from 0 to 255.", false;
				                 ScoreHeader &h = r.as<ScoreHeader>();
				                 if (h.exp_fanfare[i] != int32_t(n)) h.has_exp_fanfare = true;
				                 h.exp_fanfare[i] = int32_t(n);
				                 return true;
			                 },
			                 [](const RecordHandle &r) { return header_of(r).has_exp_fanfare; },
			                 [](const RecordHandle &r, bool present, std::string &) {
				                 r.as<ScoreHeader>().has_exp_fanfare = present;
				                 return true;
			                 }}});
		}
	}
	// --- a block -----------------------------------------------------------------------------------------
	TableKind block(RecordKindRow{kBlock, "block", "Game type", "Add game type", true});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Game type",
				"The row the block's lines go into, the first of the twelve of the name, without case [orig: sub_52D850 @ "
				"0x52D850]; a name no row has reads the block's lines for nothing. The writer puts row 2 down first (COOP), "
				"the rows then in the ladder's order.");
		name.open_choices = true;
		for (size_t row = 1; row < score::game_type_names().size(); ++row)
			name.choices.push_back({score::game_type_names()[row], int64_t(row), ""});
		block.field(RF{name,
		               {[](const RecordHandle &r, Value &out) { return out = block_of(r).name, true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                return set_quoted_name(r.as<score::GameTypeBlock>().name, v, e);
		                }}});
		TableList fields;
		fields.spec = Document::CollectionSpec{kField, "Scoreboard columns", "name", false, Applicability::Reads, 0};
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
		               {[](const RecordHandle &r, Value &out) { return out = entry_of(r).name, true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                const auto *text = std::get_if<std::string>(&v);
			                if (!text || score::field_id(*text) < 0) return e = "A column is a name of the FIELD table.", false;
			                r.as<score::Entry>().name = *text;
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
		             {[](const RecordHandle &r, Value &out) { return out = entry_of(r).name, true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              const auto *text = std::get_if<std::string>(&v);
			              if (!text || score::var_id(*text) < 0) return e = "An event is a name of the VAR table.", false;
			              r.as<score::Entry>().name = *text;
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
			if (score::field_id(later.fields[i].name) != score::field_id(earlier.fields[i].name) ||
			    (later.fields[i].value & 0xFF) != (earlier.fields[i].value & 0xFF))
				return true;
	}
	for (const score::Entry &var : later.vars) {
		const auto same = std::find_if(earlier.vars.begin(), earlier.vars.end(), [&](const score::Entry &e) {
			return score::var_id(e.name) == score::var_id(var.name);
		});
		if (same == earlier.vars.end() || same->value != var.value) return true;
	}
	return false;
}

constexpr FindingCodeEntry<ScoreFinding> kFindingEntries[] = {
	{ ScoreFinding::Version, listed_code("score.version") },
	{ ScoreFinding::FanfareUnkept, listed_code("score.fanfare_unkept") },
	{ ScoreFinding::UnknownGameType, listed_code("score.unknown_game_type") },
	{ ScoreFinding::GameTypeRepeated, listed_code("score.game_type_repeated") },
	{ ScoreFinding::FieldsPast34, listed_code("score.fields_past_34") },
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

bool is_score_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::ScoreTable; }

size_t ScoreBlockRow::footprint() const {
	size_t bytes = sizeof(ScoreBlockRow) + footprint_of(block.name) + footprint_of(block.fields) + footprint_of(block.vars) +
	               ids_footprint();
	for (const score::Entry &e : block.fields) bytes += footprint_of(e.name);
	for (const score::Entry &e : block.vars) bytes += footprint_of(e.name);
	return bytes;
}

score::File ScoreDocument::file() const {
	score::File out;
	if (const auto *noted = noted_layout(file_state())) out.note = noted->root();
	for (const auto &node : rows()) {
		if (!node) continue;
		if (node->kind == kHeader) {
			const ScoreHeader &h = static_cast<const ScoreHeaderRow &>(*node).header;
			out.version = h.version;
			out.exp_fanfare[0] = h.exp_fanfare[0];
			out.exp_fanfare[1] = h.exp_fanfare[1];
			out.has_exp_fanfare = h.has_exp_fanfare;
			out.fanfare_before = h.fanfare_before;
			out.fanfare_after = h.fanfare_after;
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
		return b.name + " (" + std::to_string(b.fields.size()) + " columns, " + std::to_string(b.vars.size()) + " events)";
	}
	const RecordHandle handle = record_in(*node, address);
	if (!handle) return record_name(address);
	const score::Entry &e = entry_of(handle);
	return e.name + " " + std::to_string(address.kind == kField ? (e.value & 0xFF) : e.value);
}

bool ScoreDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                          std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &, Diagnostic &error) {
	if (!is_score_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not score.ini.", path());
		return false;
	}
	auto notes = std::make_shared<textlayout::Notes>();
	score::File read;
	std::string message;
	score::parse(bytes.data(), bytes.size(), read, message, *notes);
	auto header = std::make_shared<ScoreHeaderRow>();
	header->header.version = read.version;
	header->header.exp_fanfare[0] = read.exp_fanfare[0];
	header->header.exp_fanfare[1] = read.exp_fanfare[1];
	header->header.has_exp_fanfare = read.has_exp_fanfare;
	header->header.fanfare_before = read.fanfare_before;
	header->header.fanfare_after = read.fanfare_after;
	shape(*header);
	rows.push_back(std::move(header));
	for (score::GameTypeBlock &block : read.blocks) {
		auto row = std::make_shared<ScoreBlockRow>();
		row->block = std::move(block);
		shape(*row);
		rows.push_back(std::move(row));
	}
	auto noted = std::make_shared<NotedFileState>();
	noted->notes = std::move(notes);
	state = std::move(noted);
	return true;
}

SerializeResult ScoreDocument::serialize() const {
	SerializeResult result;
	std::vector<uint8_t> bytes;
	std::string error;
	bool rewritten = false;
	if (!score::write(file(), noted_layout(file_state()), bytes, error, &rewritten)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The scoring table could not be written: " + error});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	if (rewritten)
		result.notes.push_back("The table is written in the game's own form (ScoreConfig_SaveFile's): the file's lines would "
		                       "not read back as it is.");
	return result;
}

std::string ScoreDocument::save_words() const {
	return "Saving writes the file in the form it was read in (the shipped score.ini is the game's own writer's form); a "
	       "changed value changes its own line, a new column or event goes after the one before it.";
}

std::shared_ptr<Node> ScoreDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                               std::string &error) {
	if (kind != kBlock) {
		error = "The scoring table's rows are its game types (the file's own values are its first row).";
		return nullptr;
	}
	auto row = std::make_shared<ScoreBlockRow>();
	// The first game type the file has no block of, in the ladder's order from row 1.
	for (size_t index = 1; index < score::game_type_names().size() && row->block.name.empty(); ++index) {
		const std::string &name = score::game_type_names()[index];
		const bool taken = std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
			return other && other->kind == kBlock && strutil::iequals(other->name(), name);
		});
		if (!taken) row->block.name = name;
	}
	if (row->block.name.empty()) row->block.name = score::game_type_names()[2];
	shape(*row);
	return row;
}

void ScoreDocument::prepare_duplicate(Node &copy, const Node &, const std::vector<std::shared_ptr<const Node>> &) const {
	if (copy.kind != kBlock) return;
	score::GameTypeBlock &b = static_cast<ScoreBlockRow &>(copy).block;
	b.note = 0;
	for (score::Entry &e : b.fields) e.note = 0;
}

void ScoreDocument::prepare_record(const Node &, const ListChange &change, DetachedRecord &record) const {
	if (change.operation == EditOperation::Duplicate && record.data && record.kind == kField)
		static_cast<score::Entry *>(record.data.get())->note = 0;
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
	std::unordered_map<std::string, const score::GameTypeBlock *> seen; // a game type's upper name -> its first block
	for (const auto &node : table->rows()) {
		if (!node) continue;
		if (node->kind == kHeader) {
			const ScoreHeader &h = static_cast<const ScoreHeaderRow &>(*node).header;
			if (h.version != score::kVersion)
				add(*node, DiagnosticSeverity::Warning, ScoreFinding::Version, "version",
				    "The version is " + std::to_string(h.version) + ": the game reads none of the file (no block of it is read "
				    "here either) and writes its own defaults over it [orig: ScoreConfig_LoadFile @ 0x52DA8A -> "
				    "ScoreConfig_SaveFile @ 0x52DA9E]. A save keeps the file's lines as they are.");
			// The fanfare line's pair where the game ends up holding another: one failing the gate (the game's own 0 0,
			// the defaults' too, holds what it says), or one a block's later line stores over.
			score::File f;
			f.exp_fanfare[0] = h.exp_fanfare[0];
			f.exp_fanfare[1] = h.exp_fanfare[1];
			f.has_exp_fanfare = h.has_exp_fanfare;
			f.fanfare_before = h.fanfare_before;
			f.fanfare_after = h.fanfare_after;
			int32_t kept[2] = {0, 0};
			const bool stored = score::kept_fanfare(f, kept);
			if (h.has_exp_fanfare && ((h.exp_fanfare[0] & 0xFF) != kept[0] || (h.exp_fanfare[1] & 0xFF) != kept[1])) {
				const std::string held = std::to_string(kept[0]) + " " + std::to_string(kept[1]);
				add(*node, DiagnosticSeverity::Info, ScoreFinding::FanfareUnkept, "fanfare_low",
				    score::exp_fanfare_kept(f)
				            ? "A later EXP_FANFARE line, in a GAMETYPE block, stores " + held + " over these, which the game "
				              "keeps [orig: ScoreConfig_LoadFile @ 0x52DC75..0x52DC9F, each line past the gate stored over "
				              "the last]."
				            : "The game keeps the fanfare only when both bytes are other than 0 and the high one is the "
				              "greater [orig: ScoreConfig_LoadFile @ 0x52DC8E]: these are read for nothing, and the game "
				              "holds " + held + (stored ? std::string(", another EXP_FANFARE line's.") : std::string(", the defaults'.")));
			}
			continue;
		}
		if (node->kind != kBlock) continue;
		const score::GameTypeBlock &b = static_cast<const ScoreBlockRow &>(*node).block;
		const int row = score::game_type_row(b.name);
		if (row < 0) {
			add(*node, DiagnosticSeverity::Warning, ScoreFinding::UnknownGameType, "name",
			    "No game type is named '" + b.name + "': the game finds no row for the block and reads its lines for "
			    "nothing [orig: sub_52D850 @ 0x52D850].");
		} else {
			const std::string key = strutil::to_upper(score::game_type_names()[size_t(row)]);
			const auto first = seen.find(key);
			// The game's own writer writes COOP twice, alike: a later block that changes nothing the row holds is no
			// matter.
			if (first != seen.end() && changes_row(*first->second, b))
				add(*node, DiagnosticSeverity::Info, ScoreFinding::GameTypeRepeated, "name",
				    "An earlier block is " + first->second->name + ": the game reads both into the same row, this block's "
				    "FIELD lines replacing the earlier's and its VARs writing over theirs [orig: ScoreConfig_LoadFile @ "
				    "0x52DB50, @ 0x52DBC3].");
			else if (first == seen.end())
				seen.emplace(key, &b);
		}
		if (b.fields.size() > score::kMaxFields)
			add(*node, DiagnosticSeverity::Warning, ScoreFinding::FieldsPast34, "",
			    b.name + " has " + std::to_string(b.fields.size()) + " columns: a row holds 34, and the game drops the rest "
			    "[orig: sub_52CD70 @ 0x52CD70].");
	}
	return findings;
}

} // namespace opennova::editor
