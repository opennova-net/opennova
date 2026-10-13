#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_ground_overlay.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/terrain_brush_overlay.h>
#include <editor/preview/terrain_heights_patch.h>
#include <editor/preview/terrain_maps_patch.h>
#include <editor/preview/terrain_patch.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/terrain_uses.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

// What a terrain viewport shows, and why not (the deep-integration plan's DI-30b): the kind's reason.
enum class TerrainViewStatus : uint8_t {
	NoProject, // no project is open
	NoTerrain, // no terrain is open at its path
	Unwritable, // the document cannot be written, so the game would read no such file (detail: why)
	Refused, // the game's gate refuses the terrain as it stands (detail: its words): it loads no ground
	Ready,
};
// "no_project", "no_terrain", "unwritable", "refused", "ready": its token on the wire.
const char *terrain_view_status_token(TerrainViewStatus status);

// The environment option's word for no mission's: the neutral one, the engine's own with no file read (a
// mission's start with no .env runs on Environment_InitDefaults' state [orig: Terrain_LoadEnvironmentConfig @
// 0x610947 -> Environment_InitDefaults @ 0x57c010], env-tod-re.md #38).
inline constexpr const char *kTerrainNeutral = "none";

// How a terrain viewport draws (its options): the using mission whose environment, tile set, tiles and clock
// it is drawn with ("" the first that runs on it; kTerrainNeutral none: the engine's own environment and the
// terrain's own tiles), what it tints the terrain with (DI-29's overlay) and whether the overlay's legend is open over
// the picture's corner (S25, the painting feedback: the canvas's alone), and the layers drawn (the foliage, the water).
struct TerrainViewportOptions {
	std::string mission;
	MissionGroundOverlay overlay = MissionGroundOverlay::None;
	bool legend = true;
	bool foliage = true;
	bool water = true;
	bool operator==(const TerrainViewportOptions &o) const {
		return mission == o.mission && overlay == o.overlay && legend == o.legend && foliage == o.foliage && water == o.water;
	}
	bool operator!=(const TerrainViewportOptions &o) const { return !(*this == o); }
};

// The options on the wire (the envelope's `options`, a SetViewport's): {mission, overlay, legend, show {foliage, water}}.
io::JsonValue terrain_options_to_json(const TerrainViewportOptions &options);
// The change a SetViewport makes to set the options to `options`, and to set the camera.
std::string terrain_options_change(const TerrainViewportOptions &options);
std::string terrain_camera_change(const OrbitCamera &camera);

// A terrain's viewport (DI-30b; ViewportKind::Terrain, the Document tab's main view; CONTEXT.md "Terrain viewport"):
// the terrain at its path as the game would read it were it saved now (the open document's bytes, which the
// device reads through the project's files: the terrain's load, its .cpt, its maps, its foliage models), drawn
// on its own, with no mission, by the Shell's device through the runtime's own terrain, water, foliage and
// environment nodes, under the environment of a mission that runs on it (session/terrain_uses: the first, or the
// option's; the header's overrides and its start time as the mission's start applies them) or the engine's own
// with none; its tiles the mission's <mission>.til, else the terrain's own polytrn_tileinfo, as the game reads
// them [orig: Terrain_Init @ 0x60FCFD; PolyTrn_LoadTerrainConfig @ 0x60E6C9, @ 0x60E6DC..0x60E6E5]. DI-29's
// overlays tint it with what the game reads at each point (mission_ground_overlay), and a point of the picture
// says what the ground there is in the game's words (DI-07's MissionGroundFacts, the device's ray). Its camera
// orbits, pans and dollies (preview/orbit_canvas), framed on the sector grid's mapped cells.
class TerrainViewport final : public ViewportModel {
public:
	explicit TerrainViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	TerrainViewStatus view_status() const { return reason_; }
	const TerrainViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	// The terrain's logical name without its extension (what a mission's header names it by), the missions that run
	// on it, and the one drawn (null: the neutral environment).
	const std::string &terrain_name() const { return terrain_name_; }
	const TerrainUses &uses() const { return uses_; }
	const TerrainMissionUse *mission() const;
	// The header the picture is drawn with (the terrain, the mission's tile set, environment, overrides and clock)
	// and the mission's own name, whose .til the game reads first ("" with none).
	const MissionSceneHeader &header() const { return header_; }
	const std::string &mission_name() const { return mission_name_; }
	// The ground as the game reads it (the terrain, its tiles, its water), for the readout and the overlay.
	const MissionGround &ground() const { return ground_; }
	// The ground followed over the project's files first (S25: the terrain brush's space and hints); while a stroke
	// feeds a file it read, as it stands: the view's own follow patches it in place (a read here, the first of a frame,
	// would read the files the stroke changed again whole per sample).
	const MissionGround &followed_ground(const SessionView &view) const {
		if (!ground_.terrain() || !gesture_reading_(view, ground_.stamps())) follow_ground_(view);
		return ground_;
	}
	// S25 (the maps and the tiles): what a paint stroke (or its undo) changed of the colour, blend and surface maps,
	// and each move of the placed tiles, since the picture was last made, as its device takes them in place
	// (terrain_maps_patch.h); the device says how far it took them (ViewportDeviceReport::maps_taken).
	const std::vector<MapsDevicePatch> &maps_patches() const { return maps_.patches(); }
	uint64_t maps_built_serial() const { return maps_.built_serial(); }
	// The overlay the options ask (DI-29) and its serial (moves each time it is made again: the device's Update).
	const MissionOverlayImage &overlay() const { return overlay_; }
	uint64_t overlay_serial() const { return overlay_serial_; }
	// Whether the device's terrain stands, and what its last presented frame drew (its report: the frames, the
	// foliage's slots, cells and instances, the placed tiles).
	bool surface() const { return surface_; }
	const io::JsonValue &drawn() const { return drawn_; }
	// The names its device asked the project's files for and did not find.
	const std::vector<std::string> &missing() const { return missing_; }
	// The camera on the sector grid's mapped cells from above and to the south, on a picture `width` x `height`, no
	// farther from its target than the fog lets it see (fog_reach).
	OrbitCamera framed(int width, int height) const;
	// How far a framing sees through the environment's fog (metres; 0 none): the mission view's rule
	// (mission_fog_reach), the engine's own environment's with no mission.
	float fog_reach() const { return fog_reach_; }
	// What the device's ray meets first under picture pixel (x, y), in the game's words (DI-07).
	MissionGroundFacts ground_under(const ViewportContext &context, float x, float y) const;
	// S25 (the heights): what a stroke or a bake's landing changed of the open heights document its terrain reads,
	// since the picture was last made, as its device takes it (terrain_heights_patch.h), each with a serial, as the
	// mission view's; the device says how far it took them (ViewportDeviceReport::heights_taken) and those are dropped.
	const std::vector<HeightsDevicePatch> &heights_patches() const { return heights_patches_; }
	uint64_t heights_built_serial() const { return heights_built_serial_; }

