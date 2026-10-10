#pragma once
// The strings the game's own code reads by name (ADR 0046 S23 B): what a string table's Uses counts beside the
// project's references. Read from the binary: every call of a text getter (718: their code references, tail calls
// among them), each whose section and key are both literal strings (642: pushed, or loaded into the register pushed,
// or put in a tail call's frame) a row of the getter's table [orig: GameText_GetString @ 0x51EBD0,
// GameText_GetStringWithFallback @ 0x51EB90 and KeyHelp_GetStringWithFallback @ 0x51ED40 read gametext.bin
// (g_TextGameText); GameErr_GetString @ 0x4C2C60 reads gameerr.bin; TextResource_GetStringWithFallback @ 0x562EE0
// reads the table it is given, gameerr.bin's g_TextGameErr or Game.bin's g_TextMenuUi (Menu_InitShellResources @
// 0x552500)], each through TextResource_FindEntryBySectionAndKey @ 0x75D250, so the expansion's table first
// (AssetGraph::text_override). Not listed: the 73 calls whose section or key the code builds as it runs (a
// sprintf'd key, a table's column), 2 of TextResource_GetStringWithFallback over a table it is handed, 1 whose
// arguments the read does not find. One row per function and key, at its first call.
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
