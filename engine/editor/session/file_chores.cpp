#include <editor/session/file_chores.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/file_plans.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/file_card.h>
#include <editor/session/navigation_controller.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace fs = std::filesystem;

namespace {

fs::path at(const ProjectPaths &paths, const std::string &relative) {
	return system_path(join_path(paths.root, relative));
}

bool exists_at(const ProjectPaths &paths, const std::string &relative) {
	std::error_code ec;
	return fs::exists(at(paths, relative), ec);
}

// Whether `path` is `folder` or lies in it (as the file system compares names).
bool within(const std::string &path, const std::string &folder) {
	if (strutil::iequals(path, folder)) return true;
	return path.size() > folder.size() && path[folder.size()] == '/' && strutil::iequals(path.substr(0, folder.size()), folder);
}

// A folder in words: "the top level", else its path with a slash.
std::string folder_words(const std::string &folder) {
	return folder.empty() ? std::string("the top level") : folder + "/";
}

// The paths of a side of a step that are the project's files (not a folder under the cache): what a scan reads
// again.
std::vector<std::string> project_files_of(const std::vector<std::string> &paths) {
	std::vector<std::string> out;
	for (const std::string &path : paths)
		if (path.rfind(".opennova/", 0) != 0 && path.rfind(".opennova\\", 0) != 0) out.push_back(path);
	return out;
}

} // namespace

size_t deleted_uses(const SessionView &view, const DeletePlan &plan, std::vector<std::string> &places, size_t most) {
	// A use from a file that goes with it is none.
	const std::vector<std::string> gone = plan.named();
	const auto going = [&](const std::string &file) {
		const auto same = [&](const std::string &each) { return strutil::iequals(each, file); };
		return std::any_of(gone.begin(), gone.end(), same) || std::any_of(plan.files.begin(), plan.files.end(), same);
	};
	size_t uses = 0;
	for (const std::string &named : gone) {
		const FileUsers users = file_users(view, named);
		for (const FileUsers::UseGroup &group : users.groups) {
			if (going(group.file)) continue;
			for (const FileUsers::Use &use : group.uses) {
				++uses;
				if (places.size() < most) places.push_back(group.name + ": " + use.line.words);
			}
		}
		if (view.project.imports)
			for (const ImportedSource &source : *view.project.imports)
				if (!going(source.source))
					for (const std::string &input : source.inputs)
						if (strutil::iequals(input, named)) {
							++uses;
							if (places.size() < most) places.push_back("the import of " + source.source);
						}
	}
	return uses;
}

