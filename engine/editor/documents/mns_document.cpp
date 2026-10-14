#include "mns_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <runtime/menu/menu_style.h>

#include <algorithm>
#include <cctype>

namespace opennova::editor {
namespace {

constexpr NodeKind kVariable = node_kind(StyleKind::Variable);
// Editor limits: retail's reader allocates each name and value to its length.
constexpr size_t kNameWidth = 128;
constexpr size_t kValueWidth = 256;

const StyleRow &style_of(const Node &node) { return static_cast<const StyleRow &>(node); }
StyleRow &style_of(Node &node) { return static_cast<StyleRow &>(node); }

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
		        field("line", FieldType::Integer, 0, true, "Line"),
		};
		out.front().defines = ReferenceKind::StyleVar; // the %NAME% a menu names the value by
		return out;
	}();
	return fields;
}

mns::KeyValueList list_of(const std::vector<std::shared_ptr<const Node>> &rows) {
	mns::KeyValueList out;
	out.nodes.reserve(rows.size());
	for (const auto &row : rows)
		if (row && row->kind == kVariable) out.nodes.push_back(style_of(*row).native);
	return out;
}

// What the game does on a stylesheet its reader hangs on: an iteration that moves nothing repeats forever [orig:
// NapiConfigMap_ParseKeyValueBuffer @ 0x639870: a value ended by a lone LF or CR with text right after it, the CR /
// LF arm @ 0x639c3b; an unknown directive, @ 0x639a11 not advancing; a value starting with '#', the '#' arm first].
constexpr const char *kHangs =
        "the game stops responding as it reads the stylesheet, its reader repeating a step that moves nothing [orig: "
        "NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b; @ 0x639a11]";

