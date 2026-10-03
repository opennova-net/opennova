#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
#include <utility>

#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/field_text.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A Use fix is for a project that has the file under another name: a few files of the
// kind are offered at most (in the scan's order), each with its rename planned.
constexpr size_t kUseFilesMax = 8;

// Every fix acts on the files, which the editor's Undo does not reach: its detail says so.
constexpr const char *kNotUndoable = " It cannot be undone with Undo.";

// The game install's spelling of a file it has (view.project.retail_files is sorted by the
// normalized name), or "" when it has none of that name.
std::string retail_name(const SessionView &view, const std::string &name) {
	const std::string wanted = normalized_logical_name(name);
	const auto found =
			std::lower_bound(view.project.retail_files.begin(), view.project.retail_files.end(),
					wanted, [](const std::string &file, const std::string &key) {
						return normalized_logical_name(file) < key;
					});
	return found != view.project.retail_files.end() && normalized_logical_name(*found) == wanted
			? *found
			: std::string();
}

// The import dialog on a file of the game install, planned with the files it needs when the
// editor's setting says so (SessionView::import_dependencies).
ProblemFix import_fix(const SessionView &view, const std::string &retail) {
	const EditorRequest request =
	        request::preview_install_import({retail}, view.project.import_dependencies);
	return {"Import " + retail + " from the game data...",
	        "Opens the import dialog on " + retail + " from the game install" +
	                (request.with_dependencies ? ", with the files it needs," : "") +
	                " to copy into the project. The copy cannot be undone with Undo.",
	        request, true};
}

std::string placeholder(const BlankFactory &factory) {
	return std::string("Creates ") + factory.summary + ": placeholder content, to replace with your own." + kNotUndoable;
}

// What the rename a Use fix makes rewrites, planned over the view, or why it is refused.
std::string use_detail(const SessionView &view, const AssetEntry &file, const std::string &name) {
	const std::string renames = "Renames " + file.logical_name + " to " + name;
	if (!view.findings.graph) return renames + "." + kNotUndoable;
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(view.project.root), *view.project.scan, *view.findings.graph, file.relative_path, name);
	if (!plan.ok()) return "Cannot rename " + file.logical_name + " to " + name + ": " + plan.refusals.front().message;
	if (plan.sites.empty()) return renames + "; nothing refers to it." + kNotUndoable;
	std::set<std::string> files;
	for (const RenameSite &site : plan.sites) files.insert(site.file);
	return renames + " and rewrites " + counted(plan.sites.size(), "reference") + " in " + counted(files.size(), "file") +
	       "." + kNotUndoable;
}

// A file the game reads by name that the project lacks, while its row is missing (a file of
// the name that is there, of the wrong kind or added since, is not this finding's to fix).
void requirement_fixes(const RequirementSubject &subject, const SessionView &view, bool plan,
                       std::vector<ProblemFix> &out) {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.project.requirements->rows)
		if (candidate.role == subject.role) row = &candidate;
	if (!row || row->state != RequirementState::Missing) return;
	if (const BlankFactory *factory = find_blank_factory_for_role(row->role)) {
		out.push_back(
		        {"Create " + row->name, placeholder(*factory), request::create_missing({row->role}), true});
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
		return normalized_logical_name(utf8_of(path_of(name).extension()));
	};
	size_t offered = 0;
	for (const AssetEntry &file : view.project.scan->entries) {
		if (file.kind != row->expected_kind || !file.imported_from.empty() ||
		    extension(file.logical_name) != extension(row->name))
			continue;
		const bool required = std::any_of(view.project.requirements->rows.begin(), view.project.requirements->rows.end(), [&file](const RequirementRow &other) {
			return normalized_logical_name(other.name) == normalized_logical_name(file.logical_name);
		});
		if (required) continue;
		if (offered++ == kUseFilesMax) break;
		out.push_back({"Use " + file.logical_name + " as " + row->name, plan ? use_detail(view, file, row->name) : std::string(),
		               request::assign_requirement(row->role, file.relative_path), false});
	}
}

// Files' Rename... on a project file: Files selects it and asks its new name, and the rename
// rewrites every file naming it.
ProblemFix rename_fix(const std::string &path) {
	return {"Rename " + basename_of(path) + "...",
	        "Shows " + path +
	                " in Files and asks its new name; the rename rewrites every file that names it.",
	        request::show_in_files(path, true), false};
}

