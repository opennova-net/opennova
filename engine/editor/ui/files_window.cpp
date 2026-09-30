#include <editor/ui/files_window.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include <base/gameprofile/required_resources.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_run.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace fs = std::filesystem;

namespace {

// An open document's name in Files.
const ImVec4 kOpenColor(0.55f, 0.78f, 1.0f, 1.0f);
const ImVec4 kRefusalColor(0.95f, 0.55f, 0.45f, 1.0f);

std::string size_text(uint64_t bytes) {
	char text[32];
	if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
	else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
	else std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return text;
}

// What a blank factory makes, as its menu entry's tooltip.
std::string makes(const BlankFactory &factory) { return std::string("Makes ") + factory.summary + "."; }

// The name a new file of a free-form kind is offered.
const char *name_hint(AssetKind kind) {
	switch (kind) {
	case AssetKind::Strings: return "newtable.bin";
	case AssetKind::Menu: return "newmenu.mnu";
	case AssetKind::Font: return "newfont.fnt";
	case AssetKind::Texture: return "newtexture.tga";
	default: return "";
	}
}

const AssetEntry *entry_at(const SessionView &view, const std::string &path) {
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.relative_path == path) return &entry;
	return nullptr;
}

const Document *open_document(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document->path() == path) return document.get();
	return nullptr;
}

// What the window's caches read of the view: the tree and each file's counts, the files and the
// findings; a file's References..., the graph (its edges name their files by path and their
// fields by the kind the scan gives: the graph moves when either does).
struct CacheKey {
	RevisionKey tree;
	RevisionKey references;
};
CacheKey cache_key(const SessionView &view) {
	return {revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Findings}),
	        revision_key(view.revisions, {ViewConcern::Graph})};
}

} // namespace

void NewFilePrompt::ask(AssetKind kind) {
	ask_ = true;
	kind_ = kind;
	name_[0] = '\0';
}