constexpr FindingCodeEntry<StyleFinding> kFindingEntries[] = {
	// A line ending in a lone CR or LF: the game hangs only where a value ends so with text right after it, which the
	// read's own hang says (style.hangs); the line end alone is listed, a save ending every line CR LF.
	{ StyleFinding::LineEnding, { "style.line_ending", FindingFix::Rewrite, "with every line ending CR LF" } },
	// Input a save does not write as the file has it beyond its layout (directives and the lines they switch off, a
	// name defined again past its first spelling and last value, a value continued onto another line): a save writes
	// the list alone. (Comments, blank lines and spacing are layout, which a save drops as every writer does.)
	{ StyleFinding::IgnoredInput, { "style.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	{ StyleFinding::NotLoaded, { "style.not_loaded" } },
	{ StyleFinding::XmlChar, { "style.xml_char" } },
	{ StyleFinding::NestedVar, { "style.nested_var" } },
	{ StyleFinding::Backslash, { "style.backslash" } },
	// The stored file's read: a hang gates (kHangs); a stop, the variables after it never defined and the menus drawn
	// with what the game read, is listed: the game runs (the gate follows retail, ADR 0046 S14).
	{ StyleFinding::Hangs, game_fails_code("style.hangs", kHangs) },
	{ StyleFinding::Stops, listed_code("style.stops") },
	{ StyleFinding::OverriddenByBrand, { "style.overridden_by_brand" } },
	{ StyleFinding::Unused, { "style.unused", FindingFix::UnusedVariable } },
	{ StyleFinding::NotAColor, { "style.not_a_color" } },
	{ StyleFinding::MixedUse, { "style.mixed_use" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(StyleFinding::kCount),
		"every StyleFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the stylesheet's rows follow StyleFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Stylesheets);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

const std::string *text_of(const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) error = "This field takes text.";
	return text;
}

// The stored text's lines with its layout aside, as the writer puts a list's down: a byte-order mark (which the
// reader skips) dropped, each line's comment ("//" on, which the reader reads as nothing) cut, the line trimmed, a
// blank one dropped, its name and the rest apart by one tab, each line ending CR LF.
std::string layout_aside(std::string text) {
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
	std::string out;
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find_first_of("\r\n", at);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(at, end - at);
		const size_t comment = line.find("//");
		if (comment != std::string::npos) line.resize(comment);
		const size_t first = line.find_first_not_of(" \t");
		if (first != std::string::npos) {
			line = line.substr(first, line.find_last_not_of(" \t") - first + 1);
			const size_t gap = line.find_first_of(" \t");
			if (gap != std::string::npos) line = line.substr(0, gap) + "\t" + line.substr(line.find_first_not_of(" \t", gap));
			out += line + "\r\n";
		}
		at = end + (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n' ? 2 : 1);
	}
	return out;
}

// The 1-based line of the first line end of `text` that is not CR LF (a lone CR or LF), 0 for none; lines counted as
// mns::line_at_offset counts them.
int first_odd_line_end(const char *text, size_t size) {
	int line = 1;
	for (size_t i = 0; i < size && text[i] != 0; ++i) {
		if (text[i] == '\r' && i + 1 < size && text[i + 1] == '\n') {
			++i;
			++line;
		} else if (text[i] == '\r' || text[i] == '\n') {
			return line;
		}
	}
	return 0;
}

} // namespace

size_t StyleRow::footprint() const {
	return sizeof(StyleRow) + collections_footprint() + footprint_of(native.name) + footprint_of(native.value);
}

const std::vector<RecordKindRow> &MnsDocument::kinds() const {
	static const std::vector<RecordKindRow> table = {
	        {kVariable, "variable", "Variable", "Add variable", true},
	};
	return table;
}

const std::vector<FieldSchema> &MnsDocument::schema(NodeKind kind) {
	static const std::vector<FieldSchema> none;
	return kind == kVariable ? variable_fields() : none;
}

bool MnsDocument::read_by_game() const { return menu::is_shell_stylesheet(basename_of(path())); }

mns::KeyValueList MnsDocument::list() const { return list_of(rows()); }

NodeId MnsDocument::winning_row(const std::string &name) const {
	const std::string wanted = mns::variable_name(name);
	for (const auto &row : rows())
		if (row->kind == kVariable && strutil::iequals(row->name(), wanted)) return row->id;
	return 0;
}

void MnsDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	if (address.kind != kVariable || use.schema->id != "value" || !read_by_game()) return;
	const Node *row = this->row(address.row);
	if (!row) return;
	// A value naming a file of a kind a style variable stands for (a font, a menu texture) is a reference to that
	// file.
	use.reference = style_value_reference(classify_asset(style_of(*row).native.value, nullptr));
}

void MnsDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	// The list's value of the name is what the game reads [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870].
	const Node *node = row(address.row);
	if (!node || node->kind != kVariable) return;
	facts.line = size_t(line_of(node->id));
	facts.value = style_of(*node).native.value;
}

bool MnsDocument::get(const NodeAddress &address, const std::string &name, Value &out) const {
	const Node *node = row(address.row);
	if (!node || address.child) return false;
	if (name == "line") {
		out = int64_t(line_of(node->id));
		return true;
	}
	return Document::get(address, name, out);
}

bool MnsDocument::read(const Node &node, const NodeAddress &address, const std::string &name, Value &out) const {
	if (address.child || node.kind != kVariable) return false;
	const mns::KeyValue &native = style_of(node).native;
	if (name == "name") out = native.name;
	else if (name == "value") out = native.value;
	else return false;
	return true;
}

SerializeResult MnsDocument::serialize() const {
	SerializeResult result;
	std::string error;
	if (!mns::write(list(), result.text, error)) {
		result.text.clear();
		result.issues.push_back({true, 0, std::string(), std::string(), "The stylesheet could not be written: " + error});
	}
	return result;
}

int MnsDocument::line_of(NodeId id) const {
	int line = 1;
	for (const auto &row : rows()) {
		if (row->id == id) return line;
		++line;
	}
	return 0;
}

NodeId MnsDocument::row_at_line(int line) const {
	if (line < 1 || size_t(line) > rows().size()) return 0;
	return rows()[size_t(line - 1)]->id;
}

bool MnsDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                        std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &,
                        Diagnostic &) {
	auto file = std::make_shared<StyleFileState>();
	mns::KeyValueList read;
	// The loader hands a single NUL for an empty file: an empty stylesheet.
	if (!(bytes.size() == 1 && bytes[0] == 0)) {
		const char *text = reinterpret_cast<const char *>(bytes.data());
		const size_t size = bytes.size();
		// The file as stored, as the game reads it [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870]: where it hangs
		// (a value ended by a lone LF or CR with text right after it, @ 0x639c3b) or stops; the list is what it read.
		const mns::ReadResult result = mns::parse_key_value_buffer(text, size, read);
		if (result.status == mns::ReadStatus::Hangs) file->stored_hang_line = mns::line_at_offset(text, size, result.offset);
		else if (result.status == mns::ReadStatus::Failed)
			file->stored_stop_line = mns::line_at_offset(text, size, result.offset);
		file->line_end_line = first_odd_line_end(text, size);
		// What a save writes of the list against the stored text with its layout aside (the reader reads a NUL as
		// the text's end).
		std::string stored(text, size);
		const size_t nul = stored.find('\0');
		if (nul != std::string::npos) stored.resize(nul);
		std::string written, refused;
		file->dropped = !mns::write(read, written, refused) || written != layout_aside(stored);
	}
	for (mns::KeyValue &native : read.nodes) {
		auto row = std::make_shared<StyleRow>();
		row->kind = kVariable;
		row->native = std::move(native);
		rows.push_back(row);
	}
	state = file;
	return true;
}

