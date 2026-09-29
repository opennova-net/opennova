#include <editor/session/problem_fixes.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>

#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A Use fix is for a project that has the file under another name: a few files of the
// kind are offered at most (in the scan's order), each with its rename planned.
constexpr size_t kUseFilesMax = 8;

// Every fix acts on the files, which the editor's Undo does not reach: its detail says so.
constexpr const char *kNotUndoable = " It cannot be undone with Undo.";

// The game install's spelling of a file it has (view.retail_files is sorted by the
// normalized name), or "" when it has none of that name.
std::string retail_name(const SessionView &view, const std::string &name) {
	const std::string wanted = normalized_logical_name(name);
	const auto found = std::lower_bound(view.retail_files.begin(), view.retail_files.end(), wanted,
	                                    [](const std::string &file, const std::string &key) {
		                                    return normalized_logical_name(file) < key;
	                                    });
	return found != view.retail_files.end() && normalized_logical_name(*found) == wanted ? *found : std::string();
}

// The import dialog on a file of the game install, planned with the files it needs when the
// editor's setting says so (SessionView::import_dependencies).
ProblemFix import_fix(const SessionView &view, const std::string &retail) {
	EditorRequest request = make_request(EditorRequestKind::PreviewRetailImport);
	request.names = {retail};
	request.flag = view.import_dependencies;
	return {"Import " + retail + " from the game data...",
	        "Opens the import dialog on " + retail + " from the game install" +
	                (request.flag ? ", with the files it needs," : "") +
	                " to copy into the project. The copy cannot be undone with Undo.",
	        request, true};
}

std::string placeholder(const BlankFactory &factory) {
	return std::string("Creates ") + factory.summary + ": placeholder content, to replace with your own." + kNotUndoable;
}

// What the rename a Use fix makes rewrites, planned over the view, or why it is refused.
std::string use_detail(const SessionView &view, const AssetEntry &file, const std::string &name) {
	const std::string renames = "Renames " + file.logical_name + " to " + name;
	if (!view.graph) return renames + "." + kNotUndoable;
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(view.project_root), view.scan, *view.graph, file.relative_path, name);
	if (!plan.ok()) return "Cannot rename " + file.logical_name + " to " + name + ": " + plan.refusals.front().message;
	if (plan.sites.empty()) return renames + "; nothing refers to it." + kNotUndoable;
	std::set<std::string> files;
	for (const RenameSite &site : plan.sites) files.insert(site.file);
	return renames + " and rewrites " + counted(plan.sites.size(), "reference") + " in " + counted(files.size(), "file") +
	       "." + kNotUndoable;
}

// A file the game reads by name that the project lacks, while its row is missing (a file of
// the name that is there, of the wrong kind or added since, is not this finding's to fix).
void requirement_fixes(const Diagnostic &d, const SessionView &view, bool plan, std::vector<ProblemFix> &out) {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.requirements.rows)
		if (candidate.role == d.role) row = &candidate;
	if (!row || row->state != RequirementState::Missing) return;
	if (const BlankFactory *factory = find_blank_factory_for_role(row->role)) {
		EditorRequest create = make_request(EditorRequestKind::CreateMissing);
		create.names = {row->role};
		out.push_back({"Create " + row->name, placeholder(*factory), create, true});
	}
	const std::string retail = retail_name(view, row->name);
	if (!retail.empty()) out.push_back(import_fix(view, retail));
	// Renaming a file of the project into place is for a file the game cannot start without;
	// an optional one is made or imported, never taken from another file.
	if (!row->required) return;
	// A file of the kind with the name's extension (a rename keeps it), which no import makes
	// (it is renamed through its source) and no other requirement names (the rename would
	// only move the problem).
	const auto extension = [](const std::string &name) {
		return normalized_logical_name(fs::path(name).extension().generic_string());
	};
	size_t offered = 0;
	for (const AssetEntry &file : view.scan.entries) {
		if (file.kind != row->expected_kind || !file.imported_from.empty() ||
		    extension(file.logical_name) != extension(row->name))
			continue;
		const bool required = std::any_of(view.requirements.rows.begin(), view.requirements.rows.end(), [&file](const RequirementRow &other) {
			return normalized_logical_name(other.name) == normalized_logical_name(file.logical_name);
		});
		if (required) continue;
		if (offered++ == kUseFilesMax) break;
		out.push_back({"Use " + file.logical_name + " as " + row->name, plan ? use_detail(view, file, row->name) : std::string(),
		               make_request(EditorRequestKind::AssignRequirement, file.relative_path, row->role), false});
	}
}