FileChores::FileChores(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

void FileChores::refuse(CoreFinding code, const std::string &message, const std::string &asset) {
	core_.refuse_request(code, message, asset, DiagnosticSeverity::Error);
}

void FileChores::refuse(const std::vector<Diagnostic> &refusals) {
	for (const Diagnostic &d : refusals) core_.refuse_request(d);
}

void FileChores::forget() {
	done_.clear();
	undone_.clear();
	show();
}

void FileChores::show() {
	ActivityView::FileHistory history;
	if (!done_.empty()) history.undo = done_.back().words;
	if (!undone_.empty()) history.redo = undone_.back().words;
	history.undo_steps = done_.size();
	history.redo_steps = undone_.size();
	ActivityView::FileHistory &shown = view_.activity.file_history;
	if (shown.undo == history.undo && shown.redo == history.redo && shown.undo_steps == history.undo_steps &&
	    shown.redo_steps == history.redo_steps)
		return;
	shown = std::move(history);
	core_.touch(ViewConcern::Files);
}

void FileChores::push(Step step) {
	done_.push_back(std::move(step));
	undone_.clear(); // a new step: what was taken back is no longer the way forward
	show();
}

void FileChores::made(const std::string &words, const std::vector<std::string> &files,
                      const std::vector<std::string> &folders) {
	if (files.empty()) return;
	Step step;
	step.words = words;
	step.came = files;
	step.folders_made = folders;
	push(std::move(step));
	if (!folders.empty()) core_.update_folders(folders, {});
}

void FileChores::close_documents(const std::vector<std::string> &paths) {
	DocumentSet &documents = core_.documents();
	std::vector<std::string> closing;
	for (const auto &document : documents.documents())
		for (const std::string &path : paths)
			if (within(document->path(), path)) {
				closing.push_back(document->path());
				break;
			}
	for (const std::string &path : closing) documents.close_document(path);
}

void FileChores::drop_sources(const std::vector<std::string> &paths, std::vector<ImportedSource> &taken) {
	if (!view_.project.imports) return;
	std::vector<ImportedSource> kept;
	bool dropped = false;
	for (const ImportedSource &source : *view_.project.imports) {
		const bool goes = std::any_of(paths.begin(), paths.end(), [&](const std::string &path) { return within(source.source, path); });
		if (goes) taken.push_back(source);
		else kept.push_back(source);
		dropped = dropped || goes;
	}
	if (dropped) view_.project.imports = std::make_shared<const std::vector<ImportedSource>>(std::move(kept));
}

void FileChores::add_sources(const std::vector<ImportedSource> &sources) {
	if (sources.empty()) return;
	std::vector<ImportedSource> all = view_.project.imports ? *view_.project.imports : std::vector<ImportedSource>();
	all.insert(all.end(), sources.begin(), sources.end());
	view_.project.imports = std::make_shared<const std::vector<ImportedSource>>(std::move(all));
}

// --- a delete ----------------------------------------------------------------------------------

void FileChores::delete_asset(const EditorRequest &request) {
	if (!view_.project.open) return;
	const DeletePlan plan = plan_delete(paths_, *view_.project.scan, request.path, request.alone);
	if (!plan.ok()) return refuse(plan.refusals);
	// Who names what goes: refused unless asked to leave those uses naming nothing.
	std::vector<std::string> places;
	const size_t uses = deleted_uses(view_, plan, places, 3);
	if (uses && !request.force) {
		std::string listed;
		for (const std::string &place : places) listed += (listed.empty() ? "" : "; ") + place;
		if (uses > places.size()) listed += "; and " + std::to_string(uses - places.size()) + " more";
		return refuse(CoreFinding::FileNamed,
		              plan.name + " is named by " + counted(uses, "use") + " (" + listed +
		                      "). Deleted, they name nothing and are Problems until changed: delete it anyway (force) "
		                      "or cancel.",
		              plan.path);
	}
	// Its open documents closed (the unsaved prompt saved their edits first); an output kept copied out of the
	// cache before the cache's copy goes.
	close_documents(plan.files);
	std::vector<std::string> kept;
	std::string error;
	const auto unkeep = [&] {
		std::error_code ignored;
		for (const std::string &file : kept) fs::remove(at(paths_, file), ignored);
	};
	std::vector<std::string> folders_made;
	for (const auto &[from, to] : plan.kept) {
		std::error_code ec;
		for (std::string dir = folder_of_path(to); !dir.empty() && !exists_at(paths_, dir); dir = folder_of_path(dir))
			if (std::find(folders_made.begin(), folders_made.end(), dir) == folders_made.end())
				folders_made.insert(folders_made.begin(), dir);
		if (!io::ensure_directory(utf8_of(at(paths_, to).parent_path()), error) ||
		    !fs::copy_file(at(paths_, from), at(paths_, to), fs::copy_options::none, ec)) {
			unkeep();
			return refuse(CoreFinding::FileWrite,
			              basename_of(to) + " could not be kept: " + (error.empty() ? ec.message() : error) + ". Nothing was deleted.",
			              plan.path);
		}
		io::refresh_last_write(join_path(paths_.root, to), error);
		kept.push_back(to);
	}
	std::vector<std::string> trashed = plan.files;
	if (!plan.output_dir.empty()) trashed.push_back(plan.output_dir);
	TrashBatch batch;
	if (!trash_paths(paths_, trashed, batch, error)) {
		unkeep();
		return refuse(CoreFinding::FileTrash, error + " Nothing was deleted.", plan.path);
	}
	Step step;
	step.words = "Delete " + plan.name + (plan.alone ? " alone" : "");
	step.gone = trashed;
	step.gone_batch = batch;
	step.came = kept;
	step.folders_made = folders_made;
	drop_sources(plan.files, step.sources);
	std::vector<std::string> read = plan.files;
	read.insert(read.end(), kept.begin(), kept.end());
	core_.update_files(read);
	if (!folders_made.empty()) core_.update_folders(folders_made, {});
	// What went, and what it says of the rest.
	std::string with;
	for (size_t i = 1; i < plan.files.size(); ++i)
		if (!strutil::ends_with_icase(plan.files[i], kImportSidecarSuffix)) with += (with.empty() ? "" : ", ") + basename_of(plan.files[i]);
	std::string line = "Deleted " + plan.name + (with.empty() ? std::string() : " with " + with);
	if (!plan.outputs.empty()) line += (with.empty() ? " with " : " and ") + std::string("its import's ") + counted(plan.outputs.size(), "output");
	if (!kept.empty()) {
		std::string names;
		for (const std::string &file : kept) names += (names.empty() ? "" : ", ") + file;
		line += " alone: its outputs are files of the project now (" + names + ")";
	}
	line += ". It is in the project's trash: Edit > Undo file brings it back.";
	if (uses) line += " " + counted(uses, "use") + " of it name nothing now (Problems).";
	core_.note(line);
	view_.activity.status = "Deleted " + plan.name + ".";
	core_.touch(ViewConcern::Output);
	push(std::move(step));
}

// --- a duplicate -------------------------------------------------------------------------------

void FileChores::duplicate_asset(const EditorRequest &request) {
	if (!view_.project.open) return;
	const BaseNames base{&view_.project.base_files};
	const DuplicatePlan plan = plan_duplicate(paths_, *view_.project.scan, request.path, request.new_name, request.alone, &base);
	if (!plan.ok()) return refuse(plan.refusals);
	std::vector<std::string> made;
	std::string error;
	for (const auto &[from, to] : plan.copies) {
		std::error_code ec;
		if (!io::ensure_directory(utf8_of(at(paths_, to).parent_path()), error) ||
		    !fs::copy_file(at(paths_, from), at(paths_, to), fs::copy_options::none, ec)) {
			std::error_code ignored;
			for (const std::string &file : made) fs::remove(at(paths_, file), ignored);
			return refuse(CoreFinding::FileWrite,
			              to + " could not be made: " + (error.empty() ? ec.message() : error) + ". Nothing was copied.", plan.path);
		}
		// A copy keeps its source's last write: as written now, every cache keyed by it reads it afresh.
		io::refresh_last_write(join_path(paths_.root, to), error);
		made.push_back(to);
	}
	Step step;
	step.words = "Duplicate " + plan.name + " as " + plan.new_name;
	step.came = made;
	push(std::move(step));
	core_.update_files(made);
	core_.documents().select_file(plan.new_path);
	std::string with;
	for (size_t i = 1; i < plan.copies.size(); ++i)
		if (!strutil::ends_with_icase(plan.copies[i].second, kImportSidecarSuffix))
			with += (with.empty() ? "" : ", ") + basename_of(plan.copies[i].second);
	std::string line = "Duplicated " + plan.name + " as " + plan.new_path + (with.empty() ? std::string() : " with " + with);
	if (plan.import) {
		std::string outputs;
		for (const std::string &output : plan.outputs) outputs += (outputs.empty() ? "" : ", ") + output;
		line += ", its import record with it: the import makes the copy's own outputs" +
		        (outputs.empty() ? std::string() : " (" + outputs + ")");
		// The import pass makes them now (a refresh, which imports the copy's new record); one that cannot start
		// says so.
		if (!core_.start_refresh()) line += " at the next refresh";
	} else if (request.alone && plan.copies.size() == 1) {
		line += ", without its import record";
	}
	core_.note(line + ".");
	view_.activity.status = "Duplicated " + plan.name + " as " + plan.new_name + ".";
	core_.touch(ViewConcern::Output);
}

// --- folders -----------------------------------------------------------------------------------

void FileChores::new_folder(const std::string &text) {
	if (!view_.project.open) return;
	std::string folder, why;
	if (!project_folder_of(paths_, *view_.project.document, text, folder, why)) return refuse(CoreFinding::FileFolder, why, text);
	if (folder.empty()) return refuse(CoreFinding::FileFolder, "The top level of the project is there already.", text);
	if (exists_at(paths_, folder)) return refuse(CoreFinding::FileExists, "Something sits at " + folder + " already.", folder);
	std::vector<std::string> made;
	for (std::string dir = folder; !dir.empty() && !exists_at(paths_, dir); dir = folder_of_path(dir)) made.insert(made.begin(), dir);
	std::string error;
	if (!io::ensure_directory(join_path(paths_.root, folder), error))
		return refuse(CoreFinding::FileWrite, "The folder " + folder + " could not be made: " + error, folder);
	Step step;
	step.words = "New folder " + folder;
	step.folders_made = made;
	push(std::move(step));
	core_.update_folders(made, {});
	core_.note("Made the folder " + folder + "/.");
	view_.activity.status = "Made the folder " + folder + "/.";
	core_.touch(ViewConcern::Output);
}

void FileChores::delete_folder(const std::string &text) {
	if (!view_.project.open) return;
	std::string folder, why;
	if (!project_folder_of(paths_, *view_.project.document, text, folder, why)) return refuse(CoreFinding::FileFolder, why, text);
	if (folder.empty()) return refuse(CoreFinding::FileFolder, "The top level of the project is no folder to delete.", text);
	std::error_code ec;
	if (!fs::is_directory(at(paths_, folder), ec)) return refuse(CoreFinding::FileUnknown, "The project has no folder " + folder + ".", folder);
	if (!fs::is_empty(at(paths_, folder), ec) || ec) {
		size_t files = 0;
		for (const AssetEntry &entry : view_.project.scan->entries) files += within(entry.relative_path, folder) ? 1 : 0;
		return refuse(CoreFinding::FileFolder,
		              folder + " is not empty" + (files ? " (" + counted(files, "file") + " of the project)" : std::string()) +
		                      ": delete or move what it holds first.",
		              folder);
	}
	if (!fs::remove(at(paths_, folder), ec))
		return refuse(CoreFinding::FileWrite, "The folder " + folder + " could not be removed: " + ec.message(), folder);
	Step step;
	step.words = "Delete folder " + folder;
	step.folders_gone = {folder};
	push(std::move(step));
	core_.update_folders({}, {folder});
	core_.note("Deleted the empty folder " + folder + "/.");
	view_.activity.status = "Deleted the folder " + folder + "/.";
	core_.touch(ViewConcern::Output);
}

void FileChores::rename_folder(const std::string &folder, const std::string &new_name) {
	if (!view_.project.open) return;
	std::string from, to;
	if (!move_folder(folder, new_name, &from, &to)) return;
	Step step;
	step.words = "Rename folder " + from + " to " + basename_of(to);
	step.from = from;
	step.to = to;
	push(std::move(step));
}

bool FileChores::move_folder(const std::string &folder, const std::string &new_name, std::string *from_out,
                             std::string *to_out) {
	const std::shared_ptr<const AssetScan> scan = view_.project.scan;
	const ProjectDocument &project = *view_.project.document;
	const FolderPlan plan = plan_folder_rename(paths_, project, *scan, folder, new_name, view_.project.imports.get());
	if (!plan.ok()) {
		refuse(plan.refusals);
		return false;
	}
	std::string error;
	std::vector<std::string> made;
	for (const std::string &each : plan.folders) {
		const std::string there = path_in_renamed(each, plan.from, plan.to);
		if (!io::ensure_directory(join_path(paths_.root, there), error)) {
			refuse(CoreFinding::FileWrite, "The folder " + there + " could not be made: " + error + ". Nothing was moved.", plan.from);
			return false;
		}
		made.push_back(there);
	}
	// Each file's move committed in turn through the rename transaction; one that does not take puts the ones
	// before it back.
	const AssetGraph &graph = core_.problems().graph();
	std::vector<std::pair<std::string, std::string>> moved;
	std::vector<std::string> touched;
	bool sources = false;
	for (const RenamePlan &move : plan.moves) {
		RenameTransaction transaction(paths_, project, *scan, graph, move);
		while (!transaction.step()) {
		}
		if (!transaction.ok()) {
			for (auto it = plan.moves.begin(); it != plan.moves.end() && &*it != &move; ++it) {
				std::error_code ignored;
				io::rename_with_retry(at(paths_, it->new_path), at(paths_, it->path), ignored);
				if (!it->sidecar.empty()) io::rename_with_retry(at(paths_, it->new_sidecar), at(paths_, it->sidecar), ignored);
			}
			for (auto it = made.rbegin(); it != made.rend(); ++it) {
				std::error_code ignored;
				if (fs::is_empty(at(paths_, *it), ignored)) fs::remove(at(paths_, *it), ignored);
			}
			refuse(transaction.findings().empty()
			               ? std::vector<Diagnostic>{make_finding(CoreFinding::FileWrite, DiagnosticSeverity::Error,
			                                                      move.path + " could not be moved. Nothing was moved.", move.path)}
			               : transaction.findings());
			core_.update_files(touched);
			return false;
		}
		moved.emplace_back(move.path, move.new_path);
		for (const std::string &file : transaction.touched()) touched.push_back(file);
		sources = sources || !move.sidecar.empty();
	}
	// The folder's old tree, emptied, removed deepest first (a move takes its own folder once it empties).
	for (auto it = plan.folders.rbegin(); it != plan.folders.rend(); ++it) {
		std::error_code ignored;
		if (fs::is_directory(at(paths_, *it), ignored) && fs::is_empty(at(paths_, *it), ignored)) fs::remove(at(paths_, *it), ignored);
	}
	core_.update_files(touched);
	core_.update_folders(made, {plan.from});
	follow(moved);
	// An import source's outputs, made again under its new place (a move took the old ones).
	if (sources) core_.start_refresh();
	const std::string line = "Renamed the folder " + plan.from + " to " + plan.to + ", " + counted(moved.size(), "file") +
	                         " moved in it, no reference rewritten: the game finds a file by its name alone.";
	core_.note(line);
	view_.activity.status = "Renamed the folder " + plan.from + " to " + basename_of(plan.to) + ".";
	core_.touch(ViewConcern::Output);
	if (from_out) *from_out = plan.from;
	if (to_out) *to_out = plan.to;
	return true;
}

void FileChores::follow(const std::vector<std::pair<std::string, std::string>> &moved) {
	if (moved.empty()) return;
	if (workspace_follows_moves(view_.workspace, moved)) core_.touch(ViewConcern::Workspace);
	core_.navigation().follow_moves(moved);
	DocumentSet &documents = core_.documents();
	const std::string active = view_.documents.active;
	std::string active_now;
	std::vector<std::string> reopen;
	for (const auto &[from, to] : moved) {
		DocumentBase *open = documents.document_for(from);
		if (!open || open->path() != from) continue;
		if (from == active) active_now = to;
		documents.close_document(from);
		reopen.push_back(to);
	}
	for (const std::string &to : reopen) documents.open_document(request::open_document(to));
	if (!active_now.empty()) documents.open_document(request::open_document(active_now));
}

// --- the history -------------------------------------------------------------------------------

std::vector<std::string> FileChores::leaving(const Step &step, bool back) const {
	std::vector<std::string> out = back ? step.came : step.gone;
	// A copied source's outputs, made by the import since the copy: they leave with it.
	if (back)
		for (const std::string &path : step.came) {
			if (!strutil::ends_with_icase(path, kImportSidecarSuffix)) continue;
			const std::string source = path.substr(0, path.size() - std::string(kImportSidecarSuffix).size());
			const std::string outputs = import_output_dir(paths_, source);
			if (exists_at(paths_, outputs) && std::find(out.begin(), out.end(), outputs) == out.end()) out.push_back(outputs);
		}
	return out;
}

bool FileChores::take(Step &step, bool back) {
	// A folder's rename: renamed back, or again.
	if (!step.from.empty()) return move_folder(back ? step.to : step.from, basename_of(back ? step.from : step.to));
	const std::vector<std::string> going = leaving(step, back);
	TrashBatch &coming = back ? step.gone_batch : step.came_batch;
	const std::vector<std::string> &made = back ? step.folders_gone : step.folders_made;
	const std::vector<std::string> &removed = back ? step.folders_made : step.folders_gone;
	close_documents(project_files_of(going));
	std::string error;
	TrashBatch batch;
	if (!going.empty() && !trash_paths(paths_, going, batch, error)) {
		refuse(CoreFinding::FileHistory, error + " " + step.words + " was not " + (back ? "taken back." : "done again."));
		return false;
	}
	if (coming.id && !restore_trash(paths_, coming, error)) {
		std::string ignored;
		if (batch.id) restore_trash(paths_, batch, ignored);
		refuse(CoreFinding::FileHistory, error + " " + step.words + " was not " + (back ? "taken back." : "done again."));
		return false;
	}
	const std::vector<std::string> came = coming.paths;
	(back ? step.came_batch : step.gone_batch) = batch;
	if (back) step.came = going;
	coming = TrashBatch();
	for (const std::string &folder : made) {
		std::string ignored;
		io::ensure_directory(join_path(paths_.root, folder), ignored);
	}
	// A folder it removes goes once empty: one that holds files now (made in it since) stays, and says so.
	for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
		std::error_code ignored;
		if (!fs::is_directory(at(paths_, *it), ignored)) continue;
		if (fs::is_empty(at(paths_, *it), ignored)) fs::remove(at(paths_, *it), ignored);
		else core_.note(*it + "/ holds files now: it stays.");
	}
	// The import sources: the side gone to the trash leaves the project's import list, the side back in it
	// returns to it.
	std::vector<ImportedSource> taken;
	drop_sources(project_files_of(going), taken);
	add_sources(step.sources);
	step.sources = std::move(taken);
	std::vector<std::string> read = project_files_of(going);
	for (const std::string &file : project_files_of(came)) read.push_back(file);
	if (!read.empty()) core_.update_files(read);
	if (!made.empty() || !removed.empty()) core_.update_folders(made, removed);
	return true;
}

