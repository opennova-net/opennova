#include <editor/graph/asset_graph.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>

#include <editor/documents/document_types.h>
#include <editor/graph/graph_names.h>
#include <runtime/menu/menu_style.h>
#include <runtime/menu/menu_text_tables.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::key;
using graph_names::style_variable;
using graph_names::symbol_name;
using graph_names::upper;

// A file the graph could not read: the reason, and that its references go unchecked.
Diagnostic unreadable(const AssetEntry &asset, const Diagnostic &error) {
	std::string reason = error.message.empty() ? std::string("The file could not be read.") : error.message;
	if (reason.back() != '.') reason += '.';
	return make_diagnostic(DiagnosticSeverity::Warning, "graph.unreadable",
	                       reason + " The references in it are not checked.", asset.relative_path);
}

std::string symbol_key(ReferenceKind kind, const std::string &name) {
	return std::string(reference_row(kind).token) + '\n' + name;
}

std::string basename_of(const std::string &path) { return std::filesystem::path(path).filename().generic_string(); }

bool same_edge(const GraphEdge &a, const GraphEdge &b) {
	return a.source == b.source && a.record == b.record && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.kind == b.kind &&
			a.value == b.value && a.target == b.target && a.scope == b.scope &&
			a.rewritable == b.rewritable && a.through == b.through &&
			a.material_type == b.material_type;
}

bool same_symbol(const GraphSymbol &a, const GraphSymbol &b) {
	return a.kind == b.kind && a.name == b.name && a.display == b.display && a.value == b.value &&
			a.file == b.file && a.record == b.record && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.scope == b.scope &&
			a.inert == b.inert && a.inert_reason == b.inert_reason;
}

// What one file references and defines, read again, is what it was.
bool same_extracted(const Extracted &a, const Extracted &b) {
	return a.edges.size() == b.edges.size() && a.symbols.size() == b.symbols.size() &&
			std::equal(a.edges.begin(), a.edges.end(), b.edges.begin(), same_edge) &&
			std::equal(a.symbols.begin(), a.symbols.end(), b.symbols.begin(), same_symbol);
}

} // namespace

std::string menu_text_scope(const std::string &table) {
	return upper(basename_of(table)) + "/" + menu::kMenuTextSection;
}

std::string menu_screen_scope(const std::string &menu_file) { return upper(basename_of(menu_file)); }

std::string menu_window_scope(const std::string &menu_file, const std::string &screen) {
	return menu_screen_scope(menu_file) + "/" + upper(screen);
}

std::vector<std::string> reference_file_candidates(ReferenceKind kind, const std::string &name, int material_type,
                                                   const std::function<bool(const std::string &)> &exists) {
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution != ReferenceResolution::File || name.empty()) return {};
	if (row.file_names) return row.file_names(name, material_type, exists);
	std::vector<std::string> names{name};
	if (row.extensions)
		for (const char *const *extension = row.extensions; *extension; ++extension) names.push_back(name + *extension);
	return names;
}

bool file_serves_reference(AssetKind file, ReferenceKind kind, int material_type) {
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution != ReferenceResolution::File) return false;
	if (file == row.file) return true;
	if (file != AssetKind::Unknown || kind != ReferenceKind::Texture || material_type < 0) return false;
	// A chunk row reads its name as a chunk container whatever the name (a chunk reader for
	// any name, renderer::material_texture_source).
	const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(material_type));
	return renderer::material_texture_source(std::string(), type, {}).reader == renderer::MaterialTextureReader::Chunk;
}

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