	ViewportStatus status() const override;
	const char *reason() const override { return terrain_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const FileStamps *picture_reads() const override { return &picture_.files(); }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	// The ground under the point (`ground`); no record is picked.
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	// The terrain tools' drags (the brush's, a tile's), drops (a Stamp's) and commands (S25, terrain_view_commands.h).
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	bool drop(const ViewportContext &context, const ViewportDrop &drop, CanvasRequests &out, std::string &error) const override;
	bool command_of(const ViewportContext &context, const ViewportCommand &command, CanvasRequests &out,
			std::string &error) const override;
	// "frame" (the camera on the mapped cells) and "top" (straight down over its target, north up).
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The missions it can be drawn under, each an item (its path as its name, kind "mission", whether it is drawn).
	io::JsonValue items_json(const ViewportInput &input) const override;
	io::JsonValue notes_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	// S25: a gesture in its own document or in a file its picture read (a terrain stroke's) holds its Rebuild.
	uint64_t gesture_held_(const ViewportInput &input) const override {
		if (const uint64_t picture = gesture_feeding_(input, picture_reads())) return picture;
		return gesture_feeding_(input, &ground_.stamps());
	}
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(TerrainViewStatus reason, const std::string &detail);
	// The mission it is drawn under and its header, from the uses and the options; true when what the picture is
	// made from (the environment, the tile set, the tiles, the overrides) moved.
	bool place_(const SessionView &view);
	void follow_ground_(const SessionView &view) const;
	// The names its picture reads the placed tiles by (the mission drawn under, the header's terrain and environment).
	TilesRead tiles_read_() const { return TilesRead{mission_name_, header_.terrain, header_.environment}; }
	// The picture made from the files (`document` the view's): its stroke, maps and heights follows start again from
	// them.
	void made_(const SessionView &view, const DocumentBase &document);
	// The overlay made again where the option or the ground moved; true when it did.
	bool follow_overlay_(const SessionView &view);
	// The heights patches the stroke follow made this follow queued for the device (true when one was), and the
	// picture made again from the files (which hold every change so far): the mission view's legs.
	bool take_heights_(const SessionView &view);
	void heights_built_(const SessionView &view, const DocumentBase &document);

	TerrainViewStatus reason_ = TerrainViewStatus::NoProject;
	std::string detail_;
	TerrainViewportOptions options_;
	OrbitCamera camera_;
	bool framed_ = false; // the camera framed on the ground once
	OrbitCamera framing_; // the camera as last framed
	bool options_moved_ = false; // an option the device draws by moved: an Update
	bool layout_moved_ = false; // the mission moved: a Rebuild
	PreviewFollow picture_;
	uint64_t layout_serial_ = 1;
	uint64_t terrain_revision_ = 0; // the document's revision as last drawn: an edit builds the terrain again
	uint64_t terrain_identity_ = 0, terrain_load_ = 0;
	std::string terrain_name_;
	TerrainUses uses_;
	RevisionKey uses_key_;
	bool uses_known_ = false;
	MissionSceneHeader header_;
	std::string mission_name_;
	std::string mission_path_; // the use drawn ("" none)
	mutable MissionGround ground_;
	// S25: the ground patched in place during a stroke; its device patches the heights and the maps in place
	// (TerrainPatchApplier).
	TerrainStrokeFollow stroke_{terrain_plane_bit(TerrainPlane::Heights) | kMapsDevicePlanes};
	TerrainMapsFollow maps_; // S25: the maps' and the placed tiles' device patches
	HeightsTilesFollow heights_tiles_;
	bool heights_stale_ = false; // a heights patch the device could not take: the picture made again
	std::vector<HeightsDevicePatch> heights_patches_;
	uint64_t heights_serial_ = 0;
	uint64_t heights_built_serial_ = 0;
	MissionOverlayImage overlay_;
	uint64_t overlay_ground_ = UINT64_MAX;
	uint64_t overlay_serial_ = 0;
	// The overlay option as the terrain brush sets it while it paints a map here (S25, the painting feedback).
	BrushOverlay brush_overlay_;
	bool surface_ = false;
	io::JsonValue drawn_;
	float fog_reach_ = 0.0f;
	std::vector<std::string> missing_;
};

} // namespace opennova::editor
