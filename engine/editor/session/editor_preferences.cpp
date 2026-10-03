#include <editor/session/editor_preferences.h>

#include <algorithm>
#include <utility>

namespace opennova::editor {

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

bool EditorPreferences::write(const Preferences &next, Diagnostic &error) {
	if (!store_.save(next, error)) return false;
	values_ = next;
	return true;
}

void EditorPreferences::remember_recent_project(const std::string &root) {
	forget_recent_project(root);
	values_.recent_projects.insert(values_.recent_projects.begin(), root);
	if (values_.recent_projects.size() > kRecentProjectsMax) values_.recent_projects.resize(kRecentProjectsMax);
}

void EditorPreferences::remember_recent_item(int64_t item) {
	std::vector<int64_t> &recent = values_.recent_items;
	recent.erase(std::remove(recent.begin(), recent.end(), item), recent.end());
	recent.insert(recent.begin(), item);
	if (recent.size() > kRecentItemsMax) recent.resize(kRecentItemsMax);
}

void EditorPreferences::forget_recent_project(const std::string &root) {
	values_.recent_projects.erase(std::remove(values_.recent_projects.begin(), values_.recent_projects.end(), root),
	                              values_.recent_projects.end());
}

} // namespace opennova::editor
