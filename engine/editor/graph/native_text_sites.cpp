#include <editor/graph/native_text_sites.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

namespace {

bool delimits(char c) {
	return std::isspace(static_cast<unsigned char>(c)) || c == '\0' || std::strchr(",;=\"'/\\()[]{}", c) != nullptr;
}

// The file's references as its parser reads them; false when it does not read.
bool edges_of(const std::string &file, AssetKind kind, const std::string &game, const std::string &text,
              std::vector<GraphEdge> &edges) {
	Extracted out;
	Diagnostic error;
	if (!extract_from_bytes(file, kind, std::vector<uint8_t>(text.begin(), text.end()), game, out, error)) return false;
	edges = std::move(out.edges);
	return true;
}

bool same_edge(const GraphEdge &a, const GraphEdge &b) {
	return a.record == b.record && a.field == b.field && a.kind == b.kind && a.value == b.value &&
	       a.loader_arg == b.loader_arg && a.scope == b.scope;
}

// Whether `after` is `before` with the site's one edge naming its new value, and nothing else changed.
bool only_the_site(const std::vector<GraphEdge> &before, const std::vector<GraphEdge> &after, const NativeTextSite &site) {
	if (before.size() != after.size()) return false;
	size_t changed = 0;
	for (size_t i = 0; i < before.size(); ++i) {
		if (same_edge(before[i], after[i])) continue;
		const GraphEdge &was = before[i];
		const GraphEdge &now = after[i];
		if (was.record != site.record || was.field != site.field || was.value != site.before || now.value != site.after)
			return false;
		GraphEdge expected = was;
		expected.value = site.after;
		if (!same_edge(expected, now)) return false;
		++changed;
	}
	return changed == 1;
}

} // namespace

bool native_text_kind(AssetKind kind) {
	return kind == AssetKind::Terrain || kind == AssetKind::Environment || kind == AssetKind::Particles ||
	       kind == AssetKind::HudPosDefs;
}

size_t rewrite_native_text(const std::string &file, AssetKind kind, const std::string &game, std::string &text,
                           const std::vector<NativeTextSite> &sites, std::vector<size_t> &missed) {
	std::vector<GraphEdge> edges;
	if (!native_text_kind(kind) || !edges_of(file, kind, game, text, edges)) {
		for (size_t i = 0; i < sites.size(); ++i) missed.push_back(i);
		return 0;
	}
	size_t rewritten = 0;
	for (size_t i = 0; i < sites.size(); ++i) {
		const NativeTextSite &site = sites[i];
		// The names are UTF-8 (the graph's, extract_from_bytes), the text the game's code page: each looked
		// for and written in the text's own bytes; a name the code page cannot hold is missed.
		std::string before, after;
		if (!utf8_to_cp1252(site.before, before) || !utf8_to_cp1252(site.after, after)) {
			missed.push_back(i);
			continue;
		}
		bool found = false;
		for (size_t at = before.empty() ? std::string::npos : text.find(before); at != std::string::npos && !found;
		     at = text.find(before, at + 1)) {
			const size_t end = at + before.size();
			if ((at > 0 && !delimits(text[at - 1])) || (end < text.size() && !delimits(text[end]))) continue;
			std::string candidate = text;
			candidate.replace(at, before.size(), after);
			std::vector<GraphEdge> now;
			if (!edges_of(file, kind, game, candidate, now) || !only_the_site(edges, now, site)) continue;
			text = std::move(candidate);
			edges = std::move(now);
			found = true;
		}
		if (found) ++rewritten;
		else missed.push_back(i);
	}
	return rewritten;
}

namespace {

// The file's references and definitions as its parser reads them; false when it does not read.
bool read_native(const std::string &file, AssetKind kind, const std::string &game, const std::string &text, Extracted &out) {
	Diagnostic error;
	return extract_from_bytes(file, kind, std::vector<uint8_t>(text.begin(), text.end()), game, out, error);
}

// A name's stem: before its extension's dot (the last, after any folder).
std::string stem_of(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	const size_t folder = name.find_last_of("/\\");
	return dot == std::string::npos || (folder != std::string::npos && dot < folder) ? name : name.substr(0, dot);
}

// Whether the text holds `name` at `at`, without case, as a whole token (rewrite_native_text's bounds).
bool token_at(const std::string &text, size_t at, size_t length) {
	const size_t end = at + length;
	return (at == 0 || delimits(text[at - 1])) && (end >= text.size() || delimits(text[end]));
}

} // namespace

bool native_text_place(const std::string &file, AssetKind kind, const std::string &game, const std::string &text,
                       const std::string &record, const std::string &field, size_t &line, size_t &column) {
	if (record.empty() && field.empty()) return false;
	Extracted read;
	if (!read_native(file, kind, game, text, read)) return false;
	// The name: the record's edge's, else a symbol the record defines.
	size_t edge = std::string::npos, symbol = std::string::npos;
	for (size_t i = 0; i < read.edges.size() && edge == std::string::npos; ++i)
		if (read.edges[i].record == record && (field.empty() || read.edges[i].field == field)) edge = i;
	for (size_t i = 0; i < read.symbols.size() && edge == std::string::npos && symbol == std::string::npos; ++i)
		if (read.symbols[i].record == record && (field.empty() || read.symbols[i].field == field)) symbol = i;
	if (edge == std::string::npos && symbol == std::string::npos) return false;
	// The text is the game's code page, the graph's names UTF-8 (rewrite_native_text).
	std::string name;
	if (!utf8_to_cp1252(edge != std::string::npos ? read.edges[edge].value : read.symbols[symbol].display, name) || name.empty())
		return false;
	// Whether the text, the token at `at` changed, reads that one name otherwise: its edge the same but for its
	// value (a name the loader derives from the same token, a face animation's .MDT twin, may move with it), or
	// its symbol's name; the others the token reaches are no matter, every name the parser reads where it was.
	const auto is_site = [&](size_t at, size_t length) {
		const std::string token = text.substr(at, length);
		const std::string stem = stem_of(token);
		std::string probed = text;
		probed.replace(at, length, stem + "Q" + token.substr(stem.size()));
		Extracted now;
		if (!read_native(file, kind, game, probed, now) || now.edges.size() != read.edges.size() ||
		    now.symbols.size() != read.symbols.size())
			return false;
		if (edge != std::string::npos) {
			GraphEdge expected = read.edges[edge];
			expected.value = now.edges[edge].value;
			return same_edge(expected, now.edges[edge]) && now.edges[edge].value != read.edges[edge].value;
		}
		// A symbol's record is its name where the file keeps no other (a particle file's effect id).
		return now.symbols[symbol].kind == read.symbols[symbol].kind && now.symbols[symbol].display != read.symbols[symbol].display;
	};
	// Each whole token spelling the name, in the text's order; the first that is the site.
	const std::string lower = strutil::to_lower(text), wanted = strutil::to_lower(name);
	size_t found = std::string::npos;
	for (size_t at = lower.find(wanted); at != std::string::npos && found == std::string::npos; at = lower.find(wanted, at + 1))
		if (token_at(text, at, wanted.size()) && is_site(at, wanted.size())) found = at;
	if (found == std::string::npos) return false;
	const size_t newline = found == 0 ? std::string::npos : text.rfind('\n', found - 1);
	const size_t line_start = newline == std::string::npos ? 0 : newline + 1;
	line = 1 + size_t(std::count(text.begin(), text.begin() + std::ptrdiff_t(found), '\n'));
	column = found - line_start + 1;
	return true;
}

} // namespace opennova::editor
