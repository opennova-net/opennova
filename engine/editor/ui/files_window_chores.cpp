// Files' chores (DI-25, the deep-integration plan): New here, a folder's menu (a new folder, its rename, its
// delete, with what it holds once asked), the top level's Empty the trash..., a file's Duplicate and Delete...
// over every row selected with it, and Delete...'s dialog, which lists who names the files before anything goes.
// Each raises a request of the session's (session/file_chores.h), one step of the file history that Edit > Undo
// file takes back, but for the trash's emptying, which takes the history with it.
#include <editor/ui/files_window.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/project_layout.h>
#include <editor/blank/blank_factory.h>
#include <editor/graph/file_plans.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/field_text.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project/project_trash.h>
#include <editor/session/file_chores.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const ImVec4 kChoreRefusalColor(0.95f, 0.55f, 0.45f, 1.0f);
const ImVec4 kChoreWarningColor(0.95f, 0.8f, 0.4f, 1.0f);

const AssetEntry *chore_entry_at(const SessionView &view, const std::string &path) {
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.relative_path == path) return &entry;
	return nullptr;
}

// A folder in words: "the top level", else its path with a slash.
std::string folder_words(const std::string &folder) {
	return folder.empty() || folder == "/" ? std::string("the top level") : folder + "/";
}

// Whether `path` lies in `folder`.
bool in_folder(const std::string &path, const std::string &folder) {
	return path.size() > folder.size() && path[folder.size()] == '/' && strutil::iequals(path.substr(0, folder.size()), folder);
}

} // namespace

// --- the selection -----------------------------------------------------------------------------

bool FilesWindow::chosen(const std::string &path) const {
	return path == selected_ || std::find(also_.begin(), also_.end(), path) != also_.end();
}

std::vector<std::string> FilesWindow::others_of(const SessionView &view, const std::string &path) const {
	if (!chosen(path)) return {};
	// Every row selected with it, but an import's outputs (their sources make them): a chore over several rows
	// leaves them out rather than be refused whole.
	std::vector<std::string> out;
	const auto take = [&](const std::string &other) {
		if (const AssetEntry *file = chore_entry_at(view, other); file && file->imported_from.empty()) out.push_back(other);
	};
	if (selected_ != path && !selected_.empty()) take(selected_);
	for (const std::string &other : also_)
		if (other != path) take(other);
	return out;
}

void FilesWindow::choose(const std::string &path) {
	const ImGuiIO &io = ImGui::GetIO();
	if (io.KeyShift && !selected_.empty() && selected_ != path) {
		// The run of rows from the one selected to this one, as they were last drawn.
		const auto from = std::find(rows_.begin(), rows_.end(), selected_);
		const auto to = std::find(rows_.begin(), rows_.end(), path);
		if (from != rows_.end() && to != rows_.end()) {
			also_.clear();
			const auto first = std::min(from, to), last = std::max(from, to);
			for (auto it = first; it <= last; ++it)
				if (*it != path) also_.push_back(*it);
			selected_ = path;
			return;
		}
	}
	if (io.KeyCtrl && !selected_.empty()) {
		if (path == selected_) {
			// Taken out: the last one added is the selected row now.
			if (!also_.empty()) {
				selected_ = also_.back();
				also_.pop_back();
			}
			return;
		}
		const auto held = std::find(also_.begin(), also_.end(), path);
		if (held != also_.end()) {
			also_.erase(held);
			return;
		}
		also_.push_back(selected_);
		selected_ = path;
		return;
	}
	also_.clear();
	selected_ = path;
}

// --- New here and a folder's menu ---------------------------------------------------------------

