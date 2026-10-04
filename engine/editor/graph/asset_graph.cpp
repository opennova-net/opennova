#include <editor/graph/asset_graph.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <editor/documents/document_types.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/graph_layer.h>
#include <editor/graph/graph_names.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <runtime/menu/menu_style.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::key;
using graph_names::style_variable;
using graph_names::symbol_name;
using graph_names::upper;
using Ref = GraphIndex::Ref;

// A file the graph could not read: the reason, and that its references go unchecked.
Diagnostic unreadable(const AssetEntry &asset, const Diagnostic &error) {
	std::string reason = error.message.empty() ? std::string("The file could not be read.") : error.message;
	if (reason.back() != '.') reason += '.';
	return make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Warning,
	                    reason + " The references in it are not checked.", asset.relative_path);
}

// An edge as its file's reading makes it: every field but the target, which its resolution sets.
bool same_reading(const GraphEdge &a, const GraphEdge &b) {
	return a.source == b.source && a.record == b.record && a.record_key == b.record_key && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.kind == b.kind &&
			a.value == b.value && a.scope == b.scope && a.rewritable == b.rewritable &&
			a.through == b.through && a.loader_arg == b.loader_arg && a.use_context == b.use_context &&
			a.span.line == b.span.line && a.span.column == b.span.column &&
			a.span.length == b.span.length && a.fallback == b.fallback &&
			a.scopes_after == b.scopes_after && a.optional == b.optional && a.scope_alternate == b.scope_alternate &&
			a.scope_owner == b.scope_owner && a.needs == b.needs;
}

// A symbol as its file's reading makes it, the first one's inert and why given apart (a slot's own
// facts, where the graph publishes the game's reading of a style variable).
bool same_reading(
		const GraphSymbol &a, bool a_inert, const std::string &a_reason, const GraphSymbol &b) {
	return a.kind == b.kind && a.name == b.name && a.display == b.display && a.value == b.value &&
			a.file == b.file && a.record == b.record && a.record_key == b.record_key && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.scope == b.scope &&
			a_inert == b.inert && a_reason == b.inert_reason && a.line == b.line;
}

// A slot's reading, and another: the same edges, symbols and outcome.
bool reads_the_same(
		const GraphSlot &slot, const Extracted &content, bool ok, const Diagnostic &failure) {
	if (slot.ok != ok || slot.failure != failure || slot.edges.size() != content.edges.size() ||
	    slot.symbols.size() != content.symbols.size())
		return false;
	for (size_t i = 0; i < slot.edges.size(); ++i)
		if (!same_reading(slot.edges[i], content.edges[i])) return false;
	for (size_t i = 0; i < slot.symbols.size(); ++i)
		if (!same_reading(
					slot.symbols[i], slot.own_inert(i), slot.own_reason(i), content.symbols[i]))
			return false;
	return true;
}

// The names whose definitions differ between a file's reading and the one that replaces it
// (`now`; none for a file gone), keyed kind + name: each name whose list of definitions in the file
// is not the same. A symbol kind's (whose references resolve against the symbols) go in
// `symbols`, a style variable's (whose references resolve against the bindings, while a finding
// about one reads every definition of the name) in `variables`.
void changed_names(const GraphSlot &slot, const std::vector<GraphSymbol> &now,
		std::set<std::string> &symbols, std::set<std::string> &variables) {
	struct Definitions {
		ReferenceKind kind = ReferenceKind::None;
		std::vector<size_t> before, after;
	};
	std::unordered_map<std::string, Definitions> by_key;
	const auto definitions = [&by_key](const GraphSymbol &symbol) -> Definitions * {
		if (!reference_row(symbol.kind).names_symbol()) return nullptr;
		Definitions &found = by_key[GraphIndex::key_of(symbol.kind, symbol.name, symbol.scope)];
		found.kind = symbol.kind;
		return &found;
	};
	for (size_t i = 0; i < slot.symbols.size(); ++i)
		if (Definitions *found = definitions(slot.symbols[i])) found->before.push_back(i);
	for (size_t i = 0; i < now.size(); ++i)
		if (Definitions *found = definitions(now[i])) found->after.push_back(i);
	for (const auto &entry : by_key) {
		const std::vector<size_t> &before = entry.second.before, &after = entry.second.after;
		bool same = before.size() == after.size();
		for (size_t i = 0; same && i < before.size(); ++i)
			same = same_reading(slot.symbols[before[i]], slot.own_inert(before[i]),
					slot.own_reason(before[i]), now[after[i]]);
		if (same) continue;
		if (entry.second.kind == ReferenceKind::StyleVar) variables.insert(entry.first);
		else symbols.insert(entry.first);
	}
}

// An edge whose resolution reads the file set: a file reference, a screen's (its lookup asks
// whether its menu file is one), and one whose scope a file decides (its table there or not, its
// owner there or not), or whose reading at all does (GraphEdge::needs).
bool reads_file_set(const GraphEdge &edge) {
	return reference_row(edge.kind).resolution == ReferenceResolution::File ||
			edge.kind == ReferenceKind::MenuScreen || !edge.scope_alternate.empty() || !edge.scope_owner.empty() ||
			!edge.needs.empty();
}

} // namespace

// What an update's reading of the files changed.
struct AssetGraph::Patch {
	std::vector<uint32_t> slots;       // read again with another result: their content replaced
	std::set<std::string> symbol_keys; // kind + name of a symbol kind whose definitions changed
	std::set<std::string> style_keys;  // kind + name of a style variable whose definitions changed
	// The names (normalized) of the files added or gone, each with whether the project had a file
	// of it before: a base layer's file of a name the project comes to have, or no longer has,
	// hides or shows.
	std::map<std::string, bool> names;
	std::vector<std::string> files;    // the files added, gone, retyped or read differently
	// Each file read again, with the findings its edges held in their order: the diagnostics move
	// when its findings come out otherwise.
	std::vector<std::pair<uint32_t, std::vector<Diagnostic>>> held;
	bool diagnostics_moved = false;    // a failure changed, or a file holding findings gone
	bool file_set = false;             // a file added, gone or of another kind
	bool base = false;                 // another base layer: everything resolves again
};

bool scope_matches(const std::string &symbol_scope, const std::string &reference_scope) {
	if (reference_scope.empty()) return true;
	const size_t symbol_slash = symbol_scope.find('/');
	const size_t reference_slash = reference_scope.find('/');
	const std::string symbol_table = symbol_scope.substr(0, symbol_slash);
	const std::string reference_table = reference_scope.substr(0, reference_slash);
	if (reference_table.empty() || upper(symbol_table) != upper(reference_table)) return false;
	if (reference_slash == std::string::npos) return true;
	const std::string symbol_section = symbol_slash == std::string::npos ? std::string() : symbol_scope.substr(symbol_slash + 1);
	return upper(symbol_section) == upper(reference_scope.substr(reference_slash + 1));
}

std::vector<std::string> reference_file_candidates(ReferenceKind kind, const std::string &name, int32_t loader_arg,
                                                   const std::function<bool(const std::string &)> &exists) {
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution != ReferenceResolution::File || name.empty()) return {};
	if (row.file_names) return row.file_names(name, loader_arg, exists);
	std::vector<std::string> names{name};
	if (row.extensions)
		for (const char *const *extension = row.extensions; *extension; ++extension) names.push_back(name + *extension);
	return names;
}

