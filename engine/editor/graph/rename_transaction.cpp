#include <editor/graph/rename_transaction.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <system_error>
#include <variant>

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_file_set.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/native_text_sites.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::key;
std::string extension_of(const std::string &name) { return key(utf8_of(path_of(name).extension())); }
std::string stem_of(const std::string &name) { return utf8_of(path_of(name).stem()); }

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
	if (edge.kind != ReferenceKind::Texture || !texture_arg_is_row_type(edge.loader_arg)) return true;
	const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(edge.loader_arg));
	return renderer::material_texture_transform(type, value, true) == renderer::material_texture_transform(type, edge.value, true);
}

Diagnostic refusal(CoreFinding code, const std::string &message, const std::string &asset = std::string(),
                   const std::string &field = std::string()) {
	return make_finding(code, DiagnosticSeverity::Error, message, asset, field);
}

// Why a file's sites cannot be rewritten: its kind has no editor, or its editor's documents hold
// neither records a rename sets nor a text whose spans it replaces (S13 D6, D9).
Diagnostic cannot_rewrite(const DocumentType *type, const std::string &file) {
	if (!type) return refusal(CoreFinding::RenameSite, file + " has no editor to rewrite it.", file);
	return refusal(CoreFinding::RenameSite, file + " holds no records for a rename to rewrite.", file);
}

// A field of a record kind of the document type a kind of file opens with, from the type's
// schema (DocumentType::fields: a type's fields never depend on a file's content).
const FieldSchema *site_field(AssetKind file, NodeKind kind, const std::string &id) {
	const DocumentType *type = document_type_for(file);
	if (!type || !type->fields) return nullptr;
	for (const FieldSchema &field : type->fields(kind))
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

// Every field naming `target` (the file, or an output or a companion renamed with it), planned as a
// site that takes `renamed`, or a refusal; the edges of `derived_from` pass (a mission names its
// companions by its own name, which the rename changes with it); where `only` is given, the fields of
// the files it lists alone (a split's).
void plan_sites(RenamePlan &plan, const AssetScan &scan, const AssetGraph &graph, const AssetEntry &target,
                const std::string &renamed, const std::string &derived_from, const std::vector<std::string> *only) {
	std::vector<const GraphEdge *> through_styles;
	for (const GraphEdge *edge : graph.referrers_of_file(target.relative_path)) {
		if (!derived_from.empty() && edge->source == derived_from) continue;
		if (only && std::find(only->begin(), only->end(), edge->source) == only->end()) continue;
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
			// A flipbook's frame is named from its graphic's name, which names every frame
			// (extractors.cpp): no one name in the file is the frame's.
			const bool frame = edge->kind == ReferenceKind::Texture && edge->field.find('[') != std::string::npos;
			plan.refusals.push_back(refusal(CoreFinding::RenameSite,
			                                frame ? where + " names " + target.logical_name + " as a frame of " + edge->field.substr(0, edge->field.find('[')) +
			                                                ", whose frames are named from the graphic's name: rename the frames together and change the graphic."
			                                      : where + " names " + target.logical_name + " and the editor cannot rewrite that file yet.",
			                                edge->source, edge->field));
			continue;
		}
		RenameSite site;
		site.file = edge->source;
		if (const AssetEntry *source = find_asset(scan, edge->source)) site.kind = source->kind;
		site.record = edge->record;
		site.record_title = edge->record_title;
		site.record_kind = edge->address.kind;
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
		// A native text is the game's code page (Windows-1252): a name it has no character of cannot be written
		// there (stage_native writes the new name in it).
		std::string stored;
		if (native_text_kind(site.kind) && !utf8_to_cp1252(site.after, stored)) {
			plan.refusals.push_back(refusal(CoreFinding::RenameName,
			                                edge->source + " is written in the game's code page (Windows-1252), which has no "
			                                               "character of '" + site.after + "' there.",
			                                edge->source, edge->field));
			continue;
		}
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
}

} // namespace

