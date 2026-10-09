#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <utility>

#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <base/io/strutil.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/rename_transaction.h>
#include <editor/graph/texture_import_needs.h>
#include <editor/graph/texture_uses.h>
#include <editor/import/importer.h>
#include <editor/import/texture_import.h>
#include <editor/import/texture_source.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/texture_use_index.h>
#include <editor/model/field_text.h>
#include <editor/model/finding_code_row.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A Use fix is for a project that has the file under another name: a few files of the
// kind are offered at most (in the scan's order), each with its rename planned.
constexpr size_t kUseFilesMax = 8;

// Every fix acts on the files, which the editor's Undo does not reach: its detail says so.
constexpr const char *kNotUndoable = " It cannot be undone with Undo.";
// A fix that edits a document is one step its Undo takes back, and its Save writes.
constexpr const char *kUndoable = " Undo takes it back, and Save writes it.";

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
// editor's setting says so (SessionView::import_dependencies). Named as the finding names it (`named`,
// a required file's name as the manifest spells it; the install's spelling when none), the install's
// own spelling what the import asks for.
ProblemFix import_fix(const SessionView &view, const std::string &retail, const std::string &named = std::string()) {
	const EditorRequest request =
	        request::preview_install_import({retail}, view.project.import_dependencies);
	const std::string name = named.empty() ? retail : named;
	return {"Import " + name + " from the game data...",
	        "Opens the import dialog on the game's own " + name + " from the game install" +
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
	// The game's own file first where the install has it (Fix all and the banner take a finding's first
	// fix): a placeholder makes a game that starts empty; it is the fix only where the install lacks the
	// file.
	const std::string retail = retail_name(view, row->name);
	if (!retail.empty()) out.push_back(import_fix(view, retail, row->name));
	if (const BlankFactory *factory = find_blank_factory_for_role(row->role)) {
		std::string detail = placeholder(*factory);
		if (!retail.empty()) detail = "Instead of the game's own: " + detail;
		out.push_back({retail.empty() ? "Create " + row->name : "Create a placeholder " + row->name, detail,
		               request::create_missing({row->role}), true});
	}
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
	if (!retail.empty()) out.push_back(import_fix(view, retail, row->name));
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

// The bank a missing sound set belongs in (DI-15): a menu SOUND's, the bank its SOUND names, where the menu looks
// it up alone; any other's, a bank the game searches a set by name in (AssetGraph::bank_rank: the global chain
// [orig: Game_StartMission @ 0x525443 over the slot table @ 0x82A5B0]), the one defining the other sets the
// search finds first, else the project's bank the search opens first; null for none.
const AssetEntry *sound_bank_for(const ReferenceSubject &missing, const SessionView &view) {
	if (!missing.scope.empty()) {
		const AssetEntry *bank = view.project.scan->find(missing.scope);
		return bank && bank->kind == AssetKind::SoundBank ? bank : nullptr;
	}
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return nullptr;
	for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::Sound))
		if (!symbol->inert && graph->on_bank_chain(symbol->file))
			if (const AssetEntry *entry = view.project.scan->at_path(symbol->file)) return entry;
	const AssetEntry *first = nullptr;
	size_t rank = SIZE_MAX;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::SoundBank && graph->bank_rank(entry.relative_path) < rank) {
			first = &entry;
			rank = graph->bank_rank(entry.relative_path);
		}
	return first;
}

} // namespace

const AssetEntry *defining_file(const ReferenceSubject &missing, const SessionView &view) {
	if (!view.project.scan) return nullptr;
	if (reference_row(missing.kind).scope_names_file) {
		const std::string scoped = missing.scope.substr(0, missing.scope.find('/'));
		return scoped.empty() ? nullptr : view.project.scan->find(scoped);
	}
	if (missing.kind == ReferenceKind::Sound) return sound_bank_for(missing, view);
	if (view.findings.graph)
		for (const GraphSymbol *symbol : view.findings.graph->symbols_of_kind(missing.kind))
			if (!symbol->inert)
				if (const AssetEntry *entry = view.project.scan->at_path(symbol->file))
					return entry;
	return view.project.requirements ? usual_table(missing.kind, view) : nullptr;
}

