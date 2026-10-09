#include <editor/session/file_chores.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/assets/project_layout.h>
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
#include <formats/pff/pff.h>

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

bool listed(const std::vector<std::string> &paths, const std::string &path) {
	return std::any_of(paths.begin(), paths.end(), [&](const std::string &each) { return strutil::iequals(each, path); });
}

// The files a chore over several files acts on: `path`, then each of `paths`, each once.
std::vector<std::string> chore_files(const EditorRequest &request) {
	std::vector<std::string> out;
	if (!request.path.empty()) out.push_back(request.path);
	for (const std::string &path : request.paths)
		if (!path.empty() && !listed(out, path)) out.push_back(path);
	return out;
}

// Names in words: "a", "a and b", "a, b and c".
std::string names_in_words(const std::vector<std::string> &names) {
	std::string out;
	for (size_t i = 0; i < names.size(); ++i)
		out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
	return out;
}

// The delete plans `plans` with those whose file another plan takes with it (a mission's companion, an import
// source's record) left out, so each file goes once.
std::vector<DeletePlan> taken_once(std::vector<DeletePlan> plans) {
	std::vector<std::string> riding;
	for (const DeletePlan &plan : plans)
		for (size_t i = 1; i < plan.files.size(); ++i) riding.push_back(plan.files[i]);
	std::vector<DeletePlan> out;
	for (DeletePlan &plan : plans)
		if (!listed(riding, plan.path) && std::none_of(out.begin(), out.end(), [&](const DeletePlan &kept) {
			    return strutil::iequals(kept.path, plan.path);
		    }))
			out.push_back(std::move(plan));
	return out;
}

} // namespace