std::string companion_path(const RenameOutput &companion) {
	return utf8_of(path_of(companion.path).parent_path() / path_of(companion.new_name));
}

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
	const std::string dir = utf8_of(path_of(asset->relative_path).parent_path());
	plan.new_path = join_path(dir, new_name);
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
	} else if (extension_of(new_name) != extension_of(asset->logical_name)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameKind, "Keep the extension: a file's kind comes from it.", asset->relative_path));
	} else if (const AssetEntry *taken = scan.find(new_name); taken && taken != asset) {
		plan.refusals.push_back(refusal(CoreFinding::RenameExists, "The project already has a file named '" + new_name + "'.",
		                                taken->relative_path));
	}
	plan_sites(plan, scan, graph, *asset, new_name, std::string(), nullptr);
	// A mission takes the files the game finds by its name that the project has (ADR 0046 S14): each
	// to the new base name with its own extension, held to the file name rules, its sites planned (its
	// mission's own, derived from the name, pass). An import's output goes only with its source (the
	// next import pass makes it again under the old name). A file the game would find by the new name
	// already (one of no mission, which the renamed mission would take as its own) refuses the rename.
	if (asset->kind == AssetKind::Mission && strutil::ends_with_icase(asset->logical_name, ".bms")) {
		for (const MissionFileSetMember &member : mission_file_set_members(scan, asset->logical_name, new_name)) {
			const AssetEntry *entry = find_asset(scan, member.path);
			if (!entry) continue;
			if (!entry->imported_from.empty()) {
				plan.refusals.push_back(refusal(CoreFinding::RenameImported, entry->logical_name + " goes with the mission, and is imported from " +
				                                                                     entry->imported_from + ": rename the source first.",
				                                entry->relative_path));
				continue;
			}
			const std::string member_dir = utf8_of(path_of(entry->relative_path).parent_path());
			FileNameProblem member_problem = FileNameProblem::None;
			std::string why;
			if (!check_project_file_name(paths.root, member_dir, member.new_name, entry->kind, member_problem, why)) {
				plan.refusals.push_back(refusal(CoreFinding::RenameName, entry->logical_name + " goes with the mission, and " +
				                                                                 why, entry->relative_path));
				continue;
			}
			plan_sites(plan, scan, graph, *entry, member.new_name, asset->relative_path, nullptr);
			plan.companions.push_back({entry->relative_path, entry->logical_name, member.new_name});
		}
		for (const MissionFileSetMember &stale : mission_file_set_members(scan, new_name, new_name)) {
			// A name its case alone changes finds the mission's own files.
			if (std::any_of(plan.companions.begin(), plan.companions.end(),
			                [&](const RenameOutput &companion) { return companion.path == stale.path; }))
				continue;
			const MissionFileSetRow *row = mission_file_set_row(stale.role);
			std::string what = row ? row->words : "file";
			if (what.rfind("its ", 0) == 0) what.erase(0, 4);
			plan.refusals.push_back(refusal(CoreFinding::RenameExists,
			                                "The project already has " + stale.old_name + ", which the game would take as " + new_name +
			                                        "'s " + what + ": rename or remove it first.",
			                                stale.path));
		}
	}
	// An import source takes its record and the outputs its importer names after it
	// (ADR 0046 d6): the sidecar moves beside the new name, each such output is renamed
	// with every site naming it rewritten, and the old outputs go; the next import pass
	// makes them again under the new names.
	if (importer_for(asset->logical_name)) {
		std::error_code ec;
		if (fs::is_regular_file(system_path(join_path(paths.root, asset->relative_path + kImportSidecarSuffix)), ec)) {
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
				plan_sites(plan, scan, graph, output, renamed.new_name, std::string(), nullptr);
			}
			plan.outputs.push_back(std::move(renamed));
		}
	}
	return plan;
}

RenamePlan plan_split(const ProjectPaths &paths, const AssetScan &scan, const AssetGraph &graph, const std::string &file,
                      const std::string &new_name, const std::vector<std::string> &referrers, const BaseNames *base) {
	RenamePlan plan;
	plan.split = true;
	plan.new_name = new_name;
	const AssetEntry *asset = find_asset(scan, file);
	if (!asset) {
		plan.refusals.push_back(refusal(CoreFinding::RenameUnknownFile, "The project has no file named '" + file + "'.", file));
		return plan;
	}
	plan.path = asset->relative_path;
	plan.old_name = asset->logical_name;
	if (asset->kind == AssetKind::ImportSource) {
		plan.refusals.push_back(refusal(CoreFinding::TextureSplit, asset->logical_name +
		                                " is an import's source, the image it reads: split the file it makes.", asset->relative_path));
		return plan;
	}
	const bool output = !asset->imported_from.empty();
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	const std::string dir = utf8_of(path_of(asset->relative_path).parent_path());
	const bool named = output ? check_file_name(new_name, asset->kind, problem, message)
	                          : check_project_file_name(paths.root, dir, new_name, asset->kind, problem, message);
	if (!named) {
		plan.refusals.push_back(refusal(problem == FileNameProblem::Kind ? CoreFinding::RenameKind : CoreFinding::RenameName, message,
		                                asset->relative_path));
		return plan;
	}
	if (extension_of(new_name) != extension_of(asset->logical_name)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameKind, "Keep the extension: a copy reads as its file does.", asset->relative_path));
		return plan;
	}
	if (const AssetEntry *taken = scan.find(new_name)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameExists, "The project already has a file named '" + new_name + "'.",
		                                taken->relative_path));
		return plan;
	}
	if (base && base->has(new_name)) {
		plan.refusals.push_back(refusal(CoreFinding::RenameExists,
		                                "The base game has a file named '" + new_name +
		                                        "': a copy of that name would stand in for it for every use. Choose another name.",
		                                asset->relative_path));
		return plan;
	}
	if (output) {
		// The source copied beside it, under a name of its own the scan has not (an import source's name
		// binds as its outputs' would: up to 16 characters).
		const std::string source_dir = utf8_of(path_of(asset->imported_from).parent_path());
		const std::string source_extension = utf8_of(path_of(asset->imported_from).extension());
		const std::string stem = stem_of(new_name);
		for (int n = 0; n < 100 && plan.split_source.empty(); ++n) {
			const std::string suffix = n == 0 ? std::string() : "_" + std::to_string(n + 1);
			const size_t room = size_t(pff::PFF_NAME_SIZE) - suffix.size() - source_extension.size();
			const std::string name = stem.substr(0, std::min(stem.size(), room)) + suffix + source_extension;
			if (key(name) != key(new_name) && !scan.find(name)) plan.split_source = join_path(source_dir, name);
		}
		if (plan.split_source.empty()) {
			plan.refusals.push_back(refusal(CoreFinding::TextureSplit, "No free name for a copy of " + asset->imported_from + ".",
			                                asset->relative_path));
			return plan;
		}
		plan.sidecar = plan.split_source + kImportSidecarSuffix;
		plan.new_sidecar = plan.sidecar;
	} else {
		plan.new_path = join_path(dir, new_name);
	}
	if (referrers.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::TextureSplit, "Name the files whose uses the copy takes.", asset->relative_path));
		return plan;
	}
	plan_sites(plan, scan, graph, *asset, new_name, std::string(), &referrers);
	if (plan.sites.empty() && plan.refusals.empty())
		plan.refusals.push_back(refusal(CoreFinding::TextureSplit, "None of the files named uses " + asset->logical_name + ".",
		                                asset->relative_path));
	return plan;
}