namespace {

// A missing name's Add it there (ADR 0046 DI-15): the record of its kind the file where it belongs gains, named
// as referenced, planned by the file's type (DocumentType::define_symbol) over its document as it stands (an open
// one's unsaved edits included). Offered only where it works: the same plan, made over the index's copy of the
// document (ProblemFixIndex::trial: a snapshot takes no edit), applies to it, and the copy then defines the name
// where the game's lookup finds it (no inert definition, in the reference's scope), read as the graph reads it (a
// record document through its schema, extract_from_document; a text as its Save writes it, extract_from_bytes);
// the copy's Undo then takes the plan back. An edit of that file alone: the file naming the name keeps it as
// written.
// Whether `copy` (of the file `name`, of `kind`), `planned` applied, defines the name where the game's lookup
// finds it (no inert definition, in the reference's scope), read as the graph reads it (a record document through
// its schema, extract_from_document; a text as its Save writes it, extract_from_bytes); the copy's Undo then takes
// the plan back, so it stands as it was.
bool defines_after(DocumentBase &copy, const PlannedFix &planned, const std::string &name, AssetKind kind,
                   const ReferenceSubject &missing, const SessionView &view) {
	Diagnostic error;
	if (!copy.apply(planned.edits, error)) return false;
	Extracted extracted;
	if (const Document *records = records_of(copy)) {
		extract_from_document(*records, extracted);
	} else {
		const SerializeResult saved = copy.serialize();
		if (saved.ok())
			extract_from_bytes(name, kind, std::vector<uint8_t>(saved.text.begin(), saved.text.end()),
			                   view.project.document->target_game, extracted, error);
	}
	copy.undo();
	const ReferenceKindRow &row = reference_row(missing.kind);
	const std::string wanted = graph_names::symbol_name(
	        missing.kind, row.spell == NameSpelling::StyleVariable ? mns::variable_name(missing.target) : missing.target);
	return std::any_of(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &symbol) {
		return symbol.kind == missing.kind && !symbol.inert && symbol.name == wanted && scope_matches(symbol.scope, missing.scope);
	});
}

void add_there_fix(const ReferenceSubject &missing, const SessionView &view, const AssetEntry &file,
                   const ProblemFixIndex &index, std::vector<ProblemFix> &out) {
	const DocumentType *type = document_type_for(file.kind);
	if (!type || !type->define_symbol || !view.project.document) return;
	const DocumentBase *document = index.definer(view, file);
	DocumentBase *copy = index.trial(view, file);
	PlannedFix planned, tried;
	if (!document || !copy || !type->define_symbol(*document, missing, planned) || planned.edits.empty()) return;
	// A closed file's copy is the document planned over: the same plan.
	if (copy == document) tried = planned;
	else if (!type->define_symbol(*copy, missing, tried)) return;
	if (defines_after(*copy, tried, file.relative_path, file.kind, missing, view)) out.push_back(edit_fix(file.relative_path, planned));
}

// The file a missing name belongs in that the project lacks (ADR 0046 DI-33), where the game would look the name
// up once it is there: the file its scope names (a string id's table), a sound set's bank (a menu SOUND's own,
// else the first bank of the chain the game searches, audio::global_bank_chain
// [orig: Game_StartMission @ 0x525443 over the slot table @ 0x82A5B0]), else the table its kind goes in (its row's
// defined_in): the one the game reads
// by name (the requirement row of that kind: weapon.def, SndProf.def, menu_style.mns; else the kind's own file
// name), else, for a kind the game reads every file of (the particle files
// [orig: CEffectSystem_Init @ 0x5F6070]), the name a new file of the kind takes. False for a kind no table defines.
bool missing_definer(const ReferenceSubject &missing, const SessionView &view, std::string &name, AssetKind &kind) {
	const ReferenceKindRow &row = reference_row(missing.kind);
	name.clear();
	kind = AssetKind::Unknown;
	if (row.scope_names_file) {
		// A string id's table. A menu's screen or window is looked up in a menu file whose own reference says it
		// is missing (its Create makes it), and a user point is a place on a model.
		if (missing.kind != ReferenceKind::TextId) return false;
		name = missing.scope.substr(0, missing.scope.find('/'));
		kind = AssetKind::Strings;
		return !name.empty();
	}
	if (missing.kind == ReferenceKind::Sound) {
		kind = AssetKind::SoundBank;
		if (!missing.scope.empty()) {
			name = missing.scope;
			return true;
		}
		const std::string expansion = view.project.document ? view.project.document->expansion.name : std::string();
		const std::vector<std::string> chain = audio::global_bank_chain(expansion);
		if (chain.empty()) return false;
		name = chain.front();
		return true;
	}
	kind = row.defined_in;
	if (kind == AssetKind::Unknown) return false;
	for (const RequirementRow &required : view.project.requirements->rows)
		if (required.expected_kind == kind) {
			name = required.name;
			return true;
		}
	const AssetKindRow &kind_row = asset_kind_row(kind);
	if (kind_row.file_name && *kind_row.file_name) name = kind_row.file_name;
	else name = kind_row.new_name;
	return !name.empty();
}