// Files' Rename... on a project file: Files selects it and asks its new name, and the rename
// rewrites every file naming it.
ProblemFix rename_fix(const std::string &path) {
	EditorRequest request = make_request(EditorRequestKind::ShowInFiles, path);
	request.flag = true;
	return {"Rename " + basename_of(path) + "...",
	        "Shows " + path + " in Files and asks its new name; the rename rewrites every file that names it.", request,
	        false};
}

// A file the game reads by name whose file there is of another kind: the game's own from the
// game install (the import dialog, whose Replace writes over the file there), or the file
// there renamed out of the way (the row is then missing, with its Create and Use). Create and
// Use are not offered while the name is taken: both would be refused (create_missing.wrong_kind,
// rename.exists).
void wrong_kind_fixes(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.requirements.rows)
		if (candidate.role == d.role) row = &candidate;
	if (!row || row->state != RequirementState::WrongKind) return;
	const std::string retail = retail_name(view, row->name);
	if (!retail.empty()) out.push_back(import_fix(view, retail));
	if (!row->asset_path.empty()) out.push_back(rename_fix(row->asset_path));
}

// The table a symbol of `kind` goes in when no file of the project defines one yet: of the
// kind of file that defines it (its row's defined_in: the weapon, item and ammo tables, the
// stylesheets, the particle files), the one the game reads by name (the first requirement row
// of that kind: weapon.def, items.def, ammo.def, menu_style.mns, the stylesheet the shell reads
// first), else the project's first file of that kind; null for a kind no table defines.
const AssetEntry *usual_table(ReferenceKind kind, const SessionView &view) {
	const AssetKind table = reference_row(kind).defined_in;
	if (table == AssetKind::Unknown) return nullptr;
	for (const RequirementRow &row : view.requirements.rows) {
		if (row.expected_kind != table) continue;
		const AssetEntry *file = view.scan.find(row.name);
		if (file && file->kind == table) return file;
	}
	for (const AssetEntry &entry : view.scan.entries)
		if (entry.kind == table) return &entry;
	return nullptr;
}

// The project file where a missing symbol belongs: the one its scope names (its row's
// scope_names_file: a string id's table, the menu an ACTION's screen or window is looked up in,
// the model whose user points an item's particle slot names), else the file that defines the
// other symbols of its kind that a lookup finds, else the table its kind goes in (usual_table:
// a catalog or a stylesheet that defines nothing yet); null when the project has none (a
// string id of no table, or of any table).
const AssetEntry *defining_file(const Diagnostic &d, const SessionView &view) {
	if (reference_row(d.reference).scope_names_file) {
		const std::string scoped = d.scope.substr(0, d.scope.find('/'));
		return scoped.empty() ? nullptr : view.scan.find(scoped);
	}
	if (view.graph)
		for (const GraphSymbol &symbol : view.graph->symbols()) {
			if (symbol.kind != d.reference || symbol.inert) continue;
			for (const AssetEntry &entry : view.scan.entries)
				if (entry.relative_path == symbol.file) return &entry;
		}
	return usual_table(d.reference, view);
}

// A missing symbol: the file where it belongs (defining_file), opened to add it or to see the
// names it has, or shown in Files when the editor does not open its kind.
void symbol_fixes(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	const AssetEntry *file = defining_file(d, view);
	if (!file) return;
	const std::string what = std::string(reference_row(d.reference).phrase) + " '" + d.target + "'";
	if (is_editable_kind(file->kind))
		out.push_back({"Open " + file->logical_name,
		               "Opens " + file->relative_path + ", where " + what + " belongs: add it there, or correct the name.",
		               make_request(EditorRequestKind::OpenDocument, file->relative_path), false});
	else
		out.push_back({"Show " + file->logical_name + " in Files",
		               "Shows " + file->relative_path + " in Files, the file where " + what +
		                       " belongs (the editor does not edit its kind).",
		               make_request(EditorRequestKind::ShowInFiles, file->relative_path), false});
}

// An animation map with no anim_reset row: the row added, keyed anim_reset (the key a new
// row of a table without one takes), in its document, opened first when it is not: one step
// Undo takes back, saved with the table.
ProblemFix reset_row_fix(const std::string &path) {
	EditorRequest add = make_request(EditorRequestKind::EditRecord, path);
	add.flag = true;
	add.edit.operation = EditOperation::Add;
	add.edit.address.kind = node_kind(AnimationMapKind::Row);
	add.edit.field = "key";
	add.edit.value = std::string("anim_reset");
	return {"Add anim_reset row",
	        "Adds a row keyed anim_reset to " + path + ", the row the game needs to load the table; give it its clip. "
	        "Undo takes it back, and Save writes it.",
	        add, false};
}

