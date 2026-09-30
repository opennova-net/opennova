#include "mns_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/project/project_files.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>
#include <runtime/menu/menu_style.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

namespace opennova::editor {
namespace {

constexpr NodeKind kVariable = node_kind(StyleKind::Variable);
constexpr NodeKind kComment = node_kind(StyleKind::Comment);
constexpr NodeKind kBlank = node_kind(StyleKind::Blank);
constexpr NodeKind kConditional = node_kind(StyleKind::Conditional);
constexpr NodeKind kInactive = node_kind(StyleKind::Inactive);
// Editor limits: retail's reader allocates each name and value to its length.
constexpr size_t kNameWidth = 128;
constexpr size_t kValueWidth = 256;
constexpr size_t kCommentWidth = 256;
// The writer's line end: the only one the game reads without stopping.
const char *const kCrlf = "\r\n";

const StyleRow &style_of(const Node &node) { return static_cast<const StyleRow &>(node); }
StyleRow &style_of(Node &node) { return static_cast<StyleRow &>(node); }

NodeKind kind_of(mns::NodeKind kind) {
	switch (kind) {
	case mns::NodeKind::Define: return kVariable;
	case mns::NodeKind::Comment: return kComment;
	case mns::NodeKind::Blank: return kBlank;
	case mns::NodeKind::Directive: return kConditional;
	case mns::NodeKind::InactiveText: return kInactive;
	}
	return kBlank;
}

size_t line_count(const mns::Node &node) {
	return node.kind == mns::NodeKind::Define ? node.define_lines.size() : 1;
}

std::vector<const mns::Node *> natives_of(const std::vector<std::shared_ptr<const Node>> &rows) {
	std::vector<const mns::Node *> out;
	out.reserve(rows.size());
	for (const auto &row : rows) out.push_back(&style_of(*row).native);
	return out;
}

FieldSchema field(const char *id, FieldType type, size_t width, bool read_only, const char *label) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.width = width;
	schema.read_only = read_only;
	schema.label = label;
	return schema;
}

const std::vector<FieldSchema> &variable_fields() {
	static const std::vector<FieldSchema> fields = [] {
		std::vector<FieldSchema> out = {
		        field("name", FieldType::Text, kNameWidth, false, "Name"),
		        field("value", FieldType::Text, kValueWidth, false, "Value"),
		        field("comment", FieldType::Text, kCommentWidth, false, "Comment"),
		        field("line", FieldType::Integer, 0, true, "Line"),
		        field("lines", FieldType::Integer, 0, true, "Lines"),
		        field("overridden", FieldType::Integer, 0, true, "Defined again below"),
		};
		out.back().choices = {{"no", 0, "No"}, {"yes", 1, "Yes"}};
		out.front().defines = ReferenceKind::StyleVar; // the %NAME% a menu names the value by
		return out;
	}();
	return fields;
}

const std::vector<FieldSchema> &comment_fields() {
	static const std::vector<FieldSchema> fields = {
	        field("text", FieldType::Text, kCommentWidth, false, "Text"),
	        field("line", FieldType::Integer, 0, true, "Line"),
	};
	return fields;
}

const std::vector<FieldSchema> &blank_fields() {
	static const std::vector<FieldSchema> fields = {field("line", FieldType::Integer, 0, true, "Line")};
	return fields;
}

const std::vector<FieldSchema> &source_fields() {
	static const std::vector<FieldSchema> fields = {
	        field("text", FieldType::Text, kCommentWidth, true, "Text"),
	        field("line", FieldType::Integer, 0, true, "Line"),
	};
	return fields;
}

constexpr FindingCodeEntry<StyleFinding> kFindingEntries[] = {
	{ StyleFinding::LineEnding, { "style.line_ending", FindingFix::Rewrite, "with every line ending CR LF" } },
	{ StyleFinding::NotLoaded, { "style.not_loaded" } },
	{ StyleFinding::XmlChar, { "style.xml_char" } },
	{ StyleFinding::NestedVar, { "style.nested_var" } },
	{ StyleFinding::Backslash, { "style.backslash" } },
	{ StyleFinding::ReadDifferently, { "style.read_differently" } },
	{ StyleFinding::DirectiveForm, { "style.directive_form" } },
	{ StyleFinding::IfWithoutArgument, { "style.if_without_argument" } },
	{ StyleFinding::NoncanonicalIfArg, { "style.noncanonical_if_arg" } },
	{ StyleFinding::UnbalancedElse, { "style.unbalanced_else" } },
	{ StyleFinding::DuplicateElse, { "style.duplicate_else" } },
	{ StyleFinding::UnbalancedEndif, { "style.unbalanced_endif" } },
	{ StyleFinding::UnknownDirective, { "style.unknown_directive" } },
	{ StyleFinding::DirectiveTail, { "style.directive_tail" } },
	{ StyleFinding::LoneBackslash, { "style.lone_backslash" } },
	{ StyleFinding::ValueIsDirective, { "style.value_is_directive" } },
	{ StyleFinding::ValueStartsWithHash, { "style.value_starts_with_hash" } },
	{ StyleFinding::ValueOnNextLine, { "style.value_on_next_line" } },
	{ StyleFinding::DuplicateName, { "style.duplicate_name" } },
	{ StyleFinding::ContinuedDuplicate, { "style.continued_duplicate" } },
	{ StyleFinding::NulByte, { "style.nul_byte" } },
	{ StyleFinding::InvalidNameChar, { "style.invalid_name_char" } },
	{ StyleFinding::MissingValueDelimiter, { "style.missing_value_delimiter" } },
	{ StyleFinding::NoValue, { "style.no_value" } },
	{ StyleFinding::ContinuationAtEof, { "style.continuation_at_eof" } },
	{ StyleFinding::UnterminatedIf, { "style.unterminated_if" } },
	{ StyleFinding::Hangs, { "style.hangs" } },
	{ StyleFinding::Stops, { "style.stops" } },
	{ StyleFinding::OverriddenByBrand, { "style.overridden_by_brand" } },
	{ StyleFinding::Unused, { "style.unused" } },
	{ StyleFinding::NotAColor, { "style.not_a_color" } },
	{ StyleFinding::MixedUse, { "style.mixed_use" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(StyleFinding::kCount),
		"every StyleFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the stylesheet's rows follow StyleFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries);

const std::string *text_of(const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) error = "This field takes text.";
	return text;
}

// "the game stops reading ..." -> "The game stops reading ....": a sentence for Problems.
std::string sentence(std::string message) {
	if (!message.empty()) message[0] = char(std::toupper(static_cast<unsigned char>(message[0])));
	if (!message.empty() && message.back() != '.') message += '.';
	return message;
}

// What the stylesheet reader says of a line, as the stylesheet's row: its code (mns::Diagnostic::code,
// "lone-backslash") is the row's token after "style.", '_' for '-' (style.lone_backslash). A code the
// reader adds that no row has yet is reported as a reading the game makes otherwise than shown.
StyleFinding reader_finding(const std::string &code) {
	std::string token = "style.";
	for (char c : code) token += c == '-' ? '_' : c;
	for (size_t i = 0; i < kFindingRows.size(); ++i)
		if (token == kFindingRows[i].token) return static_cast<StyleFinding>(i);
	return StyleFinding::ReadDifferently;
}

} // namespace

std::string StyleRow::name() const {
	switch (native.kind) {
	case mns::NodeKind::Define: return native.define_lines.front().name;
	case mns::NodeKind::Blank: return std::string();
	default: return native.text;
	}
}

bool is_style_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Styles;
}

const std::vector<RecordKindRow> &MnsDocument::kinds() const {
	static const std::vector<RecordKindRow> table = {
	        {kVariable, "variable", "Variable", "Add variable", true},
	        {kComment, "comment", "Comment", "Add comment", true},
	        {kBlank, "blank", "Blank line", "Add blank line", true},
	        {kConditional, "directive", "Directive", "", true},
	        {kInactive, "inactive", "Switched-off line", "", true},
	};
	return table;
}

const std::vector<FieldSchema> &MnsDocument::fields(NodeKind kind) const {
	static const std::vector<FieldSchema> none;
	switch (kind) {
	case kVariable: return variable_fields();
	case kComment: return comment_fields();
	case kBlank: return blank_fields();
	case kConditional:
	case kInactive: return source_fields();
	default: return none;
	}
}

bool MnsDocument::read_by_game() const { return menu::is_shell_stylesheet(basename_of(path())); }

NodeId MnsDocument::winning_row(const std::string &name) const {
	const std::string wanted = mns::variable_name(name);
	NodeId found = 0;
	for (const auto &row : rows())
		if (row->kind == kVariable && strutil::iequals(row->name(), wanted)) found = row->id;
	return found;
}

void MnsDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	if (address.kind != kVariable || use.schema->id != "value") return;
	const Node *row = this->row(address.row);
	if (!row) return;
	if (winning_row(row->name()) != row->id) {
		use.applies = Applicability::Ignored; // a later definition is the one the game reads
		return;
	}
	if (!read_by_game()) return;
	// A value naming a file of a kind a style variable stands for (a font, a menu texture) is a
	// reference to that file.
	use.reference =
			style_value_reference(classify_asset(mns::game_value(style_of(*row).native), nullptr));
}

void MnsDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	// The definition the game reads of a name is the last, carrying the value the game reads
	// [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870]; an earlier one, and one on a line
	// past the place the game stops reading the file, are defined but never read.
	const Node *node = row(address.row);
	if (!node) return;
	facts.line = size_t(line_of(node->id));
	Value value;
	if (get(address, "value", value)) facts.value = std::get<std::string>(value);
	const mns::StyleSheet &sheet = game_sheet();
	if (winning_row(node->name()) != node->id) {
		facts.inert = true;
		facts.inert_reason = "the file defines it again below, and the game reads the last";
		return;
	}
	if (!sheet.has(node->name())) {
		facts.inert = true;
		facts.inert_reason = "it comes after the place the game stops reading the file";
		return;
	}
	facts.value = sheet.get(node->name());
}

const StyleValueUse &MnsDocument::style_value_use(const NodeAddress &line, const AssetGraph *graph,
                                                  uint64_t graph_key) const {
	if (!value_uses_.made || value_uses_.load_generation != load_generation() ||
	    value_uses_.revision != revision() || value_uses_.graph != graph ||
	    value_uses_.graph_key != graph_key) {
		value_uses_.made = true;
		value_uses_.load_generation = load_generation();
		value_uses_.revision = revision();
		value_uses_.graph = graph;
		value_uses_.graph_key = graph_key;
		value_uses_.rows.clear();
	}
	const auto kept = value_uses_.rows.find(line.row);
	if (kept != value_uses_.rows.end()) return kept->second;
	StyleValueUse &out = value_uses_.rows[line.row];
	const Node *row = this->row(line.row);
	if (!row || row->kind != kVariable) return out;
	// The uses of the definition the game reads, by what its value must be there.
	out.winner = winning_row(row->name()) == row->id;
	bool colour = false, font = false, image = false, other = false;
	if (out.winner && read_by_game() && graph) {
		const GraphSymbol *binding = graph->style_binding(row->name());
		out.bound = binding && binding->file == path();
	}
	if (out.bound)
		for (const GraphEdge *edge : graph->referrers_of(ReferenceKind::StyleVar, row->name())) {
			const StyleVariableUse use = style_variable_use(edge->through);
			colour = colour || use == StyleVariableUse::Colour;
			font = font || use == StyleVariableUse::Font;
			image = image || use == StyleVariableUse::Image;
			other = other || use == StyleVariableUse::Other;
		}
	const NodeAddress address{row->id, row->kind, 0};
	FieldUse value;
	for (const FieldSchema &schema : fields(kVariable))
		if (schema.id == "value") value = field_on(address, schema);
	Value text;
	const std::string shown = get(address, "value", text) ? std::get<std::string>(text) : "";
	const bool fixed = frozen(*row);
	// The guess from the value alone only where no use says what it is: a string id's, a name's
	// or a shown text's value is none of a colour, a font and an image, hex digits or not.
	out.colour = !fixed && (colour || (!font && !image && !other && mnu::color_reads_whole(shown)));
	out.file = value;
	if (font || value.reference == ReferenceKind::Font) out.file.reference = ReferenceKind::Font;
	else if (image || value.reference == ReferenceKind::MenuTexture)
		out.file.reference = ReferenceKind::MenuTexture;
	else out.file.reference = ReferenceKind::None;
	out.picks = !fixed && !out.colour && graph && out.file.reference != ReferenceKind::None;
	return out;
}