bool file_serves_reference(AssetKind file, ReferenceKind kind, int32_t loader_arg) {
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution != ReferenceResolution::File) return false;
	if (file == row.file) return true;
	if (file != AssetKind::MaterialChunk || kind != ReferenceKind::Texture || !texture_arg_is_row_type(loader_arg))
		return false;
	// A chunk row reads its name as a chunk container whatever the name (a chunk reader for
	// any name, renderer::material_texture_source): a file no rule types by its name serves one
	// when its bytes hold a chunk (the scan's MaterialChunk), and no other texture row.
	const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(loader_arg));
	return renderer::material_texture_source(std::string(), type, {}).reader == renderer::MaterialTextureReader::Chunk;
}

std::vector<const AssetEntry *> AssetGraph::files_to_read(const AssetScan &scan,
		const std::vector<std::shared_ptr<const DocumentBase>> &open) const {
	std::unordered_set<std::string> records;
	for (const auto &document : open)
		if (document && records_of(*document)) records.insert(document->path());
	std::vector<const AssetEntry *> out;
	for (const AssetEntry &asset : scan.entries) {
		if (!graph_reads_kind(asset.kind) || records.count(asset.relative_path)) continue;
		const uint32_t id = index_.find(asset.relative_path);
		if (id != GraphIndex::kNone) {
			const GraphSlot &slot = index_.slot(id);
			// update() keeps a slot read from this file as it is (reused), whatever else it does.
			if (slot.logical_name == asset.logical_name && slot.kind == asset.kind && slot.read && !slot.open &&
					slot.size == asset.size_bytes && slot.modified == asset.modified_ticks)
				continue;
		}
		out.push_back(&asset);
	}
	return out;
}

GraphReading AssetGraph::read_file(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset) {
	GraphReading reading;
	reading.logical_name = asset.logical_name;
	reading.kind = asset.kind;
	reading.size = asset.size_bytes;
	reading.modified = asset.modified_ticks;
	reading.ok = extract_from_asset(paths, project, asset, reading.content, reading.error);
	return reading;
}

GraphUpdate AssetGraph::update(const ProjectPaths &paths, const ProjectDocument &project,
		const AssetScan &scan, const std::vector<std::shared_ptr<const DocumentBase>> &open,
		GraphReadings *read_ahead) {
	stats_ = GraphStats();
	Patch patch;
	// The open record and text documents stand in for their files (another kind of document reads
	// none).
	std::unordered_map<std::string, const DocumentBase *> documents;
	for (const auto &document : open)
		if (document && (records_of(*document) || text_of(*document)))
			documents[document->path()] = document.get();
	std::unordered_set<uint32_t> listed;
	listed.reserve(scan.entries.size());
	for (const AssetEntry &asset : scan.entries) {
		uint32_t id = index_.find(asset.relative_path);
		// Another name at a path is another file.
		if (id != GraphIndex::kNone && index_.slot(id).logical_name != asset.logical_name) {
			drop(id, patch);
			id = GraphIndex::kNone;
		}
		bool retyped = false;
		if (id == GraphIndex::kNone) {
			const std::string name_key = key(asset.logical_name);
			patch.names.emplace(name_key, index_.first_named(name_key) != GraphIndex::kNone);
			id = index_.add(asset.relative_path, asset.logical_name, name_key, asset.kind);
			patch.file_set = true;
			patch.files.push_back(asset.relative_path);
		} else if (index_.slot(id).kind != asset.kind) {
			index_.slot(id).kind = asset.kind;
			retyped = true;
			patch.file_set = true;
			patch.files.push_back(asset.relative_path);
		}
		listed.insert(id);
		GraphSlot &slot = index_.slot(id);
		// A file the graph does not read (a texture, a mission text) holds nothing; its row still
		// counts (the file set), so one added or gone reaches the edges that could name it.
		if (!graph_reads_kind(asset.kind)) {
			if (slot.read) {
				slot.read = slot.open = false;
				take(id, Extracted(), true, Diagnostic(), patch);
			}
			continue;
		}
		const auto found = documents.find(asset.relative_path);
		const DocumentBase *document = found == documents.end() ? nullptr : found->second;
		if (document) {
			if (!retyped && slot.read && slot.open && slot.identity == document->identity() &&
			    slot.revision == document->revision()) {
				++stats_.files_reused;
				continue;
			}
			Extracted content;
			if (const Document *records = records_of(*document))
				extract_from_document(*records, content);
			else
				extract_from_text(*text_of(*document), content);
			++stats_.files_extracted;
			slot.read = slot.open = true;
			slot.identity = document->identity();
			slot.revision = document->revision();
			slot.size = 0;
			slot.modified = 0;
			take(id, std::move(content), true, Diagnostic(), patch);
			continue;
		}
		if (!retyped && slot.read && !slot.open && slot.size == asset.size_bytes &&
				slot.modified == asset.modified_ticks) {
			++stats_.files_reused;
			continue;
		}
		Extracted content;
		Diagnostic error, failure;
		bool ok = false;
		// A reading made ahead of this update, while the scan lists the file as it was read.
		const auto ahead = read_ahead ? read_ahead->find(asset.relative_path) : GraphReadings::iterator();
		if (read_ahead && ahead != read_ahead->end() && ahead->second.logical_name == asset.logical_name &&
				ahead->second.kind == asset.kind && ahead->second.size == asset.size_bytes &&
				ahead->second.modified == asset.modified_ticks) {
			ok = ahead->second.ok;
			content = std::move(ahead->second.content);
			error = std::move(ahead->second.error);
			read_ahead->erase(ahead);
		} else {
			ok = extract_from_asset(paths, project, asset, content, error);
		}
		if (!ok) {
			++stats_.files_failed;
			// A document type's validation reports a file of its kinds that does not load.
			if (!is_editable_kind(asset.kind)) failure = unreadable(asset, error);
		}
		++stats_.files_extracted;
		slot.read = true;
		slot.open = false;
		slot.identity = slot.revision = 0;
		slot.size = asset.size_bytes;
		slot.modified = asset.modified_ticks;
		take(id, std::move(content), ok, failure, patch);
	}
	std::vector<uint32_t> gone;
	index_.for_each_slot([&](uint32_t id) {
		if (!listed.count(id)) gone.push_back(id);
	});
	for (const uint32_t id : gone) drop(id, patch);
	std::sort(patch.files.begin(), patch.files.end());
	patch.files.erase(std::unique(patch.files.begin(), patch.files.end()), patch.files.end());
	stats_.files_patched = patch.files.size();
	GraphUpdate out;
	out.files = patch.files;
	out.file_set = patch.file_set;
	// Everything the graph holds is made from the files' rows and what each one read: when
	// neither moved it stays as it is, every edge and symbol where it was.
	if (patch.files.empty()) return out;
	resolve_patch(patch, out);
	generation_.value = next_generation();
	out.changed = true;
	return out;
}