void AssetGraph::update(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                        const std::vector<std::shared_ptr<const Document>> &open) {
	stats_ = GraphStats();
	std::map<std::string, Extraction> next;
	// Whether what the files reference and define moved: a file read again whose edges, symbols
	// or failure differ, a file the graph did not read before, or one it no longer reads.
	bool moved = false;
	using Cached = std::map<std::string, Extraction>::const_iterator;
	const auto read_again = [&moved, this](Cached cached, const Extraction &entry) {
		moved = moved || cached == cache_.end() || cached->second.ok != entry.ok ||
				cached->second.failure != entry.failure ||
				!same_extracted(cached->second.content, entry.content);
	};
	for (const AssetEntry &asset : scan.entries) {
		if (!graph_reads_kind(asset.kind)) continue;
		const Document *document = nullptr;
		for (const auto &candidate : open)
			if (candidate && candidate->path() == asset.relative_path) document = candidate.get();
		auto cached = cache_.find(asset.relative_path);
		Extraction entry;
		if (document) {
			if (cached != cache_.end() && cached->second.open && cached->second.identity == document->identity() &&
			    cached->second.revision == document->revision()) {
				entry = std::move(cached->second);
				++stats_.files_reused;
			} else {
				entry.open = true;
				entry.identity = document->identity();
				entry.revision = document->revision();
				extract_from_document(*document, entry.content);
				++stats_.files_extracted;
				read_again(cached, entry);
			}
		} else if (cached != cache_.end() && !cached->second.open && cached->second.size == asset.size_bytes &&
		           cached->second.modified == asset.modified_ticks) {
			entry = std::move(cached->second);
			++stats_.files_reused;
		} else {
			entry.size = asset.size_bytes;
			entry.modified = asset.modified_ticks;
			Diagnostic error;
			entry.ok = extract_from_asset(paths, project, asset, entry.content, error);
			if (!entry.ok) {
				++stats_.files_failed;
				// A document type's validation reports a file of its kinds that does not load.
				if (!is_editable_kind(asset.kind)) entry.failure = unreadable(asset, error);
			}
			++stats_.files_extracted;
			read_again(cached, entry);
		}
		next[asset.relative_path] = std::move(entry);
	}
	moved = moved || next.size() != cache_.size();
	cache_ = std::move(next);
	// Everything the graph holds is made from the files' rows and what each one read: when
	// neither moved it stays as it is, every edge and symbol where it was.
	if (!moved && same_files(scan)) return;
	assemble(scan);
	++generation_;
}

bool AssetGraph::same_files(const AssetScan &scan) const {
	if (scan.entries.size() != scanned_.size()) return false;
	for (size_t i = 0; i < scanned_.size(); ++i) {
		const AssetEntry &asset = scan.entries[i];
		const FileRow &row = scanned_[i];
		if (asset.relative_path != row.path || asset.logical_name != row.logical_name ||
				asset.kind != row.kind)
			return false;
	}
	return true;
}

