#pragma once

#include <string>
#include <vector>

namespace opennova::editor {

// A file the game opens by a fixed name once a mission runs, beyond the manifest's rows
// (docs/required-resources.md names them as the hardcoded sets beside its mission rows): the HUD's
// and the first-person view effects' textures, every crosshair style's, the effects' (rain, snow,
// the emitter pool's smoke, the water rings, the impact scars, the terrain's scorch marks), the
// overcast keyframes, the fallback AI profiles, the fallback animation map and the pickup call. Each
// name is the runtime's own constant, the one its opener reads (ADR 0046 S14, review F4), so an
// import brings what a mission played from the project opens. `what` is what the game opens it
// for, read after "the game, ".
struct MissionFixedFile {
	std::string name;
	std::string what;
};

// Every one, each name once, in the order above.
const std::vector<MissionFixedFile> &mission_fixed_files();

} // namespace opennova::editor