// A file the game reads by name whose file there is of another kind: the game's own from the
// game install (the import dialog, whose Replace writes over the file there), or the file
// there renamed out of the way (the row is then missing, with its Create and Use). Create and
// Use are not offered while the name is taken: both would be refused (create_missing.wrong_kind,
// rename.exists).
void wrong_kind_fixes(const RequirementSubject &subject, const SessionView &view, std::vector<ProblemFix> &out) {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.project.requirements->rows)
		if (candidate.role == subject.role) row = &candidate;
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
	for (const RequirementRow &row : view.project.requirements->rows) {
		if (row.expected_kind != table) continue;
		const AssetEntry *file = view.project.scan->find(row.name);
		if (file && file->kind == table) return file;
	}
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == table) return &entry;
	return nullptr;
}

// The project file where a missing symbol belongs: the one its scope names (its row's
// scope_names_file: a string id's table, the menu an ACTION's screen or window is looked up in,
// the model whose user points an item's particle slot names), else the file that defines the
// other symbols of its kind that a lookup finds, else the table its kind goes in (usual_table:
// a catalog or a stylesheet that defines nothing yet); null when the project has none (a
// string id of no table, or of any table).
const AssetEntry *defining_file(const ReferenceSubject &missing, const SessionView &view) {
	if (reference_row(missing.kind).scope_names_file) {
		const std::string scoped = missing.scope.substr(0, missing.scope.find('/'));
		return scoped.empty() ? nullptr : view.project.scan->find(scoped);
	}
	if (view.findings.graph)
		for (const GraphSymbol *symbol : view.findings.graph->symbols_of_kind(missing.kind))
			if (!symbol->inert)
				if (const AssetEntry *entry = view.project.scan->at_path(symbol->file))
					return entry;
	return usual_table(missing.kind, view);
}

// A missing symbol: the file where it belongs (defining_file), opened to add it or to see the
// names it has, or shown in Files when the editor does not open its kind.
void symbol_fixes(const ReferenceSubject &missing, const SessionView &view, std::vector<ProblemFix> &out) {
	const AssetEntry *file = defining_file(missing, view);
	if (!file) return;
	const std::string what = std::string(reference_row(missing.kind).phrase) + " '" + missing.target + "'";
	if (is_editable_kind(file->kind))
		out.push_back({"Open " + file->logical_name,
		               "Opens " + file->relative_path + ", where " + what + " belongs: add it there, or correct the name.",
		               request::open_document(file->relative_path), false});
	else
		out.push_back({"Show " + file->logical_name + " in Files",
		               "Shows " + file->relative_path + " in Files, the file where " + what +
		                       " belongs (the editor does not edit its kind).",
		               request::show_in_files(file->relative_path), false});
}