bool same_file(const std::string &a, const std::string &b) { return normalized_logical_name(a) == normalized_logical_name(b); }

// The file a missing texture's placeholder takes: a name the reference's loader opens once the
// project has that file (reference_file_candidates, asked as if it were there), of the names
// that loader could open (the name as written there first, then every file there, then none)
// the first the texture factory makes (can_make_blank_texture) in the format that loader
// reads it as (a model row's: the reader its loader decodes that file with once the project
// has it alone, renderer::material_texture_source over the row's runtime type; a menu
// texture's and any other texture's reader goes by the name's last extension, as the factory
// does). None for a model's chunk row, which reads a chunk container and no image, for a
// texture of no model row whose name's own extension the factory cannot write (the runtime's
// lookup reads that name first: texture_candidate_filenames), nor for a name no loader of the
// reference opens or the factory refuses.
std::string placeholder_file(const Diagnostic &d) {
	const bool row = d.reference == ReferenceKind::Texture && d.material_type >= 0;
	const uint8_t type = row ? renderer::material_texture_runtime_type(static_cast<uint8_t>(d.material_type)) : 0;
	if (row && renderer::material_texture_source(d.target, type, {}).reader == renderer::MaterialTextureReader::Chunk)
		return std::string();
	std::string reason;
	if (d.reference == ReferenceKind::Texture && !row) {
		// The name's own extension: the text after the last '.' of its file part.
		const size_t slash = d.target.find_last_of("/\\");
		const std::string file = slash == std::string::npos ? d.target : d.target.substr(slash + 1);
		const size_t dot = file.rfind('.');
		if (dot != std::string::npos && dot + 1 < file.size() && !can_make_blank_texture(file, reason))
			return std::string();
	}
	std::vector<std::string> names;
	const auto gather = [&](const std::function<bool(const std::string &)> &exists) {
		for (const std::string &name : reference_file_candidates(d.reference, d.target, d.material_type, exists))
			if (std::none_of(names.begin(), names.end(), [&](const std::string &held) { return same_file(held, name); }))
				names.push_back(name);
	};
	gather([&](const std::string &name) { return same_file(name, d.target); });
	gather([](const std::string &) { return true; });
	gather([](const std::string &) { return false; });
	for (const std::string &name : names) {
		if (!can_make_blank_texture(name, reason)) continue;
		const auto there = [&](const std::string &file) { return same_file(file, name); };
		if (row) {
			const renderer::MaterialTextureSource source = renderer::material_texture_source(d.target, type, there);
			if (!same_file(source.file, name) || source.reader != blank_texture_reader(name)) continue;
		}
		for (const std::string &opened : reference_file_candidates(d.reference, d.target, d.material_type, there))
			if (same_file(opened, name)) return name;
	}
	return std::string();
}

// A missing texture's placeholder: the game's own missing-texture checkerboard as the file the
// reference's loader opens (placeholder_file), when the name is free and one the project's
// name rules take; a Fix all makes every one it covers.
void placeholder_fix(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	const std::string name = placeholder_file(d);
	std::string problem, message;
	if (name.empty() || view.scan.find(name) || !check_file_name(name, AssetKind::Texture, problem, message)) return;
	out.push_back({"Create a placeholder " + name,
	               "Creates " + name +
	                       ": the checkerboard the game draws for a missing texture, 128 by 128 gray squares, to replace "
	                       "with your own art." +
	                       kNotUndoable,
	               make_request(EditorRequestKind::CreateFile, name, asset_kind_token(AssetKind::Texture)), true});
}

