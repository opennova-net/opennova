#include <editor/run/behind_start.h>

namespace opennova::editor {

void BehindStarts::spawned(int64_t pid, int64_t now_ms) {
	children_[pid] = Child{ now_ms, false };
}

bool BehindStarts::tend(int64_t pid, int64_t now_ms, bool exited, bool shown, bool foreground, Step &step) {
	step = Step::Leave;
	const auto found = children_.find(pid);
	if (found == children_.end()) return false;
	Child &child = found->second;
	// Gone, brought forward, or started long enough ago: never pushed back from now on.
	if (exited || foreground || now_ms - child.since >= kTendMs) {
		children_.erase(found);
		return false;
	}
	if (!shown) return true;
	step = Step::SendBack;
	child.sent_back = true;
	return true;
}

bool BehindStarts::lock_wanted(int64_t now_ms) const {
	for (const auto &[pid, child] : children_)
		if (!child.sent_back && now_ms - child.since < kLockMs) return true;
	return false;
}

std::vector<int64_t> BehindStarts::pids() const {
	std::vector<int64_t> out;
	for (const auto &[pid, child] : children_) out.push_back(pid);
	return out;
}

} // namespace opennova::editor
