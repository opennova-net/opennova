#pragma once

#include <string>
#include <utility>
#include <vector>

#include <editor/graph/file_plans.h>
#include <editor/import/import_run.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_trash.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;
struct ProjectPaths;
struct SessionView;

// Who names what a delete takes from the project (DeletePlan::named): each use the card's Named by lists
// (DI-05's file_users) from a file the delete leaves, and each import that reads one from its place; its count,
// with the first `most` of them in words ("items.def: Wooden supply crate - Graphic", "the import of
// art/terrain/onisle1.tset") in `places`. What Files' Delete... lists and what refuses a delete_asset without force.
size_t deleted_uses(const SessionView &view, const DeletePlan &plan, std::vector<std::string> &places, size_t most);
// The same of several files deleted together: a use from a file any of them takes is none.
size_t deleted_uses(const SessionView &view, const std::vector<DeletePlan> &plans, std::vector<std::string> &places,
                    size_t most);

// The files a chore over several files acts on (a request's `path`, then each of its `paths`), each planned as
// a delete, the plans of those a plan before or after them takes with it (a mission's companion, an import
// source's record) left out, so each file goes once; their refusals in `refusals`.
std::vector<DeletePlan> plan_deletes(const ProjectPaths &paths, const AssetScan &scan, const EditorRequest &request,
                                     std::vector<Diagnostic> &refusals);
// The files a folder deletes with it (DeleteFolder's `all`): each file of the project in it, planned as a delete
// (a mission with its companions wherever they sit, an import source with its record and its outputs).
std::vector<DeletePlan> plan_folder_deletes(const ProjectPaths &paths, const AssetScan &scan, const std::string &folder,
                                            std::vector<Diagnostic> &refusals);

// Files' chores (DI-25, the deep-integration plan): files deleted, duplicated, moved together or made in a
// folder of the modder's choosing, and a folder made, renamed or deleted (with what it holds when asked), each one
// step of the project's file history that
// Edit's Undo file and Redo file take back and do again (the documents' Undo is their records'; a rename's way
// back is its own, Edit > Rename back). Planned over the scan (graph/file_plans.h), then done at once: nothing
// here is an operation, but a folder's rename runs each file's move through the rename transaction (plan_move:
// no reference rewritten, the game finding a file by its name alone) and an import source's copy or move has
// the import pass make its outputs (a refresh). A delete never removes a file: it goes to the project's trash
// (project/project_trash.h), from where Undo file brings it back; so does a step's undoing of what it made.
// Emptying the trash (EmptyTrash) is the one chore that removes files, and the file history goes with it.
class FileChores {
public:
	explicit FileChores(SessionCore &core);
	FileChores(const FileChores &) = delete;
	FileChores &operator=(const FileChores &) = delete;

	// DeleteAsset: the file and each of `paths` (a mission with the files the game finds by its name, an import
	// source with its record, and its outputs or, `alone`, its outputs kept as files of the project) to the trash
	// as one batch, their open documents closed. Refused, nothing deleted, when one is refused, and while anything
	// the delete leaves names what goes (file.named: who, in words, as the card's Named by lists them, and an
	// import that reads it), unless `force` leaves those references naming nothing: Problems rows then, until
	// Undo file brings them back.
	void delete_asset(const EditorRequest &request);
	// DuplicateAsset: the file copied in its own folder under `new_name` ("" the name duplicate_name gives), and
	// each of `paths` under the name duplicate_name gives it beside the copies before it; a mission with its
	// companions, an import source with its record (its import then makes the copy's outputs) unless `alone`;
	// the first copy selected in Files. Refused, nothing copied, when one is refused.
	void duplicate_asset(const EditorRequest &request);
	// MoveAsset with `paths`: the file and each of them moved to `folder` under its own name, each as Move to folder
	// moves one (the rename transaction, no reference rewritten, a mission with its companions beside it, an
	// import source with its record), as one step of the file history (Undo file moves them back); refused,
	// nothing moved, when one is refused, and each put back when one does not move.
	void move_files(const EditorRequest &request);
	// NewFolder, DeleteFolder (an empty one; with `all`, with every file it holds, as DeleteAsset deletes each of
	// them, `force` over the uses that names what goes; the whole folder to the trash), RenameFolder (each file of
	// it moved through the rename transaction, its open documents, its card and the navigation's places
	// following).
	void new_folder(const std::string &folder);
	void delete_folder(const EditorRequest &request);
	void rename_folder(const std::string &folder, const std::string &new_name);
	// EmptyTrash: what the project's trash holds removed for good, the file history with it (each of its steps
	// takes what it put there back from there).
	void empty_trash();
	// UndoFile, RedoFile: the history's last step taken back, the last one taken back done again; refused
	// (file.history) with none, or when the files are not as the step left them (it stays to try again).
	void undo();
	void redo();
	// A new file's files made (CreateFile: the file, a mission's text table, a menu's pointer) and the folders
	// made for them: one step, which Undo file takes to the trash, the folders with it once empty.
	void made(const std::string &words, const std::vector<std::string> &files,
	          const std::vector<std::string> &folders = {});
	// The documents with unsaved edits a DeleteAsset, a DuplicateAsset (each copied as saved), a MoveAsset of
	// several files, a DeleteFolder, a RenameFolder, an UndoFile or a RedoFile would take away, copy or move (the
	// unsaved guard's PlannedWrites); none where the request is refused anyway.
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);
	// The project closed: its history goes with it (the trash keeps what it holds).
	void forget();