bool apply_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                  const AssetGraph &graph, const RenamePlan &plan, std::vector<Diagnostic> &findings) {
	RenameTransaction transaction(paths, project, scan, graph, plan);
	while (!transaction.step()) {
	}
	findings.insert(findings.end(), transaction.findings().begin(), transaction.findings().end());
	return transaction.ok();
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
	const AssetEntry *defining = find_asset(scan, symbol.file);
	const FieldSchema *defining_field =
	        defining && !symbol.field.empty() ? site_field(defining->kind, symbol.address.kind, symbol.field) : nullptr;
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
	// A use that names the new name first and reaches another definition now through its lookup's
	// second name (a script's AMMO_satchel reaching ammo_satchel while nothing defines satchel): the
	// renamed definition would take it over, its lookup finding the first name first [orig:
	// WacScript_ResolveParameter @ 0x4F2E21..0x4F2E92], a change no site rewrites.
	for (const GraphEdge *edge : graph.edges_naming(symbol.kind, new_name)) {
		if (graph_names::symbol_name(edge->kind, edge->value) != wanted || !scope_matches(symbol.scope, edge->scope))
			continue;
		const GraphSymbol *reached = graph.symbol_reached(*edge);
		if (!reached || reached == &symbol) continue;
		const std::string where = edge->record.empty() ? edge->source + (edge->locator.empty() ? "" : " at " + edge->locator)
		                                               : "'" + edge->record + "' in " + edge->source;
		plan.refusals.push_back(refusal(CoreFinding::RenameExists,
		                                where + " names '" + edge->value + "', which reaches " + row.phrase + " '" +
		                                        reached->display + "' of " + reached->file + " through '" + edge->fallback +
		                                        "' now: renamed '" + new_name + "', " + what +
		                                        " would take that use over, its lookup finding the first name first.",
		                                edge->source, edge->field));
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
		if (!graph.rewrites(*edge) || !source) {
			// A text's use is at its span; the file is the editor's to write, that use not (a key of a
			// script that runs with whichever mission's table plays: game.wac's, server.wac's, one of a
			// mission the project lacks, AssetGraph::rewrites).
			plan.refusals.push_back(refusal(CoreFinding::RenameSite,
			                                edge->span.line ? where + " names " + what + " at " + edge->locator +
			                                                          ", a use the editor cannot rewrite yet."
			                                                : where + " names " + what + " and the editor cannot rewrite that file yet.",
			                                edge->source, edge->field));
			continue;
		}
		RenameSite site;
		site.file = edge->source;
		site.kind = source->kind;
		site.record = edge->record;
		site.record_title = edge->record_title;
		site.record_kind = edge->address.kind;
		site.locator = edge->locator;
		site.field = edge->field;
		site.span = edge->span;
		site.before = edge->value;
		site.after = spelled;
		site.target = symbol.file;
		// A use whose lookup takes a second name (its fallback: a script's AMMO_X, then "ammo_X"):
		// where the new name begins with the prefix that second name puts before the value, the span
		// takes the rest, the prefix kept once ("ammo_satchel" renamed "ammo_charge" reads
		// AMMO_charge), when nothing else defines the rest (its lookup's first name then finds
		// nothing, its second the renamed definition); else the whole new name, which its first name
		// finds.
		if (!edge->fallback.empty() && edge->fallback.size() > edge->value.size() &&
		    strutil::iequals(std::string_view(edge->fallback).substr(edge->fallback.size() - edge->value.size()),
		                     edge->value)) {
			const std::string prefix = edge->fallback.substr(0, edge->fallback.size() - edge->value.size());
			if (new_name.size() > prefix.size() && strutil::starts_with_icase(new_name, prefix)) {
				const std::string rest = new_name.substr(prefix.size());
				bool defined = false;
				for (const GraphSymbol *other : graph.symbols_named(symbol.kind, rest))
					defined = defined || other != &symbol;
				if (!defined) site.after = rest;
			}
		}
		sites.push_back({std::move(site), edge->address.kind});
	}
	// Every site's field can hold the new value: within its width, a number where it is one. A
	// text's span takes any name the game's code page holds; what its text reads back is checked
	// as it is rewritten (check_symbol_rename).
	for (const auto &[site, kind] : sites) {
		if (site.span.line) {
			std::string stored;
			std::u32string unstorable;
			if (!utf8_to_cp1252(site.after, stored, &unstorable))
				plan.refusals.push_back(refusal(CoreFinding::RenameName,
				                                site.file + " is written in the game's code page (Windows-1252), "
				                                "which has no character of '" + site.after + "' there.",
				                                site.file));
			continue;
		}
		const FieldSchema *field = site_field(site.kind, kind, site.field);
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

// A document of one of a plan's files, read as the rename meets it: an open one as it stands in the
// editor (its unsaved edits must be saved first, and that Save would have to succeed), else the file
// on disk. Null with the finding when it does not read, or the open one would not write.
std::unique_ptr<DocumentBase> read_site_file(const ProjectPaths &paths, const ProjectDocument &project,
                                             const AssetEntry &asset, const DocumentType &type,
                                             const std::vector<std::shared_ptr<const DocumentBase>> &open,
                                             const std::string &file, std::vector<Diagnostic> &findings) {
	std::unique_ptr<DocumentBase> document = type.make();
	const DocumentBase *as_open = nullptr;
	for (const auto &candidate : open)
		if (candidate && candidate->path() == asset.relative_path) as_open = candidate.get();
	const SerializeResult current = as_open ? as_open->serialize() : SerializeResult();
	// An open document that would not write as it stands is the file the rename meets (its
	// unsaved edits must be saved first, and that Save would fail): never the older file
	// on disk in its place.
	if (as_open && !current.ok()) {
		findings.push_back(refusal(CoreFinding::RenameSite,
		                           file + " as it stands in the editor would not write: " + current.issues.front().message,
		                           file, current.issues.front().field));
		return nullptr;
	}
	Diagnostic error;
	const bool loaded = as_open
	                            ? document->load_bytes(std::vector<uint8_t>(current.text.begin(), current.text.end()),
	                                                   asset.relative_path, asset.kind, project.target_game, error)
	                            : document->load(join_path(paths.root, asset.relative_path),
	                                             asset.relative_path, asset.kind, project.target_game, error);
	if (!loaded) {
		findings.push_back(error);
		return nullptr;
	}
	return document;
}

// A text document's sites (S13 D9): each span found by its place and the name it held (a use still
// reaching the renamed definition), all replaced in one batch from the last to the first (each
// replacement then leaves the places before it where they were), and each read back from the text
// as the new name at its place. False with the findings.
bool stage_text_sites(TextDocument &document, const std::vector<const RenameSite *> &sites,
                      const std::function<bool(const GraphEdge &)> &reaches, const std::string &old_name,
                      std::vector<Diagnostic> &findings) {
	const std::string &file = document.path();
	Extracted extracted;
	extract_from_text(document, extracted);
	// The names are UTF-8 (the graph's, extract_from_text), the text the game's code page: a site's old
	// name and its new one in the text's own bytes.
	struct Found {
		const RenameSite *site;
		size_t offset;
		std::string after;   // in the document's code page
		size_t length = 0;   // the old name's bytes there
	};
	std::vector<Found> found;
	std::vector<bool> taken(extracted.edges.size(), false);
	bool ok = true;
	for (const RenameSite *site : sites) {
		size_t match = extracted.edges.size();
		for (size_t i = 0; i < extracted.edges.size() && match == extracted.edges.size(); ++i) {
			const GraphEdge &edge = extracted.edges[i];
			if (!taken[i] && edge.locator == site->locator && edge.value == site->before && reaches(edge)) match = i;
		}
		std::string after, before;
		size_t offset = 0;
		if (match == extracted.edges.size() || !utf8_to_cp1252(site->after, after) || !utf8_to_cp1252(site->before, before) ||
		    !document.offset_of(extracted.edges[match].span.line, extracted.edges[match].span.column, offset))
			continue;
		taken[match] = true;
		found.push_back({site, offset, std::move(after), before.size()});
	}
	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.offset > b.offset; });
	std::vector<Edit> edits;
	for (const Found &place : found) {
		const TextSpan span = document.span_at(place.offset, place.length);
		edits.push_back(TextDocument::replace(span, place.after));
	}
	Diagnostic error;
	if (!edits.empty() && !document.apply(edits, error)) {
		findings.push_back(refusal(CoreFinding::RenameSite, file + " cannot take the new name: " + error.message, file));
		return false;
	}
	// Each read back at its place: the places before it moved by what the replacements before it
	// changed.
	Extracted again;
	extract_from_text(document, again);
	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.offset < b.offset; });
	std::ptrdiff_t moved = 0;
	for (const Found &place : found) {
		const TextSpan at = document.span_at(size_t(std::ptrdiff_t(place.offset) + moved), place.after.size());
		const std::string locator = TextDocument::locator(at.line, at.column);
		const bool read = std::any_of(again.edges.begin(), again.edges.end(), [&](const GraphEdge &edge) {
			return edge.locator == locator && edge.value == place.site->after;
		});
		if (!read) {
			findings.push_back(refusal(CoreFinding::RenameName,
			                           file + " at " + place.site->locator + " would not read '" + place.site->after +
			                                   "' as one name there: give a name its text reads whole.",
			                           file));
			ok = false;
		}
		moved += std::ptrdiff_t(place.after.size()) - std::ptrdiff_t(place.length);
	}
	if (found.size() < sites.size()) {
		findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error,
		                                file + " would still name '" + old_name + "' in " +
		                                        std::to_string(sites.size() - found.size()) + " of its " +
		                                        std::to_string(sites.size()) + " planned place(s).",
		                                file));
		ok = false;
	}
	return ok;
}