// A missing name whose file the project lacks (ADR 0046 DI-33): Create that file with it, the file made by the
// engine's own writer as create_file makes it (its requirement's blank, else its kind's), the name then added to it
// as its type's Add makes one (DI-15's define_symbol), one request: create_file's define. Offered where the name is
// free and one the project's name rules take, the blank takes no value it must be given, and the blank, read
// through its type with the name added, defines the name where the game's lookup finds it (planned with `plan`
// alone: has_fixes reads none of it). Never in bulk.
void create_there_fix(const ReferenceSubject &missing, const SessionView &view, bool plan, std::vector<ProblemFix> &out) {
	if (!view.project.document || !view.project.scan || !view.project.requirements) return;
	std::string name;
	AssetKind kind = AssetKind::Unknown;
	if (!missing_definer(missing, view, name, kind)) return;
	const DocumentType *type = document_type_for(kind);
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (!type || !type->define_symbol || view.project.scan->find(name) || !check_file_name(name, kind, problem, message)) return;
	BlankRequest blank;
	blank.logical_name = name;
	blank.project_title = view.project.document->title;
	// A file the game reads by name is made from its requirement's blank, named as the manifest spells it (a string
	// id's scope names its table upper-case: GAMETEXT.BIN, gametext.bin).
	for (const RequirementRow &row : view.project.requirements->rows)
		if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(name)) {
			blank.role = row.role;
			name = blank.logical_name = row.name;
		}
	const BlankFactory *factory = find_blank_factory(blank.role, name, kind);
	std::string why;
	if (!factory || !blank_values_fit(*factory, blank, why)) return;
	const std::string what = std::string(reference_row(missing.kind).phrase) + " '" + missing.target + "'";
	ProblemFix fix{"Create " + name + " with " + missing.target,
	               "Creates " + name + ", " + factory->summary + ", where the game looks " + what +
	                       " up, with it added as its Add makes one; Undo of " + name + " takes the name back." + kNotUndoable,
	               request::create_file_defining(name, asset_kind_token(kind), missing), false};
	if (!plan) {
		out.push_back(std::move(fix));
		return;
	}
	std::vector<uint8_t> bytes;
	Diagnostic error;
	const std::unique_ptr<DocumentBase> copy = type->make ? type->make() : nullptr;
	PlannedFix planned;
	if (!copy || !make_from(*factory, blank, bytes, error) ||
	    !copy->load_bytes(bytes, name, kind, view.project.document->target_game, error) ||
	    !type->define_symbol(*copy, missing, planned) || planned.edits.empty() ||
	    !defines_after(*copy, planned, name, kind, missing, view))
		return;
	out.push_back(std::move(fix));
}

// A missing shader tag the editor writes an effect for (blank_shader_tags, ADR 0046 DI-33): Create <tag>.fx, the
// object effect registering it, which the renderer loads with every effect of the archives [orig:
// HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF6E]. Offered where the name is free and one the name rules take.
void shader_create_fix(const ReferenceSubject &missing, const SessionView &view, std::vector<ProblemFix> &out) {
	if (missing.kind != ReferenceKind::Shader || !view.project.scan) return;
	const std::vector<std::string> &tags = blank_shader_tags();
	const auto tag = std::find_if(tags.begin(), tags.end(), [&](const std::string &each) { return strutil::iequals(each, missing.target); });
	if (tag == tags.end()) return;
	const std::string name = strutil::to_lower(*tag) + ".fx";
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (view.project.scan->find(name) || !check_file_name(name, AssetKind::Shader, problem, message)) return;
	out.push_back({"Create " + name,
	               "Creates " + name + ", OpenNova's effect for the shader tag " + *tag +
	                       ", which the renderer loads with every effect of the archives." + kNotUndoable,
	               request::create_file(name, asset_kind_token(AssetKind::Shader), {{"tag", *tag}}), false});
}