void FilesWindow::draw_new_entries(const SessionView &view, const std::string &folder) {
	const bool creates = view.allows(EditorRequestKind::CreateFile);
	const std::string where = folder.empty() ? std::string() : " in " + folder_words(folder);
	// A new file of a kind with a free-form factory: its name is asked first.
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory &factory = *blank_factory_at(i);
		if (!factory.free_form || factory.role[0] != '\0') continue;
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!creates);
		if (ImGui::Selectable((std::string(asset_kind_label(factory.kind)) + "...").c_str()) && creates)
			NewFilePrompt::ask(workspace_, factory.kind, folder);
		ImGui::EndDisabled();
		ui_kit::tooltip(std::string("Makes ") + factory.summary + where + ".");
		ImGui::PopID();
	}
	// The files the game reads by name, each made at once from its role's factory.
	ImGui::SeparatorText("Files the game reads");
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory &factory = *blank_factory_at(i);
		const gameprofile::RequiredResource *resource = gameprofile::gameprofile_required_resource_by_role(factory.role);
		if (!resource) continue;
		const bool present = view.project.scan->find(resource->name) != nullptr;
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(present || !creates);
		if (ImGui::Selectable(resource->name) && !present && creates)
			workspace_.request(folder.empty() ? request::create_file(resource->name, asset_kind_token(factory.kind))
			                                  : request::create_file_in(folder, resource->name, asset_kind_token(factory.kind)));
		ImGui::EndDisabled();
		ui_kit::tooltip(present ? std::string("The project has it.") : std::string("Makes ") + factory.summary + where + ".");
		ImGui::PopID();
	}
}