// One file of a name's rename read (an open document among `open` as it stands, else the file on
// disk) and its sites set through its type, nothing written: the document as it would be saved
// (null when it did not read), and false with the findings when a site did not take (a file that
// does not read, a site its record or its field refuses, a site no longer there or no longer
// reaching the definition, a file that would not write). A text document's sites are its spans
// (S13 D9: stage_text_sites). Every site is tried, so the findings name each.
bool stage_symbol_file(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                       const AssetGraph &graph, const SymbolRenamePlan &plan,
                       const std::vector<std::shared_ptr<const DocumentBase>> &open, const std::string &file,
                       const std::vector<const RenameSite *> &sites, std::unique_ptr<DocumentBase> &staged,
                       std::vector<Diagnostic> &findings) {
	// A use still reaches the renamed definition: the one the graph's lookup returns for it (through
	// its fallback where its value finds nothing).
	const auto reaches = [&](const GraphEdge &edge) {
		const GraphSymbol *found = graph.symbol_reached(edge);
		return found && found->file == plan.file && found->locator == plan.locator && found->field == plan.field;
	};
	const AssetEntry *asset = find_asset(scan, file);
	const DocumentType *type = asset ? document_type_for(asset->kind) : nullptr;
	const DocumentContent content = type ? document_content(*type) : DocumentContent::Other;
	// An image (a texture, S18) names nothing either.
	if (content == DocumentContent::Other || content == DocumentContent::Image) {
		findings.push_back(cannot_rewrite(type, file));
		return false;
	}
	std::unique_ptr<DocumentBase> read = read_site_file(paths, project, *asset, *type, open, file, findings);
	if (!read) return false;
	bool ok = true;
	if (content == DocumentContent::Text) {
		ok = stage_text_sites(*text_of(*read), sites, reaches, plan.old_name, findings);
		const SerializeResult written = read->serialize();
		if (!written.ok()) {
			findings.push_back(refusal(CoreFinding::RenameSite, file + " would not write with the new name: " +
			                                                  written.issues.front().message,
			                           file, written.issues.front().field));
			ok = false;
		}
		staged = std::move(read);
		return ok;
	}
	std::unique_ptr<Document> document = records_of(std::move(read));
	Diagnostic error;
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
		findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error, file + " would still name '" + plan.old_name + "' in " +
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
	staged = std::move(document);
	return ok;
}