void NewFilePrompt::draw(Workspace &workspace) {
	if (ask_) {
		ask_ = false;
		ImGui::OpenPopup("New file");
	}
	if (!ImGui::BeginPopupModal("New file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	const SessionView &v = workspace.view();
	if (!v.project.open) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::Text("New file: %s", asset_kind_label(kind_));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 21.0f);
	const bool enter = ImGui::InputTextWithHint("Name", name_hint(kind_), name_, sizeof(name_),
	                                            ImGuiInputTextFlags_EnterReturnsTrue);
	// The project's name rules, as the session checks them again when it creates the file; a
	// texture is the texture factory's placeholder, made only for a name it takes.
	std::string problem, message;
	const bool named = name_[0] != '\0';
	bool fits = named && check_file_name(name_, kind_, problem, message);
	if (fits && kind_ == AssetKind::Texture) fits = can_make_blank_texture(name_, message);
	const bool taken = named && v.project.scan->find(name_) != nullptr;
	if (named && !fits) ImGui::TextColored(kRefusalColor, "%s", message.c_str());
	else if (taken) ImGui::TextColored(kRefusalColor, "The project has a file named %s already.", name_);
	const bool ready = fits && !taken && v.allows(EditorRequestKind::CreateFile);
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create");
	ImGui::EndDisabled();
	if ((create || enter) && ready) {
		workspace.request(make_request(EditorRequestKind::CreateFile, name_, asset_kind_token(kind_)));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// The tree of the scan's folders and each file's findings, made again when what they read moves.
void FilesWindow::refresh(const SessionView &view) {
	const RevisionKey key = cache_key(view).tree;
	if (view_ == &view && key_ == key && !folders_.empty()) return;
	view_ = &view;
	key_ = key;
	++rebuilds_;
	folders_.assign(1, Folder());
	compared_.clear(); // made again by matching(), once a filter is set
	matches_made_ = false;
	std::map<std::string, size_t> index;
	for (size_t i = 0; i < view.project.scan->entries.size(); ++i) {
		const AssetEntry &entry = view.project.scan->entries[i];
		// An imported file sits in the cache: it shows beside its source.
		const std::string &place = entry.imported_from.empty() ? entry.relative_path : entry.imported_from;
		size_t at = 0;
		std::string path;
		for (const fs::path &part : fs::path(place).parent_path()) {
			path += (path.empty() ? "" : "/") + part.generic_string();
			auto found = index.find(path);
			if (found == index.end()) {
				folders_.push_back(Folder{part.generic_string(), path, {}, {}});
				found = index.emplace(path, folders_.size() - 1).first;
				folders_[at].folders.push_back(found->second);
			}
			at = found->second;
		}
		folders_[at].files.push_back(i);
	}
	for (Folder &folder : folders_)
		std::sort(folder.folders.begin(), folder.folders.end(), [this](size_t a, size_t b) {
			return normalized_logical_name(folders_[a].name) < normalized_logical_name(folders_[b].name);
		});
	counts_.clear();
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.asset.empty()) continue;
		Counts &counts = counts_[d.asset];
		if (d.severity == DiagnosticSeverity::Error) ++counts.errors;
		else if (d.severity == DiagnosticSeverity::Warning) ++counts.warnings;
	}
	if (!entry_at(view, selected_)) selected_.clear();
}

const std::vector<size_t> &FilesWindow::matching(const SessionView &view) {
	if (filter_[0] == '\0') {
		matches_.clear();
		matches_made_ = false;
		return matches_;
	}
	if (matches_made_ && matched_ == filter_) return matches_;
	matches_made_ = true;
	matched_ = filter_;
	matches_.clear();
	if (compared_.size() != view.project.scan->entries.size()) {
		compared_.clear();
		for (const AssetEntry &entry : view.project.scan->entries)
			compared_.push_back(normalized_logical_name(entry.relative_path));
	}
	const std::string wanted = normalized_logical_name(filter_);
	for (size_t i = 0; i < compared_.size(); ++i)
		if (compared_[i].find(wanted) != std::string::npos) matches_.push_back(i);
	return matches_;
}

void FilesWindow::receive(const ViewEvent &event) {
	events_.post(event);
	request_focus();
}

// The file an ask names, selected; a filter that hides it cleared; the folders on its way
// (an imported file's are its source's) to open and the row to scroll to as they draw.
void FilesWindow::show_revealed(const SessionView &view, const ViewEvent &event) {
	const AssetEntry *entry = entry_at(view, event.path);
	if (!entry) return;
	selected_ = entry->relative_path;
	if (filter_[0] != '\0' &&
	    normalized_logical_name(entry->relative_path).find(normalized_logical_name(filter_)) == std::string::npos)
		filter_[0] = '\0';
	scroll_to_ = entry->relative_path;
	open_to_ = entry->imported_from.empty() ? entry->relative_path : entry->imported_from;
	if (event.flag) start_rename(*entry);
}

void FilesWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = workspace_.view();
	// The RevealFile events held since Files last drew, taken now: the newest is shown (its file
	// selected, Rename... opened on it when it asks), as the view's one reveal was, each ask
	// overwriting the one before; an older ask is passed over, its Rename... too. With no project
	// open none shows.
	const std::vector<ViewEvent> reveals = events_.take();
	if (!v.project.open) {
		ui_kit::empty_state("No project open.", "Make one or open one in the Document window.");
		return;
	}
	refresh(v);
	const auto newest = std::find_if(reveals.rbegin(), reveals.rend(),
			[](const ViewEvent &event) { return event.kind == ViewEventKind::RevealFile; });
	if (newest != reveals.rend()) show_revealed(v, *newest);
	draw_toolbar(v);
	// The filter, and beside it how many files the project has (under it in a narrow dock).
	const size_t count = v.project.scan->entries.size();
	const std::string files = std::to_string(count) + (count == 1 ? " file" : " files");
	{
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float field = std::max(ImGui::GetFontSize() * 8.0f,
		                             ImGui::GetContentRegionAvail().x - ui_kit::text_width(files.c_str()) - spacing);
		ui_kit::WrapRow row;
		row.next(field);
		ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter files", field);
		row.next(ui_kit::text_width(files.c_str()));
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", files.c_str());
	}
	// F2 renames the selected file, as its menu's Rename... does (while the busy gate takes a
	// rename).
	if (!selected_.empty() && v.allows(EditorRequestKind::RenameAsset) && ImGui::Shortcut(ImGuiKey_F2))
		if (const AssetEntry *entry = entry_at(v, selected_)) start_rename(*entry);
	// A filter lists the files it matches flat.
	const std::vector<size_t> &matches = matching(v);
	if (v.project.scan->entries.empty()) {
		ui_kit::empty_state("The project has no files yet.", "Import files, or make one with New.");
	} else if (filter_[0] != '\0' && matches.empty()) {
		ui_kit::empty_state("No file matches the filter.");
	} else if (ImGui::BeginTable("files", 3,
	                             ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
	                                     ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
		// The name takes what the others leave. The kind is hidden until the header's menu (a
		// right click) shows it: the folders group the files by kind, and a row's tooltip says
		// it. The size is as wide as "999.9 KB".
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
		                        ui_kit::text_width("Item definitions"));
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("999.9 KB"));
		ImGui::TableHeadersRow();
		if (filter_[0] != '\0') {
			for (const size_t i : matches) draw_file(v, v.project.scan->entries[i], false);
		} else {
			draw_folder(v, folders_.front());
		}
		ImGui::EndTable();
	}
	draw_rename(v);
	draw_references(v);
}

