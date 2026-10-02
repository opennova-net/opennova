#include <editor/graph/reference_queries.h>

#include <algorithm>
#include <optional>

#include <base/io/strutil.h>
#include <editor/documents/document_types.h>
#include <editor/graph/graph_names.h>

namespace opennova::editor {

namespace {

// Whether the editor opens a project file's kind.
bool editable_file(const AssetScan &scan, const std::string &path) {
	const AssetEntry *entry = scan.at_path(path);
	return entry && is_editable_kind(entry->kind);
}

} // namespace

ReferenceTarget symbol_target(const AssetScan &scan, const GraphSymbol &symbol) {
	ReferenceTarget target;
	target.label = std::string(reference_row(symbol.kind).label) + " " + symbol.display + " in " + symbol.file;
	target.file = symbol.file;
	target.locator = symbol.locator;
	target.field = symbol.field;
	target.editable = editable_file(scan, symbol.file);
	return target;
}

ReferenceTarget file_target(const AssetScan &scan, const std::string &file) {
	ReferenceTarget target;
	target.label = file;
	target.file = file;
	target.editable = editable_file(scan, file);
	return target;
}

ReferenceTarget usage_target(const AssetScan &scan, const GraphEdge &edge) {
	ReferenceTarget target;
	target.label = edge.record.empty() ? edge.source : edge.source + ": " + edge.record;
	target.file = edge.source;
	target.locator = edge.locator;
	target.field = edge.field;
	target.editable = editable_file(scan, edge.source);
	return target;
}

bool find_definition(const AssetGraph &graph, const Document &document, const std::string &symbol, NodeAddress &out,
                     const std::string &scope) {
	// The definitions of the name that the scope matches, each with whether the document's own
	// lookup finds it.
	struct Named {
		NodeAddress address;
		bool inert = false;
	};
	std::vector<Named> named;
	const auto consider = [&](const GraphSymbol &defined, bool inert) {
		// A record of a record set goes by its index in its own file, which no other names.
		if (reference_row(defined.kind).resolution == ReferenceResolution::Record) return;
		// A style variable is found by its NAME or by the %NAME% a menu writes it as.
		const std::string name =
		        reference_row(defined.kind).spell == NameSpelling::StyleVariable ? mns::variable_name(symbol) : symbol;
		if (defined.name == graph_names::symbol_name(defined.kind, name) && scope_matches(defined.scope, scope))
			named.push_back({defined.address, inert});
	};
	if (!graph.for_each_definition(document, consider)) {
		Extracted extracted;
		extract_from_document(document, extracted);
		for (const GraphSymbol &defined : extracted.symbols) consider(defined, defined.inert);
	}
	// The definitions a lookup finds, then (with no scope) those it never reaches; a row's
	// before a nested record's each time.
	for (const bool inert : {false, true}) {
		if (inert && !scope.empty()) return false;
		for (const bool nested : {false, true})
			for (const Named &definition : named)
				if (definition.inert == inert && (definition.address.child != 0) == nested) {
					out = definition.address;
					return true;
				}
	}
	bool found = false;
	for (const auto &row : document.rows()) {
		const NodeAddress top{row->id, row->kind, 0};
		if (strutil::iequals(document.record_name(top), symbol)) {
			out = top;
			return true;
		}
		document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
			found = strutil::iequals(document.record_name(record), symbol);
			if (found) out = record;
			return !found;
		});
		if (found) return true;
	}
	return false;
}

ReferenceStatus reference_status(const AssetGraph &graph, const FieldUse &field, const Value &value,
                                 std::string *symbol) {
	ReferenceKind kind;
	std::string name, scope;
	if (!reference_target(field, value, kind, name, scope)) {
		if (symbol) symbol->clear();
		return ReferenceStatus::NotAReference;
	}
	if (symbol) *symbol = name;
	return graph.resolve(kind, name, scope, nullptr, field.loader_arg);
}

