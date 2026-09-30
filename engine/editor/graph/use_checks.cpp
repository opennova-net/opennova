#include <editor/graph/use_checks.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <base/io/strutil.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <formats/mnu/mnu_layout.h>
#include <runtime/menu/menu_style.h>

namespace opennova::editor {

namespace {

// A finding on the record a symbol's definition is in, on `field` (and on the definition's line
// where the finding names one: a stylesheet's).
Diagnostic on_definition(const GraphSymbol &symbol, DiagnosticSeverity severity, const char *code,
		const std::string &message, const char *field, size_t line) {
	Diagnostic d = make_diagnostic(severity, code, message, symbol.file, field);
	d.line = line;
	d.record = symbol.record;
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
			// An empty value: a name the game ignores.
			if (last[symbol.name] != &symbol || symbol.value.empty())
				continue;
			const std::string &name = symbol.display;
			const std::string &value = symbol.value;
			const auto add = [&](DiagnosticSeverity severity, const char *code,
									 const std::string &message) {
				out.push_back(on_definition(symbol, severity, code, message, "value", symbol.line));
			};
			const GraphSymbol *binding = graph.style_binding(name);
			const bool is_binding = binding && binding->file == path;
			if (binding && !is_binding && menu::is_shell_stylesheet(basename_of(binding->file)) &&
					strutil::iequals(basename_of(path), menu::kShellStylesheets[0].name))
				add(DiagnosticSeverity::Info, "style.overridden_by_brand",
						basename_of(binding->file) + " defines " + name +
								" too: the game reads its value, '" + binding->value + "'.");
			if (!is_binding)
				continue;
			// The uses of the variable, by what its value must be there.
			bool color = false, font = false, image = false;
			const std::vector<const GraphEdge *> uses =
					graph.referrers_of(ReferenceKind::StyleVar, name);
			for (const GraphEdge *edge : uses) {
				if (edge->through == ReferenceKind::None)
					color = true;
				else if (edge->through == ReferenceKind::Font)
					font = true;
				else
					image = true;
			}
			// Every colour, font and image a menu names through a variable is an edge (every
			// APPEARANCE, ITEM and FONT field), so none means no menu uses it.
			if (uses.empty())
				add(DiagnosticSeverity::Info, "style.unused",
						"No menu of the project names %" + name + "%.");
			if (color && !mnu::color_reads_whole(value))
				add(DiagnosticSeverity::Warning, "style.not_a_color",
						name + " is used as a colour, but '" + value +
								"' is not one (AARRGGBB hex digits): the game reads only its "
								"leading hex digits.");
			if (int(color) + int(font) + int(image) > 1)
				add(DiagnosticSeverity::Warning, "style.mixed_use",
						name + " is used as more than one of a colour, a font and an image.");
		}
	}
}

// An item whose id an item of a table the scan lists earlier has, on the first item of the id in
// its own table (a later one there is that table's own finding, validate_catalog_file): both are
// kept (the load logs "Duplicate ID number" @0x4a1e96 and goes on) [orig: ItemDefs_LoadAndValidate
// @ 0x4a1da0], and the lookup by id returns the first [orig: ItemList_FindIndexByTypeId @
// 0x49e100]. The ids are the graph's item symbols, in the files' order (an id of 0 names no item
// and defines none).
void check_item_ids(
		const AssetGraph &graph, const ValidationCache &files, std::vector<Diagnostic> &out) {
	std::unordered_map<std::string, const GraphSymbol *> first; // by id: its first item
	std::unordered_set<std::string> in_file; // the ids the file walked has
	const std::string *file = nullptr;
	for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::Item)) {
		if (!files.records_checked(symbol->file))
			continue;
		if (!file || *file != symbol->file) {
			file = &symbol->file;
			in_file.clear();
		}
		const bool first_in_file = in_file.insert(symbol->name).second;
		const auto earlier = first.emplace(symbol->name, symbol);
		if (earlier.second || !first_in_file)
			continue;
		out.push_back(on_definition(*symbol, DiagnosticSeverity::Warning, "catalog.item_identity",
				"An earlier item, \"" + earlier.first->second->record + "\", has id " +
						symbol->display +
						": the game keeps both, and a lookup by the id finds the earlier one.",
				"id", 0));
	}
}

// The cross-file checks, one row per asset kind, in AssetKind's order.
constexpr UseCheckRow kUseChecks[] = {
	{ AssetKind::MenuStyle, check_style_uses },
	{ AssetKind::ItemDefs, check_item_ids },
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