// Import, New and Refresh, wrapping in a narrow dock; each item enabled while the busy gate takes
// its request too (SessionView::allows: a build packing the files refuses a new file, a Refresh
// and a reimport).
void FilesWindow::draw_toolbar(const SessionView &view) {
	ui_kit::WrapRow row;
	const float arrow = ImGui::GetFrameHeight();
	const float import_width = ui_kit::button_width("Import") + arrow;
	row.next(import_width);
	ImGui::SetNextItemWidth(import_width);
	const bool importing = ImGui::BeginCombo("##import", "Import", ImGuiComboFlags_HeightLargest);
	if (!importing) ui_kit::tooltip("Copy files into the project: from the disk, from the game data, or every source again.");
	if (importing) {
		const bool picks = view.allows(EditorRequestKind::PreviewImport);
		ImGui::BeginDisabled(!picks);
		if (ImGui::Selectable("Files...") && picks) {
			EditorRequest pick = make_request(EditorRequestKind::PickFile);
			pick.purpose = PickPurpose::ImportFiles;
			workspace_.request(pick);
		}
		ImGui::EndDisabled();
		ui_kit::tooltip("Files from the disk, or what a PFF archive holds.");
		const bool game_data = !view.project.retail_directory.empty() && view.allows(EditorRequestKind::PreviewRetailImport);
		ImGui::BeginDisabled(!game_data);
		if (ImGui::Selectable("From the game data...") && game_data) {
			EditorRequest listed = make_request(EditorRequestKind::PreviewRetailImport);
			listed.flag = view.project.import_dependencies;
			workspace_.request(listed);
		}
		ImGui::EndDisabled();
		ui_kit::tooltip(!view.project.retail_directory.empty() ? "Files of the game install, copied into the project."
		                                               : "Choose the game install folder in File > Project settings... first.");
		const bool reimports =
				!view.project.imports->empty() && view.allows(EditorRequestKind::Reimport);
		ImGui::BeginDisabled(!reimports);
		if (ImGui::Selectable("Reimport all") && reimports) {
			EditorRequest all = make_request(EditorRequestKind::Reimport);
			all.flag = true;
			workspace_.request(all);
		}
		ImGui::EndDisabled();
		ui_kit::tooltip(view.project.imports->empty()
						? "No file of the project is imported from a source."
						: "Run every importer again (" +
								std::to_string(view.project.imports->size()) +
								(view.project.imports->size() == 1 ? " source)." : " sources)."));
		ImGui::EndCombo();
	}
	const float new_width = ui_kit::button_width("New") + arrow;
	row.next(new_width);
	ImGui::SetNextItemWidth(new_width);
	const bool making = ImGui::BeginCombo("##new", "New", ImGuiComboFlags_HeightLargest);
	if (!making)
		ui_kit::tooltip("Make a new file: a string table, a menu, a font, a placeholder texture, or a file the game reads "
		                "by name.");
	if (making) {
		const bool creates = view.allows(EditorRequestKind::CreateFile);
		// A new file of a kind with a free-form factory: its name is asked first.
		for (size_t i = 0; i < blank_factory_count(); ++i) {
			const BlankFactory &factory = *blank_factory_at(i);
			if (!factory.free_form || factory.role[0] != '\0') continue;
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginDisabled(!creates);
			if (ImGui::Selectable((std::string(asset_kind_label(factory.kind)) + "...").c_str()) && creates)
				new_file_.ask(factory.kind);
			ImGui::EndDisabled();
			ui_kit::tooltip(makes(factory));
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
				workspace_.request(make_request(EditorRequestKind::CreateFile, resource->name, asset_kind_token(factory.kind)));
			ImGui::EndDisabled();
			ui_kit::tooltip(present ? std::string("The project has it.") : makes(factory));
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (ui_kit::tool(row, "Refresh", view.allows(EditorRequestKind::Rescan), "Read the project's folder again."))
		workspace_.request(make_request(EditorRequestKind::Rescan));
}

// A folder's folders, each a node open until folded, then its files.
void FilesWindow::draw_folder(const SessionView &view, const Folder &folder) {
	for (const size_t index : folder.folders) {
		const Folder &inner = folders_[index];
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const std::string label = ui_kit::fit(inner.name, ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing());
		if (!open_to_.empty() && open_to_.rfind(inner.path + "/", 0) == 0) ImGui::SetNextItemOpen(true);
		const bool open = ImGui::TreeNodeEx(inner.path.c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen,
		                                    "%s", label.c_str());
		ui_kit::tooltip(inner.path);
		if (!open) continue;
		draw_folder(view, inner);
		ImGui::TreePop();
	}
	for (const size_t index : folder.files)
		draw_file(view, view.project.scan->entries[index], true);
}

// A file's row: a selectable under the whole row, its name (cut to what its dot and counts
// leave), its kind and its size drawn over it.
void FilesWindow::draw_file(const SessionView &view, const AssetEntry &entry, bool in_tree) {
	ImGui::PushID(entry.relative_path.c_str());
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	const Document *open = open_document(view, entry.relative_path);
	const bool dirty = open && open->dirty();
	const auto found = counts_.find(entry.relative_path);
	const Counts counts = found != counts_.end() ? found->second : Counts();
	if (ImGui::Selectable("##row", entry.relative_path == selected_,
	                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
	                              ImGuiSelectableFlags_AllowOverlap)) {
		selected_ = entry.relative_path;
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && is_editable_kind(entry.kind) &&
		    view.allows(EditorRequestKind::OpenDocument))
			workspace_.request(make_request(EditorRequestKind::OpenDocument, entry.relative_path));
	}
	// Dragged onto a reference field whose kind loads it, the file becomes its value
	// (ReferencePicker::accept_file).
	if (ImGui::BeginDragDropSource()) {
		ImGui::SetDragDropPayload(kFileDragPayload, entry.relative_path.c_str(), entry.relative_path.size() + 1);
		ImGui::TextUnformatted(entry.logical_name.c_str());
		ImGui::EndDragDropSource();
	}
	if (entry.relative_path == scroll_to_) {
		ImGui::SetScrollHereY(0.5f);
		scroll_to_.clear();
		open_to_.clear();
	}
	draw_file_menu(view, entry);
	// Its path, kind and size, made only while its tooltip shows.
	ui_kit::tooltip_lazy([&] {
		std::string tip = entry.relative_path + "\n" + asset_kind_label(entry.kind) + ", " +
		                  size_text(entry.size_bytes);
		if (!entry.imported_from.empty()) tip += "\nImported from " + entry.imported_from;
		if (dirty) tip += "\nUnsaved changes";
		if (counts.errors || counts.warnings)
			tip += "\n" + std::to_string(counts.errors) +
			       (counts.errors == 1 ? " error, " : " errors, ") +
			       std::to_string(counts.warnings) +
			       (counts.warnings == 1 ? " warning" : " warnings") + ": see Problems";
		return tip;
	});
	ImGui::SameLine(0.0f, 0.0f);
	if (in_tree) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetTreeNodeToLabelSpacing());
	const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
	float marks = 0.0f;
	if (dirty) marks += spacing + ui_kit::unsaved_dot_width();
	if (counts.errors) marks += spacing + ui_kit::severity_count_width(counts.errors);
	if (counts.warnings) marks += spacing + ui_kit::severity_count_width(counts.warnings);
	const std::string name = ui_kit::fit(entry.logical_name, ImGui::GetContentRegionAvail().x - marks);
	if (open) ImGui::TextColored(kOpenColor, "%s", name.c_str());
	else ImGui::TextUnformatted(name.c_str());
	if (dirty) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::unsaved_dot();
	}
	if (counts.errors) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::severity_count(DiagnosticSeverity::Error, counts.errors);
	}
	if (counts.warnings) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::severity_count(DiagnosticSeverity::Warning, counts.warnings);
	}
	if (ImGui::TableNextColumn()) ui_kit::clipped_text(asset_kind_label(entry.kind));
	// The size, right-aligned.
	ImGui::TableNextColumn();
	const std::string size = ui_kit::fit(size_text(entry.size_bytes), ImGui::GetContentRegionAvail().x);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_kit::text_width(size.c_str()));
	ImGui::TextUnformatted(size.c_str());
	ImGui::PopID();
}

