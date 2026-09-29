// A document's reference queries, answered by the project's asset graph (ADR 0046
// d10, S7): the inspector's badge, the picker's names and where "Go to" goes read the
// same tables the Problems rows come from; and a record found by the symbol it defines,
// read from the same extraction.
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_names.h>
#include <editor/model/document.h>
#include <editor/session/session_view.h>

#include <base/io/strutil.h>

#include <memory>

namespace opennova::editor {

namespace {

// Whether the editor opens a project file's kind.
bool editable_file(const std::string &path, const SessionView &view) {
	const AssetEntry *entry = view.scan.at_path(path);
	return entry && is_editable_kind(entry->kind);
}

} // namespace

ReferenceTarget symbol_target(const GraphSymbol &symbol, const SessionView &view) {
	ReferenceTarget target;
	target.label = std::string(reference_row(symbol.kind).label) + " " + symbol.display + " in " + symbol.file;
	target.file = symbol.file;
	target.locator = symbol.locator;
	target.field = symbol.field;
	target.editable = editable_file(symbol.file, view);
	return target;
}

ReferenceTarget file_target(const std::string &file, const SessionView &view) {
	ReferenceTarget target;
	target.label = file;
	target.file = file;
	target.editable = editable_file(file, view);
	return target;
}

ReferenceTarget usage_target(const GraphEdge &edge, const SessionView &view) {
	ReferenceTarget target;
	target.label = edge.record.empty() ? edge.source : edge.source + ": " + edge.record;
	target.file = edge.source;
	target.locator = edge.locator;
	target.field = edge.field;
	target.editable = editable_file(edge.source, view);
	return target;
}

bool Document::find(const std::string &symbol, NodeAddress &out, const std::string &scope) const {
	if (!defined_ || defined_revision_ != revision()) {
		Extracted extracted;
		extract_from_document(*this, extracted);
		defined_ = std::make_shared<const std::vector<GraphSymbol>>(std::move(extracted.symbols));
		defined_revision_ = revision();
	}
	const auto named = [&](const GraphSymbol &defined) {
		// A style variable is found by its NAME or by the %NAME% a menu writes it as.
		const std::string name =
		        reference_row(defined.kind).spell == NameSpelling::StyleVariable ? mns::variable_name(symbol) : symbol;
		return defined.name == graph_names::symbol_name(defined.kind, name) && scope_matches(defined.scope, scope);
	};
	// The definitions a lookup finds, then (with no scope) those it never reaches; a row's
	// before a nested record's each time.
	for (const bool inert : {false, true}) {
		if (inert && !scope.empty()) return false;
		for (const bool nested : {false, true})
			for (const GraphSymbol &defined : *defined_)
				if (defined.inert == inert && (defined.address.child != 0) == nested && named(defined)) {
					out = defined.address;
					return true;
				}
	}
	bool found = false;
	for (const auto &row : rows()) {
		const NodeAddress top{row->id, row->kind, 0};
		if (strutil::iequals(record_name(top), symbol)) {
			out = top;
			return true;
		}
		walk_records(*row, [&](const NodeAddress &record, const Placement &) {
			found = strutil::iequals(record_name(record), symbol);
			if (found) out = record;
			return !found;
		});
		if (found) return true;
	}
	return false;
}

ReferenceStatus Document::reference_status(const FieldSchema &field, const Value &value, const SessionView &view,
                                           std::string *symbol) const {
	ReferenceKind kind;
	std::string name, scope;
	if (!reference_target(field, value, kind, name, scope)) {
		if (symbol) symbol->clear();
		return ReferenceStatus::NotAReference;
	}
	if (symbol) *symbol = name;
	if (!view.graph) return ReferenceStatus::Unverified;
	return view.graph->resolve(kind, name, scope, nullptr, field.material_type);
}

std::vector<ReferenceChoice> Document::reference_choices(const FieldSchema &field, const SessionView &view) const {
	if (!view.graph || field.reference == ReferenceKind::None) return {};
	const AssetGraph &graph = *view.graph;
	std::vector<ReferenceChoice> out = graph.choices(field.reference, field.scope, field.material_type);
	// What the value may name instead: a stylesheet variable for a menu's font or texture (the
	// %NAME% stays in the menu and the stylesheet's value is the file, ADR 0005), a string id
	// for a text key the editor does not resolve yet; each as this field would reference it, a
	// kind the editor cannot check keeping the offered kind's own answer.
	const ReferenceKindRow &row = reference_row(field.reference);
	if (row.also_offers == ReferenceKind::None) return out;
	const bool checked = row.resolution != ReferenceResolution::Unchecked;
	for (ReferenceChoice &choice : graph.choices(row.also_offers)) {
		if (checked) choice.status = graph.resolve(field.reference, choice.name, field.scope, nullptr, field.material_type);
		out.push_back(std::move(choice));
	}
	return out;
}

bool Document::missing_finding(const NodeAddress &address, const FieldSchema &field, const Value &value,
                               const SessionView &view, Diagnostic &out) const {
	ReferenceKind kind;
	std::string name, scope;
	if (!view.graph || !reference_target(field, value, kind, name, scope)) return false;
	const AssetGraph &graph = *view.graph;
	if (graph.resolve(kind, name, scope, nullptr, field.material_type) != ReferenceStatus::Missing) return false;
	GraphEdge edge;
	edge.source = path();
	edge.record = record_path(address);
	edge.locator = locator(address);
	edge.address = address;
	edge.field = field.id;
	edge.kind = kind;
	edge.value = name;
	edge.scope = scope;
	edge.material_type = field.material_type;
	if (kind != ReferenceKind::StyleVar && graph_names::is_style_reference(name) && !graph.style_binding(name)) {
		edge.kind = ReferenceKind::StyleVar;
		edge.scope.clear();
		edge.material_type = -1;
		edge.through = kind;
	}
	out = graph.missing_finding(edge);
	return true;
}

std::string Document::reference_target_file(const FieldSchema &field, const Value &value, const SessionView &view) const {
	ReferenceKind kind;
	std::string name, scope, file;
	if (!view.graph || !reference_target(field, value, kind, name, scope)) return std::string();
	if (view.graph->resolve(kind, name, scope, &file, field.material_type) != ReferenceStatus::Present) return std::string();
	return file;
}

std::vector<ReferenceTarget> Document::reference_targets(const FieldSchema &field, const Value &value,
                                                         const SessionView &view) const {
	std::vector<ReferenceTarget> out;
	ReferenceKind kind;
	std::string name, scope;
	if (!view.graph || !reference_target(field, value, kind, name, scope)) return out;
	const AssetGraph &graph = *view.graph;
	const ReferenceKindRow &row = reference_row(kind);
	if (row.names_symbol()) {
		if (const GraphSymbol *symbol = graph.resolve_symbol(kind, name, scope)) out.push_back(symbol_target(*symbol, view));
		return out;
	}
	if (row.resolution != ReferenceResolution::File) return out;
	// A menu's font or texture through a style variable: the variable where the game reads it
	// (ADR 0005), then the file its value names.
	if (graph_names::is_style_reference(name))
		if (const GraphSymbol *binding = graph.style_binding(name)) out.push_back(symbol_target(*binding, view));
	std::string file;
	if (graph.resolve(kind, name, scope, &file, field.material_type) == ReferenceStatus::Present)
		out.push_back(file_target(file, view));
	return out;
}

} // namespace opennova::editor
