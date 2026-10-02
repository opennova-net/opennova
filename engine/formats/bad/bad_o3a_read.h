// The `.o3a` clip-set text (docs/anim/o3a-scene-format.md) read into the clip
// construction seam (bad_build.h): one rig's table rows and every clip they
// name, with rotations, pivots, translations and event velocities in MISSION
// axes (the seam owns the clip-frame conversion). opennova-3di `anim build`
// and the editor's `.o3a` import both read through here, then mint the set
// with bad_build_mint_set. Authoring text, not a port.
#pragma once

#include <istream>
#include <string_view>
#include <vector>

#include <formats/bad/bad_build.h>
#include <formats/threedi/scene_text.h>

namespace opennova::bad {

// The anim slot a table row's key names, -1 for none. The slot table is the
// runtime's (runtime/anim/adm_clip_index.h, adm_slot_index), which a format
// library may not include, so the caller passes it.
using BadSlotLookup = int (*)(std::string_view key);

// Read `.o3a` text into `set`. Every error lands in `findings` (the text has no
// notes); true when none is. The whole-set checks run too: a row whose key
// names no anim slot (`slots`), the set's own checks (bad_build_check_set), a
// clip name or variant that is not a bare file stem, two clips under one name
// and a row naming a clip the set lacks. `clip_lines`, when given, receives the
// line each clip opens on, in set order, so a later mint refusal can name its
// line.
bool bad_o3a_read(std::istream &text, BadSlotLookup slots, BadBuildSet &set,
		std::vector<threedi::SceneFinding> &findings, std::vector<int> *clip_lines = nullptr);

} // namespace opennova::bad
