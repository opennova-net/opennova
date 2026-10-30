#include <editor/session/editor_preferences.h>

#include <algorithm>
#include <utility>

#include <editor/project/project_document.h>

namespace opennova::editor {

std::string recent_items_game(const ProjectDocument &project) {
	return project.expansion.standalone() ? project.target_game : project.target_game + "/" + project.expansion.name;
}

bool EditorPreferences::load(Diagnostic &finding) {
	finding = Diagnostic();
	Preferences loaded;
	if (!store_.load(loaded, finding)) {
		values_ = Preferences();
		return false;
	}
	values_ = std::move(loaded);
	return true;
}

// `preferences` with any working copy the store keeps that they lack: a debug and a release editor share the file.
static void take_stored_copies(PreferencesStore &store, Preferences &preferences) {
	Preferences kept;
	Diagnostic finding;
	if (!store.load(kept, finding)) return;
	std::vector<std::string> &copies = preferences.working_copies;
	for (const std::string &root : kept.working_copies)
		if (copies.size() < kWorkingCopiesMax && std::find(copies.begin(), copies.end(), root) == copies.end())
			copies.push_back(root);
}

bool EditorPreferences::write(const Preferences &next, Diagnostic &error) {
	Preferences merged = next;
	take_stored_copies(store_, merged);
	if (!store_.save(merged, error)) return false;
	values_ = std::move(merged);
	return true;
}

bool EditorPreferences::save(Diagnostic &error) {
	take_stored_copies(store_, values_);
	return store_.save(values_, error);
}

void EditorPreferences::remember_recent_project(const std::string &root) {
	forget_recent_project(root);
	values_.recent_projects.insert(values_.recent_projects.begin(), root);
	if (values_.recent_projects.size() > kRecentProjectsMax) values_.recent_projects.resize(kRecentProjectsMax);
}

bool EditorPreferences::remember_working_copy(const std::string &root) {
	std::vector<std::string> &copies = values_.working_copies;
	if (!copies.empty() && copies.front() == root) return false;
	copies.erase(std::remove(copies.begin(), copies.end(), root), copies.end());
	copies.insert(copies.begin(), root);
	if (copies.size() > kWorkingCopiesMax) copies.resize(kWorkingCopiesMax);
	return true;
}

bool EditorPreferences::know_working_copy(const std::string &root) {
	std::vector<std::string> &copies = values_.working_copies;
	if (copies.size() >= kWorkingCopiesMax || std::find(copies.begin(), copies.end(), root) != copies.end()) return false;
	copies.push_back(root);
	return true;
}

bool EditorPreferences::remember_recent_item(const std::string &game, int64_t item) {
	std::vector<int64_t> &recent = values_.recent_items[game];
	if (!recent.empty() && recent.front() == item) return false;
	recent.erase(std::remove(recent.begin(), recent.end(), item), recent.end());
	recent.insert(recent.begin(), item);
	if (recent.size() > kRecentItemsMax) recent.resize(kRecentItemsMax);
	return true;
}

const std::vector<int64_t> &EditorPreferences::recent_items(const std::string &game) const {
	static const std::vector<int64_t> none;
	const auto found = values_.recent_items.find(game);
	return found == values_.recent_items.end() ? none : found->second;
}

void EditorPreferences::forget_recent_project(const std::string &root) {
	values_.recent_projects.erase(std::remove(values_.recent_projects.begin(), values_.recent_projects.end(), root),
	                              values_.recent_projects.end());
}

} // namespace opennova::editor