const mns::StyleSheet &MnsDocument::game_sheet() const {
	if (!game_sheet_.made || game_sheet_.load_generation != load_generation() ||
	    game_sheet_.revision != revision()) {
		game_sheet_.made = true;
		game_sheet_.load_generation = load_generation();
		game_sheet_.revision = revision();
		game_sheet_.sheet = native().evaluate().sheet;
	}
	return game_sheet_.sheet;
}

bool MnsDocument::get(const NodeAddress &address, const std::string &name, Value &out) const {
	const Node *node = row(address.row);
	if (!node || address.child) return false;
	if (name == "line") {
		out = int64_t(line_of(node->id));
		return true;
	}
	if (name == "overridden" && node->kind == kVariable) {
		out = int64_t(winning_row(node->name()) != node->id ? 1 : 0);
		return true;
	}
	return Document::get(address, name, out);
}

bool MnsDocument::read(const Node &node, const NodeAddress &address, const std::string &name, Value &out) const {
	if (address.child) return false;
	const mns::Node &native = style_of(node).native;
	switch (node.kind) {
	case kVariable:
		if (name == "name") out = native.define_lines.front().name;
		else if (name == "value") out = mns::game_value(native);
		else if (name == "comment") out = native.define_lines.front().comment;
		else if (name == "lines") out = int64_t(native.define_lines.size());
		else return false;
		return true;
	case kComment:
	case kConditional:
	case kInactive:
		if (name != "text") return false;
		out = native.text;
		return true;
	default: return false;
	}
}