void AssetGraph::take(
		uint32_t id, Extracted content, bool ok, const Diagnostic &failure, Patch &patch) {
	GraphSlot &slot = index_.slot(id);
	if (reads_the_same(slot, content, ok, failure)) return;
	changed_names(slot, content.symbols, patch.symbol_keys, patch.style_keys);
	// The findings its edges held, which those it holds once resolved are compared against.
	std::vector<Diagnostic> held;
	for (EdgeResolution &resolution : slot.resolutions)
		if (resolution.missing) held.push_back(std::move(resolution.finding));
	patch.held.emplace_back(id, std::move(held));
	if (slot.failure != failure) patch.diagnostics_moved = true;
	index_.erase_content(id);
	slot.edges = std::move(content.edges);
	slot.symbols = std::move(content.symbols);
	slot.ok = ok;
	slot.failure = failure;
	// A style variable's own inert and why, kept apart from what the graph publishes (derive).
	slot.own.clear();
	if (std::any_of(slot.symbols.begin(), slot.symbols.end(),
				[](const GraphSymbol &symbol) { return symbol.kind == ReferenceKind::StyleVar; })) {
		slot.own.reserve(slot.symbols.size());
		for (const GraphSymbol &symbol : slot.symbols)
			slot.own.push_back({symbol.inert, symbol.inert_reason});
	}
	index_.insert_content(id);
	patch.slots.push_back(id);
	patch.files.push_back(slot.path);
}

void AssetGraph::drop(uint32_t id, Patch &patch) {
	const GraphSlot &slot = index_.slot(id);
	changed_names(slot, {}, patch.symbol_keys, patch.style_keys);
	patch.names.emplace(slot.key, true);
	patch.files.push_back(slot.path);
	patch.file_set = true;
	// Its findings and its failure go out of the diagnostics.
	if (!slot.failure.code().empty() ||
			std::any_of(slot.resolutions.begin(), slot.resolutions.end(),
					[](const EdgeResolution &resolution) { return resolution.missing; }))
		patch.diagnostics_moved = true;
	index_.erase_content(id);
	index_.remove(id);
}

void AssetGraph::resolve_patch(Patch &patch, GraphUpdate &out) {
	// The definitions the game reads, then each style variable's inert as it reads them: a file
	// read again all of its own, every other only those of a name whose binding changed (whose own
	// value edges then count as missing, or not, again).
	out.bindings = rebind();
	std::vector<Ref> work;
	const std::unordered_set<uint32_t> read_again(patch.slots.begin(), patch.slots.end());
	const auto derive_slot = [this](uint32_t id) {
		for (uint32_t i = 0; i < index_.slot(id).symbols.size(); ++i) derive(id, i);
	};
	if (patch.base) index_.for_each_slot(derive_slot);
	else
		for (const uint32_t id : patch.slots) derive_slot(id);
	if (!patch.base)
		for (const std::string &name : out.bindings)
			for (const Ref ref : index_.symbols_named(
					     GraphIndex::key_of(ReferenceKind::StyleVar, name, std::string()))) {
				if (read_again.count(ref.slot) || !derive(ref.slot, ref.index)) continue;
				const GraphSlot &slot = index_.slot(ref.slot);
				const auto at = slot.edges_at.find(slot.symbols[ref.index].locator);
				if (at != slot.edges_at.end())
					for (const uint32_t edge : at->second) work.push_back({ref.slot, edge});
			}
	// A base layer's file of a name the project's files came to have, or no longer have, hides or
	// shows, and the names it defines with it.
	if (base_ && !patch.base)
		for (const auto &name : patch.names) {
			const uint32_t id = base_->index().first_named(name.first);
			if (id == GraphIndex::kNone ||
					(index_.first_named(name.first) != GraphIndex::kNone) == name.second)
				continue;
			for (const GraphSymbol &symbol : base_->index().slot(id).symbols)
				(symbol.kind == ReferenceKind::StyleVar ? patch.style_keys : patch.symbol_keys)
						.insert(GraphIndex::key_of(symbol.kind, symbol.name, symbol.scope));
		}
	// What resolves again: every edge of a file read again; with another file set every edge its
	// lookup reads the files for; every edge at all when the base layer changed; the edges into a
	// name whose definitions changed (a base layer's shown or hidden included); the edges naming a
	// style variable whose binding changed.
	if (patch.base || patch.file_set) {
		index_.for_each_slot([&](uint32_t id) {
			const GraphSlot &slot = index_.slot(id);
			const bool all = patch.base || read_again.count(id);
			for (uint32_t i = 0; i < slot.edges.size(); ++i)
				if (all || reads_file_set(slot.edges[i])) work.push_back({id, i});
		});
	} else {
		for (const uint32_t id : patch.slots)
			for (uint32_t i = 0; i < index_.slot(id).edges.size(); ++i) work.push_back({id, i});
	}
	for (const std::string &name_key : patch.symbol_keys) {
		for (const Ref ref : index_.edges_targeting(name_key)) work.push_back(ref);
		// An edge with a fallback reaches either of its two names.
		for (const Ref ref : index_.edges_naming(name_key)) work.push_back(ref);
	}
	for (const std::string &name : out.bindings)
		for (const Ref ref : index_.edges_through(name)) work.push_back(ref);
	const auto before = [this](Ref a, Ref b) { return index_.before(a, b); };
	const auto same = [](Ref a, Ref b) { return a.slot == b.slot && a.index == b.index; };
	std::sort(work.begin(), work.end(), before);
	work.erase(std::unique(work.begin(), work.end(), same), work.end());
	// Whether the diagnostics move: an edge resolved before tells (resolve_edge), a file read again
	// by its findings against those it held (below).
	bool moved = patch.diagnostics_moved;
	for (const Ref ref : work) moved = resolve_edge(ref) || moved;
	// The missing edges whose words read what their resolution does not (one resolved above was
	// worded there): a style variable's, every definition of its name (a stylesheet the game does
	// not read defining it); with another file set, a kind's whose row says its words read which
	// files the project has (a string id's table, the failsafe clip).
	std::vector<Ref> words, reworded;
	for (const std::string &name_key : patch.style_keys)
		for (const Ref ref : index_.edges_targeting(name_key))
			if (index_.slot(ref.slot).resolutions[ref.index].missing) words.push_back(ref);
	if (patch.file_set)
		for (const Ref ref : index_.missing())
			if (reference_row(index_.edge(ref).kind).message_reads_files) words.push_back(ref);
	std::sort(words.begin(), words.end(), before);
	words.erase(std::unique(words.begin(), words.end(), same), words.end());
	std::set_difference(words.begin(), words.end(), work.begin(), work.end(),
			std::back_inserter(reworded), before);
	for (const Ref ref : reworded) moved = reword(ref) || moved;
	for (size_t i = 0; !moved && i < patch.held.size(); ++i) {
		const std::vector<Diagnostic> &held = patch.held[i].second;
		size_t n = 0;
		for (const EdgeResolution &resolution : index_.slot(patch.held[i].first).resolutions) {
			if (!resolution.missing) continue;
			if (n == held.size() || resolution.finding != held[n]) {
				moved = true;
				break;
			}
			++n;
		}
		moved = moved || n != held.size();
	}
	if (moved) list_diagnostics();
}

