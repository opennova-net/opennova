#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::editor {

// The labels the mission's overlays draw beside its marks (ADR 0046 S15, Placing and tweaking),
// decluttered: the hovered mark's and the primary's labels always, then the other selected marks'
// nearest first, then, with the labels option on, the other shown marks' nearest first (the marks
// within the mark range), each of those only where its box overlaps no label already placed,
// kMissionLabelsMax at most: a selected mark's label before another's, and the nearer a mark, the
// likelier its label shows (two dozen selected in a base read as the nearest few). The overlays have no
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
	bool always = false; // the hovered mark's or the primary's
	bool first = false; // another selected mark's: placed before the rest, but only where it fits
};
bool operator==(const MissionLabelCandidate &a, const MissionLabelCandidate &b);

// The candidates whose labels draw, by their index, in the order placed.
std::vector<size_t> mission_label_picks(const std::vector<MissionLabelCandidate> &candidates);

// The labels' last layout (the polish): the candidates and their words it was made over and its picks,
// which a frame whose candidates and words are the same takes as they were. A canvas keeps one, so its
// labels are laid out again only when something they read moved (the selection a drag moves, the
// camera, the hover, a title), and then once a frame.
class MissionLabelLayout {
public:
	// The picks of `candidates` worded `words` (one each): the last ones where both are the last
	// layout's, else made anew (mission_label_picks) and kept.
	const std::vector<size_t> &picks(std::vector<MissionLabelCandidate> candidates, std::vector<std::string> words);
	// How many layouts it has made: a test's measure.
	size_t made() const { return made_; }

private:
	std::vector<MissionLabelCandidate> candidates_;
	std::vector<std::string> words_;
	std::vector<size_t> picks_;
	size_t made_ = 0;
};

} // namespace opennova::editor
