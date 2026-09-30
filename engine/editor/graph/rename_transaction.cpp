#include <editor/graph/rename_transaction.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>
#include <variant>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/graph/graph_names.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::key;
std::string extension_of(const std::string &name) { return key(fs::path(name).extension().generic_string()); }
std::string stem_of(const std::string &name) { return fs::path(name).stem().generic_string(); }

// The file a path names, else the first of its logical name (two files of one name are the
// scan's asset.name.duplicate: the path picks which is renamed).
const AssetEntry *find_asset(const AssetScan &scan, const std::string &file) {
	for (const AssetEntry &asset : scan.entries)
		if (asset.relative_path == file) return &asset;
	const std::string wanted = key(basename_of(file));
	for (const AssetEntry &asset : scan.entries)
		if (key(asset.logical_name) == wanted) return &asset;
	return nullptr;
}

// The value a site takes: the new name where the site names the file itself; where its
// loader reaches the file from another spelling (a name without its extension, a .tga
// whose .dds sibling is the file, a font name cut at its first dot), that spelling with the
// file's stem replaced, so the same rule reaches the renamed file and what the loader reads
// from the spelling itself stays (a normal map's .tga).
std::string replacement(const std::string &value, const std::string &old_name, const std::string &new_name) {
	if (key(value) == key(old_name)) return new_name;
	const std::string old_stem = stem_of(old_name);
	if (value.size() >= old_stem.size() && key(value.substr(0, old_stem.size())) == key(old_stem))
		return stem_of(new_name) + value.substr(old_stem.size());
	return new_name;
}

// Whether a site's field holding `value` loads the renamed file once the rename is done:
// the first name its loader reads that the project then has (the file under its new name,
// no longer under its old one) is the new name (a model's texture row: the one file its
// loader opens, renderer::material_texture_source); and a model's texture row gets from the
// loader what it got before, which reads the name's own spelling (a normal map's .tga, an
// .mdt) [orig: Material_LoadStageTexture @ 0x5B16F0] (renderer::material_texture_transform).
bool loads_renamed(const AssetScan &scan, const GraphEdge &edge, const std::string &value, const AssetEntry &target,
                   const std::string &renamed) {
	const auto exists = [&](const std::string &file) {
		if (key(file) == key(renamed)) return file_serves_reference(target.kind, edge.kind, edge.loader_arg);
		if (key(file) == key(target.logical_name)) return false;
		const AssetEntry *asset = scan.find(file);
		return asset && file_serves_reference(asset->kind, edge.kind, edge.loader_arg);
	};
	std::string first;
	for (const std::string &candidate : reference_file_candidates(edge.kind, value, edge.loader_arg, exists))
		if (exists(candidate)) {
			first = candidate;
			break;
		}
	if (first.empty() || key(first) != key(renamed)) return false;
	if (edge.kind != ReferenceKind::Texture || edge.loader_arg < 0) return true;
	const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(edge.loader_arg));
	return renderer::material_texture_transform(type, value, true) == renderer::material_texture_transform(type, edge.value, true);
}

Diagnostic refusal(CoreFinding code, const std::string &message, const std::string &asset = std::string(),
                   const std::string &field = std::string()) {
	return make_finding(code, DiagnosticSeverity::Error, message, asset, field);
}

// Why a file's sites cannot be rewritten: its kind has no editor, or its editor's documents hold
// no records a rename sets (a document of another kind, S13 D6).
Diagnostic cannot_rewrite(const DocumentType *type, const std::string &file) {
	if (!type) return refusal(CoreFinding::RenameSite, file + " has no editor to rewrite it.", file);
	return refusal(CoreFinding::RenameSite, file + " holds no records for a rename to rewrite.", file);
}