std::vector<std::string> AssetGraph::rebind() {
	// The variables the game reads: menu_style.mns, then brand.mns onto the same list, a
	// later definition winning [orig: Menu_InitShellResources @ 0x552500 (menu_style.mns @
	// 0x552604, brand.mns @ 0x552616)]. A stylesheet by any other name is never read. Each is the
	// project's file of the name when it has one (project assets win), else the base layer's.
	std::map<std::string, Binding> next;
	for (const menu::ShellStylesheet &sheet : menu::kShellStylesheets) {
		const std::string sheet_key = key(sheet.name);
		const GraphIndex *index = &index_;
		bool base = false;
		uint32_t id = index_.first_named(sheet_key);
		if (id == GraphIndex::kNone && base_) {
			index = &base_->index();
			base = true;
			id = index->first_named(sheet_key);
		}
		if (id == GraphIndex::kNone || index->slot(id).kind != AssetKind::MenuStyle) continue;
		const GraphSlot &slot = index->slot(id);
		for (uint32_t i = 0; i < slot.symbols.size(); ++i) {
			const GraphSymbol &symbol = slot.symbols[i];
			if (symbol.kind != ReferenceKind::StyleVar || slot.own_inert(i)) continue;
			Binding binding;
			binding.base = base;
			binding.symbol = {id, i};
			binding.file = symbol.file;
			binding.value = symbol.value;
			// Its own value edges (the font or texture its value names): a menu naming that file
			// through the variable is reported there, once.
			const auto at = slot.edges_at.find(symbol.locator);
			if (at != slot.edges_at.end())
				for (const uint32_t e : at->second) {
					const GraphEdge &edge = slot.edges[e];
					if (edge.field == "value" &&
							reference_row(edge.kind).resolution == ReferenceResolution::File &&
							std::find(binding.value_kinds.begin(), binding.value_kinds.end(),
									edge.kind) == binding.value_kinds.end())
						binding.value_kinds.push_back(edge.kind);
				}
			std::sort(binding.value_kinds.begin(), binding.value_kinds.end());
			next[symbol.name] = std::move(binding);
		}
	}
	std::vector<std::string> changed;
	auto was = bindings_.begin();
	auto now = next.begin();
	while (was != bindings_.end() || now != next.end()) {
		if (now == next.end() || (was != bindings_.end() && was->first < now->first)) {
			changed.push_back(was->first);
			++was;
		} else if (was == bindings_.end() || now->first < was->first) {
			changed.push_back(now->first);
			++now;
		} else {
			if (!was->second.same(now->second)) changed.push_back(now->first);
			++was;
			++now;
		}
	}
	bindings_ = std::move(next);
	return changed;
}

bool AssetGraph::derive(uint32_t id, uint32_t index) {
	GraphSlot &slot = index_.slot(id);
	GraphSymbol &symbol = slot.symbols[index];
	if (symbol.kind != ReferenceKind::StyleVar) return false;
	const auto binding = bindings_.find(symbol.name);
	const bool read = binding != bindings_.end() && !binding->second.base &&
			binding->second.symbol.slot == id && binding->second.symbol.index == index;
	std::string reason = read ? slot.own_reason(index)
			: unread_reason(symbol, slot.own_inert(index), slot.own_reason(index));
	const bool moved = symbol.inert == read;
	symbol.inert = !read;
	symbol.inert_reason = std::move(reason);
	return moved;
}

bool AssetGraph::unread(const GraphSymbol &symbol) const {
	return symbol.kind == ReferenceKind::StyleVar ? style_binding(symbol.name) != &symbol
	                                              : symbol.inert;
}

std::string AssetGraph::unread_reason(
		const GraphSymbol &symbol, bool own_inert, const std::string &own_reason) const {
	// Its stylesheet's own reason (MnsDocument::refine_symbol), else the file is none the game
	// reads, else the one it reads defines the name again.
	if (own_inert) return own_reason;
	if (!menu::is_shell_stylesheet(basename_of(symbol.file)))
		return "the game reads no stylesheet but menu_style.mns and brand.mns";
	const auto binding = bindings_.find(symbol.name);
	if (binding != bindings_.end())
		return binding->second.file + " defines it again, which the game reads after";
	return "the game does not read it";
}

bool AssetGraph::resolve_edge(Ref ref) {
	GraphSlot &slot = index_.slot(ref.slot);
	GraphEdge &edge = slot.edges[ref.index];
	EdgeResolution &resolution = slot.resolutions[ref.index];
	const std::string target = resolved_target(edge);
	std::string file;
	const ReferenceStatus status =
			target.empty() ? ReferenceStatus::NotAReference : resolve(edge, &file);
	if (status != ReferenceStatus::Present ||
			reference_row(edge.kind).resolution != ReferenceResolution::File)
		file.clear();
	const bool missing = counts_missing(slot, edge, target, status);
	// Each entry of the index moves only when it changed.
	const bool was = resolution.resolved;
	if (!was || target != edge.target) {
		if (was) index_.remove_target(GraphIndex::key_of(edge.kind, edge.target, edge.scope), ref);
		edge.target = target;
		index_.add_target(GraphIndex::key_of(edge.kind, edge.target, edge.scope), ref);
	}
	if (!was || file != resolution.file) {
		if (was && !resolution.file.empty()) index_.remove_user(resolution.file, ref);
		if (!file.empty()) index_.add_user(file, ref);
	}
	// A file read again answers for its own findings (resolve_patch).
	const bool moved = was && missing != resolution.missing;
	if (!was || missing != resolution.missing) {
		if (was && resolution.missing) index_.remove_missing(ref);
		if (missing) index_.add_missing(ref);
	}
	resolution.resolved = true;
	resolution.status = status;
	resolution.file = std::move(file);
	resolution.missing = missing;
	++stats_.edges_resolved;
	// Its finding, worded with its target as it now is (a style variable's words read it).
	if (!missing) {
		resolution.finding = Diagnostic();
		return moved;
	}
	const bool reworded = reword(ref);
	return moved || (was && reworded);
}

bool AssetGraph::reword(Ref ref) {
	EdgeResolution &resolution = index_.slot(ref.slot).resolutions[ref.index];
	Diagnostic finding = missing_finding(index_.edge(ref));
	++stats_.findings_made;
	if (finding == resolution.finding) return false;
	resolution.finding = std::move(finding);
	return true;
}