// The sites of a plan by file, in the order of the files' paths.
std::map<std::string, std::vector<const RenameSite *>> sites_by_file(const std::vector<RenameSite> &sites) {
	std::map<std::string, std::vector<const RenameSite *>> files;
	for (const RenameSite &site : sites) files[site.file].push_back(&site);
	return files;
}

} // namespace

bool check_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan,
                         const std::vector<std::shared_ptr<const DocumentBase>> &open,
                         std::vector<Diagnostic> &findings) {
	if (!plan.ok()) return false;
	bool ok = true;
	for (const auto &[file, sites] : sites_by_file(plan.sites)) {
		std::unique_ptr<DocumentBase> staged;
		ok = stage_symbol_file(paths, project, scan, graph, plan, open, file, sites, staged, findings) && ok;
	}
	return ok;
}

bool apply_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan, std::vector<Diagnostic> &findings,
                         const FileReplace &replace) {
	RenameTransaction transaction(paths, project, scan, graph, plan, replace);
	while (!transaction.step()) {
	}
	findings.insert(findings.end(), transaction.findings().begin(), transaction.findings().end());
	return transaction.ok();
}

// A file staged for the commit: its document with the sites set (a record document, or a text
// document's spans), null when it did not read; how
// many it took, and (a file's rename) the findings staging it made, which its commit reports in
// the file's turn.
struct RenameTransaction::Staged {
	std::string file;
	std::unique_ptr<DocumentBase> document; // a record document, or a text's (S13 D9)
	// A native text's (graph/native_text_sites.h): its bytes as read, and as the sites leave them.
	bool native = false;
	std::string read;
	std::string text;
	size_t rewritten = 0;
	std::vector<Diagnostic> findings;
};

RenameTransaction::RenameTransaction(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                                     const AssetGraph &graph, RenamePlan plan) :
		paths_(paths), project_(project), scan_(scan), graph_(graph), file_plan_(std::move(plan)) {
	files_ = sites_by_file(file_plan_.sites);
	at_ = files_.begin();
	if (!file_plan_.ok()) {
		findings_ = file_plan_.refusals;
		committed_ = true;
	}
}

RenameTransaction::RenameTransaction(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                                     const AssetGraph &graph, SymbolRenamePlan plan, FileReplace replace) :
		paths_(paths),
		project_(project),
		scan_(scan),
		graph_(graph),
		symbol_(true),
		symbol_plan_(std::move(plan)),
		replace_(std::move(replace)) {
	files_ = sites_by_file(symbol_plan_.sites);
	at_ = files_.begin();
	if (!symbol_plan_.ok()) {
		findings_ = symbol_plan_.refusals;
		committed_ = true;
	}
}

RenameTransaction::~RenameTransaction() = default;

bool RenameTransaction::step() {
	if (committed_) return true;
	if (at_ == files_.end()) {
		commit();
		return true;
	}
	current_ = at_->first;
	stage_file(at_->first, at_->second);
	++at_;
	++next_;
	return false;
}

std::vector<std::string> RenameTransaction::touched() const {
	std::vector<std::string> paths;
	if (!symbol_) {
		for (const std::string *path : {&file_plan_.path, &file_plan_.new_path, &file_plan_.sidecar, &file_plan_.new_sidecar,
		                                 &file_plan_.split_source})
			if (!path->empty()) paths.push_back(*path);
		for (const RenameOutput &companion : file_plan_.companions) {
			paths.push_back(companion.path);
			paths.push_back(companion_path(companion));
		}
	}
	for (const auto &[file, sites] : files_) paths.push_back(file);
	return paths;
}