// A field of a record kind of a document type, asked of a blank document of the type (a type's
// schema never depends on a file's content), one made per kind of file.
const FieldSchema *site_field(std::map<AssetKind, std::unique_ptr<Document>> &blanks, AssetKind file, NodeKind kind,
                              const std::string &id) {
	std::unique_ptr<Document> &blank = blanks[file];
	if (!blank) {
		const DocumentType *type = document_type_for(file);
		if (!type) return nullptr;
		blank = records_of(type->make());
		if (!blank) return nullptr;
	}
	for (const FieldSchema &field : blank->fields(kind))
		if (field.id == id) return &field;
	return nullptr;
}

// A site's new value as its field holds it: a text, or a number where the field is one (an item
// id); false for a text a number field cannot take.
bool site_value(const FieldSchema &field, const std::string &text, Value &out) {
	if (field.type == FieldType::Text) {
		out = text;
		return true;
	}
	int64_t number = 0;
	const auto read = std::from_chars(text.data(), text.data() + text.size(), number);
	if (text.empty() || read.ec != std::errc() || read.ptr != text.data() + text.size()) return false;
	out = number;
	return true;
}

// Where a site is, in a finding's words.
std::string site_where(const RenameSite &site) {
	return site.record.empty() ? site.file : "'" + site.record + "' in " + site.file;
}

// A field's value as a finding quotes it.
std::string value_text(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&value)) return std::to_string(*number);
	return std::to_string(std::get<double>(value));
}

} // namespace