// A missing symbol: the file where it belongs (defining_file), its Add it there where its type
// defines names of the kind (add_there_fix, planned with `plan` alone: has_fixes and a Fix all read
// none of it), then opened to add it or to see the names it has, or shown in Files when the editor
// does not open its kind.
void symbol_fixes(const ReferenceSubject &missing, const SessionView &view, const ProblemFixIndex &index, bool plan,
                  std::vector<ProblemFix> &out) {
	const AssetEntry *file = defining_file(missing, view);
	shader_create_fix(missing, view, out);
	if (!file) return create_there_fix(missing, view, plan, out);
	if (plan) add_there_fix(missing, view, *file, index, out);
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
	// The pointer's name makes the pointer (find_blank_factory), never the checkerboard.
	const BlankFactory *factory = find_blank_factory("", name, AssetKind::Texture);
	if (factory && factory != find_blank_factory_for_kind(AssetKind::Texture)) {
		out.push_back({"Create " + name, placeholder(*factory), request::create_file(name, asset_kind_token(AssetKind::Texture)), true});
		return;
	}
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
void reference_fixes(const ReferenceSubject &missing, const SessionView &view, const ProblemFixIndex &index, bool plan,
                     std::vector<ProblemFix> &out) {
	if (reference_row(missing.kind).names_symbol()) return symbol_fixes(missing, view, index, plan, out);
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

// What a use asks of a texture an import makes (ADR 0046 S18), weighed with every other use of the file
// (texture_import_state's needs, never the one use alone: another use may read what this one would
// change): the import made as they all ask, where they agree and its options ask otherwise now, the words
// naming the file it makes and saying when that is another name; where they ask what no one file serves,
// the split that gives the uses that ask otherwise a copy of their own. The finding's subject is the use's
// reference, which reaches the file; the use is the referrer's field naming it. None for a file no import
// makes, a use not among its uses, or one already made as they ask.
void import_fit_fix(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	const ReferenceSubject *reference = reference_subject(d);
	if (!reference || !view.findings.graph || !view.documents.texture_uses) return;
	std::string served;
	if (view.findings.graph->resolve(reference->kind, reference->target, reference->scope, &served, reference->loader_arg) !=
	    ReferenceStatus::Present)
		return;
	TextureImportState state;
	std::string error;
	if (!texture_import_state(view, served, state, error) || !state.importer) return;
	const TextureUse *use = nullptr;
	for (const TextureUse &each : view.documents.texture_uses->uses_of(view, served))
		if (!each.fixed && each.referrer == d.asset && each.field == d.field && each.record == d.record) use = &each;
	if (!use) return;
	const TextureImportNeeds &needs = state.needs;
	const std::string file = basename_of(served);
	if (!needs.conflicts.empty()) {
		// No one file serves its uses: the ones that ask otherwise given a copy of their own.
		const std::string copy = free_texture_copy_name(view, served);
		if (needs.split_referrers.empty() || copy.empty()) return;
		std::string files;
		for (const std::string &referrer : needs.split_referrers) files += (files.empty() ? "" : ", ") + basename_of(referrer);
		out.push_back({"Split " + file + " into two files",
		               "Its uses ask what no one file serves (" + needs.conflicts.front() + ") Makes " + copy + ", a copy of " +
		                       file + ", which the uses in " + files +
		                       " name from then on; each file is then made as its own uses ask." + kNotUndoable,
		               request::split_texture(served, copy, needs.split_referrers), false});
		return;
	}
	std::vector<std::pair<std::string, std::string>> changes;
	std::string words;
	ImportOptions after = state.sidecar.options;
	for (const auto &[option, value] : needs.options) {
		const ImportOptionRow *row = import_option_row(state.importer->options, option);
		if (!row || strutil::to_lower(import_option_value(state, *row)) == strutil::to_lower(value)) continue;
		changes.emplace_back(option, value);
		after[option] = value;
		words += (words.empty() ? "" : ", ") + option + " " + value;
	}
	if (changes.empty()) return;
	std::string reasons;
	for (const std::string &reason : needs.reasons) reasons += (reasons.empty() ? "" : "; ") + reason;
	// The file it makes then, by the format's extension and the name option.
	const std::string made = image_import_output_name(state.source, renderer::image_import_settings(after));
	const bool renamed = normalized_logical_name(made) != normalized_logical_name(file);
	out.push_back({"Make " + file + "'s import fit " + (needs.uses > 1 ? "its uses" : "this use"),
	               "Sets the import of " + state.source + " to " + words + " (" + reasons + "), then imports it again, which makes " +
	                       (renamed ? made + " in place of " + file + ": every use of it reads " + made + " from then on."
	                                : file + " anew.") +
	                       kNotUndoable,
	               request::set_import_options(state.source, std::move(changes)), false});
}

// The first item id the engine keeps for nothing that no item of the project has and no file names
// (free_item_id over the graph's item definitions and the ids its edges name).
int project_free_item_id(const AssetGraph &graph) {
	std::set<int> used;
	for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::Item))
		if (const std::optional<int> id = strutil::parse_int(symbol->name)) used.insert(*id);
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (edge.kind != ReferenceKind::Item) return;
		if (const std::optional<int> id = strutil::parse_int(edge.value)) used.insert(*id);
	});
	return free_item_id([&](int id) { return used.count(id) != 0; });
}