bool AssetGraph::counts_missing(const GraphSlot &slot, const GraphEdge &edge,
		const std::string &target, ReferenceStatus status) const {
	if (target.empty() || status != ReferenceStatus::Missing) return false;
	// A kind the graph never finds missing (no message for it: a Record reference, an index past
	// its collection, which its file's own validation reports with what the game makes of it); a
	// file the game runs without (GraphEdge::optional).
	if (!reference_row(edge.kind).missing_message || edge.optional) return false;
	const bool file = reference_row(edge.kind).resolution == ReferenceResolution::File;
	// A file named through a variable no stylesheet the game reads defines: the variable's own
	// edge reports it.
	if (file && is_style_reference(target)) return false;
	// A file named through a variable whose value names a file of the same kind: the
	// stylesheet's own edge reports it, once, where it is defined. A kind that does not
	// match (a font variable used as an image) is reported here.
	if (file && is_style_reference(edge.value)) {
		const auto binding = bindings_.find(style_variable(edge.value));
		if (binding != bindings_.end() &&
				std::find(binding->second.value_kinds.begin(), binding->second.value_kinds.end(),
						edge.kind) != binding->second.value_kinds.end())
			return false;
	}
	// A stylesheet value the game never reads (a definition brand.mns replaces [orig:
	// NapiConfigMap_ParseKeyValueBuffer @ 0x639e09], one after the place the game stops reading
	// the file) loads no file.
	if (edge.field == "value") {
		const auto at = slot.symbols_at.find(edge.locator);
		if (at != slot.symbols_at.end())
			for (const uint32_t s : at->second)
				if (slot.symbols[s].kind == ReferenceKind::StyleVar && slot.symbols[s].inert)
					return false;
	}
	return true;
}

void AssetGraph::list_diagnostics() {
	diagnostics_.clear();
	diagnostics_.reserve(index_.missing().size());
	for (const Ref ref : index_.missing())
		diagnostics_.push_back(index_.slot(ref.slot).resolutions[ref.index].finding);
	std::vector<const GraphSlot *> failed;
	index_.for_each_slot([&](uint32_t id) {
		const GraphSlot &slot = index_.slot(id);
		if (!slot.ok && !slot.failure.code().empty()) failed.push_back(&slot);
	});
	std::sort(failed.begin(), failed.end(),
			[](const GraphSlot *a, const GraphSlot *b) { return a->path < b->path; });
	for (const GraphSlot *slot : failed) diagnostics_.push_back(slot->failure);
}

GraphUpdate AssetGraph::set_base(std::shared_ptr<const GraphLayer> base) {
	GraphUpdate out;
	if (base == base_) return out;
	base_ = std::move(base);
	stats_ = GraphStats();
	Patch patch;
	patch.base = true;
	resolve_patch(patch, out);
	generation_.value = next_generation();
	out.changed = true;
	return out;
}

uint64_t AssetGraph::next_generation() {
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1, std::memory_order_relaxed);
}

void AssetGraph::clear() {
	*this = AssetGraph(); // the assignment takes the generation anew
}

void AssetGraph::for_each_edge(const std::function<void(const GraphEdge &)> &visit) const {
	index_.for_each_slot([&](uint32_t id) {
		for (const GraphEdge &edge : index_.slot(id).edges) visit(edge);
	});
}

void AssetGraph::for_each_symbol(const std::function<void(const GraphSymbol &)> &visit) const {
	index_.for_each_slot([&](uint32_t id) {
		for (const GraphSymbol &symbol : index_.slot(id).symbols) visit(symbol);
	});
}

const GraphSlot *AssetGraph::file_named(const std::string &name_key) const {
	const uint32_t id = index_.first_named(name_key);
	if (id != GraphIndex::kNone) return &index_.slot(id);
	if (!base_) return nullptr;
	const uint32_t base = base_->index().first_named(name_key);
	return base == GraphIndex::kNone ? nullptr : &base_->index().slot(base);
}

bool AssetGraph::base_file_shows(const GraphSlot &slot) const {
	return index_.first_named(slot.key) == GraphIndex::kNone;
}

const GraphSlot *AssetGraph::named_file(const std::string &file) const {
	const uint32_t id = index_.find(file);
	if (id != GraphIndex::kNone) return &index_.slot(id);
	// A name alone: the file it resolves to.
	return file.find('/') == std::string::npos ? file_named(key(file)) : nullptr;
}

const GraphSymbol *AssetGraph::style_binding(const std::string &name) const {
	const auto found = bindings_.find(style_variable(name));
	if (found == bindings_.end()) return nullptr;
	const Binding &binding = found->second;
	if (!binding.base) return &index_.symbol(binding.symbol);
	return base_ ? &base_->index().symbol(binding.symbol) : nullptr;
}

// The name an edge looks up: a style variable's value where one stands, normalized
// for its namespace. A style variable still standing after resolution stays as
// written, so the StyleVar edge reports it and the file edge stays quiet.
std::string AssetGraph::resolved_target(const GraphEdge &edge) const {
	switch (reference_row(edge.kind).resolution) {
	case ReferenceResolution::StyleVariable: return is_style_reference(edge.value) ? style_variable(edge.value) : std::string();
	case ReferenceResolution::Symbol:
		// The name it reaches: its value, else its fallback where only that one is defined (in the
		// first scope a lookup finds either in).
		return symbol_name(edge.kind, reached_name(edge));
	// A record's index, in the file the edge's scope names (the key holds it).
	case ReferenceResolution::Record: return edge.value;
	case ReferenceResolution::File:
	case ReferenceResolution::Unchecked: break;
	}
	const std::string resolved = resolve_style(edge.value);
	return is_style_reference(resolved) ? resolved : key(resolved);
}

std::string AssetGraph::resolve_style(const std::string &value) const {
	if (!is_style_reference(value)) return value;
	const GraphSymbol *binding = style_binding(value);
	return binding ? binding->value : value;
}

namespace {

// Where an edge's lookup starts, and whether it goes on to the scopes after it: its own scope; any
// table, and nothing after, where its owner is a file the project does not have
// (GraphEdge::scope_owner); its alternate table's section where the project has no file of its own
// scope's table (GraphEdge::scope_alternate).
struct FirstScope {
	const std::string *own = nullptr;
	std::string made;
	bool use_made = false;
	bool then_after = true;
	const std::string &scope() const { return use_made ? made : *own; }
};

FirstScope first_scope(const AssetGraph &graph, const GraphEdge &edge) {
	FirstScope out;
	out.own = &edge.scope;
	if (!edge.scope_owner.empty() && !graph.has_file(edge.scope_owner)) {
		out.use_made = true;
		out.then_after = false;
		return out;
	}
	if (!edge.scope_alternate.empty()) {
		const size_t slash = edge.scope.find('/');
		if (!graph.has_file(edge.scope.substr(0, slash))) {
			out.made = edge.scope_alternate + (slash == std::string::npos ? std::string() : edge.scope.substr(slash));
			out.use_made = true;
		}
	}
	return out;
}

// The scopes an edge's lookup tries, in order: its first (first_scope), then each of scopes_after.
template <class Try> bool each_scope(const AssetGraph &graph, const GraphEdge &edge, Try try_scope) {
	const FirstScope first = first_scope(graph, edge);
	if (try_scope(first.scope(), true)) return true;
	if (!first.then_after) return false;
	for (const std::string &scope : edge.scopes_after)
		if (try_scope(scope, false)) return true;
	return false;
}

} // namespace

std::string AssetGraph::lookup_scope(const GraphEdge &edge) const { return first_scope(*this, edge).scope(); }

bool AssetGraph::rewrites(const GraphEdge &edge) const {
	return edge.rewritable && (edge.scope_owner.empty() || has_file(edge.scope_owner));
}