std::shared_ptr<Node> MnsDocument::make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
                                             std::string &error) {
	if (kind != kVariable) {
		error = "A stylesheet's rows are its variables.";
		return nullptr;
	}
	auto row = std::make_shared<StyleRow>();
	row->kind = kVariable;
	// A name the list does not hold yet; a value white, until the modder sets one (a variable needs one).
	std::string name = "VAR" + std::to_string(id);
	const auto taken = [&](const std::string &wanted) {
		return std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
			return other && strutil::iequals(other->name(), wanted);
		});
	};
	for (int n = 2; taken(name); ++n) name = "VAR" + std::to_string(id) + "_" + std::to_string(n);
	row->native = mns::KeyValue{name, "FFFFFFFF"};
	return row;
}

bool MnsDocument::set_field(Node &node, const NodeAddress &address, const std::string &name, const Value &value,
                            std::string &error) {
	StyleRow &row = style_of(node);
	if (address.child || node.kind != kVariable) {
		error = "A stylesheet's variables hold no records.";
		return false;
	}
	const std::string *text = text_of(value, error);
	if (!text) return false;
	if (name == "name") {
		if (text->size() >= kNameWidth || !mns::is_valid_name(*text)) {
			error = "A name is one word the reader reads whole: no blank, line end, \\ / % < > or #.";
			return false;
		}
		row.native.name = *text;
		return true;
	}
	if (name == "value") {
		if (text->size() >= kValueWidth || !mns::is_valid_value(*text)) {
			error = "The reader reads that value otherwise: a value is one line with no // comment, every \\ doubled, "
			        "no blank at either end, not starting with #.";
			return false;
		}
		row.native.value = *text;
		return true;
	}
	error = name == "line" ? "This is the variable's place in the file." : "Unknown field.";
	return false;
}

void MnsDocument::prepare_duplicate(Node &copy, const Node &original,
                                    const std::vector<std::shared_ptr<const Node>> &rows) const {
	if (copy.kind != kVariable) return;
	const auto taken = [&](const std::string &wanted) {
		return std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
			return other && strutil::iequals(other->name(), wanted);
		});
	};
	const std::string base = style_of(original).native.name;
	std::string name;
	for (int n = 2; name.empty() || taken(name); ++n) name = base + "_" + std::to_string(n);
	if (name.size() < kNameWidth) style_of(copy).native.name = name;
}

bool MnsDocument::edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &, std::string &error) {
	error = "A stylesheet's variables hold no records.";
	return false;
}

bool MnsDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	// The list the step leaves is one the writer puts down and the reader reads back as it: two names the reader takes
	// for one are refused [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870, the names compared without case].
	std::string text, error;
	if (mns::write(list_of(staged.rows()), text, error)) return true;
	refusal.message = "The game would read that otherwise: " + error;
	return false;
}