RenamePlan plan_rename(const ProjectPaths &paths, const AssetScan &scan, const AssetGraph &graph, const std::string &file,
                       const std::string &new_name) {
	RenamePlan plan;
	plan.new_name = new_name;
	const AssetEntry *asset = find_asset(scan, file);
	if (!asset) {
		plan.refusals.push_back(refusal(CoreFinding::RenameUnknownFile, "The project has no file named '" + file + "'.", file));
		return plan;
	}
	plan.path = asset->relative_path;
	plan.old_name = asset->logical_name;
	const std::string dir = fs::path(asset->relative_path).parent_path().generic_string();
	plan.new_path = (fs::path(dir) / new_name).generic_string();
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (!asset->imported_from.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::RenameImported, asset->logical_name + " is imported from " + asset->imported_from +
		                                ": rename the source instead.", asset->relative_path));
	} else if (!check_project_file_name(paths.root, dir, new_name, asset->kind, problem, message)) {
		const CoreFinding code = problem == FileNameProblem::Kind   ? CoreFinding::RenameKind
		                         : problem == FileNameProblem::Path ? CoreFinding::RenamePath
		                                                            : CoreFinding::RenameName;
		plan.refusals.push_back(refusal(code, message, asset->relative_path));
	} else if (archive_name_limit_binds(asset->kind) &&
			!logical_name_fits_archive(new_name)) {
		// check_project_file_name takes Unknown for a kind not decided yet (an import before its
		// bytes are read); this file's is decided, none the game knows, and the build packs it all
		// the same (route_asset), so the archives' name limit binds it as any packed kind's.
		plan.refusals.push_back(refusal(CoreFinding::RenameName,
				"'" + new_name +
						"' does not fit the game's archives: names are up to 16 characters.",
				asset->relative_path));
	} else if (extension_of(new_name) != extension_of(asset->logical_name)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameKind, "Keep the extension: a file's kind comes from it.", asset->relative_path));
	} else if (const AssetEntry *taken = scan.find(new_name); taken && taken != asset) {
		plan.refusals.push_back(refusal(CoreFinding::RenameExists, "The project already has a file named '" + new_name + "'.",
		                                taken->relative_path));
	}
	// Every field naming `target` (the file, or an output renamed with it), planned as a
	// site that takes `renamed`, or a refusal.
	const auto plan_sites = [&](const AssetEntry &target, const std::string &renamed) {
		std::vector<const GraphEdge *> through_styles;
		for (const GraphEdge *edge : graph.referrers_of_file(target.relative_path)) {
			// Of two files of one name, a reference reaches the one the game finds (the scan's
			// first): renaming the other rewrites none.
			std::string resolved;
			graph.resolve(*edge, &resolved);
			if (resolved != target.relative_path) continue;
			const std::string where = edge->record.empty() ? edge->source : "'" + edge->record + "' in " + edge->source;
			if (is_style_reference(edge->value)) {
				through_styles.push_back(edge); // after the stylesheets' own sites are planned
				continue;
			}
			if (!edge->rewritable) {
				plan.refusals.push_back(refusal(CoreFinding::RenameSite, where + " names " + target.logical_name +
				                                " and the editor cannot rewrite that file yet.", edge->source, edge->field));
				continue;
			}
			RenameSite site;
			site.file = edge->source;
			if (const AssetEntry *source = find_asset(scan, edge->source)) site.kind = source->kind;
			site.record = edge->record;
			site.locator = edge->locator;
			site.field = edge->field;
			site.before = edge->value;
			site.after = replacement(edge->value, target.logical_name, renamed);
			// The spelling the site wrote with the new stem, else the new name: one that loads the
			// renamed file the same way, or the rename is refused.
			if (!loads_renamed(scan, *edge, site.after, target, renamed)) site.after = renamed;
			if (!loads_renamed(scan, *edge, site.after, target, renamed)) {
				plan.refusals.push_back(refusal(CoreFinding::RenameSite, where + " names " + target.logical_name + " as '" + edge->value +
				                                "', and no spelling of " + renamed + " there would load it the same way.",
				                                edge->source, edge->field));
				continue;
			}
			site.target = target.relative_path;
			plan.sites.push_back(std::move(site));
		}
		// A field naming the file through a style variable keeps the variable: the site is
		// the variable's value in the stylesheet the game reads it from, when this rename
		// rewrites it; otherwise (a value without the extension, a stylesheet it cannot
		// rewrite) the rename is refused.
		for (const GraphEdge *edge : through_styles) {
			const GraphSymbol *binding = graph.style_binding(edge->value);
			const bool planned = binding && std::any_of(plan.sites.begin(), plan.sites.end(), [&](const RenameSite &site) {
				return site.file == binding->file && site.record == binding->record && site.field == "value";
			});
			if (planned) continue;
			const std::string where = edge->record.empty() ? edge->source : "'" + edge->record + "' in " + edge->source;
			plan.refusals.push_back(refusal(
			        CoreFinding::RenameStyle,
			        where + " names " + target.logical_name + " through " + edge->value + ", whose value " +
			                (binding ? "in " + binding->file + " " : std::string()) +
			                "this rename cannot rewrite (a name without its extension): change the variable instead.",
			        edge->source, edge->field));
		}
	};
	plan_sites(*asset, new_name);
	// An import source takes its record and the outputs its importer names after it
	// (ADR 0046 d6): the sidecar moves beside the new name, each such output is renamed
	// with every site naming it rewritten, and the old outputs go; the next import pass
	// makes them again under the new names.
	if (importer_for(asset->logical_name)) {
		std::error_code ec;
		if (fs::is_regular_file(fs::path(paths.root) / (asset->relative_path + kImportSidecarSuffix), ec)) {
			plan.sidecar = asset->relative_path + kImportSidecarSuffix;
			plan.new_sidecar = plan.new_path + kImportSidecarSuffix;
		}
		plan.output_dir = import_output_dir(paths, asset->relative_path);
		plan.new_output_dir = import_output_dir(paths, plan.new_path);
		for (const AssetEntry &output : scan.entries) {
			if (output.imported_from != asset->relative_path) continue;
			RenameOutput renamed;
			renamed.path = output.relative_path;
			renamed.old_name = output.logical_name;
			renamed.new_name = renamed_import_output(output.logical_name, asset->logical_name, new_name);
			if (renamed.new_name != renamed.old_name) {
				const AssetEntry *taken = scan.find(renamed.new_name);
				if (!logical_name_fits_archive(renamed.new_name)) {
					plan.refusals.push_back(refusal(CoreFinding::RenameName, "The import output " + renamed.new_name +
					                                " would not fit the game's archives: names are up to 16 characters.",
					                                asset->relative_path));
				} else if (taken && key(taken->logical_name) != key(renamed.old_name)) {
					plan.refusals.push_back(refusal(CoreFinding::RenameExists, "The project already has a file named '" + renamed.new_name +
					                                "', the name the import output of " + new_name + " would take.",
					                                taken->relative_path));
				}
				plan_sites(output, renamed.new_name);
			}
			plan.outputs.push_back(std::move(renamed));
		}
	}
	return plan;
}