// An item on an id an earlier item of its table has (catalog.item_identity, DI-11): no lookup by the id finds
// it [orig: ItemList_FindIndexByTypeId @ 0x49e100, the first of an id], so nothing reaches it by its id and
// what names the id means the earlier one: its id set (an edit of its document, no rename) to one of its own,
// the project's free id by the reserved-id rule.
void item_identity_fix(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	if (!view.findings.graph || d.asset.empty() || d.row_id == 0) return;
	const int id = project_free_item_id(*view.findings.graph);
	const std::string to = std::to_string(id);
	Edit set;
	set.address = NodeAddress{d.row_id, d.record_kind, d.child_id};
	set.field = "id";
	set.value = int64_t(id);
	out.push_back({"Use id " + to,
	               "Gives " + d.record + " id " + to + ", one the engine keeps for nothing and no file of the project names: a "
	               "lookup by its id then finds it. What names its id now still means the earlier item." + kUndoable,
	               request::edit_record(d.asset, set, true), false});
}

// An item on an id the engine keeps for another kind (catalog.reserved_kind), or named as a place the
// engine finds by an id the project lacks (catalog.reserved_name): its id renamed everywhere
// (graph/rename_transaction: items.def and every file that names it, a mission's entities among them),
// to an id of its own the engine keeps for nothing (free_item_id over every item id the project has or
// names), or to the engine's where an item of the engine's kind takes it. The item is the finding's
// record among the graph's item definitions of its file, the one on that kind of id.
void item_id_fix(const Diagnostic &d, const SessionView &view, bool plan, std::vector<ProblemFix> &out) {
	if (d.row() == &finding_code(CatalogFinding::ItemIdentity)) return item_identity_fix(d, view, out);
	if (!view.findings.graph || d.asset.empty() || d.record.empty()) return;
	const AssetGraph &graph = *view.findings.graph;
	const bool kind = d.row() == &finding_code(CatalogFinding::ReservedKind);
	const GraphSymbol *item = nullptr;
	const def::ReservedItem *wanted = nullptr;
	for (const GraphSymbol *symbol : graph.symbols_of(d.asset, d.record)) {
		if (symbol->kind != ReferenceKind::Item) continue;
		const std::optional<int> id = strutil::parse_int(symbol->name);
		const std::optional<int> type = strutil::parse_int(symbol->value);
		if (!id || !type) continue;
		const def::ReservedItem *held = def::reserved_item_by_id(*id);
		if (kind && held && !def::reserved_item_kind_matches(*held, *type)) item = symbol;
		if (!kind && !held && (wanted = reserved_item_named(d.record)) && def::reserved_item_kind_matches(*wanted, *type))
			item = symbol;
		if (item) break;
	}
	if (!item) return;
	std::string to;
	if (kind) {
		to = std::to_string(project_free_item_id(graph));
	} else {
		to = std::to_string(def::DEF_ITEM_ID_BASE + wanted->type);
		if (graph.resolve_symbol(ReferenceKind::Item, to)) return; // another item has it: no fix of this one
	}
	std::string detail = "Gives " + d.record + " id " + to +
	                     (kind ? ", one the engine keeps for nothing," : ", the id the engine finds it by,") +
	                     " in " + basename_of(d.asset) + " and in every file that names it.";
	if (plan && view.project.scan) {
		const SymbolRenamePlan rename = plan_symbol_rename_project(*view.project.scan, graph, *item, to);
		if (!rename.ok()) detail = "Cannot give " + d.record + " id " + to + ": " + rename.refusals.front().message;
		else detail += " It rewrites " + counted(rename.sites.size(), "use") + ".";
	}
	out.push_back({"Use id " + to, detail + kNotUndoable,
	               request::rename_symbol(item->file, item->locator, item->field, to), false});
}