void RenameTransaction::stage_file(const std::string &file, const std::vector<const RenameSite *> &sites) {
	auto staged = std::make_unique<Staged>();
	staged->file = file;
	if (symbol_) {
		// A name's: every file's findings as it is staged; the commit writes only when each took
		// every site and would write.
		if (!stage_symbol_file(paths_, project_, scan_, graph_, symbol_plan_, {}, file, sites, staged->document, findings_))
			staged_ok_ = false;
		if (staged->document) staged_.push_back(std::move(staged));
		return;
	}
	// A file's: the referencing document rewritten through its type, the planned sites found again
	// on the freshly loaded rows (the same record place, field and value, still resolving to this
	// file), so the plan needs no identities, two records of the same name stay apart, and nothing
	// the plan did not list is touched (a model named like the animation map keeps its name).
	std::vector<Diagnostic> &found = staged->findings;
	const AssetEntry *asset = find_asset(scan_, file);
	if (asset && native_text_kind(asset->kind)) {
		stage_native(*asset, sites, *staged);
		staged_.push_back(std::move(staged));
		return;
	}
	const DocumentType *type = asset ? document_type_for(asset->kind) : nullptr;
	std::unique_ptr<Document> document = type ? records_of(type->make()) : nullptr;
	if (!document) {
		found.push_back(cannot_rewrite(type, file));
		staged_ok_ = false;
		staged_.push_back(std::move(staged));
		return;
	}
	Diagnostic error;
	if (!document->load(join_path(paths_.root, asset->relative_path), asset->relative_path, asset->kind,
	                    project_.target_game, error)) {
		found.push_back(error);
		staged_ok_ = false;
		staged_.push_back(std::move(staged));
		return;
	}
	Extracted extracted;
	extract_from_document(*document, extracted);
	std::vector<bool> done(sites.size(), false);
	for (const GraphEdge &edge : extracted.edges) {
		if (!edge.rewritable) continue;
		size_t match = sites.size();
		for (size_t i = 0; i < sites.size() && match == sites.size(); ++i)
			if (!done[i] && sites[i]->locator == edge.locator && sites[i]->field == edge.field && sites[i]->before == edge.value)
				match = i;
		if (match == sites.size()) continue;
		std::string resolved;
		if (graph_.resolve(edge, &resolved) != ReferenceStatus::Present || resolved != sites[match]->target) continue;
		Edit edit;
		edit.operation = EditOperation::Set;
		edit.address = edge.address;
		edit.field = edge.field;
		edit.value = sites[match]->after;
		if (!document->apply(edit, error)) {
			found.push_back(error);
			staged_ok_ = false;
			continue;
		}
		done[match] = true;
		++staged->rewritten;
	}
	if (staged->rewritten < sites.size()) {
		// Named by what the sites left say (a renamed import output's sites name the
		// output, not the source).
		std::vector<std::string> left;
		for (size_t i = 0; i < sites.size(); ++i)
			if (!done[i] && std::find(left.begin(), left.end(), sites[i]->before) == left.end()) left.push_back(sites[i]->before);
		std::string names;
		for (const std::string &name : left) names += (names.empty() ? "'" : ", '") + name + "'";
		found.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error, file + " still names " + names + " in " + std::to_string(sites.size() - staged->rewritten) +
		                                        " of its " + std::to_string(sites.size()) + " planned place(s).",
		                                file));
		staged_ok_ = false;
	}
	staged->document = std::move(document);
	staged_.push_back(std::move(staged));
}

// A native text's sites (graph/native_text_sites.h): the file read as its loader reads it (one stored in
// the SCR form unwrapped, which the game's text reader takes either way [orig: File_ParseASCIIFile @
// 0x53D860], so it is written back as plain text, as a document's Save writes one); each site found
// again on the file's edges as it reads now (the same record, field and value, still resolving to the
// file the site names), then its token rewritten in the text, nothing written.
void RenameTransaction::stage_native(const AssetEntry &asset, const std::vector<const RenameSite *> &sites, Staged &staged) {
	staged.native = true;
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(join_path(paths_.root, asset.relative_path), bytes, message)) {
		staged.findings.push_back(refusal(CoreFinding::RenameSite, asset.relative_path + " could not be read: " + message, asset.relative_path));
		staged_ok_ = false;
		return;
	}
	staged.read.assign(bytes.begin(), bytes.end());
	if (!vfs_decode_payload(bytes, gameprofile::gameprofile_scr_policy_for_code(project_.target_game.c_str()))) {
		staged.findings.push_back(refusal(CoreFinding::RenameSite, asset.relative_path + " could not be decoded.", asset.relative_path));
		staged_ok_ = false;
		return;
	}
	staged.text.assign(bytes.begin(), bytes.end());
	Extracted now;
	Diagnostic error;
	extract_from_bytes(asset.relative_path, asset.kind, bytes, project_.target_game, now, error);
	std::vector<NativeTextSite> wanted;
	for (const RenameSite *site : sites) {
		const bool there = std::any_of(now.edges.begin(), now.edges.end(), [&](const GraphEdge &edge) {
			std::string resolved;
			return edge.rewritable && edge.record == site->record && edge.field == site->field && edge.value == site->before &&
			       graph_.resolve(edge, &resolved) == ReferenceStatus::Present && resolved == site->target;
		});
		// The names as the graph reads them (UTF-8; rewrite_native_text writes them in the text's code page);
		// a new name the code page cannot hold was refused as the rename was planned.
		std::string stored;
		if (there && utf8_to_cp1252(site->after, stored)) wanted.push_back({site->record, site->field, site->before, site->after});
	}
	std::vector<size_t> missed;
	staged.rewritten = rewrite_native_text(asset.relative_path, asset.kind, project_.target_game, staged.text, wanted, missed);
	if (staged.rewritten < sites.size()) {
		// Named by the sites no token was found for, else (a site the file no longer has) by every site's.
		std::vector<std::string> left;
		const auto keep = [&left](const std::string &name) {
			if (std::find(left.begin(), left.end(), name) == left.end()) left.push_back(name);
		};
		for (size_t i : missed) keep(wanted[i].before);
		if (left.empty())
			for (const RenameSite *site : sites) keep(site->before);
		std::string names;
		for (const std::string &name : left) names += (names.empty() ? "'" : ", '") + name + "'";
		staged.findings.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Error,
		                                       asset.relative_path + " still names " + names + " in " +
		                                               std::to_string(sites.size() - staged.rewritten) + " of its " +
		                                               std::to_string(sites.size()) + " planned place(s).",
		                                       asset.relative_path));
		staged_ok_ = false;
	}
}