const FindingCodeRow &finding_code(StyleFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable style_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

// A style variable no stylesheet the game reads defines (DI-15): a variable named as the menu names it, at the end of
// a stylesheet the game reads (menu_style.mns, brand.mns [orig: Menu_InitShellResources @ 0x552500]): the line Add
// variable makes, its value the colour it writes until one is set.
bool define_style_variable(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *styles = dynamic_cast<const MnsDocument *>(&document);
	if (!styles || missing.kind != ReferenceKind::StyleVar || !styles->read_by_game()) return false;
	const std::string name = mns::variable_name(missing.target);
	if (name.empty() || !mns::is_valid_name(name)) return false;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kVariable;
	add.field = "name";
	add.value = name;
	out = PlannedFix();
	out.edits.push_back(std::move(add));
	out.label = "Add %" + name + "% to " + basename_of(document.path());
	out.detail = "Adds the variable " + name + " at the end of " + basename_of(document.path()) +
	             ", its value FFFFFFFF (white, as Add variable writes one), and selects it to set the value it stands "
	             "for: the game then reads that value wherever a menu names %" + name + "%.";
	return true;
}

std::vector<Diagnostic> validate_styles_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *styles = dynamic_cast<const MnsDocument *>(&document);
	if (!styles) return findings;
	const std::string &path = styles->path();
	const std::vector<std::shared_ptr<const Node>> &rows = styles->rows();
	// A finding on a line of the stored file (no row: the rows are the list a save writes).
	const auto on_stored = [&](DiagnosticSeverity severity, StyleFinding code, const std::string &message, int line) {
		Diagnostic d = make_finding(code, severity, message, path);
		d.line = size_t(line > 0 ? line : 0);
		findings.push_back(std::move(d));
	};
	// A finding on a variable's row.
	const auto on_row = [&](DiagnosticSeverity severity, StyleFinding code, const std::string &message, const Node &row,
	                        int line) {
		Diagnostic d = make_finding(code, severity, message, path, "value");
		d.line = size_t(line);
		d.row_id = row.id;
		d.record_kind = row.kind;
		d.record = row.name();
		findings.push_back(std::move(d));
	};
	// What the stored file holds that a save leaves out, until a save writes it: an error of a stylesheet the game
	// reads where its reader would hang, a warning of one it does not (nothing reads it; the gate follows retail, ADR
	// 0046 S14).
	const DiagnosticSeverity read_error = styles->read_by_game() ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
	const auto *file = dynamic_cast<const StyleFileState *>(styles->file_state());
	if (file && !styles->wrote_file()) {
		if (file->stored_hang_line)
			on_stored(read_error, StyleFinding::Hangs,
			          "The game stops responding reading this line as the file stores it [orig: "
			          "NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]: the list holds what it read before. A save writes "
			          "the list, every line ending CR LF.",
			          file->stored_hang_line);
		if (file->stored_stop_line)
			on_stored(DiagnosticSeverity::Warning, StyleFinding::Stops,
			          "The game stops reading the stylesheet on this line as the file stores it (a define name ended "
			          "where the reader refuses one), and every variable after it stays undefined [orig: "
			          "NapiConfigMap_ParseKeyValueBuffer @ 0x639870]: a save writes only what it read.",
			          file->stored_stop_line);
		if (file->line_end_line)
			on_stored(DiagnosticSeverity::Warning, StyleFinding::LineEnding,
			          "This line ends in a lone LF or CR, which the game reads on but for a value ended so with text right "
			          "after it, where it stops responding. A save ends every line CR LF.",
			          file->line_end_line);
		if (file->dropped)
			on_stored(DiagnosticSeverity::Warning, StyleFinding::IgnoredInput,
			          "The file holds what a save does not write as it stands (directives and the lines they switch off, a "
			          "name defined again, a value continued onto another line): a save writes the variables the game reads, "
			          "one a line, each its first spelling and its last value.",
			          0);
	}
	// The game reads only the shell's two [orig: Menu_InitShellResources @ 0x552604, @ 0x552616].
	if (!styles->read_by_game()) {
		findings.push_back(make_finding(StyleFinding::NotLoaded, DiagnosticSeverity::Warning,
		                                "The game reads only menu_style.mns and brand.mns: nothing reads " + basename_of(path) +
		                                        ".",
		                                path));
		return findings;
	}
	// On each variable; what the menus make of it (a name brand.mns defines too, one no menu uses, a value used as what
	// it is not) is graph/use_checks'.
	for (size_t i = 0; i < rows.size(); ++i) {
		const Node &row = *rows[i];
		if (row.kind != kVariable) continue;
		const std::string name = row.name();
		const std::string &value = style_of(row).native.value;
		const int line = int(i) + 1;
		if (value.find_first_of("<>&\"") != std::string::npos)
			on_row(DiagnosticSeverity::Warning, StyleFinding::XmlChar,
			       "The game pastes " + name +
			               "'s value into each menu before reading the menu, so its < > & or \" can break the menus "
			               "that use it.",
			       row, line);
		// The game pastes a value and scans on after it, so a reference in it stays as written [orig:
		// NapiXML_ExpandVariablesInText @ 0x63a000, the copy @ 0x63a450..0x63a4d1].
		if (mns::holds_variable_reference(value))
			on_row(DiagnosticSeverity::Warning, StyleFinding::NestedVar,
			       "The game does not expand a %NAME% inside a value: the menus get " + name + "'s value as written.", row,
			       line);
		if (value.find("\\\\") != std::string::npos)
			on_row(DiagnosticSeverity::Info, StyleFinding::Backslash,
			       "The game keeps both backslashes of each '\\\\' in " + name + "'s value.", row, line);
	}
	return findings;
}

} // namespace opennova::editor