// An items.def whose first row is no Null marker (catalog.first_row): a Null marker added first, the
// row every lookup that finds nothing resolves to (a marker of an id of its own, as a new item is), in
// its document, opened first when it is not: one step Undo takes back, saved with the file.
ProblemFix fallback_row_fix(const std::string &path) {
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(def::DefRecordKind::Item);
	add.position = 0;
	add.field = "display_name";
	add.value = std::string("Null");
	return {"Add a Null marker first",
	        "Adds a marker named Null as the first row of " + path + ", the row the engine gives every id it finds "
	        "no item of, as retail's first row is. Undo takes it back, and Save writes it.",
	        request::edit_record(path, std::move(add), true), false};
}

// A stylesheet variable no menu names (style.unused, DI-11): its line removed, an edit of its document that
// Undo takes back, where no menu's text names it inside a longer text either (the render check's
// variables: the graph's edges, which the finding reads, are the whole values alone). Nothing else reads a
// variable by name [orig: NapiXML_ExpandVariablesInText @ 0x63a000, the stylesheet's list read by the
// expansion alone; docs/mnu/menu-re.md, "%VAR% expansion"], so the game reads nothing less.
void unused_variable_fix(const Diagnostic &d, const SessionView &view, std::vector<ProblemFix> &out) {
	if (!view.findings.graph || d.asset.empty() || d.row_id == 0) return;
	std::string name;
	for (const GraphSymbol *symbol : view.findings.graph->symbols_of(d.asset, d.record))
		if (symbol->kind == ReferenceKind::StyleVar) name = symbol->display;
	const MenuRenderCheck *menus = menu_render_check(view.findings.project_checks.get());
	if (name.empty() || !menus || menus->names_variable(name)) return;
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = NodeAddress{d.row_id, d.record_kind, d.child_id};
	out.push_back({"Remove the line",
	               "Removes the line defining %" + name + "% from " + basename_of(d.asset) +
	                       ": no menu of the project names it, as a whole value or inside a text, and nothing else the "
	                       "game runs reads a variable by name." + kUndoable,
	               request::edit_record(d.asset, remove, true), false});
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
		if (const ReferenceSubject *missing = reference_subject(d)) reference_fixes(*missing, view, index, plan, out);
		return;
	case FindingFix::UnimportedTexture:
		// A texture an import's model names that the import did not bring: the reference's own
		// fixes, while the project still lacks the file (the finding is the import's, kept to
		// the next validation, whose graph reports the reference as missing too).
		if (const ReferenceSubject *missing = reference_subject(d);
		    missing && view.findings.graph &&
		    view.findings.graph->resolve(missing->kind, missing->target, missing->scope, nullptr, missing->loader_arg) ==
		            ReferenceStatus::Missing)
			reference_fixes(*missing, view, index, plan, out);
		return;
	case FindingFix::Reload: {
		// An open document whose file changed outside the editor: read it again, its unsaved
		// edits dropped (the Reload asks about them first, as any Reload does); or, while it has
		// unsaved edits, they kept and written over what the other program saved (ADR 0046 DI-01:
		// a Save that writes over, which Problems confirms first: fix_asks_first).
		const auto open = std::find_if(view.documents.open.begin(), view.documents.open.end(),
		                               [&d](const auto &document) { return document && document->path() == d.asset; });
		if (d.asset.empty() || open == view.documents.open.end()) return;
		out.push_back({"Reload " + basename_of(d.asset),
		               "Reads " + d.asset + " again from its file, which changed outside the editor: its unsaved "
		               "edits are lost (it asks first) and its history starts again." + kNotUndoable,
		               request::reload_document(d.asset), false});
		if ((*open)->dirty())
			out.push_back({"Keep my edits and save over it",
			               "Writes " + d.asset + " with your unsaved edits over its file: what the other program "
			               "saved there is lost." + kNotUndoable,
			               request::save_over(d.asset), false});
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
	case FindingFix::TextureRows:
		// A TGA stored top first: its rows saved bottom first, an edit of its document (opened first) that
		// Undo takes back and Save writes; a file an import makes is made again by its import.
		if (const AssetEntry *file = view.project.scan->at_path(d.asset); file && file->imported_from.empty())
			out.push_back({"Save " + basename_of(d.asset) + " bottom first",
			               "Writes " + d.asset + "'s rows bottom first, the way up its header meant, so the game draws it "
			               "the right way up. Undo takes it back, and Save writes it.",
			               request::texture_operation(d.asset, "reorder_rows", {}, true), false});
		return;
	case FindingFix::ImportFitsUse: import_fit_fix(d, view, out); return;
	case FindingFix::ItemId: item_id_fix(d, view, plan, out); return;
	case FindingFix::FallbackRow:
		if (!d.asset.empty()) out.push_back(fallback_row_fix(d.asset));
		return;
	case FindingFix::NormalRowType: {
		// A finished normal map a normal-map slot's row loads as a diffuse: its row given type 4, which the
		// normal-map loader reads, an .mdt as it is, capped at 512 a side [orig: Material_LoadStageTexture
		// @ 0x5B1778..0x5B1790]. Not a .tga's: type 4 makes a normal map of a .tga's alpha as its height.
		const ReferenceSubject *subject = reference_subject(d);
		if (!subject || d.asset.empty() || d.row_id == 0 ||
		    strutil::to_lower(utf8_of(path_of(subject->target).extension())) != ".mdt")
			return;
		Edit edit;
		edit.operation = EditOperation::Set;
		edit.address = NodeAddress{d.row_id, d.record_kind, d.child_id};
		edit.field = "type";
		edit.value = int64_t(4);
		out.push_back({"Give its row type 4 (normal map)",
		               "Sets the type of the row of " + basename_of(d.asset) + " that names " + subject->target +
		                       " to 4: the game then loads it as a normal map, halved until it fits 512 a side. Undo takes it "
		                       "back, and Save writes it.",
		               request::edit_record(d.asset, edit, true), false});
		return;
	}
	case FindingFix::SetAsideUnread: {
		// A texture no use reads (a .tga beside the .dds its model row loads, which the Blender add-on's export
		// leaves): set aside under .replaced/, never deleted. Offered where it is no import's output and every
		// use of it opens another file.
		const AssetEntry *file = view.project.scan->at_path(d.asset);
		if (!file || !file->imported_from.empty() || !view.documents.texture_uses) return;
		const std::vector<TextureUse> &uses = view.documents.texture_uses->uses_of(view, file->relative_path);
		if (uses.empty() || std::any_of(uses.begin(), uses.end(), [](const TextureUse &use) { return use.reads_file; })) return;
		out.push_back({"Set " + file->logical_name + " aside",
		               "Moves " + file->relative_path + " under " + std::string(kReplacedFolder) + "/, never deleted: no use of it "
		               "reads it, each one's loader opening " + basename_of(uses.front().served) + " in its place, so the build "
		               "packs it for nothing." + kNotUndoable,
		               request::set_aside_texture(file->relative_path), true});
		return;
	}
	case FindingFix::EditRecord:
		// What the finding's maker planned with its file at hand (Diagnostic::planned): each an edit of the file.
		if (!d.asset.empty())
			for (const PlannedFix &planned : d.planned) out.push_back(edit_fix(d.asset, planned));
		return;
	case FindingFix::UnusedVariable: unused_variable_fix(d, view, out); return;
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

namespace {

const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &open : view.documents.open)
		if (open && open->path() == path) return open.get();
	return nullptr;
}

} // namespace