std::vector<ReferenceChoice> reference_choices(const AssetGraph &graph, const FieldUse &field) {
	if (field.reference == ReferenceKind::None) return {};
	std::vector<ReferenceChoice> out = graph.choices(field.reference, field.scope, field.loader_arg);
	const ReferenceKindRow &row = reference_row(field.reference);
	// A record by its index: only an index the field can hold (a byte-sized parameter takes the
	// registers 0 to 255) and one that names a record (a frame byte of 128 names none).
	if (row.resolution == ReferenceResolution::Record) {
		const FieldSchema &schema = *field.schema;
		out.erase(std::remove_if(out.begin(), out.end(),
		                         [&](const ReferenceChoice &choice) {
			                         const std::optional<int> index = strutil::parse_int(choice.name);
			                         int64_t named = 0;
			                         if (!index) return true;
			                         const double at = double(*index);
			                         return (schema.ranged && (at < schema.min || at > schema.max)) ||
			                                !record_index(field.reference, Value(int64_t(*index)), named);
		                         }),
		          out.end());
		return out;
	}
	// What the value may name instead: a stylesheet variable for a menu's font or texture (the
	// %NAME% stays in the menu and the stylesheet's value is the file, ADR 0005), a string id
	// for a text key the editor does not resolve yet; each as this field would reference it, a
	// kind the editor cannot check keeping the offered kind's own answer.
	if (row.also_offers == ReferenceKind::None) return out;
	const bool checked = row.resolution != ReferenceResolution::Unchecked;
	for (ReferenceChoice &choice : graph.choices(row.also_offers)) {
		if (checked) choice.status = graph.resolve(field.reference, choice.name, field.scope, nullptr, field.loader_arg);
		out.push_back(std::move(choice));
	}
	return out;
}

bool missing_finding(const AssetGraph &graph, const Document &document, const NodeAddress &address,
                     const FieldUse &field, const Value &value, Diagnostic &out) {
	ReferenceKind kind;
	std::string name, scope;
	if (!reference_target(field, value, kind, name, scope)) return false;
	// A kind the graph finds none of missing (a Record reference) makes none here either.
	if (!reference_row(kind).missing_message) return false;
	if (graph.resolve(kind, name, scope, nullptr, field.loader_arg) != ReferenceStatus::Missing) return false;
	GraphEdge edge;
	edge.source = document.path();
	edge.record = document.record_path(address);
	edge.locator = document.locator(address);
	edge.address = address;
	edge.field = field.schema->id;
	edge.kind = kind;
	edge.value = name;
	edge.scope = scope;
	edge.loader_arg = field.loader_arg;
	if (field.reference == ReferenceKind::None) edge.through = field.variable_through; // a text's %NAME%
	if (kind != ReferenceKind::StyleVar && graph_names::is_style_reference(name) && !graph.style_binding(name)) {
		edge.kind = ReferenceKind::StyleVar;
		edge.scope.clear();
		edge.loader_arg = -1;
		edge.through = kind;
	}
	out = graph.missing_finding(edge);
	return true;
}

std::string reference_target_file(const AssetGraph &graph, const FieldUse &field, const Value &value) {
	ReferenceKind kind;
	std::string name, scope, file;
	if (!reference_target(field, value, kind, name, scope)) return std::string();
	if (graph.resolve(kind, name, scope, &file, field.loader_arg) != ReferenceStatus::Present) return std::string();
	return file;
}

std::vector<ReferenceTarget> reference_targets(const AssetGraph &graph, const AssetScan &scan, const FieldUse &field,
                                               const Value &value) {
	std::vector<ReferenceTarget> out;
	ReferenceKind kind;
	std::string name, scope;
	if (!reference_target(field, value, kind, name, scope)) return out;
	const ReferenceKindRow &row = reference_row(kind);
	if (row.names_symbol()) {
		if (const GraphSymbol *symbol = graph.resolve_symbol(kind, name, scope)) out.push_back(symbol_target(scan, *symbol));
		return out;
	}
	if (row.resolution != ReferenceResolution::File) return out;
	// A menu's font or texture through a style variable: the variable where the game reads it
	// (ADR 0005), then the file its value names.
	if (graph_names::is_style_reference(name))
		if (const GraphSymbol *binding = graph.style_binding(name)) out.push_back(symbol_target(scan, *binding));
	std::string file;
	if (graph.resolve(kind, name, scope, &file, field.loader_arg) == ReferenceStatus::Present)
		out.push_back(file_target(scan, file));
	return out;
}

} // namespace opennova::editor