void FileChores::undo() {
	if (!view_.project.open) return;
	if (done_.empty()) return refuse(CoreFinding::FileHistory, "No file chore of this project is left to take back.");
	Step &step = done_.back();
	if (!take(step, true)) return;
	const std::string words = step.words;
	undone_.push_back(std::move(step));
	done_.pop_back();
	show();
	core_.note("Took back: " + words + ".");
	view_.activity.status = "Took back: " + words + ".";
	core_.touch(ViewConcern::Output);
}

void FileChores::redo() {
	if (!view_.project.open) return;
	if (undone_.empty()) return refuse(CoreFinding::FileHistory, "No file chore taken back is left to do again.");
	Step &step = undone_.back();
	if (!take(step, false)) return;
	const std::string words = step.words;
	done_.push_back(std::move(step));
	undone_.pop_back();
	show();
	core_.note("Did again: " + words + ".");
	view_.activity.status = "Did again: " + words + ".";
	core_.touch(ViewConcern::Output);
}

// --- the unsaved guard -------------------------------------------------------------------------

void FileChores::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project.open || !documents.documents_dirty()) return;
	std::vector<std::string> touched;
	switch (request.kind) {
	case EditorRequestKind::DeleteAsset: {
		const DeletePlan plan = plan_delete(paths_, *view_.project.scan, request.path, request.alone);
		if (plan.ok()) touched = plan.files;
		break;
	}
	case EditorRequestKind::RenameFolder: {
		const FolderPlan plan = plan_folder_rename(paths_, *view_.project.document, *view_.project.scan, request.folder,
		                                           request.new_name, view_.project.imports.get());
		if (plan.ok()) touched.push_back(plan.from);
		break;
	}
	case EditorRequestKind::UndoFile:
	case EditorRequestKind::RedoFile: {
		const bool back = request.kind == EditorRequestKind::UndoFile;
		const std::vector<Step> &steps = back ? done_ : undone_;
		if (steps.empty()) break;
		const Step &step = steps.back();
		if (!step.from.empty()) touched.push_back(back ? step.to : step.from);
		else touched = project_files_of(leaving(step, back));
		break;
	}
	default: break;
	}
	for (const auto &document : documents.documents()) {
		if (!document->dirty()) continue;
		for (const std::string &path : touched)
			if (within(document->path(), path)) {
				files.push_back(document->path());
				break;
			}
	}
}

} // namespace opennova::editor