void RenameTransaction::commit() {
	committed_ = true;
	if (symbol_) commit_symbol_rename();
	else if (file_plan_.split) commit_split();
	else commit_file_rename();
	staged_.clear();
}

bool RenameTransaction::save_staged() {
	bool ok = staged_ok_;
	for (const auto &staged : staged_) {
		findings_.insert(findings_.end(), staged->findings.begin(), staged->findings.end());
		if (staged->native) {
			// A native text written as the sites left it, unless it changed since it was read.
			if (!staged->rewritten) continue;
			const std::string absolute = join_path(paths_.root, staged->file);
			std::vector<uint8_t> now;
			std::string message;
			if (!read_file_bytes(absolute, now, message) || std::string(now.begin(), now.end()) != staged->read) {
				findings_.push_back(refusal(CoreFinding::RenameConflict, staged->file + " changed outside the editor while it was renamed in.",
				                            staged->file));
				ok = false;
			} else if (!write_file_atomic(absolute, staged->text, message)) {
				findings_.push_back(refusal(CoreFinding::RenameWrite, staged->file + " could not be written: " + message, staged->file));
				ok = false;
			}
			continue;
		}
		Diagnostic error;
		if (staged->document && staged->rewritten && !staged->document->save(error)) {
			findings_.push_back(error);
			ok = false;
		}
	}
	return ok;
}

// A split's commit: the copy made (the file under its new name, dated now; an import's output's source
// copied, its record making the new name with the output's options), then the sites' files written; the
// file itself stays, with every use the split did not take. A copy refused writes nothing else.
void RenameTransaction::commit_split() {
	const RenamePlan &plan = file_plan_;
	const auto at = [this](const std::string &relative) { return system_path(join_path(paths_.root, relative)); };
	std::error_code ec;
	const std::string copy = plan.split_source.empty() ? plan.new_path : plan.split_source;
	const AssetEntry *asset = find_asset(scan_, plan.path);
	const std::string from = plan.split_source.empty() ? plan.path : (asset ? asset->imported_from : std::string());
	if (from.empty() || fs::exists(at(copy), ec)) {
		findings_.push_back(refusal(CoreFinding::RenameExists, from.empty() ? "The file's source is gone." : "A file already sits at '" + copy + "'.",
		                            plan.path));
		return;
	}
	fs::copy_file(at(from), at(copy), ec);
	std::string dated;
	if (ec || !refresh_last_write(utf8_of(at(copy)), dated)) {
		findings_.push_back(refusal(CoreFinding::RenameCopy, "The copy could not be made: " + (ec ? ec.message() : dated), plan.path));
		std::error_code ignored;
		fs::remove(at(copy), ignored);
		return;
	}
	if (!plan.split_source.empty()) {
		ImportSidecar record;
		Diagnostic error;
		load_import_sidecar(join_path(paths_.root, from + kImportSidecarSuffix), record, error);
		ImportSidecar made;
		made.importer = record.importer;
		made.version = record.version;
		made.options = record.options;
		made.options["name"] = plan.new_name;
		if (!save_import_sidecar(join_path(paths_.root, plan.sidecar), made, error)) {
			findings_.push_back(error);
			std::error_code ignored;
			fs::remove(at(copy), ignored);
			return;
		}
	}
	if (!save_staged()) {
		findings_.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Warning,
		                                 copy + " is made, and some of the uses it was to take still name " + plan.old_name + ".",
		                                 plan.path));
		return;
	}
	ok_ = true;
}