// A reference to a file the project lacks: the names its loader reads that make a file of
// its kind (the graph resolves a name only to a file of the kind, as the scan lists it),
// looked up in the game install (a loader that picks by what is there, a menu texture's
// .tga or its .dds, a model texture's DDS sibling, picks from the game install's files);
// then a texture's placeholder (placeholder_fix), or a blank file of any other kind, a
// required name's factory winning over the kind's free-form one (as CreateFile picks), when
// the name is free (a file of another kind there would only open, the reference still
// missing) and one the project's name rules take (check_file_name).
void reference_fixes(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	if (reference_row(d.reference).names_symbol()) return symbol_fixes(d, view, out);
	const AssetKind kind = reference_row(d.reference).file;
	if (kind == AssetKind::Unknown || d.target.empty()) return;
	const auto in_game = [&view](const std::string &name) { return !retail_name(view, name).empty(); };
	std::vector<std::string> names;
	for (const std::string &name : reference_file_candidates(d.reference, d.target, d.material_type, in_game))
		if (asset_name_fits_kind(name, kind)) names.push_back(name);
	for (const std::string &name : names) {
		const std::string retail = retail_name(view, name);
		if (retail.empty()) continue;
		out.push_back(import_fix(view, retail));
		break;
	}
	if (kind == AssetKind::Texture) return placeholder_fix(d, view, out);
	if (names.empty()) return;
	const std::string &name = names.front();
	std::string problem, message;
	if (view.scan.find(name) || !check_file_name(name, kind, problem, message)) return;
	const BlankFactory *factory = nullptr;
	for (const RequirementRow &row : view.requirements.rows)
		if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(name))
			factory = find_blank_factory_for_role(row.role);
	if (!factory) factory = find_blank_factory_for_kind(kind);
	if (factory)
		out.push_back({"Create " + name, placeholder(*factory),
		               make_request(EditorRequestKind::CreateFile, name, asset_kind_token(kind)), false});
}

// What the rewrite of a file drops or normalizes, for a finding it fixes; null for another.
const char *rewrite_does(const std::string &code) {
	if (code == "style.line_ending") return "with every line ending CR LF";
	if (code == "catalog.ignored_input" || code == "menu.ignored_input" || code == "animation_map.ignored_input")
		return "without the input the game ignores";
	if (code == "strings.regrouped") return "with its strings grouped by section the way the game reads them";
	return nullptr;
}

bool ends_with(const std::string &text, const char *tail) {
	const size_t n = std::char_traits<char>::length(tail);
	return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

// The fixes of one finding, over the view's index (a Rewrite reads it).
void collect(const Diagnostic &d, const SessionView &view, const ProblemFixIndex &index, bool plan,
             std::vector<ProblemFix> &out) {
	if (!view.project_open) return;
	if (d.code == "requirement.missing" || d.code == "requirement.optional_missing" || d.code == "play.boot_missing") {
		if (!d.role.empty()) requirement_fixes(d, view, plan, out);
	} else if (d.code == "requirement.wrong_kind") {
		if (!d.role.empty()) wrong_kind_fixes(d, view, out);
	} else if (d.code == "asset.name.too_long" || d.code == "asset.name.duplicate" || d.code == "build.name_unstorable") {
		// A name the archives cannot take, or another file has: Files' Rename....
		if (std::any_of(view.scan.entries.begin(), view.scan.entries.end(),
		                [&d](const AssetEntry &entry) { return entry.relative_path == d.asset; }))
			out.push_back(rename_fix(d.asset));
	} else if (d.code == "animation_map.no_reset" && !d.asset.empty()) {
		out.push_back(reset_row_fix(d.asset));
	} else if (d.code == "reference.missing") {
		reference_fixes(d, view, out);
	} else if (d.code == "import.texture_not_imported") {
		// A texture an import's model names that the import did not bring: the reference's own
		// fixes, while the project still lacks the file (the finding is the import's, kept to
		// the next validation, whose graph reports the reference as missing too).
		if (view.graph &&
		    view.graph->resolve(d.reference, d.target, d.scope, nullptr, d.material_type) == ReferenceStatus::Missing)
			reference_fixes(d, view, out);
	} else if (d.code == "document.conflict" && !d.asset.empty()) {
		// An open document whose file changed outside the editor: read it again, its unsaved
		// edits dropped (the Reload asks about them first, as any Reload does).
		const bool open = std::any_of(view.documents.begin(), view.documents.end(),
		                              [&d](const auto &document) { return document && document->path() == d.asset; });
		if (open)
			out.push_back({"Reload " + basename_of(d.asset),
			               "Reads " + d.asset + " again from its file, which changed outside the editor: its unsaved "
			               "edits are lost (it asks first) and its history starts again." + kNotUndoable,
			               make_request(EditorRequestKind::ReloadDocument, d.asset), false});
	} else if (d.code == "import.output_missing" && !d.asset.empty()) {
		EditorRequest again = make_request(EditorRequestKind::Reimport, d.asset);
		again.flag = true;
		out.push_back({"Import " + basename_of(d.asset) + " again",
		               "Runs the importer on " + d.asset + " again, which makes the files it lists." + kNotUndoable, again,
		               true});
	} else if (const char *does = rewrite_does(d.code); does && !d.asset.empty() && !index.unserializable.count(d.asset)) {
		std::string detail = "Writes " + d.asset + " again " + does + ".";
		for (const auto &document : view.documents)
			if (document && document->path() == d.asset && document->dirty()) detail += " Its unsaved edits are saved with it.";
		detail += kNotUndoable;
		out.push_back({"Rewrite " + basename_of(d.asset), detail, make_request(EditorRequestKind::Save, d.asset), true});
	}
}

// The fixes of one finding over the index given, or one found for this ask alone.
std::vector<ProblemFix> fixes_over(const Diagnostic &d, const SessionView &view, const ProblemFixIndex *index, bool plan) {
	std::vector<ProblemFix> fixes;
	if (index) collect(d, view, *index, plan, fixes);
	else collect(d, view, ProblemFixIndex(view), plan, fixes);
	return fixes;
}

} // namespace