private:
	// One step: what it put in the trash (`gone`, there while it is done) and what it made (`came`, in the
	// project while it is done), each side's batch while it is in the trash; the folders it made and removed;
	// a folder's rename (`from` to `to`); files moved together (`moves`: each file's path before and after, a
	// mission's companions going with it); the import sources of the side in the trash, put back in the
	// project's import list as they come back.
	struct Step {
		std::string words;
		std::vector<std::string> gone, came;
		TrashBatch gone_batch, came_batch;
		std::vector<std::string> folders_made, folders_gone;
		std::string from, to;
		std::vector<std::pair<std::string, std::string>> moves;
		std::vector<ImportedSource> sources;
	};
	// The step taken back (`back`) or done again; false, refused with why, nothing changed.
	bool take(Step &step, bool back);
	// A folder renamed now (plan_folder_rename, each move committed in turn, put back when one fails), its files
	// read again, its documents and places following; false refused with why.
	bool move_folder(const std::string &folder, const std::string &new_name, std::string *from = nullptr,
	                 std::string *to = nullptr);
	// Each file moved to its folder (`asked`: the file, the folder), planned and committed together; the moves
	// done (`moved`: each file before and after) on true; false refused with why, nothing moved.
	bool move_now(const std::vector<std::pair<std::string, std::string>> &asked,
	              std::vector<std::pair<std::string, std::string>> *moved = nullptr);
	// Each move committed in turn through the rename transaction; one that does not take puts the ones before it
	// back (false, refused with why). `moved` the files moved and their companions, `touched` what the scan reads
	// again, `sources` whether an import source moved.
	bool commit_moves(const std::vector<RenamePlan> &moves, std::vector<std::pair<std::string, std::string>> &moved,
	                  std::vector<std::string> &touched, bool &sources);
	// The project's files at `paths` (a file, or each file in a folder among them: the scan's while they are
	// there, the disk's once they are back): what the scan reads again.
	std::vector<std::string> files_at(const std::vector<std::string> &paths) const;
	// The files moved `moved` (from, to): their open documents closed and opened again there (the active one
	// active still), the card and the navigation's places following.
	void follow(const std::vector<std::pair<std::string, std::string>> &moved);
	// The open documents of `paths` (and of the files in the folders among them) closed.
	void close_documents(const std::vector<std::string> &paths);
	// The import sources among `paths` taken out of the project's import list (into `taken`), or `sources` put
	// back in it.
	void drop_sources(const std::vector<std::string> &paths, std::vector<ImportedSource> &taken);
	void add_sources(const std::vector<ImportedSource> &sources);
	// What a step leaving the project would take, as the trash takes it (a copied source's outputs made since).
	std::vector<std::string> leaving(const Step &step, bool back) const;
	void push(Step step);
	void show();
	void refuse(CoreFinding code, const std::string &message, const std::string &asset = std::string());
	void refuse(const std::vector<Diagnostic> &refusals);

	std::vector<Step> done_, undone_;
	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
};

} // namespace opennova::editor