ReferenceStatus AssetGraph::resolve(const GraphEdge &edge, std::string *file_out) const {
	// A file the reader reads only beside another the project lacks: no reference (GraphEdge::needs).
	if (!edge.needs.empty() && !has_file(edge.needs)) {
		if (file_out) file_out->clear();
		return ReferenceStatus::NotAReference;
	}
	const FirstScope first = first_scope(*this, edge);
	const ReferenceStatus status = resolve(edge.kind, edge.value, first.scope(), file_out, edge.loader_arg);
	if (status != ReferenceStatus::Missing || (edge.fallback.empty() && (edge.scopes_after.empty() || !first.then_after)))
		return status;
	// The lookup's second name where the first finds nothing, then each later scope, both names.
	ReferenceStatus found = status;
	each_scope(*this, edge, [&](const std::string &scope, bool at_first) {
		for (const std::string *name : {&edge.value, &edge.fallback}) {
			if (name->empty() || (name == &edge.value && at_first)) continue;
			const ReferenceStatus second = resolve(edge.kind, *name, scope, file_out, edge.loader_arg);
			if (second != ReferenceStatus::Present) continue;
			found = second;
			return true;
		}
		return false;
	});
	return found;
}

const std::string &AssetGraph::reached_name(const GraphEdge &edge) const {
	const std::string *reached = &edge.value;
	each_scope(*this, edge, [&](const std::string &scope, bool) {
		if (resolve_symbol(edge.kind, edge.value, scope)) return true;
		if (!edge.fallback.empty() && resolve_symbol(edge.kind, edge.fallback, scope)) {
			reached = &edge.fallback;
			return true;
		}
		return false;
	});
	return *reached;
}

const GraphSymbol *AssetGraph::symbol_reached(const GraphEdge &edge) const {
	const GraphSymbol *found = nullptr;
	each_scope(*this, edge, [&](const std::string &scope, bool) {
		found = resolve_symbol(edge.kind, edge.value, scope);
		if (!found && !edge.fallback.empty()) found = resolve_symbol(edge.kind, edge.fallback, scope);
		return found != nullptr;
	});
	return found;
}

std::vector<const GraphEdge *> AssetGraph::edges_naming(ReferenceKind kind, const std::string &name) const {
	std::vector<const GraphEdge *> out;
	// A fallback is a symbol kind's (never a Record kind's, whose key would hold its file).
	for (const Ref ref : index_.edges_naming(GraphIndex::key_of(kind, graph_names::symbol_name(kind, name), std::string())))
		out.push_back(&index_.edge(ref));
	return out;
}

ReferenceStatus AssetGraph::resolve(ReferenceKind kind, const std::string &name, const std::string &scope,
                                    std::string *file_out, int32_t loader_arg) const {
	if (file_out) file_out->clear();
	if (kind == ReferenceKind::None) return ReferenceStatus::NotAReference;
	if (name.empty()) return ReferenceStatus::NotAReference;
	const ReferenceResolution resolution = reference_row(kind).resolution;
	if (resolution == ReferenceResolution::Unchecked) return ReferenceStatus::Unverified;
	if (resolution == ReferenceResolution::StyleVariable) {
		// Only what the game reads counts: a definition in a stylesheet it does not load,
		// or after the place it stops reading one, leaves the name literal.
		const GraphSymbol *binding = style_binding(name);
		if (!binding) return ReferenceStatus::Missing;
		if (file_out) *file_out = binding->file;
		return ReferenceStatus::Present;
	}
	if (resolution == ReferenceResolution::Record) {
		// A record of the file the scope names, by its index: Missing past the collection (its
		// file's validation says what the game makes of that; the graph finds none missing).
		const GraphSymbol *record = resolve_symbol(kind, name, scope);
		if (!record) return ReferenceStatus::Missing;
		if (file_out) *file_out = record->file;
		return ReferenceStatus::Present;
	}
	if (resolution == ReferenceResolution::Symbol) {
		// A screen's own lookup, which no column describes: of a menu file the project does
		// not have, that file's own edge says so.
		if (kind == ReferenceKind::MenuScreen) {
			const GraphSlot *file = file_named(key(scope));
			if (!file || file->kind != AssetKind::Menu) return ReferenceStatus::Unverified;
		}
		if (const GraphSymbol *symbol = resolve_symbol(kind, name, scope)) {
			if (file_out) *file_out = symbol->file;
			return ReferenceStatus::Present;
		}
		// A SCREEN row loads its FILE, then selects among every screen the game has loaded,
		// newest first, whatever the file held [orig: UIScene_LoadAndParseContent @ 0x63c830
		// returns the parse's result; CUIScene_SelectNodeByName @ 0x63b6b0]: a screen of the
		// name in another menu is found when the game loaded that menu before, which the
		// graph cannot know.
		if (kind == ReferenceKind::MenuScreen)
			for (const GraphSymbol *symbol : symbols_named(kind, name))
				if (!symbol->inert) return ReferenceStatus::Unverified;
		return ReferenceStatus::Missing;
	}
	const std::string resolved = resolve_style(name);
	if (is_style_reference(resolved)) return ReferenceStatus::Missing;
	// The first name the kind's loader reads that the project (else the base) has as a file it can
	// load.
	const auto loadable = [this, kind, loader_arg](const std::string &file) -> const GraphSlot * {
		const GraphSlot *found = file_named(key(file));
		if (!found || !file_serves_reference(found->kind, kind, loader_arg)) return nullptr;
		return found;
	};
	const auto exists = [&loadable](const std::string &file) { return loadable(file) != nullptr; };
	for (const std::string &candidate : reference_file_candidates(kind, resolved, loader_arg, exists)) {
		const GraphSlot *found = loadable(candidate);
		if (!found) continue;
		if (file_out) *file_out = found->path;
		return ReferenceStatus::Present;
	}
	return ReferenceStatus::Missing;
}

const GraphSymbol *AssetGraph::resolve_symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const {
	const ReferenceResolution resolution = reference_row(kind).resolution;
	if (resolution == ReferenceResolution::StyleVariable) return style_binding(name);
	// A record of the project's file the scope names, by its index (a base layer keeps no record
	// sets: a Record reference resolves in its own file).
	if (resolution == ReferenceResolution::Record) {
		const std::vector<Ref> &named =
				index_.symbols_named(GraphIndex::key_of(kind, symbol_name(kind, name), scope));
		return named.empty() ? nullptr : &index_.symbol(named.front());
	}
	if (resolution != ReferenceResolution::Symbol) return nullptr;
	const std::string name_key = GraphIndex::key_of(kind, symbol_name(kind, name), scope);
	for (const Ref ref : index_.symbols_named(name_key)) {
		const GraphSymbol &symbol = index_.symbol(ref);
		if (!symbol.inert && scope_matches(symbol.scope, scope)) return &symbol;
	}
	if (!base_) return nullptr;
	const GraphIndex &base = base_->index();
	for (const Ref ref : base.symbols_named(name_key)) {
		const GraphSymbol &symbol = base.symbol(ref);
		if (base_file_shows(base.slot(ref.slot)) && !symbol.inert &&
				scope_matches(symbol.scope, scope))
			return &symbol;
	}
	return nullptr;
}