bool apply_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                  const AssetGraph &graph, const RenamePlan &plan, std::vector<Diagnostic> &findings) {
	if (!plan.ok()) {
		findings.insert(findings.end(), plan.refusals.begin(), plan.refusals.end());
		return false;
	}
	const fs::path old_path = fs::path(paths.root) / plan.path;
	const fs::path new_path = fs::path(paths.root) / plan.new_path;
	std::error_code ec;
	const bool same_file = key(plan.old_name) == key(plan.new_name);
	if (!same_file) {
		if (fs::exists(new_path, ec)) {
			findings.push_back(refusal(CoreFinding::RenameExists, "A file already sits at '" + plan.new_path + "'.", plan.new_path));
			return false;
		}
		fs::copy_file(old_path, new_path, ec);
		if (ec) {
			findings.push_back(refusal(CoreFinding::RenameCopy, "The file could not be copied to its new name: " + ec.message(), plan.path));
			return false;
		}
		// The import record travels with its source (a stray record already at the new
		// name, whose source was never there, is replaced).
		if (!plan.sidecar.empty()) {
			fs::copy_file(fs::path(paths.root) / plan.sidecar, fs::path(paths.root) / plan.new_sidecar,
			              fs::copy_options::overwrite_existing, ec);
			if (ec) {
				findings.push_back(refusal(CoreFinding::RenameCopy, "The import record could not be copied to its new name: " +
				                                   ec.message(), plan.sidecar));
				std::error_code ignored;
				fs::remove(new_path, ignored);
				return false;
			}
		}
	}
	// Every referencing document, rewritten through its type: the planned sites are
	// found again on the freshly loaded rows (the same record place, field and value,
	// still resolving to this file), so the plan needs no identities, two records of the
	// same name stay apart, and nothing the plan did not list is touched (a model named
	// like the animation map keeps its name).
	std::map<std::string, std::vector<const RenameSite *>> files;
	for (const RenameSite &site : plan.sites) files[site.file].push_back(&site);
	bool ok = true;
	for (const auto &entry : files) {
		const AssetEntry *asset = find_asset(scan, entry.first);
		const DocumentType *type = asset ? document_type_for(asset->kind) : nullptr;
		std::unique_ptr<Document> document = type ? records_of(type->make()) : nullptr;
		if (!document) {
			findings.push_back(cannot_rewrite(type, entry.first));
			ok = false;
			continue;
		}
		Diagnostic error;
		if (!document->load((fs::path(paths.root) / asset->relative_path).generic_string(), asset->relative_path, asset->kind,
		                    project.target_game, error)) {
			findings.push_back(error);
			ok = false;
			continue;
		}
		Extracted extracted;
		extract_from_document(*document, extracted);
		const std::vector<const RenameSite *> &sites = entry.second;
		std::vector<bool> done(sites.size(), false);
		size_t rewritten = 0;
		for (const GraphEdge &edge : extracted.edges) {
			if (!edge.rewritable) continue;
			size_t match = sites.size();
			for (size_t i = 0; i < sites.size() && match == sites.size(); ++i)
				if (!done[i] && sites[i]->locator == edge.locator && sites[i]->field == edge.field &&
				    sites[i]->before == edge.value)
					match = i;
			if (match == sites.size()) continue;
			std::string resolved;
			if (graph.resolve(edge, &resolved) != ReferenceStatus::Present || resolved != sites[match]->target) continue;
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = edge.address;
			edit.field = edge.field;
			edit.value = sites[match]->after;
			if (!document->apply(edit, error)) {
				findings.push_back(error);
				ok = false;
				continue;
			}
			done[match] = true;
			++rewritten;
		}
		if (rewritten < sites.size()) {
			// Named by what the sites left say (a renamed import output's sites name the
			// output, not the source).
			std::vector<std::string> left;
			for (size_t i = 0; i < sites.size(); ++i)
				if (!done[i] && std::find(left.begin(), left.end(), sites[i]->before) == left.end()) left.push_back(sites[i]->before);
			std::string names;
			for (const std::string &name : left) names += (names.empty() ? "'" : ", '") + name + "'";
			findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error,
			                                entry.first + " still names " + names + " in " +
			                                        std::to_string(sites.size() - rewritten) + " of its " +
			                                        std::to_string(sites.size()) + " planned place(s).",
			                                entry.first));
			ok = false;
		}
		if (rewritten && !document->save(error)) {
			findings.push_back(error);
			ok = false;
		}
	}
	if (!ok) {
		findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Warning,
		                                "Both '" + plan.old_name + "' and '" + plan.new_name +
		                                        "' are in the project until every reference is rewritten.",
		                                plan.path));
		return false;
	}
	if (!same_file) {
		fs::remove(old_path, ec);
		if (ec) {
			findings.push_back(refusal(CoreFinding::RenameRemove, "The old file could not be removed: " + ec.message(), plan.path));
			return false;
		}
		if (!plan.sidecar.empty()) fs::remove(fs::path(paths.root) / plan.sidecar, ec);
	} else {
		fs::rename(old_path, new_path, ec);
		if (ec) {
			findings.push_back(refusal(CoreFinding::RenameMove, "The file could not be renamed: " + ec.message(), plan.path));
			return false;
		}
		if (!plan.sidecar.empty()) fs::rename(fs::path(paths.root) / plan.sidecar, fs::path(paths.root) / plan.new_sidecar, ec);
	}
	// The old outputs are disposable: the next import pass makes the new ones. (A rename
	// that only changes the case keeps its output directory, which is keyed case-blind.)
	if (!plan.output_dir.empty() && plan.output_dir != plan.new_output_dir)
		fs::remove_all(fs::path(paths.root) / plan.output_dir, ec);
	return true;
}