// An animation map with no anim_reset row: the row added, keyed anim_reset (the key a new
// row of a table without one takes), in its document, opened first when it is not: one step
// Undo takes back, saved with the table.
ProblemFix reset_row_fix(const std::string &path) {
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(AnimationMapKind::Row);
	add.field = "key";
	add.value = std::string("anim_reset");
	return {"Add anim_reset row",
	        "Adds a row keyed anim_reset to " + path + ", the row the game needs to load the table; give it its clip. "
	        "Undo takes it back, and Save writes it.",
	        request::edit_record(path, std::move(add), true), false};
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
// texture of no model row whose name's own extension the factory cannot write (its role's loader
// reads that name, texture_reference_load), nor for a name no loader of the reference opens or the
// factory refuses.
std::string placeholder_file(const ReferenceSubject &missing) {
	const bool row = missing.kind == ReferenceKind::Texture && texture_arg_is_row_type(missing.loader_arg);
	const uint8_t type = row ? renderer::material_texture_runtime_type(static_cast<uint8_t>(missing.loader_arg)) : 0;
	if (row && renderer::material_texture_source(missing.target, type, {}).reader == renderer::MaterialTextureReader::Chunk)
		return std::string();
	std::string reason;
	if (missing.kind == ReferenceKind::Texture && !row) {
		// The name's own extension: the text after the last '.' of its file part.
		const size_t slash = missing.target.find_last_of("/\\");
		const std::string file = slash == std::string::npos ? missing.target : missing.target.substr(slash + 1);
		const size_t dot = file.rfind('.');
		if (dot != std::string::npos && dot + 1 < file.size() && !can_make_blank_texture(file, reason))
			return std::string();
	}
	std::vector<std::string> names;
	const auto gather = [&](const std::function<bool(const std::string &)> &exists) {
		for (const std::string &name : reference_file_candidates(missing.kind, missing.target, missing.loader_arg, exists))
			if (std::none_of(names.begin(), names.end(), [&](const std::string &held) { return same_file(held, name); }))
				names.push_back(name);
	};
	gather([&](const std::string &name) { return same_file(name, missing.target); });
	gather([](const std::string &) { return true; });
	gather([](const std::string &) { return false; });
	for (const std::string &name : names) {
		if (!can_make_blank_texture(name, reason)) continue;
		const auto there = [&](const std::string &file) { return same_file(file, name); };
		if (row) {
			const renderer::MaterialTextureSource source = renderer::material_texture_source(missing.target, type, there);
			if (!same_file(source.file, name) || source.reader != blank_texture_reader(name)) continue;
		}
		for (const std::string &opened : reference_file_candidates(missing.kind, missing.target, missing.loader_arg, there))
			if (same_file(opened, name)) return name;
	}
	return std::string();
}

// A missing texture's placeholder: the game's own missing-texture checkerboard as the file the
// reference's loader opens (placeholder_file), when the name is free and one the project's
// name rules take; a Fix all makes every one it covers.
void placeholder_fix(const ReferenceSubject &missing, const SessionView &view, std::vector<ProblemFix> &out) {
	const std::string name = placeholder_file(missing);
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (name.empty() || view.project.scan->find(name) || !check_file_name(name, AssetKind::Texture, problem, message)) return;
	out.push_back({"Create a placeholder " + name,
	               "Creates " + name +
	                       ": the checkerboard the game draws for a missing texture, 128 by 128 gray squares, to replace "
	                       "with your own art." +
	                       kNotUndoable,
	               request::create_file(name, asset_kind_token(AssetKind::Texture)), true});
}

// A reference to a file the project lacks: the names its loader reads that make a file of
// its kind (the graph resolves a name only to a file of the kind, as the scan lists it),
// looked up in the game install (a loader that picks by what is there, a menu texture's
// .tga or its .dds, a model texture's DDS sibling, picks from the game install's files);
// then a texture's placeholder (placeholder_fix), or a blank file of any other kind, a
// required name's factory winning over the kind's free-form one (as CreateFile picks), when
// the name is free (a file of another kind there would only open, the reference still
// missing) and one the project's name rules take (check_file_name).
void reference_fixes(const ReferenceSubject &missing, const SessionView &view, std::vector<ProblemFix> &out) {
	if (reference_row(missing.kind).names_symbol()) return symbol_fixes(missing, view, out);
	const AssetKind kind = reference_row(missing.kind).file;
	if (kind == AssetKind::Unknown || missing.target.empty()) return;
	const auto in_game = [&view](const std::string &name) { return !retail_name(view, name).empty(); };
	std::vector<std::string> names;
	for (const std::string &name : reference_file_candidates(missing.kind, missing.target, missing.loader_arg, in_game))
		if (asset_name_fits_kind(name, kind)) names.push_back(name);
	for (const std::string &name : names) {
		const std::string retail = retail_name(view, name);
		if (retail.empty()) continue;
		out.push_back(import_fix(view, retail));
		break;
	}
	if (kind == AssetKind::Texture) return placeholder_fix(missing, view, out);
	if (names.empty()) return;
	const std::string &name = names.front();
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (view.project.scan->find(name) || !check_file_name(name, kind, problem, message)) return;
	const BlankFactory *factory = nullptr;
	for (const RequirementRow &row : view.project.requirements->rows)
		if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(name))
			factory = find_blank_factory_for_role(row.role);
	if (!factory) factory = find_blank_factory_for_kind(kind);
	if (factory)
		out.push_back({"Create " + name, placeholder(*factory),
		               request::create_file(name, asset_kind_token(kind)), false});
}

