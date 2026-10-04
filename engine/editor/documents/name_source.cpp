#include <editor/documents/name_source.h>

#include <base/io/strutil.h>
#include <editor/graph/reference_kinds.h>
#include <runtime/hud/game_text_lookup.h>

namespace opennova::editor {

std::string wepdes_text(const NameSource *names, const std::string &key) {
	if (!names || key.empty()) return std::string();
	static const std::string scope = strutil::to_upper(hud::kGameTextTable) + "/" + hud::kGameTextWepDes;
	const GraphSymbol *text = names->symbol(ReferenceKind::TextId, key, scope);
	return text ? text->value : std::string();
}

std::string definition_words(const GraphSymbol &symbol, const NameSource *names) {
	if (names && symbol.kind == ReferenceKind::Weapon) {
		std::string shown = wepdes_text(names, symbol.display);
		if (shown.empty()) shown = wepdes_text(names, symbol.value);
		if (!shown.empty()) return shown;
	}
	if (names && symbol.kind == ReferenceKind::Ammo) {
		const std::string shown = wepdes_text(names, symbol.display);
		if (!shown.empty()) return shown;
	}
	return symbol_words(symbol);
}

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
