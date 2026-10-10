#pragma once
// The strings the game's own code reads by name (ADR 0046 S23 B): what a string table's Uses counts beside the
// project's references. Read from the binary: each call of a game-text getter whose section and key are both
// literal strings, the getter's table [orig: GameText_GetString @ 0x51EBD0, GameText_GetStringWithFallback @
// 0x51EB90 and KeyHelp_GetStringWithFallback @ 0x51ED40 read gametext.bin (g_TextGameText); GameErr_GetString @
// 0x4C2C60 reads gameerr.bin], each through TextResource_FindEntryBySectionAndKey @ 0x75D250, so the expansion's
// table first (AssetGraph::text_override). A key the code builds as it runs (a sprintf'd one, a table's column)
// is not listed: 47 calls of the four pass no two literals.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/graph/graph_edge.h>

namespace opennova::editor {

class AssetGraph;

// One function of the game that reads a string by name: the table (upper case), the section and the key as the
// code passes them, the function and its first call's address.
struct CodeTextKey {
	const char *table;
	const char *section;
	const char *key;
	const char *reader;
	uint32_t at;
};

// Every such read, by table, section and key.
const std::vector<CodeTextKey> &code_text_keys();

// The game's own reads of a string: those whose lookup reaches `symbol` (a string id the graph defines) as the
// game's does (AssetGraph::resolve_symbol over the read's table and section: the expansion's table first, the
// first section of the name, an earlier key of the name before it); none for a symbol of another kind.
std::vector<const CodeTextKey *> code_reads_of(const AssetGraph &graph, const GraphSymbol &symbol);

} // namespace opennova::editor