// A file whose own finding says it does not serialize (input the model cannot hold, a value
// it cannot write): its Save is refused (document.unserializable), so no Rewrite.
ProblemFixIndex::ProblemFixIndex(const SessionView &view) {
	for (const Diagnostic &d : view.diagnostics)
		if (ends_with(d.code, ".unserializable") || ends_with(d.code, ".invalid_input")) unserializable.insert(d.asset);
}

std::vector<ProblemFix> fixes_for(const Diagnostic &diagnostic, const SessionView &view, const ProblemFixIndex *index) {
	return fixes_over(diagnostic, view, index, true);
}

bool has_fixes(const Diagnostic &diagnostic, const SessionView &view, const ProblemFixIndex *index) {
	return !fixes_over(diagnostic, view, index, false).empty();
}

std::vector<ProblemFix> bulk_fixes_for(const Diagnostic &diagnostic, const SessionView &view, const ProblemFixIndex *index) {
	std::vector<ProblemFix> fixes = fixes_over(diagnostic, view, index, false);
	fixes.erase(std::remove_if(fixes.begin(), fixes.end(), [](const ProblemFix &fix) { return !fix.bulk; }), fixes.end());
	return fixes;
}

std::vector<EditorRequest> merge_fixes(const std::vector<ProblemFix> &fixes) {
	std::vector<EditorRequest> requests;
	for (const ProblemFix &fix : fixes) {
		if (!fix.bulk) continue;
		const EditorRequest &request = fix.request;
		// The files to create and the files to list join one request each; any other
		// request is raised once.
		const bool joins =
		        request.kind == EditorRequestKind::CreateMissing || request.kind == EditorRequestKind::PreviewRetailImport;
		const auto made = std::find_if(requests.begin(), requests.end(), [&](const EditorRequest &earlier) {
			return earlier.kind == request.kind &&
			       (joins || (earlier.path == request.path && earlier.text == request.text && earlier.flag == request.flag));
		});
		if (made == requests.end()) {
			requests.push_back(request);
			continue;
		}
		if (!joins) continue;
		for (const std::string &name : request.names)
			if (std::find(made->names.begin(), made->names.end(), name) == made->names.end()) made->names.push_back(name);
	}
	return requests;
}

const ProblemFixIndex &ProblemFixCache::follow(const SessionView &view) {
	if (view_ != &view || revision_ != view.revision || !index_) {
		fixes_.clear();
		view_ = &view;
		revision_ = view.revision;
		index_ = std::make_unique<ProblemFixIndex>(view);
	}
	return *index_;
}

const std::vector<ProblemFix> &ProblemFixCache::fixes(const SessionView &view, size_t index) {
	const ProblemFixIndex &view_index = follow(view);
	auto found = fixes_.find(index);
	if (found == fixes_.end())
		found = fixes_.emplace(index, index < view.diagnostics.size() ? fixes_for(view.diagnostics[index], view, &view_index)
		                                                              : std::vector<ProblemFix>())
		                .first;
	return found->second;
}

std::vector<ProblemFix> ProblemFixCache::bulk(const SessionView &view, size_t index) {
	const ProblemFixIndex &view_index = follow(view);
	return index < view.diagnostics.size() ? bulk_fixes_for(view.diagnostics[index], view, &view_index)
	                                       : std::vector<ProblemFix>();
}

} // namespace opennova::editor
