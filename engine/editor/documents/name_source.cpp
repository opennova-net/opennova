#include <editor/documents/name_source.h>

#include <editor/graph/reference_kinds.h>

namespace opennova::editor {

std::string symbol_words(const GraphSymbol &symbol) {
	switch (symbol.kind) {
	// An item is named by its id (the symbol) on the record whose display name is its name: the
	// record, the row of the catalog (its name field, DefCatalogDocument).
	case ReferenceKind::Item: return symbol.record.empty() ? symbol.display : symbol.record;
	// A string id carries its text (StringsDocument::refine_symbol).
	case ReferenceKind::TextId: return symbol.value.empty() ? symbol.display : symbol.value;
	default: break;
	}
	// A record of a record set by its own name where it has one (a register's NAME).
	if (reference_row(symbol.kind).resolution == ReferenceResolution::Record && !symbol.value.empty()) return symbol.value;
	return symbol.display;
}

} // namespace opennova::editor