size_t deleted_uses(const SessionView &view, const std::vector<DeletePlan> &plans, std::vector<std::string> &places,
                    size_t most) {
	// A use from a file that goes with them is none.
	std::vector<std::string> gone, files;
	for (const DeletePlan &plan : plans) {
		const std::vector<std::string> named = plan.named();
		gone.insert(gone.end(), named.begin(), named.end());
		files.insert(files.end(), plan.files.begin(), plan.files.end());
	}
	const auto going = [&](const std::string &file) { return listed(gone, file) || listed(files, file); };
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

size_t deleted_uses(const SessionView &view, const DeletePlan &plan, std::vector<std::string> &places, size_t most) {
	return deleted_uses(view, std::vector<DeletePlan>{plan}, places, most);
}

std::vector<DeletePlan> plan_deletes(const ProjectPaths &paths, const AssetScan &scan, const EditorRequest &request,
                                     std::vector<Diagnostic> &refusals) {
	std::vector<DeletePlan> plans;
	for (const std::string &file : chore_files(request)) {
		DeletePlan plan = plan_delete(paths, scan, file, request.alone);
		refusals.insert(refusals.end(), plan.refusals.begin(), plan.refusals.end());
		if (plan.ok()) plans.push_back(std::move(plan));
	}
	return taken_once(std::move(plans));
}

std::vector<DeletePlan> plan_folder_deletes(const ProjectPaths &paths, const AssetScan &scan, const std::string &folder,
                                            std::vector<Diagnostic> &refusals) {
	std::vector<DeletePlan> plans;
	for (const AssetEntry &entry : scan.entries) {
		if (!entry.imported_from.empty() || !within(entry.relative_path, folder)) continue;
		DeletePlan plan = plan_delete(paths, scan, entry.relative_path, false);
		refusals.insert(refusals.end(), plan.refusals.begin(), plan.refusals.end());
		if (plan.ok()) plans.push_back(std::move(plan));
	}
	return taken_once(std::move(plans));
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

std::vector<std::string> FileChores::files_at(const std::vector<std::string> &paths) const {
	std::vector<std::string> out;
	const auto add = [&out](const std::string &file) {
		if (!listed(out, file)) out.push_back(file);
	};
	for (const std::string &path : project_files_of(paths)) {
		std::error_code ec;
		bool inside = false;
		for (const AssetEntry &entry : view_.project.scan->entries)
			if (entry.imported_from.empty() && within(entry.relative_path, path) && !strutil::iequals(entry.relative_path, path)) {
				add(entry.relative_path);
				inside = true;
			}
		if (fs::is_directory(at(paths_, path), ec)) {
			const fs::path top = at(paths_, path);
			for (fs::recursive_directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec))
				if (it->is_regular_file(ec)) add(join_path(path, utf8_of(it->path().lexically_relative(top))));
		} else if (!inside) {
			add(path);
		}
	}
	return out;
}

// --- a delete ----------------------------------------------------------------------------------

void FileChores::delete_asset(const EditorRequest &request) {
	if (!view_.project.open) return;
	std::vector<Diagnostic> refusals;
	const std::vector<DeletePlan> plans = plan_deletes(paths_, *view_.project.scan, request, refusals);
	if (!refusals.empty()) return refuse(refusals);
	if (plans.empty()) return refuse(CoreFinding::FileUnknown, "No file was named to delete.");
	const DeletePlan &first = plans.front();
	// Who names what goes: refused unless asked to leave those uses naming nothing.
	std::vector<std::string> places;
	const size_t uses = deleted_uses(view_, plans, places, 3);
	std::vector<std::string> names;
	for (const DeletePlan &plan : plans) names.push_back(plan.name);
	const std::string what = names_in_words(names);
	if (uses && !request.force) {
		std::string said;
		for (const std::string &place : places) said += (said.empty() ? "" : "; ") + place;
		if (uses > places.size()) said += "; and " + std::to_string(uses - places.size()) + " more";
		return refuse(CoreFinding::FileNamed,
		              what + (plans.size() == 1 ? " is" : " are") + " named by " + counted(uses, "use") + " (" + said +
		                      "). Deleted, they name nothing and are Problems until changed: delete " +
		                      (plans.size() == 1 ? "it" : "them") + " anyway (force) or cancel.",
		              first.path);
	}
	// Their open documents closed (the unsaved prompt saved their edits first); an output kept copied out of the
	// cache before the cache's copy goes.
	std::vector<std::string> files, outputs;
	for (const DeletePlan &plan : plans) {
		files.insert(files.end(), plan.files.begin(), plan.files.end());
		if (!plan.output_dir.empty()) outputs.push_back(plan.output_dir);
	}
	close_documents(files);
	std::vector<std::string> kept;
	std::string error;
	const auto unkeep = [&] {
		std::error_code ignored;
		for (const std::string &file : kept) fs::remove(at(paths_, file), ignored);
	};
	std::vector<std::string> folders_made;
	for (const DeletePlan &plan : plans)
		for (const auto &[from, to] : plan.kept) {
			std::error_code ec;
			for (std::string dir = folder_of_path(to); !dir.empty() && !exists_at(paths_, dir); dir = folder_of_path(dir))
				if (std::find(folders_made.begin(), folders_made.end(), dir) == folders_made.end())
					folders_made.insert(folders_made.begin(), dir);
			if (!io::ensure_directory(utf8_of(at(paths_, to).parent_path()), error) ||
			    !fs::copy_file(at(paths_, from), at(paths_, to), fs::copy_options::none, ec)) {
				unkeep();
				return refuse(CoreFinding::FileWrite,
				              basename_of(to) + " could not be kept: " + (error.empty() ? ec.message() : error) +
				                      ". Nothing was deleted.",
				              plan.path);
			}
			io::refresh_last_write(join_path(paths_.root, to), error);
			kept.push_back(to);
		}
	std::vector<std::string> trashed = files;
	trashed.insert(trashed.end(), outputs.begin(), outputs.end());
	TrashBatch batch;
	if (!trash_paths(paths_, trashed, batch, error)) {
		unkeep();
		return refuse(CoreFinding::FileTrash, error + " Nothing was deleted.", first.path);
	}
	Step step;
	step.words = "Delete " + (plans.size() == 1 ? first.name + (first.alone ? " alone" : "")
	                                            : first.name + " and " + counted(plans.size() - 1, "other file"));
	step.gone = trashed;
	step.gone_batch = batch;
	step.came = kept;
	step.folders_made = folders_made;
	drop_sources(files, step.sources);
	std::vector<std::string> read = files;
	read.insert(read.end(), kept.begin(), kept.end());
	core_.update_files(read);
	if (!folders_made.empty()) core_.update_folders(folders_made, {});
	// What went, and what it says of the rest.
	std::string with;
	size_t output_count = 0;
	for (const DeletePlan &plan : plans) {
		for (size_t i = 1; i < plan.files.size(); ++i)
			if (!strutil::ends_with_icase(plan.files[i], kImportSidecarSuffix)) with += (with.empty() ? "" : ", ") + basename_of(plan.files[i]);
		if (!plan.alone) output_count += plan.outputs.size();
	}
	std::string line = "Deleted " + what + (with.empty() ? std::string() : " with " + with);
	if (output_count)
		line += (with.empty() ? " with " : " and ") + std::string(plans.size() == 1 ? "its" : "their") + " import's " +
		        counted(output_count, "output");
	if (!kept.empty()) {
		std::string names_kept;
		for (const std::string &file : kept) names_kept += (names_kept.empty() ? "" : ", ") + file;
		line += " alone: " + std::string(plans.size() == 1 ? "its" : "their") + " outputs are files of the project now (" +
		        names_kept + ")";
	}
	line += std::string(". ") + (plans.size() == 1 ? "It is" : "They are") +
	        " in the project's trash: Edit > Undo file brings " + (plans.size() == 1 ? "it" : "them") + " back.";
	if (uses) line += " " + counted(uses, "use") + " of " + (plans.size() == 1 ? "it" : "them") + " name nothing now (Problems).";
	core_.note(line);
	view_.activity.status = plans.size() == 1 ? "Deleted " + first.name + "." : "Deleted " + counted(plans.size(), "file") + ".";
	core_.touch(ViewConcern::Output);
	push(std::move(step));
}

// --- a duplicate -------------------------------------------------------------------------------

void FileChores::duplicate_asset(const EditorRequest &request) {
	if (!view_.project.open) return;
	const AssetScan &scan = *view_.project.scan;
	const BaseNames base{&view_.project.base_files};
	const std::vector<std::string> asked = chore_files(request);
	// The files another's copy takes with it (a mission's companions, a source's record) copy with it alone.
	std::vector<std::string> riding;
	if (asked.size() > 1)
		for (const std::string &file : asked) {
			const DuplicatePlan alone = plan_duplicate(paths_, scan, file, std::string(), request.alone, &base);
			for (size_t i = 1; i < alone.copies.size(); ++i) riding.push_back(alone.copies[i].first);
		}
	// Each copy named beside the copies before it: their names are taken too.
	std::vector<std::string> taken = view_.project.base_files;
	std::vector<DuplicatePlan> plans;
	std::vector<Diagnostic> refusals;
	for (const std::string &file : asked) {
		const AssetEntry *entry = scan.named(file);
		if (entry && listed(riding, entry->relative_path)) continue;
		std::sort(taken.begin(), taken.end(), [](const std::string &a, const std::string &b) {
			return pff::normalized_logical_name(a) < pff::normalized_logical_name(b);
		});
		const BaseNames beside{&taken};
		DuplicatePlan plan = plan_duplicate(paths_, scan, file, plans.empty() ? request.new_name : std::string(),
		                                    request.alone, plans.empty() ? &base : &beside);
		refusals.insert(refusals.end(), plan.refusals.begin(), plan.refusals.end());
		if (!plan.ok()) continue;
		for (const auto &[from, to] : plan.copies)
			if (!strutil::ends_with_icase(to, kImportSidecarSuffix)) taken.push_back(basename_of(to));
		for (const std::string &output : plan.outputs) taken.push_back(output);
		plans.push_back(std::move(plan));
	}
	if (!refusals.empty()) return refuse(refusals);
	if (plans.empty()) return refuse(CoreFinding::FileUnknown, "No file was named to duplicate.");
	std::vector<std::string> made;
	std::string error;
	for (const DuplicatePlan &plan : plans)
		for (const auto &[from, to] : plan.copies) {
			std::error_code ec;
			if (!io::ensure_directory(utf8_of(at(paths_, to).parent_path()), error) ||
			    !fs::copy_file(at(paths_, from), at(paths_, to), fs::copy_options::none, ec)) {
				std::error_code ignored;
				for (const std::string &file : made) fs::remove(at(paths_, file), ignored);
				return refuse(CoreFinding::FileWrite,
				              to + " could not be made: " + (error.empty() ? ec.message() : error) + ". Nothing was copied.",
				              plan.path);
			}
			// A copy keeps its source's last write: as written now, every cache keyed by it reads it afresh.
			io::refresh_last_write(join_path(paths_.root, to), error);
			made.push_back(to);
		}
	const DuplicatePlan &first = plans.front();
	Step step;
	step.words = plans.size() == 1 ? "Duplicate " + first.name + " as " + first.new_name
	                               : "Duplicate " + first.name + " and " + counted(plans.size() - 1, "other file");
	step.came = made;
	push(std::move(step));
	core_.update_files(made);
	core_.documents().select_file(first.new_path);
	bool imports = false;
	std::string lines;
	for (const DuplicatePlan &plan : plans) {
		std::string with;
		for (size_t i = 1; i < plan.copies.size(); ++i)
			if (!strutil::ends_with_icase(plan.copies[i].second, kImportSidecarSuffix))
				with += (with.empty() ? "" : ", ") + basename_of(plan.copies[i].second);
		std::string line = plan.name + " as " + plan.new_path + (with.empty() ? std::string() : " with " + with);
		if (plan.import) {
			std::string outputs;
			for (const std::string &output : plan.outputs) outputs += (outputs.empty() ? "" : ", ") + output;
			line += ", its import record with it: the import makes the copy's own outputs" +
			        (outputs.empty() ? std::string() : " (" + outputs + ")");
			imports = true;
		} else if (request.alone && plan.copies.size() == 1) {
			line += ", without its import record";
		}
		lines += (lines.empty() ? "" : "; ") + line;
	}
	// The import pass makes the copies' outputs now (a refresh, which imports their new records); one that cannot
	// start says so.
	if (imports && !core_.start_refresh()) lines += ", at the next refresh";
	core_.note("Duplicated " + lines + ".");
	view_.activity.status = plans.size() == 1 ? "Duplicated " + first.name + " as " + first.new_name + "."
	                                          : "Duplicated " + counted(plans.size(), "file") + ".";
	core_.touch(ViewConcern::Output);
}

// --- files moved together ----------------------------------------------------------------------

void FileChores::move_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	std::vector<std::pair<std::string, std::string>> asked;
	for (const std::string &file : chore_files(request)) asked.emplace_back(file, request.folder);
	std::vector<std::pair<std::string, std::string>> moved;
	if (!move_now(asked, &moved)) {
		view_.activity.status = "The move was refused.";
		return;
	}
	std::string folder, why;
	normalize_project_folder(request.folder, folder, why);
	Step step;
	step.words = "Move " + basename_of(moved.front().first) +
	             (moved.size() == 1 ? std::string() : " and " + counted(moved.size() - 1, "other file")) + " to " +
	             folder_words(folder);
	step.moves = moved;
	push(std::move(step));
	std::vector<std::string> names;
	for (const auto &[from, to] : moved) names.push_back(basename_of(from));
	core_.note("Moved " + names_in_words(names) + " to " + folder_words(folder) +
	           ", no reference rewritten: the game finds a file by its name alone. Edit > Undo file moves them back.");
	view_.activity.status = "Moved " + counted(moved.size(), "file") + " to " + folder_words(folder) + ".";
	core_.touch(ViewConcern::Output);
}

