#include <editor/preview/mission_labels.h>

#include <algorithm>
#include <numeric>

namespace opennova::editor {

namespace {

struct Box {
	float left, top, right, bottom;
};

Box box_of(const MissionLabelCandidate &label) {
	const float left = label.x + kMissionLabelDx, top = label.y + kMissionLabelDy;
	return Box{ left, top, left + float(label.length) * kMissionLabelCharWidth, top + kMissionLabelHeight };
}

bool overlaps(const Box &a, const Box &b) {
	return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

} // namespace

std::vector<size_t> mission_label_picks(const std::vector<MissionLabelCandidate> &candidates) {
	std::vector<size_t> order(candidates.size());
	std::iota(order.begin(), order.end(), size_t(0));
	// The ones that always show first, then the other selected, each nearest first.
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		if (candidates[a].always != candidates[b].always) return candidates[a].always;
		if (candidates[a].first != candidates[b].first) return candidates[a].first;
		return candidates[a].depth < candidates[b].depth;
	});
	std::vector<size_t> out;
	std::vector<Box> placed;
	for (const size_t index : order) {
		const MissionLabelCandidate &label = candidates[index];
		const Box box = box_of(label);
		if (!label.always) {
			if (out.size() >= kMissionLabelsMax) continue;
			bool clear = true;
			for (const Box &other : placed)
				if (overlaps(box, other)) {
					clear = false;
					break;
				}
			if (!clear) continue;
		}
		placed.push_back(box);
		out.push_back(index);
	}
	return out;
}

} // namespace opennova::editor