void AssetGraph::assemble(const AssetScan &scan) {
	files_.clear();
	scanned_.clear();
	for (const AssetEntry &asset : scan.entries) {
		FileRow row;
		row.path = asset.relative_path;
		row.logical_name = asset.logical_name;
		row.kind = asset.kind;
		scanned_.push_back(row);
		files_.emplace(key(asset.logical_name), std::move(row));
	}
	edges_.clear();
	symbols_.clear();
	symbol_index_.clear();
	record_symbols_.clear();
	file_symbols_.clear();
	file_users_.reset();
	edge_index_.clear();
	style_bindings_.clear();
	style_value_edges_.clear();
	inert_style_values_.clear();
	for (const AssetEntry &asset : scan.entries) {
		const auto cached = cache_.find(asset.relative_path);
		if (cached == cache_.end()) continue;
		for (const GraphSymbol &symbol : cached->second.content.symbols) {
			symbol_index_.emplace(symbol_key(symbol.kind, symbol.name), symbols_.size());
			record_symbols_[{symbol.file, symbol.record}].push_back(symbols_.size());
			file_symbols_[symbol.file].push_back(symbols_.size());
			symbols_.push_back(symbol);
		}
	}
	// The variables the game reads: menu_style.mns, then brand.mns onto the same list, a
	// later definition winning [orig: Menu_InitShellResources @ 0x552500 (menu_style.mns @
	// 0x552604, brand.mns @ 0x552616)]. A stylesheet by any other name is never read.
	for (const menu::ShellStylesheet &sheet : menu::kShellStylesheets) {
		const auto file = files_.find(key(sheet.name));
		if (file == files_.end() || file->second.kind != AssetKind::MenuStyle) continue;
		for (size_t i = 0; i < symbols_.size(); ++i) {
			const GraphSymbol &symbol = symbols_[i];
			if (symbol.kind == ReferenceKind::StyleVar && symbol.file == file->second.path && !symbol.inert)
				style_bindings_[symbol.name] = i;
		}
	}
	for (size_t i = 0; i < symbols_.size(); ++i) {
		GraphSymbol &symbol = symbols_[i];
		if (symbol.kind != ReferenceKind::StyleVar) continue;
		const auto binding = style_bindings_.find(symbol.name);
		const bool read = binding != style_bindings_.end() && binding->second == i;
		// Why the game does not read it: its stylesheet's own reason (MnsDocument::refine_symbol),
		// else the file is none the game reads, else the one it reads defines the name again.
		if (!read && !symbol.inert)
			symbol.inert_reason = !menu::is_shell_stylesheet(basename_of(symbol.file))
			                              ? "the game reads no stylesheet but menu_style.mns and brand.mns"
			                      : binding != style_bindings_.end()
			                              ? symbols_[binding->second].file + " defines it again, which the game reads after"
			                              : "the game does not read it";
		symbol.inert = !read;
		if (symbol.inert) inert_style_values_.insert({symbol.file, symbol.locator});
	}
	for (const AssetEntry &asset : scan.entries) {
		const auto cached = cache_.find(asset.relative_path);
		if (cached == cache_.end()) continue;
		for (const GraphEdge &edge : cached->second.content.edges) {
			edges_.push_back(edge);
			edges_.back().target = resolved_target(edges_.back());
			edge_index_.emplace(symbol_key(edge.kind, edges_.back().target), edges_.size() - 1);
		}
	}
	// A binding's own value edges (the font or texture its value names): a menu naming
	// that file through the variable is reported there, once.
	for (const auto &binding : style_bindings_) {
		const GraphSymbol &symbol = symbols_[binding.second];
		for (const GraphEdge &edge : edges_)
			if (edge.source == symbol.file && edge.locator == symbol.locator && edge.field == "value" &&
			    reference_row(edge.kind).resolution == ReferenceResolution::File)
				style_value_edges_.insert({binding.first, edge.kind});
	}
}

const GraphSymbol *AssetGraph::style_binding(const std::string &name) const {
	const auto found = style_bindings_.find(style_variable(name));
	return found == style_bindings_.end() ? nullptr : &symbols_[found->second];
}