SymbolRenamePlan plan_symbol_rename_project(const AssetScan &scan, const AssetGraph &graph, const GraphSymbol &symbol,
                                            const std::string &typed) {
	SymbolRenamePlan plan;
	// A style variable's new name typed as a use writes it (%NAME%) is its NAME.
	const ReferenceKindRow &row = reference_row(symbol.kind);
	std::string new_name =
	        row.spell == NameSpelling::StyleVariable && mns::is_variable_reference(typed) ? mns::variable_name(typed) : typed;
	// A name its defining field holds as a number (an item's id) is that number as the field
	// writes it ("0100301" is 100301), before it is compared with the others.
	std::map<AssetKind, std::unique_ptr<Document>> blanks;
	const AssetEntry *defining = find_asset(scan, symbol.file);
	const FieldSchema *defining_field =
	        defining && !symbol.field.empty() ? site_field(blanks, defining->kind, symbol.address.kind, symbol.field) : nullptr;
	Value number;
	const bool numeric = defining_field && defining_field->type != FieldType::Text;
	const bool is_number = numeric && site_value(*defining_field, new_name, number);
	if (is_number) new_name = std::to_string(std::get<int64_t>(number));
	plan.kind = symbol.kind;
	plan.file = symbol.file;
	plan.locator = symbol.locator;
	plan.field = symbol.field;
	plan.scope = symbol.scope;
	plan.old_name = symbol.display;
	plan.new_name = new_name;
	const std::string what = std::string(row.phrase) + " '" + symbol.display + "'";
	if (new_name.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::RenameName, "Give " + what + " a new name.", symbol.file, symbol.field));
		return plan;
	}
	if (numeric && !is_number) {
		plan.refusals.push_back(refusal(CoreFinding::RenameName, symbol.file + " holds " + what + " as a number: '" + new_name + "' is none.",
		                                symbol.file, symbol.field));
		return plan;
	}
	if (new_name == symbol.display) {
		plan.refusals.push_back(refusal(CoreFinding::RenameUnchanged, what + " already has that name.", symbol.file, symbol.field));
		return plan;
	}
	if (symbol.field.empty() || !defining || !document_type_for(defining->kind)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameSite, symbol.file + " defines " + what + " and the editor cannot rewrite that file yet.",
		                                symbol.file));
		return plan;
	}
	// Another definition of the new name where a lookup would find it (a change of case alone
	// keeps the same name to the lookups).
	const std::string wanted = graph_names::symbol_name(symbol.kind, new_name);
	if (wanted != symbol.name)
		for (const GraphSymbol *other : graph.symbols_named(symbol.kind, new_name))
			if (graph_names::upper(other->scope) == graph_names::upper(symbol.scope)) {
				plan.refusals.push_back(refusal(CoreFinding::RenameExists,
				                                other->file + " already defines " + row.phrase + " '" + other->display + "'" +
				                                        (symbol.scope.empty() ? "" : " in " + symbol.scope) + ".",
				                                other->file, other->field));
				break;
			}
	// The definition, then every use that reaches it.
	RenameSite definition;
	definition.file = symbol.file;
	definition.kind = defining->kind;
	definition.record = symbol.record;
	definition.locator = symbol.locator;
	definition.field = symbol.field;
	definition.before = symbol.display;
	definition.after = new_name;
	definition.target = symbol.file;
	std::vector<std::pair<RenameSite, NodeKind>> sites{{definition, symbol.address.kind}};
	const std::string spelled = row.spell == NameSpelling::StyleVariable ? "%" + new_name + "%" : new_name;
	for (const GraphEdge *edge : graph.users_of(symbol)) {
		const bool listed = std::any_of(sites.begin(), sites.end(), [&](const std::pair<RenameSite, NodeKind> &site) {
			return site.first.file == edge->source && site.first.locator == edge->locator && site.first.field == edge->field;
		});
		if (listed) continue; // a field naming it twice (a font through a variable) is one site
		const std::string where = edge->record.empty() ? edge->source : "'" + edge->record + "' in " + edge->source;
		const AssetEntry *source = find_asset(scan, edge->source);
		if (!edge->rewritable || !source) {
			plan.refusals.push_back(refusal(CoreFinding::RenameSite, where + " names " + what + " and the editor cannot rewrite that file yet.",
			                                edge->source, edge->field));
			continue;
		}
		RenameSite site;
		site.file = edge->source;
		site.kind = source->kind;
		site.record = edge->record;
		site.locator = edge->locator;
		site.field = edge->field;
		site.before = edge->value;
		site.after = spelled;
		site.target = symbol.file;
		sites.push_back({std::move(site), edge->address.kind});
	}
	// Every site's field can hold the new value: within its width, a number where it is one.
	for (const auto &[site, kind] : sites) {
		const FieldSchema *field = site_field(blanks, site.kind, kind, site.field);
		Value value;
		if (!field) {
			plan.refusals.push_back(refusal(CoreFinding::RenameSite, site_where(site) + " has no field " + site.field + " the editor writes.",
			                                site.file, site.field));
		} else if (!site_value(*field, site.after, value)) {
			plan.refusals.push_back(refusal(CoreFinding::RenameName, site_where(site) + " holds " + what + " as a number: '" + site.after +
			                                        "' is none.",
			                                site.file, site.field));
		} else if (field->type == FieldType::Text && field->width && site.after.size() >= field->width) {
			plan.refusals.push_back(refusal(CoreFinding::RenameTooLong,
			                                site_where(site) + " holds at most " + std::to_string(field->width - 1) +
			                                        " characters in its " + site.field + ": '" + site.after + "' has " +
			                                        std::to_string(site.after.size()) + ".",
			                                site.file, site.field));
		}
	}
	for (auto &site : sites) plan.sites.push_back(std::move(site.first));
	return plan;
}

