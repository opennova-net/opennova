#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_stamps.h>
#include <editor/documents/terrain_planes.h>
#include <formats/til/til.h>

namespace opennova::editor {

class MissionGround;
class ProjectAssetSource;
struct SessionView;
struct TerrainPatches;

// What a viewport's device takes of a terrain's maps and placed tiles since its picture last showed them (ADR 0046
// S25, the maps and the tiles): a painted region of the colour map or the detail blend map (its texels as the device's
// loader takes them, read from the open document that holds the map), a move of the surface map (which the device
// draws nothing of: its stamp alone), and the placed tiles as the game reads them now (the mission's <mission>.til,
// else the terrain's own, through the project's files, an open document standing in for its file). The device patches
// the terrain it built (godot/src/authoring/terrain_patch_applier: the colour pages, the blend texture, the tile pages,
// each in place) and its read of each file in `files` takes the stamp there, so what it built stands: an Update, never
// a Rebuild.
struct MapsDevicePatch {
	enum class Kind : uint8_t { Colour, Blend, Surface, Tiles };
	Kind kind = Kind::Colour;
	int width = 0, height = 0;      // Colour, Blend: the map's size
	int x = 0, y = 0, w = 0, h = 0; // the changed rectangle of it
	std::vector<uint8_t> rgba;      // its texels, RGBA8, row-major, the top row first
	TilFile tiles;                  // Tiles: the placement the game reads now (none read: no tiles)
	std::vector<FileStamp> files;   // the files it accounts for, by the names the device read them, at their stamps
	uint64_t serial = 0;            // the viewport's count of patches made, this one's
};

// The planes whose patches a 3D view's device takes as MapsDevicePatch (TerrainStrokeFollow's in-place planes beside the
// heights).
inline constexpr uint32_t kMapsDevicePlanes = terrain_plane_bit(TerrainPlane::Colour) |
                                              terrain_plane_bit(TerrainPlane::Blend) |
                                              terrain_plane_bit(TerrainPlane::Surface);

// The names a view's picture reads its placed tiles by, as the game's load takes them (terrain::read_placed_tile_bytes):
// the mission's own name (its <mission>.til; "" none), the header's terrain and its environment (the .env its terrain's
// load reads after the .trn, no extension).
struct TilesRead {
	std::string mission;
	std::string terrain;
	std::string environment;
};

// One viewport's follow of its terrain's maps and placed tiles for its device (S25): the device patches made from a
// frame's map patches (the stroke follow's: TerrainStrokeFollow) and from each move of the placed tiles the game reads,
// queued, each with a serial, until the device says it took them.
class TerrainMapsFollow {
public:
	// The frame's map patches (`patches`: the colour, blend and surface maps' regions its ground took in place) and the
	// placed tiles' move since the last follow (`read` the names they are read by; the ground takes them in place too:
	// MissionGround::retile), each queued for the device, the names of the files they account for added to `names` (the
	// picture takes their stamps). True when it queued one. A colour or blend map no open document holds as a
	// paintable plane is no patch: the caller's picture finds it moved and is made again.
	bool take(const SessionView &view, const TerrainPatches &patches, const TilesRead &read, MissionGround &ground,
	          std::vector<std::string> &names);
	// The picture made again from the files (or first made): what is queued dropped (the device reads the files), and the
	// placed tiles' files as they stand now (read by `read`).
	void built(const SessionView &view, const TilesRead &read);
	// The device took the patches up to `serial` (its report): those dropped. `refused`: one it could not take, so the
	// picture is made again at the next follow (stale()).
	void taken(uint64_t serial, bool refused);
	const std::vector<MapsDevicePatch> &patches() const { return patches_; }
	bool stale() const { return stale_; }
	// The serial the picture was last made at: every patch up to it stands in a picture built from the files.
	uint64_t built_serial() const { return built_serial_; }

private:
	// The placed tiles as the game reads them now (`took` the file taken, "" none), the files the read looked at with
	// their stamps.
	static void read_tiles_(const std::shared_ptr<const ProjectAssetSource> &assets, const TilesRead &read,
	                        std::string &took, std::vector<uint8_t> &bytes, FileStamps &stamps);

	std::vector<MapsDevicePatch> patches_;
	uint64_t serial_ = 0;
	uint64_t built_serial_ = 0;
	bool stale_ = false;
	uint64_t generation_ = UINT64_MAX; // the files' generation the tiles were last compared at
	FileStamps tiles_;                 // what the tiles' read looked at, as last taken
};

} // namespace opennova::editor
