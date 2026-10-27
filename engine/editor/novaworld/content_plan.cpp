#include <editor/novaworld/content_plan.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

namespace opennova::editor {

namespace {

using project::ProjectFile;
using Files = std::map<std::string, const ProjectFile *>;

Files by_path(const std::vector<ProjectFile> &files) {
	Files out;
	for (const ProjectFile &file : files)
		out.emplace(file.path, &file);
	return out;
}

const ProjectFile *at(const Files &files, const std::string &path) {
	const auto it = files.find(path);
	return it == files.end() ? nullptr : it->second;
}

bool same(const ProjectFile *a, const ProjectFile *b) {
	if (!a || !b)
		return a == b;
	return a->sha256 == b->sha256;
}

// What a side did to the base's file: none when it left it as it was.
std::optional<ContentChange> change_of(const ProjectFile *base, const ProjectFile *side) {
	if (same(base, side))
		return std::nullopt;
	if (!base)
		return ContentChange::Added;
	if (!side)
		return ContentChange::Deleted;
	return ContentChange::Modified;
}

std::optional<ProjectFile> copy_of(const ProjectFile *file) {
	if (!file)
		return std::nullopt;
	return *file;
}

} // namespace

const char *content_change_name(ContentChange change) {
	switch (change) {
		case ContentChange::Added:
			return "added";
		case ContentChange::Modified:
			return "modified";
		case ContentChange::Deleted:
			return "deleted";
	}
	return "modified";
}

std::vector<LocalChange> local_changes(const std::vector<ProjectFile> &base,
		const std::vector<ProjectFile> &local, const std::vector<std::string> &unread,
		bool listed_whole) {
	const Files base_files = by_path(base), local_files = by_path(local);
	const std::unordered_set<std::string> unread_paths(unread.begin(), unread.end());
	std::set<std::string> paths;
	for (const auto &[path, file] : base_files)
		paths.insert(path);
	for (const auto &[path, file] : local_files)
		paths.insert(path);
	std::vector<LocalChange> out;
	for (const std::string &path : paths) {
		if (unread_paths.count(path))
			continue;
		if (const auto change = change_of(at(base_files, path), at(local_files, path)))
			if (listed_whole || *change != ContentChange::Deleted)
				out.push_back({ path, *change });
	}
	return out;
}

size_t PullPlan::conflicts() const {
	return static_cast<size_t>(std::count_if(items.begin(), items.end(),
			[](const PullItem &item) { return item.step == PullStep::Conflict; }));
}

std::vector<ProjectFile> PullPlan::downloads() const {
	std::vector<ProjectFile> out;
	std::unordered_set<std::string> seen;
	for (const PullItem &item : items)
		if (item.step == PullStep::Write && item.theirs && seen.insert(item.theirs->sha256).second)
			out.push_back(*item.theirs);
	return out;
}

const PullItem *PullPlan::find(const std::string &path) const {
	for (const PullItem &item : items)
		if (item.path == path)
			return &item;
	return nullptr;
}

PullPlan plan_pull(const std::vector<ProjectFile> &base, const std::vector<ProjectFile> &local,
		const std::vector<std::string> &unread, const std::vector<ProjectFile> &theirs) {
	const Files base_files = by_path(base), local_files = by_path(local),
				their_files = by_path(theirs);
	const std::unordered_set<std::string> unread_paths(unread.begin(), unread.end());
	std::set<std::string> paths;
	for (const Files *files : { &base_files, &local_files, &their_files })
		for (const auto &[path, file] : *files)
			paths.insert(path);
	PullPlan plan;
	for (const std::string &path : paths) {
		const ProjectFile *b = at(base_files, path), *t = at(their_files, path);
		const ProjectFile *l = at(local_files, path);
		PullItem item;
		item.path = path;
		item.theirs = copy_of(t);
		item.mine = copy_of(l);
		item.theirs_change = change_of(b, t);
		if (unread_paths.count(path)) {
			// The folder holds the file but its bytes could not be read: the revision's change to
			// it is the author's to settle, since the folder's own cannot be told.
			if (same(b, t))
				continue;
			item.step = PullStep::Conflict;
			item.mine_change = ContentChange::Modified;
			item.unread = true;
			plan.items.push_back(std::move(item));
			continue;
		}
		item.mine_change = change_of(b, l);
		if (same(l, t))
			continue; // the folder holds the revision's file already
		if (same(l, b)) {
			item.step = t ? PullStep::Write : PullStep::Remove; // only the revision changed it
		} else if (same(t, b)) {
			continue; // only the folder changed it: a change to push
		} else {
			item.step = PullStep::Conflict;
		}
		plan.items.push_back(std::move(item));
	}
	return plan;
}

PullPlan plan_restore(const std::vector<ProjectFile> &local, const std::vector<std::string> &unread,
		const std::vector<ProjectFile> &revision, const std::vector<std::string> &paths) {
	const Files local_files = by_path(local), their_files = by_path(revision);
	const std::unordered_set<std::string> unread_paths(unread.begin(), unread.end());
	const std::set<std::string> asked(paths.begin(), paths.end());
	PullPlan plan;
	for (const std::string &path : asked) {
		const ProjectFile *l = at(local_files, path), *t = at(their_files, path);
		const bool held_unread = unread_paths.count(path) != 0;
		if (!held_unread && same(l, t))
			continue; // the folder holds the revision's file already
		if (!t && !l && !held_unread)
			continue; // neither has a file there
		PullItem item;
		item.path = path;
		item.theirs = copy_of(t);
		item.mine = copy_of(l);
		item.unread = held_unread;
		item.step = t ? PullStep::Write : PullStep::Remove;
		plan.items.push_back(std::move(item));
	}
	return plan;
}

bool resolve_conflict(PullPlan &plan, const std::string &path, Resolution resolution) {
	const auto it = std::find_if(plan.items.begin(), plan.items.end(), [&](const PullItem &item) {
		return item.path == path && item.step == PullStep::Conflict;
	});
	if (it == plan.items.end())
		return false;
	if (resolution == Resolution::KeepMine) {
		plan.items.erase(it);
		return true;
	}
	it->step = it->theirs ? PullStep::Write : PullStep::Remove;
	return true;
}

} // namespace opennova::editor