namespace {

// Each file of a plan read (an open document as it stands, else the file on disk) and its sites
// set through its type, nothing written: the documents as they would be saved, or false with the
// findings (a file that does not read, a site its record or its field refuses, a site no longer
// there or no longer reaching the definition, a file that would not write). Every site of every
// file is tried, so the findings name each.
bool stage_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan,
                         const std::vector<std::shared_ptr<const DocumentBase>> &open,
                         std::vector<std::unique_ptr<Document>> &staged, std::vector<Diagnostic> &findings) {
	std::map<std::string, std::vector<const RenameSite *>> files;
	for (const RenameSite &site : plan.sites) files[site.file].push_back(&site);
	// A use still reaches the renamed definition: the one the graph's lookup returns for it.
	const auto reaches = [&](const GraphEdge &edge) {
		const GraphSymbol *found = graph.resolve_symbol(edge.kind, edge.value, edge.scope);
		return found && found->file == plan.file && found->locator == plan.locator && found->field == plan.field;
	};
	bool ok = true;
	for (const auto &[file, sites] : files) {
		const AssetEntry *asset = find_asset(scan, file);
		const DocumentType *type = asset ? document_type_for(asset->kind) : nullptr;
		std::unique_ptr<Document> document = type ? records_of(type->make()) : nullptr;
		if (!document) {
			findings.push_back(cannot_rewrite(type, file));
			ok = false;
			continue;
		}
		Diagnostic error;
		const Document *as_open = nullptr;
		for (const auto &candidate : open)
			if (candidate && candidate->path() == asset->relative_path)
				as_open = records_of(*candidate);
		const SerializeResult current = as_open ? as_open->serialize() : SerializeResult();
		// An open document that would not write as it stands is the file the rename meets (its
		// unsaved edits must be saved first, and that Save would fail): never the older file
		// on disk in its place.
		if (as_open && !current.ok()) {
			findings.push_back(refusal(CoreFinding::RenameSite,
			                           file + " as it stands in the editor would not write: " +
			                                   current.issues.front().message,
			                           file, current.issues.front().field));
			ok = false;
			continue;
		}
		const bool loaded = as_open
		                            ? document->load_bytes(std::vector<uint8_t>(current.text.begin(), current.text.end()),
		                                                   asset->relative_path, asset->kind, project.target_game, error)
		                            : document->load((fs::path(paths.root) / asset->relative_path).generic_string(),
		                                             asset->relative_path, asset->kind, project.target_game, error);
		if (!loaded) {
			findings.push_back(error);
			ok = false;
			continue;
		}
		// Every site found on the rows as read, before any is set (a set keeps the records'
		// identities, so the addresses hold).
		Extracted extracted;
		extract_from_document(*document, extracted);
		std::vector<NodeAddress> found(sites.size());
		std::vector<bool> matched(sites.size(), false);
		for (size_t i = 0; i < sites.size(); ++i) {
			const RenameSite &site = *sites[i];
			if (file == plan.file && site.locator == plan.locator && site.field == plan.field) {
				for (const GraphSymbol &defined : extracted.symbols)
					if (!matched[i] && defined.locator == site.locator && defined.field == site.field && defined.display == site.before) {
						found[i] = defined.address;
						matched[i] = true;
					}
				continue;
			}
			for (const GraphEdge &edge : extracted.edges)
				if (!matched[i] && edge.rewritable && edge.locator == site.locator && edge.field == site.field &&
				    edge.value == site.before && reaches(edge)) {
					found[i] = edge.address;
					matched[i] = true;
				}
		}
		size_t rewritten = 0;
		for (size_t i = 0; i < sites.size(); ++i) {
			if (!matched[i]) continue;
			const RenameSite &site = *sites[i];
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = found[i];
			edit.field = site.field;
			const FieldSchema *field = nullptr;
			for (const FieldSchema &schema : document->fields(found[i].kind))
				if (schema.id == site.field) field = &schema;
			error = Diagnostic();
			if (!field || !site_value(*field, site.after, edit.value) || !document->apply(edit, error)) {
				Diagnostic refused = refusal(CoreFinding::RenameSite,
				                             site_where(site) + " cannot take '" + site.after + "'" +
				                                     (error.message.empty() ? std::string(".") : ": " + error.message),
				                             file, site.field);
				findings.push_back(std::move(refused));
				ok = false;
				continue;
			}
			// The field holds what was planned, not a form its setter made of it (a stylesheet's
			// name trimmed): the uses are written with the planned spelling, and the name the
			// other definitions were compared with is that one.
			Value stored;
			if (!document->get(found[i], site.field, stored) || stored != edit.value) {
				findings.push_back(refusal(CoreFinding::RenameName,
				                           site_where(site) + " would hold '" + value_text(stored) + "', not '" +
				                                   site.after + "': give the name as it is written there.",
				                           file, site.field));
				ok = false;
				continue;
			}
			++rewritten;
		}
		if (rewritten < sites.size()) {
			findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error,
			                                file + " would still name '" + plan.old_name + "' in " +
			                                        std::to_string(sites.size() - rewritten) + " of its " +
			                                        std::to_string(sites.size()) + " planned place(s).",
			                                file));
			ok = false;
		}
		// What it would write: a text the format carries.
		const SerializeResult written = document->serialize();
		if (!written.ok()) {
			findings.push_back(refusal(CoreFinding::RenameSite, file + " would not write with the new name: " +
			                                                  written.issues.front().message,
			                           file, written.issues.front().field));
			ok = false;
		}
		staged.push_back(std::move(document));
	}
	return ok;
}

} // namespace