// A file's menu (a right click on its row).
void FilesWindow::draw_file_menu(const SessionView &view, const AssetEntry &entry) {
	if (!ImGui::BeginPopupContextItem("file_menu")) return;
	selected_ = entry.relative_path;
	const bool opens = is_editable_kind(entry.kind) && view.allows(EditorRequestKind::OpenDocument);
	if (ImGui::MenuItem("Open", nullptr, false, opens) && opens)
		workspace_.request(make_request(EditorRequestKind::OpenDocument, entry.relative_path));
	const bool renames = view.allows(EditorRequestKind::RenameAsset);
	if (ImGui::MenuItem("Rename...", "F2", false, renames) && renames) start_rename(entry);
	if (ImGui::MenuItem("References...", nullptr, false, view.findings.graph != nullptr) &&
			view.findings.graph) {
		references_ = entry.relative_path;
		open_references_ = true;
	}
	const bool reveals = view.allows(EditorRequestKind::RevealPath);
	if (ImGui::MenuItem("Show in folder", nullptr, false, reveals) && reveals)
		workspace_.request(make_request(EditorRequestKind::RevealPath, (fs::path(view.project.root) / entry.relative_path).generic_string()));
	const bool reimports = view.allows(EditorRequestKind::Reimport);
	if (entry.kind == AssetKind::ImageSource && ImGui::MenuItem("Import again", nullptr, false, reimports) && reimports) {
		EditorRequest one = make_request(EditorRequestKind::Reimport, entry.relative_path);
		one.flag = true;
		workspace_.request(one);
	}
	ImGui::EndPopup();
}