bool FileChores::move_now(const std::vector<std::pair<std::string, std::string>> &asked,
                          std::vector<std::pair<std::string, std::string>> *moved_out) {
	const std::shared_ptr<const AssetScan> scan = view_.project.scan;
	const ProjectDocument &project = *view_.project.document;
	std::vector<RenamePlan> plans;
	for (const auto &[file, folder] : asked)
		plans.push_back(plan_move(paths_, project, *scan, file, folder, view_.project.imports.get()));
	// A mission's companions go with its move: none is a move of its own. Of several, one in the folder already
	// stays where it is.
	std::vector<std::string> riding;
	for (const RenamePlan &plan : plans)
		for (const RenameOutput &companion : plan.companions) riding.push_back(companion.path);
	const auto stays = [&](const RenamePlan &plan) {
		return listed(riding, plan.path) ||
		       (asked.size() > 1 && plan.refusals.size() == 1 && plan.refusals.front().code() == "rename.unchanged");
	};
	std::vector<RenamePlan> unchanged;
	for (const RenamePlan &plan : plans)
		if (stays(plan) && !listed(riding, plan.path)) unchanged.push_back(plan);
	plans.erase(std::remove_if(plans.begin(), plans.end(), stays), plans.end());
	std::vector<Diagnostic> refusals;
	for (const RenamePlan &plan : plans) refusals.insert(refusals.end(), plan.refusals.begin(), plan.refusals.end());
	if (plans.empty() && !unchanged.empty()) refusals = unchanged.front().refusals;
	if (!refusals.empty()) {
		refuse(refusals);
		return false;
	}
	// The folders the moves make, and those they leave (each goes once empty).
	std::vector<std::string> made, left;
	for (const RenamePlan &plan : plans) {
		for (std::string dir = folder_of_path(plan.new_path); !dir.empty() && !exists_at(paths_, dir); dir = folder_of_path(dir))
			if (!listed(made, dir)) made.push_back(dir);
		for (std::string dir = folder_of_path(plan.path); !dir.empty(); dir = folder_of_path(dir))
			if (!listed(left, dir)) left.push_back(dir);
	}
	std::vector<std::pair<std::string, std::string>> moved;
	std::vector<std::string> touched;
	bool sources = false;
	if (!commit_moves(plans, moved, touched, sources)) {
		core_.update_files(touched);
		return false;
	}
	std::vector<std::string> gone;
	for (const std::string &dir : left)
		if (!exists_at(paths_, dir)) gone.push_back(dir);
	core_.update_files(touched);
	if (!made.empty() || !gone.empty()) core_.update_folders(made, gone);
	follow(moved);
	// An import source's outputs, made again under its new place (a move took the old ones).
	if (sources) core_.start_refresh();
	if (moved_out) {
		moved_out->clear();
		for (const RenamePlan &plan : plans) moved_out->emplace_back(plan.path, plan.new_path);
	}
	return true;
}

