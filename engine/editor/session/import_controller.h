#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/import/texture_source.h>
#include <editor/model/finding_code_row.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class ImportOperation;
class ImportPlanOperation;
class SessionCore;
struct ImportPlan;
struct ProjectPaths;
struct SessionView;

// Imports in the project session (ADR 0046 S8, S11g, S13 A2): the import dialog's preview (the
// files to choose from, the files chosen, the plan of importing them with the files they need,
// planned again whenever it is asked for and before it is written), the editor's "Include the
// files these need", the import written (import_assets) and the refresh after it, a source
// imported again, and the game install's file names the Import fixes read. The unsaved guard asks
// it which documents with unsaved edits an import would write over. The plans, the import and a
// source imported again run as operations (S13 A3: ImportPlanOperation, ImportOperation,
// RefreshOperation), each finishing here or in the core.
class ImportController {
public:
	explicit ImportController(SessionCore &core);
	ImportController(const ImportController &) = delete;
	ImportController &operator=(const ImportController &) = delete;

	// PreviewImport: the picked files, a loose one chosen, an archive's members listed.
	void preview_files(const EditorRequest &request);
	// PlanImport: the files chosen, planned again, the list kept.
	void plan(const EditorRequest &request);
	// PreviewInstallImport: the game install's files, those named alone and chosen, or every one
	// listed to choose from; with `all`, every one chosen at once and none to choose from, planned
	// with no walk (ADR 0046 S14: the closure of everything is everything).
	void preview_install(const EditorRequest &request);
	void set_dependencies(bool with_dependencies);
	void cancel();
	void import_files(const EditorRequest &request);
	void reimport(const std::string &source, bool force);
	// SetImportOptions (S18): the record of the import path names (a source, or a file an import makes)
	// given values, each an option's key and a value its row takes ("" its default), written when it
	// changed, then the refresh that imports it again. Refused, nothing written: a file no import makes, a
	// key no row has, a value its row does not take (import.option).
	void set_options(const std::string &path, const std::vector<std::pair<std::string, std::string>> &values);
	// ReplaceTexture (S18): the texture made from the image the request names (import/texture_source.h's
	// plan, done: the image an import source in art/, its record, the replaced file set aside), then the
	// refresh that imports it; the texture's open document, clean, closed. Refused, nothing written
	// (texture.replace): what the plan refuses, an image that does not read, a texture open with unsaved
	// edits.
	void replace_texture(const EditorRequest &request);
	// EditExternally (S18): the texture's source opened in its program (import/texture_source.h's
	// plan_texture_source: made once for a plain texture, then imported), the OpenExternally view event the
	// Shell opens it by; the texture_source dialog closed. Refused (texture.external): what the plan
	// refuses, a texture or a source edited in place open with unsaved edits.
	void edit_externally(const EditorRequest &request);
	// StoreAsDds (S18): the .tga texture the request names stored as the .dds of its name, which every use of
	// it reads first (import/texture_source.h's plan_texture_dds over its uses: texture_use_opens), then the
	// refresh that imports it; a plain file's clean open document closed. Refused, nothing written
	// (texture.store_dds): what the plan refuses, a texture open with unsaved edits.
	void store_as_dds(const EditorRequest &request);
	// SetAsideTexture (S18): a texture no use of it reads (each use's loader opens another file of its name)
	// set aside under .replaced/ (import/texture_source.h set_aside_project_file), its clean open document
	// closed, then a rescan. Refused, nothing moved (texture.set_aside): no such texture, a file an import
	// makes, a texture no use names, one a use reads, one open with unsaved edits.
	void set_aside_texture(const EditorRequest &request);
	// OpenTextureSource (S18): an existing source opened alike, nothing written. Refused (texture.external):
	// a texture with none yet, a source edited in place open with unsaved edits.
	void open_texture_source(const EditorRequest &request);
	// NewTerrain (S20): a terrain made from images: its terrain set written in art/terrain/ naming the
	// images (each copied in from disk, or a project file named where it is), its record the importer's
	// options, then the refresh that imports it. Refused, nothing written (import.terrain): a name that
	// does not fit or that the project has a file of, a value of no key it takes, an image that does not
	// read or fit its role, foliage definitions the game would not read as given (import/terrain_import.h).
	void new_terrain(const EditorRequest &request);
	// PreviewTextureSource (S18): what a Replace (an image in paths) or an Edit externally (none) would do,
	// into the view's texture_source dialog, nothing written: the plan's changes, its before and after in
	// words and as pictures (the texture's thumbnail, the file the import would make), the stored forms
	// offered and the one written, or the plan's refusal.
	void preview_texture_source(const EditorRequest &request);
	// The texture_source dialog closed (CancelTextureSource, and a Replace or an Edit externally done).
	void close_texture_source();
	// The game install's file names, for the Import fixes (problem_fixes.h), and for a project that
	// builds as an expansion its base game's, for its build's gate (ADR 0046 S16).
	void refresh_install_files();
	// Those an Open read (OpenOperation).
	void set_install_files(std::vector<std::string> names, std::vector<std::string> base);
	// The documents with unsaved edits an ImportFiles would write over (the unsaved guard's).
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);
	// The project closes: the dialog, the imported sources and the install's names go.
	void clear();

	// An import's plan done (ImportPlanOperation's finish): the plan the dialog shows.
	OperationOutcome absorb_plan(ImportPlanOperation &operation);
	// An import done (ImportOperation's finish): the plan made again shown, and nothing more when it
	// was not the one shown (import.changed) or lacks a row asked for (import.not_planned); else the
	// dialog closed, the open documents whose files it replaced read again, the refresh after it
	// taken into the view, what it imported said.
	OperationOutcome absorb_import(ImportOperation &operation);

private:
	void preview(std::vector<ImportChoice> choices, std::vector<ImportChoiceFacts> facts, std::vector<ImportChoice> roots,
	             bool with_dependencies, bool all = false);
	// What an ImportFiles takes: its imports and its replace, or with `planned` the open preview's rows as
	// the dialog's Import takes them (import_selection over the workspace's checks: the checked rows the project
	// can take, once per source, replacing where Replace existing files or a checked held row says so, or the
	// request does); false, said why, with planned and no preview open, or a plan named that is not the one shown.
	bool sources_of(const EditorRequest &request, std::vector<ImportChoice> &imports, bool &replace);
	// The open preview planned again as an operation (ImportPlanOperation): the dialog shows its
	// files at once and the plan once it is made; a plan that runs gives way to the new one (the
	// rows' Supersede).
	void start_plan();
	// A plan made for the dialog, shown: its rows, whether it is not `shown` (the plan an Import was
	// shown), the ImportPlanned event on which the dialog takes its checks again, the status line.
	void show_plan(std::shared_ptr<const ImportPlan> plan, const ImportPlan *shown);
	// An import's one Output line: its files, bytes and kinds (its files folded under it).
	std::string import_words(const std::vector<std::string> &paths) const;
	// What a Replace request would do (its image read, what the texture's uses ask).
	TextureSourcePlan replace_plan(const EditorRequest &request) const;
	// The OpenExternally event for a source, and the status line.
	void post_open_externally(const std::string &source);
	// A source edited in place open with unsaved edits: refused with `code`, true.
	bool source_dirty(const std::string &source, CoreFinding code);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
	// The plans made for the dialog, each the next serial (DialogsView::ImportPreview::plan_serial), and the plan
	// the workspace's checks were of while another is planned (its checks carried over by row).
	uint64_t plans_ = 0;
	std::shared_ptr<const ImportPlan> checked_plan_;
};

} // namespace opennova::editor
