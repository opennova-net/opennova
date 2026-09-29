#include "mns_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/reference_kinds.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>
#include <runtime/menu/menu_style.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

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

// "lone-backslash" -> "style.lone_backslash".
std::string style_code(const std::string &code) {
	std::string out = "style.";
	for (char c : code) out += c == '-' ? '_' : c;
	return out;
}

std::string basename_of(const std::string &path) { return std::filesystem::path(path).filename().generic_string(); }

} // namespace

std::string StyleRow::name() const {
	switch (native.kind) {
	case mns::NodeKind::Define: return native.define_lines.front().name;
	case mns::NodeKind::Blank: return std::string();
	default: return native.text;
	}
}

bool is_style_kind(AssetKind kind) { return kind == AssetKind::MenuStyle; }

const char *MnsDocument::kind_label(NodeKind kind) const {
	switch (kind) {
	case kVariable: return "Variable";
	case kComment: return "Comment";
	case kBlank: return "Blank line";
	case kConditional: return "Directive";
	case kInactive: return "Switched-off line";
	default: return "";
	}
}

NodeKind MnsDocument::kind_from_name(const std::string &name) const {
	if (name == "variable") return kVariable;
	if (name == "comment") return kComment;
	if (name == "blank") return kBlank;
	if (name == "directive") return kConditional;
	if (name == "inactive") return kInactive;
	return -1;
}

bool MnsDocument::is_top_kind(NodeKind kind) const { return kind >= kVariable && kind <= kInactive; }