// The file copied under its new name (and an import source's record beside it), each rewritten
// document saved in its file's turn with the findings its staging made, then the old file, its
// record and its old outputs removed; a copy refused leaves everything as it was.
void RenameTransaction::commit_file_rename() {
	const RenamePlan &plan = file_plan_;
	// Every call through the system's path (project_files.h): a project file past MAX_PATH renames too.
	const auto at = [this](const std::string &relative) {
		return system_path(join_path(paths_.root, relative));
	};
	const fs::path old_path = at(plan.path);
	const fs::path new_path = at(plan.new_path);
	std::error_code ec;
	const bool same_file = key(plan.old_name) == key(plan.new_name);
	if (!same_file) {
		if (fs::exists(new_path, ec)) {
			findings_.push_back(refusal(CoreFinding::RenameExists, "A file already sits at '" + plan.new_path + "'.", plan.new_path));
			return;
		}
		fs::copy_file(old_path, new_path, ec);
		if (ec) {
			findings_.push_back(refusal(CoreFinding::RenameCopy, "The file could not be copied to its new name: " + ec.message(), plan.path));
			return;
		}
		// A copy keeps the last write of the file it came from, so a cache that knows a file by its
		// size and last write (the build's, the import's) would take it for the file that held the
		// name before (two of a size swapping names): it is dated now (S13 A8).
		std::string dated;
		if (!refresh_last_write(utf8_of(new_path), dated)) {
			findings_.push_back(refusal(CoreFinding::RenameCopy, "The file could not be copied to its new name: " + dated, plan.path));
			std::error_code ignored;
			fs::remove(new_path, ignored);
			return;
		}
		// The import record travels with its source (a stray record already at the new
		// name, whose source was never there, is replaced).
		if (!plan.sidecar.empty()) {
			fs::copy_file(at(plan.sidecar), at(plan.new_sidecar),
			              fs::copy_options::overwrite_existing, ec);
			if (ec) {
				findings_.push_back(refusal(CoreFinding::RenameCopy, "The import record could not be copied to its new name: " +
				                                    ec.message(), plan.sidecar));
				std::error_code ignored;
				fs::remove(new_path, ignored);
				return;
			}
		}
		// A mission's companions copied under their new names with it (each dated now, as the file);
		// one that cannot be copied takes every copy back.
		std::vector<fs::path> copied;
		for (const RenameOutput &companion : plan.companions) {
			const fs::path to = at(companion_path(companion));
			std::string dated;
			fs::copy_file(at(companion.path), to, ec);
			if (!ec && !refresh_last_write(utf8_of(to), dated)) ec = std::make_error_code(std::errc::io_error);
			if (ec) {
				findings_.push_back(refusal(CoreFinding::RenameCopy, companion.old_name + " could not be copied to its new name: " +
				                                    (dated.empty() ? ec.message() : dated), companion.path));
				std::error_code ignored;
				for (const fs::path &made : copied) fs::remove(made, ignored);
				fs::remove(new_path, ignored);
				if (!plan.sidecar.empty()) fs::remove(at(plan.new_sidecar), ignored);
				return;
			}
			copied.push_back(to);
		}
	}
	if (!save_staged()) {
		findings_.push_back(make_finding(CoreFinding::RenamePartial, DiagnosticSeverity::Warning, "Both '" + plan.old_name + "' and '" + plan.new_name +
		                                            "' are in the project until every reference is rewritten.",
		                                    plan.path));
		return;
	}
	if (!same_file) {
		fs::remove(old_path, ec);
		if (ec) {
			findings_.push_back(refusal(CoreFinding::RenameRemove, "The old file could not be removed: " + ec.message(), plan.path));
			return;
		}
		if (!plan.sidecar.empty()) fs::remove(at(plan.sidecar), ec);
		for (const RenameOutput &companion : plan.companions) fs::remove(at(companion.path), ec);
	} else {
		// A rename an indexer or a scanner holding the file refuses for a moment is tried again
		// (rename_with_retry: a bounded few ms), as a save's replace and the build's publish are.
		if (!rename_with_retry(old_path, new_path, ec)) {
			findings_.push_back(refusal(CoreFinding::RenameMove, "The file could not be renamed: " + ec.message(), plan.path));
			return;
		}
		if (!plan.sidecar.empty()) rename_with_retry(at(plan.sidecar), at(plan.new_sidecar), ec);
		for (const RenameOutput &companion : plan.companions)
			rename_with_retry(at(companion.path), at(companion_path(companion)), ec);
	}
	// The old outputs are disposable: the next import pass makes the new ones. (A rename
	// that only changes the case keeps its output directory, which is keyed case-blind.)
	if (!plan.output_dir.empty() && plan.output_dir != plan.new_output_dir)
		fs::remove_all(at(plan.output_dir), ec);
	ok_ = true;
}

// Every file written as one, only when each staged file took every site and would write: a file
// changed since it was read is refused as a Save refuses it, and one the system will not replace
// (write-protected) puts back the ones replaced before.
void RenameTransaction::commit_symbol_rename() {
	if (!staged_ok_) return;
	std::vector<FileText> writes;
	for (const auto &staged : staged_) {
		if (!staged->document->matches_file()) {
			findings_.push_back(refusal(CoreFinding::RenameConflict,
			                            staged->document->path() + " changed outside the editor. Nothing was renamed.",
			                            staged->document->path()));
			return;
		}
		writes.push_back({join_path(paths_.root, staged->document->path()),
		                  staged->document->serialize().text});
	}
	std::vector<std::string> problems;
	if (write_files_together(writes, problems, replace_)) {
		ok_ = true;
		return;
	}
	findings_.push_back(refusal(CoreFinding::RenameWrite, "Nothing was renamed: " + problems.front(), symbol_plan_.file));
	for (size_t i = 1; i < problems.size(); ++i)
		findings_.push_back(refusal(CoreFinding::RenamePartial, problems[i], symbol_plan_.file));
}

} // namespace opennova::editor