void FilesWindow::draw_folder_menu(const SessionView &view, const std::string &folder) {
	// The names typed start afresh as the menu opens on a folder.
	if (ImGui::IsWindowAppearing() || folder_menu_ != folder) {
		folder_menu_ = folder;
		folder_new_[0] = '\0';
		std::snprintf(folder_rename_, sizeof(folder_rename_), "%s", basename_of(folder).c_str());
	}
	const std::string here = folder.empty() ? std::string("/") : folder;
	if (ImGui::BeginMenu("New file here")) {
		draw_new_entries(view, here);
		ImGui::EndMenu();
	}
	ui_kit::tooltip("A new file in " + folder_words(folder) + ": the game finds it by its name wherever it sits.");
	// A folder in this one, by its name.
	ImGui::SeparatorText("New folder");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	const bool make_enter = ImGui::InputTextWithHint("##folder_new", "Name", folder_new_, sizeof(folder_new_),
	                                                 ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	std::string made, why;
	const bool named = folder_new_[0] != '\0' && normalize_project_folder(join_path(folder, folder_new_), made, why);
	const bool makes = named && view.allows(EditorRequestKind::NewFolder);
	ImGui::BeginDisabled(!makes);
	const bool make = ImGui::Button("Make");
	ImGui::EndDisabled();
	if (folder_new_[0] != '\0' && !named) ImGui::TextColored(kChoreRefusalColor, "%s", why.c_str());
	if ((make || make_enter) && makes) {
		workspace_.request(request::new_folder(made));
		ImGui::CloseCurrentPopup();
	}
	if (folder.empty()) {
		// The top level's: the trash emptied, asked first.
		ImGui::Separator();
		const bool empties = view.allows(EditorRequestKind::EmptyTrash);
		if (ImGui::MenuItem("Empty the trash...", nullptr, false, empties) && empties) {
			trash_asked_ = true;
			trash_files_ = trash_file_count(ProjectPaths::for_root(view.project.root));
		}
		ui_kit::tooltip("Removes for good what the project's deletes put in .opennova/trash/, once asked. The file "
		                "history goes with it.");
		return;
	}
	// This folder: renamed (each file of it moved, so what names them follows), deleted (with what it holds,
	// asked first).
	ImGui::SeparatorText(("Folder " + basename_of(folder)).c_str());
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	const bool rename_enter = ImGui::InputText("##folder_rename", folder_rename_, sizeof(folder_rename_),
	                                           ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	const bool renames = folder_rename_[0] != '\0' && basename_of(folder) != folder_rename_ &&
	                     view.allows(EditorRequestKind::RenameFolder);
	ImGui::BeginDisabled(!renames);
	const bool rename = ImGui::Button("Rename");
	ImGui::EndDisabled();
	ui_kit::tooltip("Each file in it moves to the new name, as Move to folder moves one: nothing that names a file "
	                "changes, the game finding a file by its name alone. Edit > Undo file renames it back.");
	if ((rename || rename_enter) && renames) {
		workspace_.request(request::rename_folder(folder, folder_rename_));
		ImGui::CloseCurrentPopup();
	}
	bool empty = true;
	for (const Folder &node : folders_)
		if (node.path == folder) empty = node.files.empty() && node.folders.empty();
	const bool deletes = view.allows(EditorRequestKind::DeleteFolder);
	if (ImGui::MenuItem(empty ? "Delete folder" : "Delete folder...", nullptr, false, deletes) && deletes) {
		if (empty) workspace_.request(request::delete_folder(folder));
		else folder_delete_ = folder;
	}
	ui_kit::tooltip(empty ? std::string("It holds nothing. Edit > Undo file makes it again.")
	                      : std::string("With what it holds, to the project's trash, once who names it is shown. Edit > "
	                                    "Undo file brings it back."));
	if (ImGui::MenuItem("Show in folder", nullptr, false, view.allows(EditorRequestKind::RevealPath)))
		workspace_.request(request::reveal_path(join_path(view.project.root, folder)));
}

// --- a file's chores ---------------------------------------------------------------------------

void FilesWindow::draw_chore_entries(const SessionView &view, const AssetEntry &entry) {
	const bool output = !entry.imported_from.empty();
	const bool source = entry.kind == AssetKind::ImportSource;
	// Every row selected with it, but an import's outputs (their sources make them).
	const std::vector<std::string> others = others_of(view, entry.relative_path);
	const std::string several = others.empty() ? std::string() : " " + counted(others.size() + 1, "file");
	const bool copies = !output && view.allows(EditorRequestKind::DuplicateAsset);
	if (ImGui::MenuItem(("Duplicate" + several).c_str(), nullptr, false, copies))
		workspace_.request(request::duplicate_asset(entry.relative_path, std::string(), false, others));
	ui_kit::tooltip(output ? "Made by the import of " + entry.imported_from + ": duplicate the source."
	                : !others.empty() ? std::string("A copy of each beside it, named by the project's rules, as one step.")
	                : source ? std::string("A copy beside it with its import record: the import makes the copy's own outputs.")
	                         : std::string("A copy beside it, named by the project's rules (oncrate1.3di makes oncrate2.3di); a "
	                                       "mission's copy comes with its own companions."));
	if (source && others.empty()) {
		if (ImGui::MenuItem("Duplicate source alone", nullptr, false, copies))
			workspace_.request(request::duplicate_asset(entry.relative_path, std::string(), true));
		ui_kit::tooltip("A copy of the source alone, without its import record: no import makes outputs of it.");
	}
	const bool deletes = !output && view.allows(EditorRequestKind::DeleteAsset);
	if (ImGui::MenuItem(("Delete" + several + "...").c_str(), "Del", false, deletes)) start_delete(entry, others);
	ui_kit::tooltip(output ? "Made by the import of " + entry.imported_from + ": delete the source."
	                       : std::string("To the project's trash, once who names it is shown. Edit > Undo file brings it back."));
}

void FilesWindow::start_delete(const AssetEntry &entry, const std::vector<std::string> &others) {
	io::JsonValue part = io::JsonValue::make_object();
	part.set("path", io::JsonValue::make_string(entry.relative_path));
	io::JsonValue paths = io::JsonValue::make_array();
	for (const std::string &other : others) paths.push(io::JsonValue::make_string(other));
	part.set("paths", std::move(paths));
	window_requests::set_workspace(workspace_, "file_delete", std::move(part));
}

void FilesWindow::draw_delete(const SessionView &view) {
	const WorkspaceView::FileDelete &held = view.workspace.file_delete;
	const AssetEntry *entry = held.path.empty() ? nullptr : chore_entry_at(view, held.path);
	const auto close = [this] { window_requests::set_workspace(workspace_, "file_delete", "path", io::JsonValue::make_string("")); };
	if (!delete_popup_.begin("Delete", entry != nullptr && modal_may_show(view, HeldModal::FileDelete), false,
	                         ImGuiWindowFlags_AlwaysAutoResize, false, view.workspace.opened)) {
		if (delete_popup_.dismissed()) close();
		return;
	}
	// What it lists, made again as the files, their choice, the files or the graph move.
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Graph});
	std::string asked = held.path + (held.alone ? "\nalone" : "");
	for (const std::string &other : held.paths) asked += "\n" + other;
	bool sources = entry->kind == AssetKind::ImportSource;
	if (asked != delete_for_ || key != delete_key_) {
		delete_for_ = asked;
		delete_key_ = key;
		EditorRequest request = request::delete_asset(held.path, false, held.alone, held.paths);
		std::vector<Diagnostic> refusals;
		const std::vector<DeletePlan> plans =
		        plan_deletes(ProjectPaths::for_root(view.project.root), *view.project.scan, request, refusals);
		delete_refusals_.clear();
		for (const Diagnostic &refusal : refusals) delete_refusals_.push_back(refusal.message);
		delete_with_.clear();
		delete_kept_.clear();
		size_t outputs = 0;
		for (const DeletePlan &plan : plans) {
			for (size_t i = 1; i < plan.files.size(); ++i) delete_with_.push_back(plan.files[i]);
			if (!plan.alone) outputs += plan.outputs.size();
			for (const auto &[from, to] : plan.kept) delete_kept_.push_back(to);
		}
		if (outputs) delete_with_.push_back(counted(outputs, "output") + " of " + (plans.size() == 1 ? "its" : "their") + " import");
		delete_uses_.clear();
		delete_use_count_ = refusals.empty() ? deleted_uses(view, plans, delete_uses_, 12) : 0;
	}
	for (const std::string &other : held.paths)
		if (const AssetEntry *file = chore_entry_at(view, other)) sources = sources || file->kind == AssetKind::ImportSource;
	if (held.paths.empty()) ImGui::Text("Delete %s?", entry->logical_name.c_str());
	else ImGui::Text("Delete %s?", counted(held.paths.size() + 1, "file").c_str());
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
	if (!held.paths.empty()) {
		std::string names = entry->logical_name;
		for (const std::string &other : held.paths) names += ", " + basename_of(other);
		ImGui::TextWrapped("%s.", names.c_str());
	}
	for (const std::string &refusal : delete_refusals_) ImGui::TextColored(kChoreRefusalColor, "%s", refusal.c_str());
	// An import source: with its outputs, or alone, its outputs kept as files of the project.
	if (sources) {
		bool alone = held.alone;
		if (ImGui::RadioButton("With the outputs", !alone)) alone = false;
		ImGui::SameLine();
		if (ImGui::RadioButton("Alone: keep the outputs", alone)) alone = true;
		ui_kit::tooltip("An import source's outputs stay as files of the project, where a file of their kind goes, and "
		                "nothing naming them is left without them.");
		if (alone != held.alone) window_requests::set_workspace(workspace_, "file_delete", "alone", io::JsonValue::make_bool(alone));
	}
	if (!delete_with_.empty()) {
		std::string with;
		for (const std::string &file : delete_with_) with += (with.empty() ? "" : ", ") + file;
		ImGui::TextWrapped("With %s: %s.", held.paths.empty() ? "it" : "them", with.c_str());
	}
	if (!delete_kept_.empty()) {
		std::string kept;
		for (const std::string &file : delete_kept_) kept += (kept.empty() ? "" : ", ") + file;
		ImGui::TextWrapped("Kept as files of the project: %s.", kept.c_str());
	}
	// Who names it, before anything goes (DI-05's uses).
	if (delete_use_count_) {
		ImGui::TextColored(kChoreWarningColor, "Named by %s:", counted(delete_use_count_, "use").c_str());
		for (const std::string &place : delete_uses_) ImGui::BulletText("%s", place.c_str());
		if (delete_use_count_ > delete_uses_.size()) ImGui::TextDisabled("and %zu more", delete_use_count_ - delete_uses_.size());
		ImGui::TextWrapped("Deleted, they name nothing: Problems rows until they are changed, or Undo file brings it back.");
	} else if (delete_refusals_.empty()) {
		ImGui::TextDisabled("Nothing names %s.", held.paths.empty() ? "it" : "them");
	}
	ImGui::TextDisabled("It goes to the project's trash (.opennova/trash/): Edit > Undo file brings it back.");
	ImGui::PopTextWrapPos();
	const bool allowed = delete_refusals_.empty() && view.allows(EditorRequestKind::DeleteAsset);
	ImGui::BeginDisabled(!allowed);
	const bool remove = ImGui::Button(delete_use_count_ ? "Delete anyway" : "Delete");
	ImGui::EndDisabled();
	if (remove && allowed) {
		// The session closes Delete... as it takes the delete (delete_asset alone, over the wire); its button closes
		// it too.
		workspace_.request(request::delete_asset(entry->relative_path, delete_use_count_ > 0, held.alone, held.paths));
		close();
		delete_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		close();
		delete_popup_.close();
	}
	ImGui::EndPopup();
}