SerializeResult MnsDocument::serialize() const {
	SerializeResult result;
	const auto *state = dynamic_cast<const StyleFileState *>(file_state());
	if (state && state->has_bom) result.text = "\xEF\xBB\xBF";
	result.text += mns::render_nodes(natives_of(rows()), kCrlf);
	return result;
}

mns::Document MnsDocument::native() const {
	return mns::Document::parse(mns::render_nodes(natives_of(rows()), kCrlf));
}

int MnsDocument::line_of(NodeId id) const {
	int line = 1;
	for (const auto &row : rows()) {
		if (row->id == id) return line;
		line += int(line_count(style_of(*row).native));
	}
	return 0;
}

NodeId MnsDocument::row_at_line(int line) const {
	int first = 1;
	for (const auto &row : rows()) {
		const int next = first + int(line_count(style_of(*row).native));
		if (line >= first && line < next) return row->id;
		first = next;
	}
	return 0;
}

bool MnsDocument::frozen(const Node &row) const {
	if (row.kind == kConditional || row.kind == kInactive) return true;
	return row.kind == kVariable && mns::crosses_structure(style_of(row).native);
}

bool MnsDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                        std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &,
                        Diagnostic &error) {
	if (!is_style_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a menu stylesheet.", path());
		return false;
	}
	// The loader hands a single NUL for an empty file: an empty stylesheet.
	const bool empty = bytes.size() == 1 && bytes[0] == 0;
	const mns::Document doc = empty ? mns::Document()
	                                : mns::Document::parse(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	auto file = std::make_shared<StyleFileState>();
	file->has_bom = doc.has_bom();
	// A line end the game does not read: remembered for Problems; the rows end CR LF, as
	// Save writes them, so every check reads what the saved file will hold.
	for (const mns::Diagnostic &d : doc.diagnostics())
		if (d.code == "line-ending" && !file->line_end_line) {
			file->line_end_line = d.line;
			file->line_end_message = d.message;
		}
	for (const mns::Node &native : doc.nodes()) {
		auto row = std::make_shared<StyleRow>();
		row->kind = kind_of(native.kind);
		row->native = native;
		mns::end_lines_with(row->native, kCrlf);
		rows.push_back(row);
	}
	state = file;
	return true;
}

std::shared_ptr<Node> MnsDocument::make_node(NodeKind kind, NodeId id, std::string &error) {
	auto row = std::make_shared<StyleRow>();
	row->kind = kind;
	switch (kind) {
	case kVariable:
		// A variable needs a value: with nothing after its name the game reads the next
		// line as its value. White, until the modder sets one.
		row->native = mns::make_define("VAR" + std::to_string(id), "FFFFFFFF", std::string(), kCrlf);
		return row;
	case kComment:
		row->native = mns::make_comment(std::string(), kCrlf);
		row->native.text = "// ";
		return row;
	case kBlank:
		row->native = mns::make_blank(kCrlf);
		return row;
	default:
		error = "Directives and switched-off lines are written in the file itself, not added here.";
		return nullptr;
	}
}

bool MnsDocument::set_field(Node &node, const NodeAddress &address, const std::string &name, const Value &value,
                            std::string &error) {
	StyleRow &row = style_of(node);
	if (address.child) {
		error = "A stylesheet's lines hold no records.";
		return false;
	}
	for (const FieldSchema &schema : fields(node.kind)) {
		if (schema.id != name) continue;
		if (schema.read_only) {
			error = "This is read from the file.";
			return false;
		}
		const std::string *text = text_of(value, error);
		if (!text) return false;
		if (text->size() >= schema.width) {
			error = "The text is too long.";
			return false;
		}
		if (node.kind == kVariable) {
			if (name == "name") return mns::set_define_name(row.native, *text, &error);
			if (name == "value") return mns::set_define_chunk(row.native, *text, &error);
			return mns::set_define_comment(row.native, *text, &error);
		}
		// A comment line.
		if (text->find_first_of("\r\n") != std::string::npos) {
			error = "A comment is one line.";
			return false;
		}
		if (text->find_first_not_of(" \t") == std::string::npos) {
			error = "A comment line needs text: remove the line instead.";
			return false;
		}
		row.native.text = mns::normalize_comment(*text);
		return true;
	}
	error = "Unknown field.";
	return false;
}

bool MnsDocument::edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &, std::string &error) {
	error = "A stylesheet's lines hold no records.";
	return false;
}