const DocumentBase *ProblemFixIndex::definer(const SessionView &view, const AssetEntry &file) const {
	if (const DocumentBase *open = open_at(view, file.relative_path)) return open;
	return trial(view, file);
}

DocumentBase *ProblemFixIndex::trial(const SessionView &view, const AssetEntry &file) const {
	// The state it is a copy of: the open document's (its instance, its load and its revision), else the file's.
	const DocumentBase *open = open_at(view, file.relative_path);
	const uint64_t identity = open ? open->identity() : 0, generation = open ? open->load_generation() : 0,
	               revision = open ? open->revision() : 0;
	Copy &held = copies_[file.relative_path];
	if (held.read && held.identity == identity && held.generation == generation && held.revision == revision)
		return held.document.get();
	held = Copy{nullptr, identity, generation, revision, true};
	const DocumentType *type = document_type_for(file.kind);
	if (!type || !type->make || !view.project.document || view.project.root.empty()) return nullptr;
	std::shared_ptr<DocumentBase> document = type->make();
	Diagnostic error;
	bool loaded = false;
	if (open) {
		// The open document as it stands: read again from what it would save.
		const SerializeResult now = open->serialize();
		loaded = now.ok() && document->load_bytes(std::vector<uint8_t>(now.text.begin(), now.text.end()), file.relative_path,
		                                          file.kind, view.project.document->target_game, error);
	} else {
		loaded = document->load(join_path(view.project.root, file.relative_path), file.relative_path, file.kind,
		                        view.project.document->target_game, error);
	}
	if (loaded) held.document = std::move(document);
	return held.document.get();
}

