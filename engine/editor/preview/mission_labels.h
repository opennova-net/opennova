#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::editor {

// The labels the mission's overlays draw beside its marks (ADR 0046 S15, Placing and tweaking),
// decluttered: the hovered and the selected marks' labels always (nearest first), then, with the
// labels option on, the other shown marks' nearest first (the marks within the mark range), each only
// where its box overlaps no label already placed, kMissionLabelsMax at most: the nearer a mark, the
// likelier its label shows. The overlays have no
// font: a label's box is its text's length in kMissionLabelCharWidth pixels by kMissionLabelHeight,
// beside its mark at (kMissionLabelDx, kMissionLabelDy), which the editor's 13-pixel font fits.

inline constexpr float kMissionLabelCharWidth = 7.0f;
inline constexpr float kMissionLabelHeight = 15.0f;
inline constexpr float kMissionLabelDx = 10.0f, kMissionLabelDy = -7.0f;
inline constexpr size_t kMissionLabelsMax = 300;

struct MissionLabelCandidate {
	float x = 0.0f, y = 0.0f; // the mark's pixel
	float depth = 0.0f; // in front of the eye
	size_t length = 0; // the label's characters
	bool always = false; // the hovered or a selected mark's
};

// The candidates whose labels draw, by their index, in the order placed.
std::vector<size_t> mission_label_picks(const std::vector<MissionLabelCandidate> &candidates);

} // namespace opennova::editor