bool check_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan,
                         const std::vector<std::shared_ptr<const DocumentBase>> &open,
                         std::vector<Diagnostic> &findings) {
	if (!plan.ok()) return false;
	std::vector<std::unique_ptr<Document>> staged;
	return stage_symbol_rename(paths, project, scan, graph, plan, open, staged, findings);
}

bool apply_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan, std::vector<Diagnostic> &findings,
                         const FileReplace &replace) {
	if (!plan.ok()) {
		findings.insert(findings.end(), plan.refusals.begin(), plan.refusals.end());
		return false;
	}
	// Every file rewritten in memory first: one that refuses leaves every file as it was.
	std::vector<std::unique_ptr<Document>> staged;
	if (!stage_symbol_rename(paths, project, scan, graph, plan, {}, staged, findings)) return false;
	// Then written as one: a file changed since it was read is refused as a Save refuses it,
	// and one the system will not replace (write-protected) puts back the ones replaced before.
	std::vector<FileText> writes;
	for (const auto &document : staged) {
		if (!document->matches_file()) {
			findings.push_back(refusal(CoreFinding::RenameConflict, document->path() + " changed outside the editor. Nothing was renamed.",
			                           document->path()));
			return false;
		}
		writes.push_back({(fs::path(paths.root) / document->path()).generic_string(), document->serialize().text});
	}
	std::vector<std::string> problems;
	if (write_files_together(writes, problems, replace)) return true;
	findings.push_back(refusal(CoreFinding::RenameWrite, "Nothing was renamed: " + problems.front(), plan.file));
	for (size_t i = 1; i < problems.size(); ++i) findings.push_back(refusal(CoreFinding::RenamePartial, problems[i], plan.file));
	return false;
}

} // namespace opennova::editor
