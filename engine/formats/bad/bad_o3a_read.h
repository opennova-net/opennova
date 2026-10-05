// The `.o3a` clip-set text (docs/anim/o3a-scene-format.md) read into the clip
// construction seam (bad_build.h): one rig's table rows and every clip they
// name, with rotations, pivots, translations and event velocities in MISSION
// axes (the seam owns the clip-frame conversion). opennova-3di `anim build`
// and the editor's `.o3a` import both read through here, then mint the set
// for the retail target with bad_build_mint_set, which checks what the game
// holds (anim slots, bones, name and duration words, packed names). The text
// itself carries none of those limits: the reader checks the text alone.
// Authoring text, not a port.
#pragma once

#include <istream>
#include <vector>

#include <formats/bad/bad_build.h>
#include <formats/threedi/scene_text.h>

namespace opennova::bad {

// Where the set's parts open in the text, in set order: each clip, each of a
// clip's bones, each table row. A set check or mint problem names its line
// through them (bad_o3a_line).
struct BadO3aLines {
	std::vector<int> clips;
	std::vector<std::vector<int>> bones; // per clip
	std::vector<int> rows;
};

// Read `.o3a` text into `set`. Every error lands in `findings` (the text has no
// notes); true when none is. The reader checks the text alone: every field,
// a key duration on every key of a bone or none, a clip name or variant that
// is not a bare file stem, two clips under one name and a row naming a clip
// the set lacks. What the target holds is bad_build_mint_set's to say.
bool bad_o3a_read(std::istream &text, BadBuildSet &set, std::vector<threedi::SceneFinding> &findings,
		BadO3aLines *lines = nullptr);

// The line a set problem belongs to: its bone's, else its clip's, else its
// row's; 0 for the whole set.
int bad_o3a_line(const BadO3aLines &lines, const BadBuildProblem &problem);

// Each problem as an error finding at its line.
void bad_o3a_findings(const BadO3aLines &lines, const std::vector<BadBuildProblem> &problems,
		std::vector<threedi::SceneFinding> &findings);

} // namespace opennova::bad