bool FileChores::commit_moves(const std::vector<RenamePlan> &moves, std::vector<std::pair<std::string, std::string>> &moved,
                              std::vector<std::string> &touched, bool &sources) {
	const std::shared_ptr<const AssetScan> scan = view_.project.scan;
	const ProjectDocument &project = *view_.project.document;
	const AssetGraph &graph = core_.problems().graph();
	const auto put_back = [&](const std::string &now, const std::string &was) {
		std::string ignored;
		std::error_code ec;
		io::ensure_directory(utf8_of(at(paths_, was).parent_path()), ignored);
		io::rename_with_retry(at(paths_, now), at(paths_, was), ec);
	};
	for (const RenamePlan &move : moves) {
		RenameTransaction transaction(paths_, project, *scan, graph, move);
		while (!transaction.step()) {
		}
		if (transaction.ok()) {
			moved.emplace_back(move.path, move.new_path);
			for (const RenameOutput &companion : move.companions) moved.emplace_back(companion.path, companion_path(companion));
			for (const std::string &file : transaction.touched()) touched.push_back(file);
			sources = sources || !move.sidecar.empty();
			continue;
		}
		// The moves before it put back.
		for (auto it = moves.begin(); it != moves.end() && &*it != &move; ++it) {
			put_back(it->new_path, it->path);
			if (!it->sidecar.empty()) put_back(it->new_sidecar, it->sidecar);
			for (const RenameOutput &companion : it->companions) put_back(companion_path(companion), companion.path);
		}
		refuse(transaction.findings().empty()
		               ? std::vector<Diagnostic>{make_finding(CoreFinding::FileWrite, DiagnosticSeverity::Error,
		                                                      move.path + " could not be moved. Nothing was moved.", move.path)}
		               : transaction.findings());
		return false;
	}
	return true;
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

void FileChores::delete_folder(const EditorRequest &request) {
	if (!view_.project.open) return;
	const std::string &text = request.folder;
	std::string folder, why;
	if (!project_folder_of(paths_, *view_.project.document, text, folder, why)) return refuse(CoreFinding::FileFolder, why, text);
	if (folder.empty()) return refuse(CoreFinding::FileFolder, "The top level of the project is no folder to delete.", text);
	std::error_code ec;
	if (!fs::is_directory(at(paths_, folder), ec)) return refuse(CoreFinding::FileUnknown, "The project has no folder " + folder + ".", folder);
	if (fs::is_empty(at(paths_, folder), ec) && !ec) {
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
		return;
	}
	const std::vector<std::string> inside = files_at({folder});
	if (!request.all)
		return refuse(CoreFinding::FileFolder,
		              folder + " is not empty" + (inside.empty() ? std::string() : " (" + counted(inside.size(), "file") + ")") +
		                      ": delete it with what it holds (all), or move or delete that first.",
		              folder);
	// Each file of the project in it, as a delete takes it: who names what goes asked first.
	std::vector<Diagnostic> refusals;
	const std::vector<DeletePlan> plans = plan_folder_deletes(paths_, *view_.project.scan, folder, refusals);
	if (!refusals.empty()) return refuse(refusals);
	std::vector<std::string> places;
	const size_t uses = deleted_uses(view_, plans, places, 3);
	if (uses && !request.force) {
		std::string said;
		for (const std::string &place : places) said += (said.empty() ? "" : "; ") + place;
		if (uses > places.size()) said += "; and " + std::to_string(uses - places.size()) + " more";
		return refuse(CoreFinding::FileNamed,
		              "What " + folder + " holds is named by " + counted(uses, "use") + " (" + said +
		                      "). Deleted, they name nothing and are Problems until changed: delete it anyway (force) or "
		                      "cancel.",
		              folder);
	}
	// The folder whole to the trash, with what goes with its files from elsewhere (a mission's companions kept in
	// their kind's folder, an import's outputs under the cache).
	std::vector<std::string> trashed{folder}, files = inside;
	for (const DeletePlan &plan : plans) {
		for (const std::string &file : plan.files)
			if (!within(file, folder)) {
				trashed.push_back(file);
				files.push_back(file);
			}
		if (!plan.output_dir.empty()) trashed.push_back(plan.output_dir);
	}
	std::vector<std::string> folders_gone{folder};
	for (const auto &[held, stamp] : view_.project.scan->folders())
		if (within(held, folder) && !strutil::iequals(held, folder)) folders_gone.push_back(held);
	close_documents(trashed);
	std::string error;
	TrashBatch batch;
	if (!trash_paths(paths_, trashed, batch, error)) return refuse(CoreFinding::FileTrash, error + " Nothing was deleted.", folder);
	Step step;
	step.words = "Delete folder " + folder + " with " + counted(inside.size(), "file");
	step.gone = trashed;
	step.gone_batch = batch;
	step.folders_gone = folders_gone;
	drop_sources(trashed, step.sources);
	core_.update_files(files);
	core_.update_folders({}, {folder});
	std::string line = "Deleted the folder " + folder + "/ with the " + counted(inside.size(), "file") + " it held";
	if (trashed.size() > 1) {
		std::string with;
		for (size_t i = 1; i < trashed.size(); ++i)
			if (project_files_of({trashed[i]}).size()) with += (with.empty() ? "" : ", ") + basename_of(trashed[i]);
		if (!with.empty()) line += " and " + with + " that went with them";
	}
	line += ". It is in the project's trash: Edit > Undo file brings it back.";
	if (uses) line += " " + counted(uses, "use") + " of what it held name nothing now (Problems).";
	core_.note(line);
	view_.activity.status = "Deleted the folder " + folder + "/.";
	core_.touch(ViewConcern::Output);
	push(std::move(step));
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
	std::vector<std::pair<std::string, std::string>> moved;
	std::vector<std::string> touched;
	bool sources = false;
	if (!commit_moves(plan.moves, moved, touched, sources)) {
		for (auto it = made.rbegin(); it != made.rend(); ++it) {
			std::error_code ignored;
			if (fs::is_empty(at(paths_, *it), ignored)) fs::remove(at(paths_, *it), ignored);
		}
		core_.update_files(touched);
		return false;
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

// --- the trash ---------------------------------------------------------------------------------

void FileChores::empty_trash() {
	if (!view_.project.open) return;
	size_t files = 0;
	std::string error;
	const bool emptied = opennova::editor::empty_trash(paths_, files, error);
	// Each step of the history takes what it put in the trash back from there: none can now.
	const size_t steps = done_.size() + undone_.size();
	done_.clear();
	undone_.clear();
	show();
	if (!emptied) return refuse(CoreFinding::FileTrash, error + (steps ? " The file history went with what it held." : ""));
	std::string line = files ? "Emptied the project's trash: " + counted(files, "file") + " removed for good."
	                         : std::string("The project's trash held nothing.");
	if (steps) line += " The file history went with it (" + counted(steps, "step") + "): nothing it held can come back.";
	core_.note(line);
	view_.activity.status = files ? "Emptied the trash (" + counted(files, "file") + ")." : std::string("The trash was empty.");
	core_.touch(ViewConcern::Output);
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
	// Files moved together: each moved back to the folder it left, or again.
	if (!step.moves.empty()) {
		std::vector<std::pair<std::string, std::string>> asked;
		for (const auto &[from, to] : step.moves) asked.emplace_back(back ? to : from, folder_of_path(back ? from : to));
		return move_now(asked);
	}
	const std::vector<std::string> going = leaving(step, back);
	TrashBatch &coming = back ? step.gone_batch : step.came_batch;
	const std::vector<std::string> &made = back ? step.folders_gone : step.folders_made;
	const std::vector<std::string> &removed = back ? step.folders_made : step.folders_gone;
	close_documents(project_files_of(going));
	// What the scan reads again: the files that leave, as the scan has them before they go.
	std::vector<std::string> read = files_at(going);
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
	for (const std::string &file : files_at(came)) read.push_back(file);
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
		std::vector<Diagnostic> refusals;
		const std::vector<DeletePlan> plans = plan_deletes(paths_, *view_.project.scan, request, refusals);
		if (refusals.empty())
			for (const DeletePlan &plan : plans) touched.insert(touched.end(), plan.files.begin(), plan.files.end());
		break;
	}
	case EditorRequestKind::DuplicateAsset:
		// Each file is copied as saved.
		for (const std::string &file : chore_files(request))
			if (const AssetEntry *entry = view_.project.scan->named(file)) touched.push_back(entry->relative_path);
		break;
	case EditorRequestKind::MoveAsset:
		for (const std::string &file : chore_files(request)) {
			const RenamePlan plan = plan_move(paths_, *view_.project.document, *view_.project.scan, file, request.folder,
			                                  view_.project.imports.get());
			if (!plan.ok()) continue;
			touched.push_back(plan.path);
			for (const RenameOutput &companion : plan.companions) touched.push_back(companion.path);
		}
		break;
	case EditorRequestKind::DeleteFolder: {
		std::string folder, why;
		if (request.all && project_folder_of(paths_, *view_.project.document, request.folder, folder, why) && !folder.empty()) {
			touched.push_back(folder);
			std::vector<Diagnostic> refusals;
			for (const DeletePlan &plan : plan_folder_deletes(paths_, *view_.project.scan, folder, refusals))
				touched.insert(touched.end(), plan.files.begin(), plan.files.end());
		}
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
		else if (!step.moves.empty())
			for (const auto &[from, to] : step.moves) touched.push_back(back ? to : from);
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