std::vector<ReferenceChoice> AssetGraph::choices(ReferenceKind kind, const std::string &scope, int32_t loader_arg) const {
	std::vector<ReferenceChoice> out;
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution == ReferenceResolution::File) {
		// Each name once, the file it resolves to (the first of it by path); the base's files the
		// project has none of the name of after the project's.
		const auto offer_files = [&](const GraphIndex &index, bool base) {
			const std::string *last = nullptr;
			index.for_each_slot([&](uint32_t id) {
				const GraphSlot &slot = index.slot(id);
				if (last && *last == slot.key) return;
				last = &slot.key;
				if ((base && !base_file_shows(slot)) ||
						!file_serves_reference(slot.kind, kind, loader_arg))
					return;
				ReferenceChoice choice;
				choice.name = slot.logical_name;
				choice.kind = kind;
				choice.file = slot.path;
				choice.status = resolve(kind, choice.name, scope, &choice.served, loader_arg);
				if (choice.status != ReferenceStatus::Present) choice.served.clear();
				out.push_back(std::move(choice));
			});
		};
		offer_files(index_, false);
		if (base_) offer_files(base_->index(), true);
		return out;
	}
	if (row.resolution == ReferenceResolution::Record) {
		// The records the collection holds in the file the scope names (its record set), in their
		// order, each by its index.
		const uint32_t id = index_.find(scope);
		if (id == GraphIndex::kNone) return out;
		for (const GraphSymbol &symbol : index_.slot(id).symbols) {
			if (symbol.kind != kind) continue;
			ReferenceChoice choice;
			choice.name = symbol.display;
			choice.kind = kind;
			choice.file = symbol.file;
			choice.record = symbol.record;
			choice.label = symbol.value;
			out.push_back(std::move(choice));
		}
		return out;
	}
	if (!row.names_symbol()) return out;
	// Each name once: the definitions a lookup finds, the project's then the base's, then (inert)
	// those defined only where no lookup finds them, the same way; so a name the project defines
	// only where none does is the base's live one when the base has one.
	std::set<std::string> offered;
	const bool variable = row.spell == NameSpelling::StyleVariable;
	for (const bool inert : {false, true})
		for (const bool base : {false, true}) {
			if (base && !base_) continue;
			const GraphIndex &index = base ? base_->index() : index_;
			for (const Ref ref : index.symbols_of_kind(kind)) {
				const GraphSymbol &symbol = index.symbol(ref);
				if ((base && !base_file_shows(index.slot(ref.slot))) || unread(symbol) != inert ||
						(row.picker_scoped && !scope_matches(symbol.scope, scope)) ||
						!offered.insert(symbol.name).second)
					continue;
				ReferenceChoice choice;
				choice.name = variable ? "%" + symbol.display + "%" : symbol.display;
				choice.kind = kind;
				choice.file = symbol.file;
				choice.record = symbol.record;
				choice.status = resolve(kind, choice.name, scope, nullptr, loader_arg);
				choice.inert = inert;
				// A base layer's style variable keeps only its own file's reason (unread).
				choice.reason = inert && base && symbol.kind == ReferenceKind::StyleVar
				                        ? unread_reason(symbol, symbol.inert, symbol.inert_reason)
				                        : symbol.inert_reason;
				out.push_back(std::move(choice));
			}
		}
	return out;
}

std::vector<const GraphEdge *> AssetGraph::references_of(const std::string &file) const {
	std::vector<const GraphEdge *> out;
	const GraphSlot *slot = named_file(file);
	if (!slot) return out;
	out.reserve(slot->edges.size());
	for (const GraphEdge &edge : slot->edges) out.push_back(&edge);
	return out;
}

std::vector<const GraphEdge *> AssetGraph::referrers_of_file(const std::string &file) const {
	std::vector<const GraphEdge *> out;
	const GraphSlot *slot = named_file(file);
	if (!slot) return out;
	for (const Ref ref : index_.users_of(slot->path)) out.push_back(&index_.edge(ref));
	return out;
}

std::vector<const GraphEdge *> AssetGraph::referrers_of(ReferenceKind kind, const std::string &name,
                                                        const std::string &scope) const {
	std::vector<const GraphEdge *> out;
	// A record's index names it in its own file alone, which its key holds.
	const bool record = reference_row(kind).resolution == ReferenceResolution::Record;
	for (const Ref ref :
			index_.edges_targeting(GraphIndex::key_of(kind, symbol_name(kind, name), scope))) {
		const GraphEdge &edge = index_.edge(ref);
		// An edge reads the symbol's scope through its own, or one it tries after it.
		if (!record && !scope.empty() &&
		    !each_scope(*this, edge, [&](const std::string &tried, bool) { return scope_matches(scope, tried); }))
			continue;
		out.push_back(&edge);
	}
	return out;
}

std::vector<const GraphEdge *> AssetGraph::usages_of(const std::string &file) const {
	std::vector<const GraphEdge *> out = referrers_of_file(file);
	const GraphSlot *slot = named_file(file);
	if (!slot) return out;
	// An edge using two of its symbols (a string id of no scope, a key two sections define) is
	// listed once, where it is met first. A record of the file's record sets is used by the file's
	// own edges alone: no use of the file.
	std::set<const GraphEdge *> listed(out.begin(), out.end());
	for (const GraphSymbol &symbol : slot->symbols) {
		if (unread(symbol) || reference_row(symbol.kind).resolution == ReferenceResolution::Record) continue;
		for (const GraphEdge *edge : referrers_of(symbol.kind, symbol.name, symbol.scope))
			if (listed.insert(edge).second) out.push_back(edge);
	}
	return out;
}

std::vector<const GraphEdge *> AssetGraph::users_of(const GraphSymbol &symbol) const {
	std::vector<const GraphEdge *> out;
	if (symbol.inert) return out;
	// A record of a record set is named in its own file alone, which its key holds.
	const bool record = reference_row(symbol.kind).resolution == ReferenceResolution::Record;
	for (const GraphEdge *edge : referrers_of(symbol.kind, symbol.name, record ? symbol.scope : std::string()))
		if (symbol_reached(*edge) == &symbol) out.push_back(edge);
	return out;
}

const GraphSymbol *AssetGraph::symbol_at(const std::string &file, const std::string &locator, const std::string &field) const {
	const uint32_t id = index_.find(file);
	if (id == GraphIndex::kNone || field.empty()) return nullptr;
	const GraphSlot &slot = index_.slot(id);
	const auto at = slot.symbols_at.find(locator);
	if (at == slot.symbols_at.end()) return nullptr;
	for (const uint32_t index : at->second)
		if (slot.symbols[index].field == field) return &slot.symbols[index];
	return nullptr;
}

std::vector<const GraphSymbol *> AssetGraph::symbols_of(const std::string &file, const std::string &record) const {
	std::vector<const GraphSymbol *> out;
	const uint32_t id = index_.find(file);
	if (id == GraphIndex::kNone) return out;
	const GraphSlot &slot = index_.slot(id);
	const auto found = slot.symbols_in.find(record);
	if (found != slot.symbols_in.end())
		for (const uint32_t index : found->second) out.push_back(&slot.symbols[index]);
	return out;
}