// The fixes of one finding, over the view's index (a Rewrite reads it): what its code's row offers
// (FindingCodeRow::fixes), for what the finding is about.
void collect(const Diagnostic &d, const SessionView &view, const ProblemFixIndex &index, bool plan,
             std::vector<ProblemFix> &out) {
	if (!view.project.open) return;
	const FindingCodeRow *row = d.row();
	if (!row) return;
	switch (row->fixes) {
	case FindingFix::None: return;
	case FindingFix::Requirement:
		if (const RequirementSubject *subject = requirement_subject(d); subject && !subject->role.empty())
			requirement_fixes(*subject, view, plan, out);
		return;
	case FindingFix::WrongKind:
		if (const RequirementSubject *subject = requirement_subject(d); subject && !subject->role.empty())
			wrong_kind_fixes(*subject, view, out);
		return;
	case FindingFix::Rename:
		// A name the archives cannot take, or another file has: Files' Rename....
		if (view.project.scan->at_path(d.asset)) out.push_back(rename_fix(d.asset));
		return;
	case FindingFix::ResetRow:
		if (!d.asset.empty()) out.push_back(reset_row_fix(d.asset));
		return;
	case FindingFix::Reference:
		if (const ReferenceSubject *missing = reference_subject(d)) reference_fixes(*missing, view, out);
		return;
	case FindingFix::UnimportedTexture:
		// A texture an import's model names that the import did not bring: the reference's own
		// fixes, while the project still lacks the file (the finding is the import's, kept to
		// the next validation, whose graph reports the reference as missing too).
		if (const ReferenceSubject *missing = reference_subject(d);
		    missing && view.findings.graph &&
		    view.findings.graph->resolve(missing->kind, missing->target, missing->scope, nullptr, missing->loader_arg) ==
		            ReferenceStatus::Missing)
			reference_fixes(*missing, view, out);
		return;
	case FindingFix::Reload: {
		// An open document whose file changed outside the editor: read it again, its unsaved
		// edits dropped (the Reload asks about them first, as any Reload does).
		const bool open = !d.asset.empty() &&
		                  std::any_of(view.documents.open.begin(), view.documents.open.end(),
		                              [&d](const auto &document) { return document && document->path() == d.asset; });
		if (open)
			out.push_back({"Reload " + basename_of(d.asset),
			               "Reads " + d.asset + " again from its file, which changed outside the editor: its unsaved "
			               "edits are lost (it asks first) and its history starts again." + kNotUndoable,
			               request::reload_document(d.asset), false});
		return;
	}
	case FindingFix::Reimport:
		if (!d.asset.empty())
			out.push_back({"Import " + basename_of(d.asset) + " again",
			               "Runs the importer on " + d.asset + " again, which makes the files it lists." +
			                       kNotUndoable,
			               request::reimport(d.asset, true), true});
		return;
	case FindingFix::Rewrite: {
		// What writing the file again does (the row's words), unless a finding of the file says it
		// does not serialize.
		if (d.asset.empty() || index.unserializable.count(d.asset)) return;
		std::string detail = "Writes " + d.asset + " again " + row->rewrite_does + ".";
		for (const auto &document : view.documents.open)
			if (document && document->path() == d.asset && document->dirty()) detail += " Its unsaved edits are saved with it.";
		detail += kNotUndoable;
		out.push_back({"Rewrite " + basename_of(d.asset), detail, request::save(d.asset), true});
		return;
	}
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
// it cannot write: its code's row's blocks_save): its Save is refused (document.unserializable),
// so no Rewrite.
ProblemFixIndex::ProblemFixIndex(const SessionView &view) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.row() && d.row()->blocks_save) unserializable.insert(d.asset);
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
		// The files to create and the files to list join one request each (their roles, their
		// names); any other request is raised once.
		const bool creates = request.kind == EditorRequestKind::CreateMissing;
		const bool joins = creates || request.kind == EditorRequestKind::PreviewInstallImport;
		const auto made = std::find_if(requests.begin(), requests.end(), [&](const EditorRequest &earlier) {
			return joins ? earlier.kind == request.kind : earlier == request;
		});
		if (made == requests.end()) {
			requests.push_back(request);
			continue;
		}
		if (!joins) continue;
		std::vector<std::string> &joined = creates ? made->roles : made->names;
		for (const std::string &name : creates ? request.roles : request.names)
			if (std::find(joined.begin(), joined.end(), name) == joined.end())
				joined.push_back(name);
	}
	return requests;
}

RevisionKey problem_fix_key(const SessionView &view) {
	return revision_key(view.revisions,
			{ViewConcern::Project, ViewConcern::Files, ViewConcern::Findings, ViewConcern::Graph,
					ViewConcern::DocumentSet, ViewConcern::Preferences});
}

const ProblemFixIndex &ProblemFixCache::follow(const SessionView &view) {
	const RevisionKey key = problem_fix_key(view);
	if (view_ != &view || key_ != key || !index_) {
		fixes_.clear();
		view_ = &view;
		key_ = key;
		index_ = std::make_unique<ProblemFixIndex>(view);
		++generation_;
	}
	return *index_;
}

const std::vector<ProblemFix> &ProblemFixCache::fixes(const SessionView &view, size_t index) {
	const ProblemFixIndex &view_index = follow(view);
	auto found = fixes_.find(index);
	if (found == fixes_.end())
		found = fixes_.emplace(index, index < view.findings.diagnostics.size() ? fixes_for(view.findings.diagnostics[index], view, &view_index)
		                                                              : std::vector<ProblemFix>())
		                .first;
	return found->second;
}

std::vector<ProblemFix> ProblemFixCache::bulk(const SessionView &view, size_t index) {
	const ProblemFixIndex &view_index = follow(view);
	return index < view.findings.diagnostics.size() ? bulk_fixes_for(view.findings.diagnostics[index], view, &view_index)
	                                       : std::vector<ProblemFix>();
}

} // namespace opennova::editor