void FilesWindow::start_rename(const AssetEntry &entry) {
	renaming_ = entry.relative_path;
	previewed_.clear();
	const size_t n = std::min(entry.logical_name.size(), sizeof(rename_) - 1);
	std::memcpy(rename_, entry.logical_name.data(), n);
	rename_[n] = '\0';
	open_rename_ = true;
}

// Rename...: the new name; every file naming the old one is rewritten, or the rename is
// refused with the reasons in Problems.
void FilesWindow::draw_rename(const SessionView &view) {
	if (open_rename_) {
		open_rename_ = false;
		ImGui::OpenPopup("Rename");
	}
	if (!ImGui::BeginPopup("Rename")) return;
	const AssetEntry *entry = entry_at(view, renaming_);
	if (!entry) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::Text("Rename %s to", entry->logical_name.c_str());
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 17.0f);
	const bool enter = ImGui::InputText("##name", rename_, sizeof(rename_), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool changed = rename_[0] != '\0' && entry->logical_name != rename_;
	// What it would rewrite, or why it would be refused, planned as the name is typed (while the
	// busy gate takes the plan; a rename, which writes the files, waits for a build).
	if (changed && previewed_ != rename_ && view.allows(EditorRequestKind::PreviewRename)) {
		previewed_ = rename_;
		EditorRequest preview = make_request(EditorRequestKind::PreviewRename, entry->relative_path);
		preview.edit.value = previewed_;
		workspace_.request(std::move(preview));
	}
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	if (changed && !plan.symbol && plan.path == entry->relative_path && plan.requested == rename_) {
		for (const Diagnostic &refusal : plan.refusals) {
			ui_kit::severity_marker(refusal.severity);
			ImGui::SameLine();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24.0f);
			ImGui::TextWrapped("%s", refusal.message.c_str());
			ImGui::PopTextWrapPos();
		}
		const std::vector<RenameSite> &sites = *plan.sites;
		if (plan.refusals.empty())
			ImGui::TextDisabled("%s", sites.empty() ? "Nothing names it." :
			                          ("Rewrites " + std::to_string(sites.size()) + " reference" +
			                           (sites.size() == 1 ? ":" : "s:")).c_str());
		for (size_t i = 0; i < sites.size() && i < 12; ++i) {
			const RenameSite &site = sites[i];
			ImGui::BulletText("%s", (site.file + ": " + (site.record.empty() ? "" : site.record + " - ") + site.field + ": " +
			                         site.before + " -> " + site.after).c_str());
		}
		if (sites.size() > 12) ImGui::TextDisabled("and %zu more", sites.size() - 12);
	}
	const bool allowed = view.allows(EditorRequestKind::RenameAsset);
	ImGui::BeginDisabled(!changed || !allowed);
	const bool rename = ImGui::Button("Rename");
	ImGui::EndDisabled();
	ui_kit::tooltip(allowed ? "Every file naming it is rewritten, or the rename is refused (Problems says why)."
	                        : "A rename rewrites the project's files: it waits for the running operation.");
	if ((rename || enter) && changed && allowed) {
		workspace_.request(make_request(EditorRequestKind::RenameAsset, entry->relative_path, rename_));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// References...: what the file names and who uses it (the asset graph: who names it, and who
// names what it defines, a string table's ids or a catalog's names); a use a click away.
void FilesWindow::draw_references(const SessionView &view) {
	if (open_references_) {
		open_references_ = false;
		ImGui::OpenPopup("References");
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetTextLineHeightWithSpacing() * 24.0f));
	if (!ImGui::BeginPopup("References")) return;
	const AssetEntry *entry = entry_at(view, references_);
	if (!entry || !view.findings.graph) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::TextUnformatted(entry->logical_name.c_str());
	// Both ways, asked of the graph once per file while it stands (its edges stay where they are).
	const RevisionKey key = cache_key(view).references;
	if (listed_.view != &view || listed_.key != key || listed_.file != entry->relative_path) {
		listed_.view = &view;
		listed_.key = key;
		listed_.file = entry->relative_path;
		listed_.references = view.findings.graph->references_of(entry->relative_path);
		listed_.users = view.findings.graph->usages_of(entry->relative_path);
	}
	const std::vector<const GraphEdge *> &references = listed_.references;
	const std::vector<const GraphEdge *> &users = listed_.users;
	// Each line names a field by the name the inspector shows (its id in the tooltip).
	if (ImGui::TreeNodeEx("references", ImGuiTreeNodeFlags_DefaultOpen, "References (%zu)", references.size())) {
		if (references.empty()) ui_kit::empty_state("It names no other file or record.");
		for (const GraphEdge *edge : references) {
			std::string file;
			const ReferenceStatus status = edge->target.empty() ? ReferenceStatus::NotAReference : view.findings.graph->resolve(*edge, &file);
			const std::string field = edge_field_title(view, *edge);
			ImGui::BulletText("%s%s = %s", edge->record.empty() ? "" : (edge->record + " - ").c_str(), field.c_str(),
			                  edge->value.c_str());
			ui_kit::tooltip(edge->field);
			if (status == ReferenceStatus::NotAReference) continue;
			ImGui::SameLine();
			ImGui::TextColored(ui_kit::reference_color(status), "%s", ui_kit::reference_word(status));
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("referrers", ImGuiTreeNodeFlags_DefaultOpen, "Referenced by (%zu)", users.size())) {
		if (users.empty()) ui_kit::empty_state("No file of the project names it or what it defines.");
		for (size_t i = 0; i < users.size(); ++i) {
			const GraphEdge &edge = *users[i];
			ImGui::PushID(static_cast<int>(i));
			const std::string line =
			        edge.source + ": " + (edge.record.empty() ? "" : edge.record + " - ") + edge_field_title(view, edge);
			const bool pressed = ImGui::Selectable((line + "###use").c_str());
			if (pressed || ImGui::IsItemHovered()) {
				const ReferenceTarget target = usage_target(*view.project.scan, edge);
				if (pressed) window_requests::go_to(workspace_, target);
				ui_kit::tooltip(edge.field + "\n" +
				                (target.editable ? "Open " + target.file + " at it." : "Show " + target.file + " in Files."));
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