std::vector<Document::KindSpec> MnsDocument::top_kinds() const {
	return {{kVariable, "Add variable"}, {kComment, "Add comment"}, {kBlank, "Add blank line"}};
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

FieldSchema MnsDocument::field_on(const NodeAddress &address, const FieldSchema &schema) const {
	if (address.kind != kVariable || schema.id != "value") return schema;
	const Node *row = this->row(address.row);
	if (!row) return schema;
	FieldSchema out = schema;
	if (winning_row(row->name()) != row->id) {
		out.applies = Applicability::Ignored; // a later definition is the one the game reads
		return out;
	}
	if (!read_by_game()) return out;
	// A value naming a file of a kind a style variable stands for (a font, a menu texture) is a
	// reference to that file.
	out.reference = style_value_reference(classify_asset(mns::game_value(style_of(*row).native), nullptr));
	return out;
}

void MnsDocument::refine_symbol(const NodeAddress &address, GraphSymbol &symbol) const {
	// The definition the game reads of a name is the last, carrying the value the game reads
	// [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870]; an earlier one, and one on a line
	// past the place the game stops reading the file, are defined but never read.
	const Node *node = row(address.row);
	if (!node) return;
	Value value;
	if (get(address, "value", value)) symbol.value = std::get<std::string>(value);
	const mns::StyleSheet &sheet = game_sheet();
	if (winning_row(node->name()) != node->id) {
		symbol.inert = true;
		symbol.inert_reason = "the file defines it again below, and the game reads the last";
		return;
	}
	if (!sheet.has(node->name())) {
		symbol.inert = true;
		symbol.inert_reason = "it comes after the place the game stops reading the file";
		return;
	}
	symbol.value = sheet.get(node->name());
}

const StyleValueUse &MnsDocument::style_value_use(const NodeAddress &line, const AssetGraph *graph,
                                                  uint64_t graph_key) const {
	if (!value_uses_.made || value_uses_.revision != revision() || value_uses_.graph != graph ||
	    value_uses_.graph_key != graph_key) {
		value_uses_.made = true;
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
	bool colour = false, font = false, image = false;
	if (out.winner && read_by_game() && graph) {
		const GraphSymbol *binding = graph->style_binding(row->name());
		out.bound = binding && binding->file == path();
	}
	if (out.bound)
		for (const GraphEdge *edge : graph->referrers_of(ReferenceKind::StyleVar, row->name())) {
			if (edge->through == ReferenceKind::None) colour = true;
			else if (edge->through == ReferenceKind::Font) font = true;
			else image = true;
		}
	const NodeAddress address{row->id, row->kind, 0};
	FieldSchema value;
	for (const FieldSchema &schema : fields(kVariable))
		if (schema.id == "value") value = field_on(address, schema);
	Value text;
	const std::string shown = get(address, "value", text) ? std::get<std::string>(text) : "";
	const bool fixed = frozen(*row);
	out.colour = !fixed && (colour || (!font && !image && mnu::color_reads_whole(shown)));
	out.file = value;
	if (font || value.reference == ReferenceKind::Font) out.file.reference = ReferenceKind::Font;
	else if (image || value.reference == ReferenceKind::MenuTexture)
		out.file.reference = ReferenceKind::MenuTexture;
	else out.file.reference = ReferenceKind::None;
	out.picks = !fixed && !out.colour && graph && out.file.reference != ReferenceKind::None;
	return out;
}

const mns::StyleSheet &MnsDocument::game_sheet() const {
	if (!game_sheet_.made || game_sheet_.revision != revision()) {
		game_sheet_.made = true;
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
		error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This file is not a menu stylesheet.", path());
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

bool MnsDocument::set_file_value(std::shared_ptr<const FileState> &, const Edit &, Diagnostic &error) {
	error = make_diagnostic(DiagnosticSeverity::Error, "document.value", "A stylesheet has no file-wide values.", path());
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

std::vector<Diagnostic> validate_styles(const ValidationInput &input, const AssetGraph &graph) {
	std::vector<Diagnostic> findings;
	for (const AssetEntry &asset : input.scan.entries) {
		if (!is_style_kind(asset.kind)) continue;
		Diagnostic error;
		const std::shared_ptr<const Document> document = input.document(asset, error);
		if (!document) {
			findings.push_back(error);
			continue;
		}
		const auto *styles = dynamic_cast<const MnsDocument *>(document.get());
		if (!styles) continue;
		const std::string &path = styles->path();
		// A finding on the row a line is in.
		auto on_line = [&](DiagnosticSeverity severity, const std::string &code, const std::string &message, int line,
		                   const std::string &field = std::string()) {
			Diagnostic d = make_diagnostic(severity, code, message, path, field);
			d.line = size_t(line > 0 ? line : 0);
			if (const Node *row = styles->row(styles->row_at_line(line))) {
				d.row_id = row->id;
				d.record_kind = row->kind;
				d.record = row->name();
			}
			findings.push_back(std::move(d));
		};
		const mns::EvaluationResult evaluated = styles->native().evaluate();
		// What the game does with each odd line.
		for (const mns::Diagnostic &d : evaluated.diagnostics)
			on_line(d.severity == mns::Severity::Error ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
			        style_code(d.code), sentence(d.message), d.line);
		// The rows end CR LF; the file keeps the line ends it was read with until Save
		// writes it (the build packs the file).
		const auto *file = dynamic_cast<const StyleFileState *>(styles->file_state());
		if (file && file->line_end_line && !styles->wrote_file())
			on_line(DiagnosticSeverity::Error, "style.line_ending",
			        sentence(file->line_end_message) + " The editor ends every line CR LF when it saves the file.",
			        file->line_end_line);
		// The game reads only the shell's two [orig: Menu_InitShellResources @ 0x552604, @ 0x552616].
		if (!styles->read_by_game()) {
			findings.push_back(make_diagnostic(DiagnosticSeverity::Warning, "style.not_loaded",
			                                   "The game reads only menu_style.mns and brand.mns: nothing reads " +
			                                           basename_of(path) + ".",
			                                   path));
			continue;
		}
		for (const auto &row : styles->rows()) {
			if (row->kind != kVariable || styles->winning_row(row->name()) != row->id) continue;
			const std::string name = row->name();
			const std::string value = mns::game_value(style_of(*row).native);
			const int line = styles->line_of(row->id);
			if (value.empty()) continue; // a name the game ignores (no-value)
			const GraphSymbol *binding = graph.style_binding(name);
			const bool is_binding = binding && binding->file == path;
			if (binding && !is_binding && menu::is_shell_stylesheet(basename_of(binding->file)) &&
			    strutil::iequals(basename_of(path), menu::kShellStylesheets[0].name))
				on_line(DiagnosticSeverity::Info, "style.overridden_by_brand",
				        basename_of(binding->file) + " defines " + name + " too: the game reads its value, '" +
				                binding->value + "'.",
				        line, "value");
			if (is_binding) {
				// The uses of the variable, by what its value must be there.
				bool color = false, font = false, image = false;
				const std::vector<const GraphEdge *> uses = graph.referrers_of(ReferenceKind::StyleVar, name);
				for (const GraphEdge *edge : uses) {
					if (edge->through == ReferenceKind::None) color = true;
					else if (edge->through == ReferenceKind::Font) font = true;
					else image = true;
				}
				// Every colour, font and image a menu names through a variable is an edge
				// (every APPEARANCE, ITEM and FONT field), so none means no menu uses it.
				if (uses.empty())
					on_line(DiagnosticSeverity::Info, "style.unused",
					        "No menu of the project names %" + name + "%.", line, "value");
				if (color && !mnu::color_reads_whole(value))
					on_line(DiagnosticSeverity::Warning, "style.not_a_color",
					        name + " is used as a colour, but '" + value +
					                "' is not one (AARRGGBB hex digits): the game reads only its leading hex digits.",
					        line, "value");
				if (int(color) + int(font) + int(image) > 1)
					on_line(DiagnosticSeverity::Warning, "style.mixed_use",
					        name + " is used as more than one of a colour, a font and an image.", line, "value");
			}
			if (value.find_first_of("<>&\"") != std::string::npos)
				on_line(DiagnosticSeverity::Warning, "style.xml_char",
				        "The game pastes " + name + "'s value into each menu before reading the menu, so its < > & or \" "
				        "can break the menus that use it.",
				        line, "value");
			// The game pastes a value and scans on after it, so a reference in it stays as
			// written [orig: NapiXML_ExpandVariablesInText @ 0x63a000, the copy @ 0x63a450..0x63a4d1].
			if (mns::holds_variable_reference(value))
				on_line(DiagnosticSeverity::Warning, "style.nested_var",
				        "The game does not expand a %NAME% inside a value: the menus get " + name + "'s value as written.",
				        line, "value");
			if (value.find("\\\\") != std::string::npos)
				on_line(DiagnosticSeverity::Info, "style.backslash",
				        "The game keeps both backslashes of each '\\\\' in " + name + "'s value.", line, "value");
			// The document's reading against the game's own, where the game reads this far.
			if (evaluated.stopped_line && line >= evaluated.stopped_line) continue;
			if (!evaluated.sheet.has(name))
				on_line(DiagnosticSeverity::Warning, "style.read_differently",
				        "The game does not read " + name + " as it stands here.", line, "value");
			else if (evaluated.sheet.get(name) != value)
				on_line(DiagnosticSeverity::Warning, "style.read_differently",
				        "The game reads " + name + " as '" + evaluated.sheet.get(name) + "', not as shown.", line, "value");
		}
	}
	return findings;
}

} // namespace opennova::editor
