#include <editor/graph/native_text_sites.h>

#include <cctype>
#include <cstring>
#include <utility>

#include <base/io/cp1252.h>
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
	       kind == AssetKind::HudPosDefs || kind == AssetKind::FaceAnimation;
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

} // namespace opennova::editor
