#include <editor/graph/use_checks.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_validation.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/def/def.h>
#include <formats/mission/authoring.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>
#include <runtime/menu/menu_style.h>

namespace opennova::editor {

namespace {

// A finding on the record a symbol's definition is in, on `field` (and on the definition's line
// where the finding names one: a stylesheet's).
Diagnostic on_definition(const GraphSymbol &symbol, DiagnosticSeverity severity, const FindingCodeRow &code,
		const std::string &message, const char *field, size_t line) {
	Diagnostic d = make_finding(code, severity, message, symbol.file, field);
	d.line = line;
	d.record = symbol.record;
	d.record_key = symbol.record_key;
	d.row_id = symbol.address.row;
	d.child_id = symbol.address.child;
	d.record_kind = symbol.address.kind;
	return d;
}

// What the menus make of the variables of the stylesheets the game reads (ADR 0046 S9i), on the
// definition the game reads of each name in its file (the last one: the graph keeps a
// stylesheet's variables in its line order) and its value as the game reads it: a name brand.mns
// defines too; and on the one the game reads of all the shell's stylesheets (the graph's binding),
// through the menus' uses of it, a name no menu uses, a value used as a colour that is not one,
// and a value used as more than one of a colour, a font and an image.
void check_style_uses(
		const AssetGraph &graph, const ValidationCache &files, std::vector<Diagnostic> &out) {
	const std::vector<const GraphSymbol *> symbols = graph.symbols_of_kind(ReferenceKind::StyleVar);
	for (size_t begin = 0, end = 0; begin < symbols.size(); begin = end) {
		const std::string &path = symbols[begin]->file;
		for (end = begin; end < symbols.size() && symbols[end]->file == path; ++end) {
		}
		if (!menu::is_shell_stylesheet(basename_of(path)) || !files.records_checked(path))
			continue;
		// The last definition of each name in the file: the one the game reads of it.
		std::unordered_map<std::string, const GraphSymbol *> last;
		for (size_t i = begin; i < end; ++i)
			last[symbols[i]->name] = symbols[i];
		for (size_t i = begin; i < end; ++i) {
			const GraphSymbol &symbol = *symbols[i];
			// An empty value: a name the game ignores. A name written as a %NAME% itself is no
			// definition the file reads of that name (MnsDocument::winning_row looks the name up
			// by its variable_name).
			if (last[symbol.name] != &symbol || symbol.value.empty() ||
					mns::is_variable_reference(symbol.display))
				continue;
			const std::string &name = symbol.display;
			const std::string &value = symbol.value;
			const auto add = [&](DiagnosticSeverity severity, StyleFinding code,
									 const std::string &message) {
				out.push_back(on_definition(symbol, severity, finding_code(code), message, "value", symbol.line));
			};
			const GraphSymbol *binding = graph.style_binding(name);
			const bool is_binding = binding && binding->file == path;
			if (binding && !is_binding && menu::is_shell_stylesheet(basename_of(binding->file)) &&
					strutil::iequals(basename_of(path), menu::kShellStylesheets[0].name))
				add(DiagnosticSeverity::Info, StyleFinding::OverriddenByBrand,
						basename_of(binding->file) + " defines " + name +
								" too: the game reads its value, '" + binding->value + "'.");
			if (!is_binding)
				continue;
			// The uses of the variable, by what its value must be there (a string id, a screen's
			// or a window's NAME and any other text are none of a colour, a font and an image).
			bool color = false, font = false, image = false;
			const std::vector<const GraphEdge *> uses =
					graph.referrers_of(ReferenceKind::StyleVar, name);
			for (const GraphEdge *edge : uses) {
				const StyleVariableUse use = style_variable_use(edge->through);
				color = color || use == StyleVariableUse::Colour;
				font = font || use == StyleVariableUse::Font;
				image = image || use == StyleVariableUse::Image;
			}
			// Every field value of a menu that is one whole %NAME% is an edge: a colour, a font, an
			// image, a string id, a screen's or a window's name, and any other text the game reads
			// (a NAME, a shown text, an ACTION's target: FieldUse::variable_through), so none
			// means no menu uses it. A %NAME% inside a longer text is no edge: the frame compiler
			// reads a whole value alone.
			if (uses.empty())
				add(DiagnosticSeverity::Info, StyleFinding::Unused,
						"No menu of the project names %" + name + "%.");
			if (color && !mnu::color_reads_whole(value))
				add(DiagnosticSeverity::Warning, StyleFinding::NotAColor,
						name + " is used as a colour, but '" + value +
								"' is not one (AARRGGBB hex digits): the game reads only its "
								"leading hex digits.");
			if (int(color) + int(font) + int(image) > 1)
				add(DiagnosticSeverity::Warning, StyleFinding::MixedUse,
						name + " is used as more than one of a colour, a font and an image.");
		}
	}
}

// The pool an item's TYPE puts a record in, as the game's editor places it (formats/mission/
// authoring.h: a vehicle, an object and a powerup among the items, a decoration, foliage and a
// building among the buildings, a person among the organics, a marker among the markers).
NodeKind pool_of_type(int type) {
	switch (mission::authoring::entity_kind_for_item_type(type)) {
	case mission::EntityKind::Building: return node_kind(MissionKind::Building);
	case mission::EntityKind::Marker: return node_kind(MissionKind::Marker);
	case mission::EntityKind::Organic: return node_kind(MissionKind::Organic);
	default: return node_kind(MissionKind::Item);
	}
}

const char *pool_words(NodeKind kind) {
	switch (static_cast<MissionKind>(kind)) {
	case MissionKind::Building: return "buildings";
	case MissionKind::Marker: return "markers";
	case MissionKind::Organic: return "organics";
	default: return "items";
	}
}

// What a mission's records make of their items' TYPE (ADR 0046 S14): an entity in another pool than
// the one the game's editor places its item's TYPE in (one to one over the shipped missions,
// D-MIS-1; what the game makes of such a record is not known), read through the graph: each
// entity's item edge and the item symbol it reaches, whose value is the item's TYPE
// (DefCatalogDocument::refine_symbol). An item the project lacks is the reference's own finding.
void pool_findings(const AssetGraph &graph, const std::string &path, std::vector<Diagnostic> &out) {
	for (const GraphEdge *edge : graph.references_of(path)) {
		if (edge->kind != ReferenceKind::Item || edge->field != "item" || !is_entity_kind(edge->address.kind)) continue;
		const GraphSymbol *item = graph.symbol_reached(*edge);
		const std::optional<int> type = item ? strutil::parse_int(item->value) : std::nullopt;
		if (!type || *type < 0) continue;
		const NodeKind placed = pool_of_type(*type);
		if (placed == edge->address.kind) continue;
		Diagnostic d = make_finding(finding_code(MissionFinding::Pool), DiagnosticSeverity::Warning,
		                            "Item " + edge->value + " is a " + def::def_item_type_name(*type) +
		                                    ", which the game's editor places among the " + pool_words(placed) +
		                                    "; this record is among the " + pool_words(edge->address.kind) +
		                                    ". What the game makes of it is not known.",
		                            path, edge->field);
		d.record = edge->record;
		d.record_key = edge->record_key;
		d.row_id = edge->address.row;
		d.child_id = edge->address.child;
		d.record_kind = edge->address.kind;
		out.push_back(std::move(d));
	}
}

// Every mission's pool_findings, made once per state of the graph (AssetGraph::generation: while it
// stands, every edge and symbol is where it was), not at every composition of the rows (the editor
// composes them after every edit that validates a file); a composition takes those of each mission
// whose records its validation checked.
void check_mission_pools(const AssetGraph &graph, const ValidationCache &files, std::vector<Diagnostic> &out) {
	struct Made {
		std::mutex mutex;
		uint64_t generation = 0; // no graph's (the counter starts at 1)
		std::vector<std::pair<std::string, std::vector<Diagnostic>>> missions; // each mission's path, its findings
	};
	static Made made;
	const std::lock_guard<std::mutex> lock(made.mutex);
	if (made.generation != graph.generation()) {
		made.missions.clear();
		std::string last;
		for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::MissionEntity)) {
			const std::string &path = symbol->file;
			if (path == last) continue;
			last = path;
			made.missions.emplace_back(path, std::vector<Diagnostic>());
			pool_findings(graph, path, made.missions.back().second);
		}
		made.generation = graph.generation();
	}
	for (const auto &[path, findings] : made.missions)
		if (files.records_checked(path)) out.insert(out.end(), findings.begin(), findings.end());
}

// The cross-file checks, one row per asset kind, in AssetKind's order.
constexpr UseCheckRow kUseChecks[] = {
	{ AssetKind::Mission, check_mission_pools },
	{ AssetKind::MenuStyle, check_style_uses },
};

constexpr bool rows_in_kind_order() {
	for (size_t i = 0; i < sizeof(kUseChecks) / sizeof(kUseChecks[0]); ++i) {
		if (kUseChecks[i].kind == AssetKind::Unknown || kUseChecks[i].kind >= AssetKind::kCount ||
				!kUseChecks[i].check)
			return false;
		if (i > 0 && !(kUseChecks[i - 1].kind < kUseChecks[i].kind))
			return false;
	}
	return true;
}
static_assert(rows_in_kind_order(),
		"one use check per asset kind, in AssetKind's order, each with its check");

} // namespace

const UseCheckRow *use_check(AssetKind kind) {
	for (const UseCheckRow &row : kUseChecks)
		if (row.kind == kind)
			return &row;
	return nullptr;
}

void run_use_checks(
		const AssetGraph &graph, const ValidationCache &files, std::vector<Diagnostic> &out) {
	for (const UseCheckRow &row : kUseChecks)
		row.check(graph, files, out);
}

} // namespace opennova::editor