bool MnsDocument::accept_change(const Change &change, std::string &error) const {
	// A frozen row keeps its place: only an in-place Set of its own fields passes.
	const bool in_place = change.before && change.after && change.before->id == change.after->id &&
	                      change.before_position == change.after_position;
	if (!in_place) {
		for (const Node *row : {change.before.get(), change.after.get()}) {
			if (!row || !frozen(*row)) continue;
			if (row->kind == kConditional)
				error = "The #if, #else and #endif lines stay where they are: change them in the file itself.";
			else if (row->kind == kInactive)
				error = "The lines of a switched-off #if block stay where they are.";
			else
				error = "'" + row->name() + "' continues across other lines (a directive, a comment or a blank "
				        "line): it stays where it is.";
			return false;
		}
	}
	std::vector<std::shared_ptr<const Node>> proposed = rows();
	apply_change(proposed, change, true);
	size_t first = 0;
	switch (mns::reread(natives_of(proposed), kCrlf, &first)) {
	case mns::Reread::Same: return true;
	case mns::Reread::Inactive:
		error = "The game would not read that line: it would sit inside a switched-off #if block.";
		return false;
	case mns::Reread::Continued:
		error = "The line above continues onto that place: the game would read the new line as part of its value.";
		return false;
	case mns::Reread::Changed:
		break;
	}
	int line = 1;
	for (size_t i = 0; i < first && i < proposed.size(); ++i) line += int(line_count(style_of(*proposed[i]).native));
	error = "The game would read the lines around line " + std::to_string(line) + " differently.";
	return false;
}