// The name an edge looks up: a style variable's value where one stands, normalized
// for its namespace. A style variable still standing after resolution stays as
// written, so the StyleVar edge reports it and the file edge stays quiet.
std::string AssetGraph::resolved_target(const GraphEdge &edge) const {
	switch (reference_row(edge.kind).resolution) {
	case ReferenceResolution::StyleVariable: return is_style_reference(edge.value) ? style_variable(edge.value) : std::string();
	case ReferenceResolution::Symbol: return symbol_name(edge.kind, edge.value);
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

ReferenceStatus AssetGraph::resolve(const GraphEdge &edge, std::string *file_out) const {
	return resolve(edge.kind, edge.value, edge.scope, file_out, edge.material_type);
}

ReferenceStatus AssetGraph::resolve(ReferenceKind kind, const std::string &name, const std::string &scope,
                                    std::string *file_out, int material_type) const {
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
	if (resolution == ReferenceResolution::Symbol) {
		// A screen's own lookup, which no column describes: of a menu file the project does
		// not have, that file's own edge says so.
		if (kind == ReferenceKind::MenuScreen) {
			const auto file = files_.find(key(scope));
			if (file == files_.end() || file->second.kind != AssetKind::Menu) return ReferenceStatus::Unverified;
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
	// The first name the kind's loader reads that the project has as a file it can load.
	const auto loadable = [this, kind, material_type](const std::string &file) -> const FileRow * {
		const auto found = files_.find(key(file));
		if (found == files_.end() || !file_serves_reference(found->second.kind, kind, material_type)) return nullptr;
		return &found->second;
	};
	const auto exists = [&loadable](const std::string &file) { return loadable(file) != nullptr; };
	for (const std::string &candidate : reference_file_candidates(kind, resolved, material_type, exists)) {
		const FileRow *found = loadable(candidate);
		if (!found) continue;
		if (file_out) *file_out = found->path;
		return ReferenceStatus::Present;
	}
	return ReferenceStatus::Missing;
}

const GraphSymbol *AssetGraph::resolve_symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const {
	const ReferenceResolution resolution = reference_row(kind).resolution;
	if (resolution == ReferenceResolution::StyleVariable) return style_binding(name);
	if (resolution != ReferenceResolution::Symbol) return nullptr;
	const auto range = symbol_index_.equal_range(symbol_key(kind, symbol_name(kind, name)));
	for (auto it = range.first; it != range.second; ++it) {
		const GraphSymbol &symbol = symbols_[it->second];
		if (!symbol.inert && scope_matches(symbol.scope, scope)) return &symbol;
	}
	return nullptr;
}

std::vector<ReferenceChoice> AssetGraph::choices(ReferenceKind kind, const std::string &scope, int material_type) const {
	std::vector<ReferenceChoice> out;
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution == ReferenceResolution::File) {
		for (const auto &entry : files_) {
			if (!file_serves_reference(entry.second.kind, kind, material_type)) continue;
			ReferenceChoice choice;
			choice.name = entry.second.logical_name;
			choice.kind = kind;
			choice.file = entry.second.path;
			choice.status = resolve(kind, choice.name, scope, nullptr, material_type);
			out.push_back(std::move(choice));
		}
		return out;
	}
	if (!row.names_symbol()) return out;
	// Each name once: the definition a lookup finds, else (inert) the first defined.
	std::set<std::string> offered;
	const auto offer = [&](const GraphSymbol &symbol) {
		if (!offered.insert(symbol.name).second) return;
		ReferenceChoice choice;
		const bool variable = row.spell == NameSpelling::StyleVariable;
		choice.name = variable ? "%" + symbol.display + "%" : symbol.display;
		choice.kind = kind;
		choice.file = symbol.file;
		choice.record = symbol.record;
		choice.status = resolve(kind, choice.name, scope, nullptr, material_type);
		choice.inert = symbol.inert;
		choice.reason = symbol.inert_reason;
		out.push_back(std::move(choice));
	};
	for (const bool inert : {false, true})
		for (const GraphSymbol &symbol : symbols_)
			if (symbol.kind == kind && symbol.inert == inert && (!row.picker_scoped || scope_matches(symbol.scope, scope)))
				offer(symbol);
	return out;
}

std::vector<const GraphEdge *> AssetGraph::references_of(const std::string &file) const {
	std::vector<const GraphEdge *> out;
	const std::string wanted = key(basename_of(file));
	for (const GraphEdge &edge : edges_)
		if (edge.source == file || key(basename_of(edge.source)) == wanted) out.push_back(&edge);
	return out;
}

std::vector<const GraphEdge *> AssetGraph::referrers_of_file(const std::string &logical_name) const {
	std::vector<const GraphEdge *> out;
	const auto file = files_.find(key(basename_of(logical_name)));
	if (file == files_.end()) return out;
	if (!file_users_) {
		file_users_ = std::make_shared<std::map<std::string, std::vector<size_t>>>();
		for (size_t i = 0; i < edges_.size(); ++i) {
			const GraphEdge &edge = edges_[i];
			if (reference_row(edge.kind).resolution != ReferenceResolution::File || edge.target.empty()) continue;
			std::string resolved;
			if (resolve(edge, &resolved) == ReferenceStatus::Present) (*file_users_)[resolved].push_back(i);
		}
	}
	const auto users = file_users_->find(file->second.path);
	if (users != file_users_->end())
		for (const size_t index : users->second) out.push_back(&edges_[index]);
	return out;
}

std::vector<const GraphEdge *> AssetGraph::referrers_of(ReferenceKind kind, const std::string &name,
                                                        const std::string &scope) const {
	std::vector<const GraphEdge *> out;
	const auto range = edge_index_.equal_range(symbol_key(kind, symbol_name(kind, name)));
	for (auto it = range.first; it != range.second; ++it) {
		const GraphEdge &edge = edges_[it->second];
		if (!scope.empty() && !scope_matches(scope, edge.scope)) continue;
		out.push_back(&edge);
	}
	return out;
}

std::vector<const GraphEdge *> AssetGraph::usages_of(const std::string &file) const {
	std::vector<const GraphEdge *> out = referrers_of_file(file);
	const auto row = files_.find(key(basename_of(file)));
	if (row == files_.end()) return out;
	// An edge using two of its symbols (a string id of no scope, a key two sections define) is
	// listed once, where it is met first.
	std::set<const GraphEdge *> listed(out.begin(), out.end());
	const auto defined = file_symbols_.find(row->second.path);
	if (defined == file_symbols_.end()) return out;
	for (const size_t index : defined->second) {
		const GraphSymbol &symbol = symbols_[index];
		if (symbol.inert) continue;
		for (const GraphEdge *edge : referrers_of(symbol.kind, symbol.name, symbol.scope))
			if (listed.insert(edge).second) out.push_back(edge);
	}
	return out;
}

std::vector<const GraphEdge *> AssetGraph::users_of(const GraphSymbol &symbol) const {
	std::vector<const GraphEdge *> out;
	if (symbol.inert) return out;
	for (const GraphEdge *edge : referrers_of(symbol.kind, symbol.name))
		if (resolve_symbol(edge->kind, edge->value, edge->scope) == &symbol) out.push_back(edge);
	return out;
}

const GraphSymbol *AssetGraph::symbol_at(const std::string &file, const std::string &locator, const std::string &field) const {
	const auto defined = file_symbols_.find(file);
	if (defined == file_symbols_.end() || field.empty()) return nullptr;
	for (const size_t index : defined->second)
		if (symbols_[index].locator == locator && symbols_[index].field == field) return &symbols_[index];
	return nullptr;
}

std::vector<GraphSearchHit> AssetGraph::search(const std::string &text) const {
	std::vector<GraphSearchHit> hits;
	if (text.empty()) return hits;
	const std::string wanted = upper(text);
	const auto holds = [&wanted](const std::string &name) { return upper(name).find(wanted) != std::string::npos; };
	for (const auto &entry : files_) {
		if (!holds(entry.second.logical_name)) continue;
		GraphSearchHit hit;
		hit.name = entry.second.logical_name;
		hit.file = entry.second.path;
		hit.usages = usages_of(entry.second.path).size();
		hits.push_back(std::move(hit));
	}
	for (const GraphSymbol &symbol : symbols_) {
		if (!holds(symbol.display)) continue;
		GraphSearchHit hit;
		hit.symbol = &symbol;
		hit.name = symbol.display;
		hit.file = symbol.file;
		hit.usages = users_of(symbol).size();
		hits.push_back(std::move(hit));
	}
	return hits;
}

std::vector<const GraphSymbol *> AssetGraph::symbols_of_kind(ReferenceKind kind) const {
	// The index's keys of a kind start with its token and a newline; its symbols listed in the
	// order the files define them (the index's order is the name's).
	const std::string prefix = symbol_key(kind, std::string());
	std::vector<size_t> indexes;
	for (auto it = symbol_index_.lower_bound(prefix);
			it != symbol_index_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
		indexes.push_back(it->second);
	std::sort(indexes.begin(), indexes.end());
	std::vector<const GraphSymbol *> out;
	out.reserve(indexes.size());
	for (const size_t index : indexes) out.push_back(&symbols_[index]);
	return out;
}

std::vector<const GraphSymbol *> AssetGraph::symbols_named(ReferenceKind kind, const std::string &name) const {
	std::vector<const GraphSymbol *> out;
	const auto range = symbol_index_.equal_range(symbol_key(kind, symbol_name(kind, name)));
	for (auto it = range.first; it != range.second; ++it) out.push_back(&symbols_[it->second]);
	return out;
}

bool AssetGraph::has_file(const std::string &name) const { return files_.count(key(basename_of(name))) != 0; }

std::vector<const GraphSymbol *> AssetGraph::symbols_of(const std::string &file, const std::string &record) const {
	std::vector<const GraphSymbol *> out;
	const auto found = record_symbols_.find({file, record});
	if (found != record_symbols_.end())
		for (const size_t index : found->second) out.push_back(&symbols_[index]);
	return out;
}

std::vector<const GraphEdge *> AssetGraph::missing() const {
	std::vector<const GraphEdge *> out;
	for (const GraphEdge &edge : edges_) {
		if (edge.target.empty()) continue;
		const bool file = reference_row(edge.kind).resolution == ReferenceResolution::File;
		if (file && is_style_reference(edge.target)) continue;
		// A file named through a variable whose value names a file of the same kind: the
		// stylesheet's own edge reports it, once, where it is defined. A kind that does not
		// match (a font variable used as an image) is reported here.
		if (file && is_style_reference(edge.value) && style_value_edges_.count({style_variable(edge.value), edge.kind}))
			continue;
		// A stylesheet value the game never reads (a definition brand.mns replaces [orig:
		// NapiConfigMap_ParseKeyValueBuffer @ 0x639e09], one after the place the game stops reading
		// the file) loads no file.
		if (edge.field == "value" && inert_style_values_.count({edge.source, edge.locator})) continue;
		if (resolve(edge) == ReferenceStatus::Missing) out.push_back(&edge);
	}
	return out;
}

Diagnostic AssetGraph::missing_finding(const GraphEdge &edge) const {
	// How much it matters and what the game does instead are the kind's (graph/reference_kinds:
	// what the game shrugs off is a warning).
	const ReferenceKindRow &row = reference_row(edge.kind);
	const std::string who = edge.record.empty() ? edge.source : "'" + edge.record + "' in " + edge.source;
	const std::string message = who + " names " + row.phrase + " '" + edge.value + "'" +
	                            (row.missing_message ? row.missing_message(*this, edge)
	                                                 : std::string(", which the project does not have."));
	Diagnostic d = make_diagnostic(row.severity_when_missing, "reference.missing", message, edge.source, edge.field);
	d.record = edge.record;
	d.row_id = edge.address.row;
	d.child_id = edge.address.child;
	d.record_kind = edge.address.kind;
	// What is missing: a file by the name the game loads (the style variable's value where one
	// stands), a symbol as written.
	d.reference = edge.kind;
	d.target = row.resolution == ReferenceResolution::File ? resolve_style(edge.value) : edge.value;
	d.scope = edge.scope;
	d.material_type = edge.material_type;
	return d;
}

std::vector<Diagnostic> AssetGraph::diagnostics() const {
	std::vector<Diagnostic> findings;
	for (const GraphEdge *edge : missing()) findings.push_back(missing_finding(*edge));
	for (const auto &entry : cache_)
		if (!entry.second.ok && !entry.second.failure.code.empty()) findings.push_back(entry.second.failure);
	return findings;
}

} // namespace opennova::editor