// --- a folder with what it holds, and the trash ------------------------------------------------

void FilesWindow::draw_folder_delete(const SessionView &view) {
	const bool there = !folder_delete_.empty() && view.project.open &&
	                   std::any_of(folders_.begin(), folders_.end(), [&](const Folder &node) { return node.path == folder_delete_; });
	if (!folder_delete_popup_.begin("Delete folder", there, true, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (folder_delete_popup_.dismissed() || !there) folder_delete_.clear();
		return;
	}
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Graph});
	if (key != folder_delete_key_) {
		folder_delete_key_ = key;
		std::vector<Diagnostic> refusals;
		const std::vector<DeletePlan> plans =
		        plan_folder_deletes(ProjectPaths::for_root(view.project.root), *view.project.scan, folder_delete_, refusals);
		folder_delete_refusals_.clear();
		for (const Diagnostic &refusal : refusals) folder_delete_refusals_.push_back(refusal.message);
		folder_delete_files_ = 0;
		for (const AssetEntry &file : view.project.scan->entries)
			if (file.imported_from.empty() && in_folder(file.relative_path, folder_delete_)) ++folder_delete_files_;
		folder_delete_uses_.clear();
		folder_delete_use_count_ = refusals.empty() ? deleted_uses(view, plans, folder_delete_uses_, 12) : 0;
	}
	ImGui::Text("Delete %s with what it holds (%s)?", folder_words(folder_delete_).c_str(),
	            counted(folder_delete_files_, "file").c_str());
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
	for (const std::string &refusal : folder_delete_refusals_) ImGui::TextColored(kChoreRefusalColor, "%s", refusal.c_str());
	if (folder_delete_use_count_) {
		ImGui::TextColored(kChoreWarningColor, "What it holds is named by %s:", counted(folder_delete_use_count_, "use").c_str());
		for (const std::string &place : folder_delete_uses_) ImGui::BulletText("%s", place.c_str());
		if (folder_delete_use_count_ > folder_delete_uses_.size())
			ImGui::TextDisabled("and %zu more", folder_delete_use_count_ - folder_delete_uses_.size());
		ImGui::TextWrapped("Deleted, they name nothing: Problems rows until they are changed, or Undo file brings it back.");
	} else if (folder_delete_refusals_.empty()) {
		ImGui::TextDisabled("Nothing outside it names what it holds.");
	}
	ImGui::TextDisabled("It goes to the project's trash (.opennova/trash/) whole: Edit > Undo file brings it back.");
	ImGui::PopTextWrapPos();
	const bool allowed = folder_delete_refusals_.empty() && view.allows(EditorRequestKind::DeleteFolder);
	ImGui::BeginDisabled(!allowed);
	const bool remove = ImGui::Button(folder_delete_use_count_ ? "Delete anyway" : "Delete");
	ImGui::EndDisabled();
	if (remove && allowed) {
		workspace_.request(request::delete_folder(folder_delete_, true, folder_delete_use_count_ > 0));
		folder_delete_.clear();
		folder_delete_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		folder_delete_.clear();
		folder_delete_popup_.close();
	}
	ImGui::EndPopup();
}

void FilesWindow::draw_empty_trash(const SessionView &view) {
	if (!trash_popup_.begin("Empty the trash", trash_asked_ && view.project.open, true, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (trash_popup_.dismissed() || !view.project.open) trash_asked_ = false;
		return;
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
	if (trash_files_) ImGui::TextWrapped("Remove the %s the project's trash holds for good?", counted(trash_files_, "file").c_str());
	else ImGui::TextWrapped("The project's trash holds nothing.");
	const size_t steps = view.activity.file_history.undo_steps + view.activity.file_history.redo_steps;
	if (steps)
		ImGui::TextColored(kChoreWarningColor, "The file history goes with it (%s): Undo file brings nothing back after.",
		                   counted(steps, "step").c_str());
	ImGui::PopTextWrapPos();
	const bool allowed = (trash_files_ || steps) && view.allows(EditorRequestKind::EmptyTrash);
	ImGui::BeginDisabled(!allowed);
	const bool empty = ImGui::Button("Empty the trash");
	ImGui::EndDisabled();
	if (empty && allowed) {
		workspace_.request(request::empty_trash(true));
		trash_asked_ = false;
		trash_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		trash_asked_ = false;
		trash_popup_.close();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
