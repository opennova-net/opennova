#pragma once

#include <cstdint>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <base/io/json.h>
#include <editor/preview/mission_ground_overlay.h>

namespace godot {

class Terrain;

// The ground overlay's device leg (ADR 0046 DI-29; S25, the painting feedback), shared by the mission's and the
// terrain's 3D views: the viewport's picture (MissionOverlayImage) given the terrain as it moves, over two pictures kept
// between uploads and handed over in turn (a texture update a rendering thread runs a frame later reads the one this
// does not write). The same picture patched (its `made` and size a buffer's) copies only the rows its patches stamped
// past that buffer's, and the terrain's texture takes it in place (Terrain::set_ground_overlay's update of a texture
// of the same size); another picture is copied whole. What the last upload cost is reported (the device's
// `drawn.overlay`), the painting's frame cost read over the MCP.
class GroundOverlayUpload {
public:
	// `overlay` given `terrain` (the caller calls it as the viewport's overlay serial moves): None clears it.
	void apply(Terrain &terrain, const opennova::editor::MissionOverlayImage &overlay);
	// The terrain built again or let go: the next picture is copied whole.
	void reset();
	// {uploads, rows (the last's), whole (the last copied whole), last_us (its cost on the main thread, the copy and the
	// texture's upload)}.
	opennova::io::JsonValue to_json() const;

private:
	// Two pictures, each with the make and the stamp it holds, handed over in turn.
	struct Buffer {
		Ref<Image> image;
		uint64_t made = 0;
		uint64_t stamp = 0;
	};
	Buffer buffers_[2];
	int next_ = 0;
	int64_t uploads_ = 0;
	int64_t last_us_ = 0;
	int last_rows_ = 0;
	bool last_whole_ = false;
};

} // namespace godot