std::vector<ProblemFix> fixes_for(const Diagnostic &diagnostic, const SessionView &view, const ProblemFixIndex *index) {
	return fixes_over(diagnostic, view, index, true);
}

ProblemFix edit_fix(const std::string &path, const PlannedFix &planned) {
	return {planned.label, planned.detail + kUndoable, request::edit_record(path, planned.edits, true), false};
}

bool missing_target(const ReferenceSubject &missing, const SessionView &view, ReferenceTarget &out) {
	// A record of a file by its index (a Record reference) is its own file's: no other place it belongs.
	const ReferenceKindRow &row = reference_row(missing.kind);
	if (!view.project.open || !view.project.scan || !row.names_symbol() || row.resolution == ReferenceResolution::Record)
		return false;
	const AssetEntry *file = defining_file(missing, view);
	if (!file) return false;
	out = ReferenceTarget();
	out.label = "where " + std::string(reference_row(missing.kind).phrase) + " '" + missing.target + "' belongs: " +
	            file->relative_path;
	out.file = file->relative_path;
	out.editable = is_editable_kind(file->kind);
	out.missing = true;
	return true;
}

bool missing_target(const FieldUse &field, const Value &value, const SessionView &view, ReferenceTarget &out) {
	ReferenceKind kind;
	std::string name, scope;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph || !reference_target(field, value, kind, name, scope)) return false;
	if (graph->resolve(kind, name, scope, nullptr, field.loader_arg) != ReferenceStatus::Missing) return false;
	// A %NAME% the stylesheets the game reads do not define stands for the file: the variable is what is
	// missing (missing_finding's subject), and belongs in a stylesheet.
	if (kind != ReferenceKind::StyleVar && graph_names::is_style_reference(name) && !graph->style_binding(name)) {
		kind = ReferenceKind::StyleVar;
		scope.clear();
	}
	ReferenceSubject missing;
	missing.kind = kind;
	missing.target = name;
	missing.scope = scope;
	missing.loader_arg = field.loader_arg;
	return missing_target(missing, view, out);
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
	// What writes the project at once (the placeholders made) before what starts an operation over its files
	// (the import dialog's plan, which holds them while it runs): raised in one frame, neither is refused as
	// busy (a Create after the plan started would be).
	std::stable_partition(requests.begin(), requests.end(), [](const EditorRequest &request) {
		return request.kind != EditorRequestKind::PreviewInstallImport && request.kind != EditorRequestKind::PreviewImport;
	});
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