bool AssetGraph::for_each_definition(const Document &document,
		const std::function<void(const GraphSymbol &symbol, bool inert)> &visit) const {
	const uint32_t id = index_.find(document.path());
	if (id == GraphIndex::kNone) return false;
	const GraphSlot &slot = index_.slot(id);
	if (!slot.read || !slot.open || slot.identity != document.identity() ||
			slot.revision != document.revision())
		return false;
	for (size_t i = 0; i < slot.symbols.size(); ++i) visit(slot.symbols[i], slot.own_inert(i));
	return true;
}

const std::vector<AssetGraph::ViaRecord> *AssetGraph::records_naming_(const std::string &path) const {
	// Made once per generation: every file's records in other files whose fields name the file itself (an
	// item's graphic, a material row's texture), never a record naming a symbol the file defines.
	std::lock_guard<std::mutex> lock(via_.mutex);
	if (!via_.built || via_.generation != generation()) {
		via_.by_file.clear();
		index_.for_each_slot([&](uint32_t id) {
			const GraphSlot &slot = index_.slot(id);
			for (const GraphEdge *edge : referrers_of_file(slot.path)) {
				if (edge->source == slot.path || edge->record.empty()) continue;
				via_.by_file[slot.path].push_back({upper(edge->record), edge->record, edge->source});
			}
		});
		via_.generation = generation();
		via_.built = true;
	}
	const auto found = via_.by_file.find(path);
	return found == via_.by_file.end() ? nullptr : &found->second;
}

std::vector<GraphSearchHit> AssetGraph::search(const std::string &text) const {
	std::vector<GraphSearchHit> hits;
	if (text.empty()) return hits;
	const std::string wanted = upper(text);
	const auto holds = [&wanted](const std::string &name) { return upper(name).find(wanted) != std::string::npos; };
	// A name's file once: the one it resolves to.
	const std::string *last = nullptr;
	index_.for_each_slot([&](uint32_t id) {
		const GraphSlot &slot = index_.slot(id);
		if (last && *last == slot.key) return;
		last = &slot.key;
		GraphSearchHit hit;
		if (!holds(slot.logical_name)) {
			// Found by what names it, from three letters on: the first record of another file naming the
			// file itself whose name holds the text (a model by the item whose graphic it is, a texture by the
			// material row naming it). A record naming a symbol the file defines (an item a weapons table
			// names) does not find the file; nor do a file's own records naming it.
			if (wanted.size() < kSearchByRecordLetters) return;
			const std::vector<ViaRecord> *records = records_naming_(slot.path);
			if (!records) return;
			const auto by = std::find_if(records->begin(), records->end(), [&](const ViaRecord &r) {
				return r.upper_record.find(wanted) != std::string::npos;
			});
			if (by == records->end()) return;
			hit.via = by->record;
			hit.via_file = by->source;
		}
		hit.name = slot.logical_name;
		hit.file = slot.path;
		hit.usages = usages_of(slot.path).size();
		hits.push_back(std::move(hit));
	});
	for_each_symbol([&](const GraphSymbol &symbol) {
		// A record set's records go by their index, no name.
		if (reference_row(symbol.kind).resolution == ReferenceResolution::Record) return;
		// An item by its catalog's name too (ADR 0046 S17: a soldier found by the name a modder knows
		// it by, "Indonesian Soldier #1", not its id).
		const std::string words = symbol.kind == ReferenceKind::Item ? symbol_words(symbol) : std::string();
		if (!holds(symbol.display) && (words.empty() || !holds(words))) return;
		GraphSearchHit hit;
		hit.symbol = &symbol;
		hit.name = symbol.display;
		hit.words = words == symbol.display ? std::string() : words;
		hit.file = symbol.file;
		hit.usages = users_of(symbol).size();
		hits.push_back(std::move(hit));
	});
	return hits;
}

std::vector<const GraphSymbol *> AssetGraph::symbols_of_kind(ReferenceKind kind) const {
	const std::vector<Ref> &refs = index_.symbols_of_kind(kind);
	std::vector<const GraphSymbol *> out;
	out.reserve(refs.size());
	for (const Ref ref : refs) out.push_back(&index_.symbol(ref));
	return out;
}

std::vector<const GraphSymbol *> AssetGraph::symbols_named(ReferenceKind kind, const std::string &name,
                                                           const std::string &scope) const {
	std::vector<const GraphSymbol *> out;
	const std::string name_key = GraphIndex::key_of(kind, symbol_name(kind, name), scope);
	for (const Ref ref : index_.symbols_named(name_key)) out.push_back(&index_.symbol(ref));
	if (!base_) return out;
	const GraphIndex &base = base_->index();
	for (const Ref ref : base.symbols_named(name_key))
		if (base_file_shows(base.slot(ref.slot))) out.push_back(&base.symbol(ref));
	return out;
}

bool AssetGraph::has_file(const std::string &name) const {
	return file_named(key(basename_of(name))) != nullptr;
}

std::vector<const GraphEdge *> AssetGraph::missing() const {
	std::vector<const GraphEdge *> out;
	out.reserve(index_.missing().size());
	for (const Ref ref : index_.missing()) out.push_back(&index_.edge(ref));
	return out;
}

Diagnostic AssetGraph::missing_finding(const GraphEdge &edge) const {
	// How much it matters and what the game does instead are the kind's (graph/reference_kinds:
	// what the game shrugs off is a warning).
	const ReferenceKindRow &row = reference_row(edge.kind);
	const std::string who = edge.record.empty() ? edge.source : "'" + edge.record + "' in " + edge.source;
	// A file of a name its loader opens that the project holds, of another kind: no name missing, a
	// file the game reads as what it is not (review F3), an error that gates.
	const GraphSlot *other = nullptr;
	if (row.resolution == ReferenceResolution::File)
		for (const std::string &candidate :
		     reference_file_candidates(edge.kind, resolve_style(edge.value), edge.loader_arg,
		                               [this](const std::string &name) { return file_named(key(name)) != nullptr; }))
			if ((other = file_named(key(candidate))) != nullptr) break;
	const std::string message =
	        who + " names " + row.phrase + " '" + edge.value + "'" +
	        (other ? ", but the project's " + other->logical_name + " is " + asset_kind_label(other->kind) +
	                         ", which the game does not load as " + row.phrase + "."
	         : row.missing_message ? row.missing_message(*this, edge)
	                               : std::string(", which the project does not have."));
	Diagnostic d = other ? make_finding(CoreFinding::ReferenceWrongKind, DiagnosticSeverity::Error, message, edge.source, edge.field)
	                     : make_finding(CoreFinding::ReferenceMissing, row.severity_when_missing, message, edge.source, edge.field);
	d.record = edge.record;
	d.record_key = edge.record_key;
	// A text's reference: its place, where Problems opens the document.
	d.line = edge.span.line;
	d.column = edge.span.column;
	d.row_id = edge.address.row;
	d.child_id = edge.address.child;
	d.record_kind = edge.address.kind;
	// What is missing: a file by the name the game loads (the style variable's value where one
	// stands), a symbol as written.
	d.subject = ReferenceSubject{ edge.kind,
		row.resolution == ReferenceResolution::File ? resolve_style(edge.value) : edge.value,
		edge.scope, edge.loader_arg };
	return d;
}

} // namespace opennova::editor