const FindingCodeRow &finding_code(StyleFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable style_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_styles_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *styles = dynamic_cast<const MnsDocument *>(&document);
	if (!styles)
		return findings;
	const std::string &path = styles->path();
	const std::vector<std::shared_ptr<const Node>> &rows = styles->rows();
	// In one pass over the rows (S13 D4: each row had asked winning_row and line_of, each a walk
	// of the rows): each row's first line, and the last definition of each name in the file, the
	// one the game reads.
	std::vector<int> first_lines;
	first_lines.reserve(rows.size());
	std::unordered_map<std::string, NodeId> last_of; // a name, upper case: its last variable row
	int end = 1; // the line after the last row's
	for (const auto &row : rows) {
		first_lines.push_back(end);
		end += int(line_count(style_of(*row).native));
		if (row->kind == kVariable)
			last_of[strutil::to_upper(row->name())] = row->id;
	}
	const auto read_by_the_game = [&](const Node &row) {
		const auto last = last_of.find(strutil::to_upper(mns::variable_name(row.name())));
		return last != last_of.end() && last->second == row.id;
	};
	// A finding on the row a line is in (row_at_line's).
	auto on_line = [&](DiagnosticSeverity severity, StyleFinding code,
						   const std::string &message, int line,
						   const std::string &field = std::string()) {
		Diagnostic d = make_finding(code, severity, message, path, field);
		d.line = size_t(line > 0 ? line : 0);
		const size_t at = size_t(std::upper_bound(first_lines.begin(), first_lines.end(), line) -
				first_lines.begin());
		if (line >= 1 && line < end && at > 0) {
			const Node &row = *rows[at - 1];
			d.row_id = row.id;
			d.record_kind = row.kind;
			d.record = row.name();
		}
		findings.push_back(std::move(d));
	};
	const mns::EvaluationResult evaluated = styles->native().evaluate();
	// What the game does with each odd line.
	for (const mns::Diagnostic &d : evaluated.diagnostics)
		on_line(d.severity == mns::Severity::Error ? DiagnosticSeverity::Error
												   : DiagnosticSeverity::Warning,
				reader_finding(d.code), sentence(d.message), d.line);
	// The rows end CR LF; the file keeps the line ends it was read with until Save
	// writes it (the build packs the file).
	const auto *file = dynamic_cast<const StyleFileState *>(styles->file_state());
	if (file && file->line_end_line && !styles->wrote_file())
		on_line(DiagnosticSeverity::Error, StyleFinding::LineEnding,
				sentence(file->line_end_message) +
						" The editor ends every line CR LF when it saves the file.",
				file->line_end_line);
	// The game reads only the shell's two [orig: Menu_InitShellResources @ 0x552604, @ 0x552616].
	if (!styles->read_by_game()) {
		findings.push_back(make_finding(StyleFinding::NotLoaded, DiagnosticSeverity::Warning,
				"The game reads only menu_style.mns and brand.mns: nothing reads " +
						basename_of(path) + ".",
				path));
		return findings;
	}
	// On the definition the game reads of each name; what the menus make of it (a name brand.mns
	// defines too, one no menu uses, a value used as what it is not) is graph/use_checks'.
	for (size_t i = 0; i < rows.size(); ++i) {
		const Node &row = *rows[i];
		if (row.kind != kVariable || !read_by_the_game(row))
			continue;
		const std::string name = row.name();
		const std::string value = mns::game_value(style_of(row).native);
		const int line = first_lines[i];
		if (value.empty())
			continue; // a name the game ignores (no-value)
		if (value.find_first_of("<>&\"") != std::string::npos)
			on_line(DiagnosticSeverity::Warning, StyleFinding::XmlChar,
					"The game pastes " + name +
							"'s value into each menu before reading the menu, so its < > & or \" "
							"can break the menus that use it.",
					line, "value");
		// The game pastes a value and scans on after it, so a reference in it stays as
		// written [orig: NapiXML_ExpandVariablesInText @ 0x63a000, the copy @ 0x63a450..0x63a4d1].
		if (mns::holds_variable_reference(value))
			on_line(DiagnosticSeverity::Warning, StyleFinding::NestedVar,
					"The game does not expand a %NAME% inside a value: the menus get " + name +
							"'s value as written.",
					line, "value");
		if (value.find("\\\\") != std::string::npos)
			on_line(DiagnosticSeverity::Info, StyleFinding::Backslash,
					"The game keeps both backslashes of each '\\\\' in " + name + "'s value.", line,
					"value");
		// The document's reading against the game's own, where the game reads this far.
		if (evaluated.stopped_line && line >= evaluated.stopped_line)
			continue;
		if (!evaluated.sheet.has(name))
			on_line(DiagnosticSeverity::Warning, StyleFinding::ReadDifferently,
					"The game does not read " + name + " as it stands here.", line, "value");
		else if (evaluated.sheet.get(name) != value)
			on_line(DiagnosticSeverity::Warning, StyleFinding::ReadDifferently,
					"The game reads " + name + " as '" + evaluated.sheet.get(name) +
							"', not as shown.",
					line, "value");
	}
	return findings;
}

} // namespace opennova::editor
