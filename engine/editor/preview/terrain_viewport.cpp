#include <editor/preview/terrain_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/terrain_document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/env/env_weather.h>
#include <runtime/environment/environment_state.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The size its device draws at where no canvas sizes it (a headless Shell's).
constexpr ViewportState kHeadlessSize{ 1024, 768 };
// The framing: looking down from the south at this pitch (radians, the orbit's: above the target).
constexpr float kFramePitch = 0.75f;

std::string base_of(const std::string &path) {
	std::string name = path;
	if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
	return name;
}

std::string stem_of(const std::string &path) {
	std::string name = base_of(path);
	if (const size_t dot = name.find_last_of('.'); dot != std::string::npos) name.erase(dot);
	return name;
}

// A SetViewport's options over `held` (each member optional); `uses` the missions that run on the terrain (a
// mission named must be one, or kTerrainNeutral).
bool read_options(const JsonValue &json, const TerrainUses &uses, TerrainViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {mission, overlay, show}.";
		return false;
	}
	TerrainViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "mission") {
			if (!value.is_string()) {
				error = "options.mission is the path of a mission that runs on the terrain (\"\" the first, \"none\" the "
				        "engine's own environment).";
				return false;
			}
			if (!value.string.empty() && value.string != kTerrainNeutral) {
				const auto found = std::find_if(uses.missions.begin(), uses.missions.end(),
						[&](const TerrainMissionUse &use) { return use.mission == value.string; });
				if (found == uses.missions.end()) {
					std::string listed;
					for (size_t i = 0; i < uses.missions.size(); ++i) listed += (i ? ", " : "") + uses.missions[i].mission;
					error = "No mission that runs on the terrain is " + value.string +
					        (listed.empty() ? std::string(" (none does; \"none\" draws it under the engine's own environment).")
					                        : " (" + listed + ", or \"none\").");
					return false;
				}
			}
			options.mission = value.string;
		} else if (key == "overlay") {
			if (!value.is_string() || !mission_ground_overlay_from_token(value.string, options.overlay)) {
				error = "options.overlay is none, surfaces or foliage: what the game reads at each point, tinted over the "
				        "terrain.";
				return false;
			}
		} else if (key == "show") {
			if (!value.is_object()) {
				error = "options.show is {foliage, water}.";
				return false;
			}
			for (const io::JsonMember &shown : value.object) {
				if ((shown.key != "foliage" && shown.key != "water") || !shown.value.is_bool()) {
					error = "options.show takes foliage and water, each true or false.";
					return false;
				}
				(shown.key == "foliage" ? options.foliage : options.water) = shown.value.boolean;
			}
		} else {
			error = "Unknown terrain option \"" + key + "\" (it takes mission, overlay, show).";
			return false;
		}
	}
	held = options;
	return true;
}

} // namespace

const char *terrain_view_status_token(TerrainViewStatus status) {
	switch (status) {
	case TerrainViewStatus::NoProject: return "no_project";
	case TerrainViewStatus::NoTerrain: return "no_terrain";
	case TerrainViewStatus::Unwritable: return "unwritable";
	case TerrainViewStatus::Refused: return "refused";
	case TerrainViewStatus::Ready: return "ready";
	}
	return "no_project";
}

io::JsonValue terrain_options_to_json(const TerrainViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("mission", json_string(options.mission));
	out.set("overlay", json_string(mission_ground_overlay_token(options.overlay)));
	JsonValue show = JsonValue::make_object();
	show.set("foliage", JsonValue::make_bool(options.foliage));
	show.set("water", JsonValue::make_bool(options.water));
	out.set("show", std::move(show));
	return out;
}

std::string terrain_options_change(const TerrainViewportOptions &options) {
	return viewport_change(ViewportKind::Terrain, "options", terrain_options_to_json(options));
}

std::string terrain_camera_change(const OrbitCamera &camera) {
	JsonValue view = mission_camera_to_json(camera);
	JsonValue taken = JsonValue::make_object();
	for (const char *member : { "target", "yaw", "pitch", "distance" })
		if (const JsonValue *value = view.get(member)) taken.set(member, *value);
	return viewport_change(ViewportKind::Terrain, "camera", std::move(taken));
}

// --- TerrainViewport -------------------------------------------------------------------------------

TerrainViewport::TerrainViewport(std::string path) : ViewportModel(ViewportKind::Terrain, std::move(path), kHeadlessSize) {
	camera_.yaw = 0.0f;
	camera_.pitch = kFramePitch;
	camera_.distance = 1500.0f;
	camera_.near_plane = 0.5f;
	camera_.far_plane = 16000.0f;
}

std::unique_ptr<ViewportModel> TerrainViewport::make(const std::string &path) {
	return std::make_unique<TerrainViewport>(path);
}

ViewportStatus TerrainViewport::status() const {
	switch (reason_) {
	case TerrainViewStatus::Ready: return ViewportStatus::Ready;
	case TerrainViewStatus::Unwritable:
	case TerrainViewStatus::Refused: return ViewportStatus::Failed;
	case TerrainViewStatus::NoProject:
	case TerrainViewStatus::NoTerrain: break;
	}
	return ViewportStatus::Empty;
}

std::string TerrainViewport::message() const {
	switch (reason_) {
	case TerrainViewStatus::NoProject: return "Open a project to see its terrains.";
	case TerrainViewStatus::NoTerrain: return "Open a terrain to see its ground.";
	case TerrainViewStatus::Unwritable: return "The terrain cannot be written, so the game would read no such file: " + detail_;
	case TerrainViewStatus::Refused: return detail_ + " Its Problems row holds the fix.";
	case TerrainViewStatus::Ready: break;
	}
	return std::string();
}

std::string TerrainViewport::caption() const {
	if (reason_ != TerrainViewStatus::Ready) return std::string();
	if (const TerrainMissionUse *use = mission()) return " under " + base_of(use->mission) + "'s environment";
	return " under the engine's own environment";
}

const TerrainMissionUse *TerrainViewport::mission() const {
	for (const TerrainMissionUse &use : uses_.missions)
		if (use.mission == mission_path_) return &use;
	return nullptr;
}

ViewportAction TerrainViewport::stop_(TerrainViewStatus reason, const std::string &detail) {
	reason_ = reason;
	detail_ = detail;
	shown_none();
	return picture_.stop();
}

bool TerrainViewport::place_(const SessionView &view) {
	const RevisionKey key =
			revision_key(view.revisions, { ViewConcern::Project, ViewConcern::Files, ViewConcern::Graph, ViewConcern::Documents });
	if (!uses_known_ || key != uses_key_) {
		uses_ = terrain_uses(view, path());
		uses_key_ = key;
		uses_known_ = true;
	}
	// The option's mission where it still runs on it, else the first; none where the option says so.
	const TerrainMissionUse *use = nullptr;
	if (options_.mission != kTerrainNeutral) {
		for (const TerrainMissionUse &each : uses_.missions)
			if (!options_.mission.empty() && each.mission == options_.mission) use = &each;
		if (!use && !uses_.missions.empty()) use = &uses_.missions.front();
	}
	MissionSceneHeader header;
	header.terrain = terrain_name_;
	std::string name;
	if (use) {
		// What a mission's load reads beside its terrain [orig: Game_LoadTerrainDuringConnect @ 0x520710;
		// Game_StartMission @ 0x525371..0x525399, @ 0x5253d5]: its environment and the header's overrides over it,
		// its tile set over the terrain's own, its clock.
		header.environment = use->environment;
		header.tile_set = use->tile_set;
		header.start_time = use->start_time;
		header.minutes_per_day = use->minutes_per_day;
		header.attrib_flags = use->attrib_flags;
		header.water_override = use->water_override;
		header.fog_override = use->fog_override;
		header.water_murk = use->water_murk;
		for (int i = 0; i < 3; ++i) {
			header.fog_color[i] = use->fog_color[i];
			header.water_color[i] = use->water_color[i];
		}
		name = use->name;
	}
	const std::string drawn = use ? use->mission : std::string();
	const bool moved = header != header_ || name != mission_name_ || drawn != mission_path_;
	header_ = header;
	mission_name_ = name;
	mission_path_ = drawn;
	if (moved) ++layout_serial_;
	return moved;
}

void TerrainViewport::follow_ground_(const SessionView &view) const {
	if (!view.findings.assets) return;
	ground_.follow(view.findings.assets, view.findings.assets->generation(), header_, mission_name_);
}

bool TerrainViewport::follow_overlay_(const SessionView &view) {
	const MissionGroundOverlay kind = options_.overlay;
	if (kind == MissionGroundOverlay::None) {
		if (overlay_.kind == MissionGroundOverlay::None) return false;
		overlay_ = MissionOverlayImage();
		++overlay_serial_;
		return true;
	}
	follow_ground_(view);
	if (overlay_.kind == kind && overlay_reads_ == ground_.reads()) return false;
	overlay_ = mission_ground_overlay(ground_, kind);
	overlay_reads_ = ground_.reads();
	++overlay_serial_;
	return true;
}

OrbitCamera TerrainViewport::framed(int width, int height) const {
	OrbitCamera camera = camera_;
	const terrain::TerrainHeightField *field = ground_.height_field();
	if (!field) return camera;
	// The mapped cells' extent: the grid cells holding a sector (the whole grid where none does), each 512 units
	// from the .trn's origin [orig: Terrain_GetHeightAtPosition @ 0x606720, the cell a position routes through
	// less the origin; runtime/terrain_query/coords.h], the mission's y the world's -z.
	const terrain::SectorLayout &layout = field->layout;
	int min_col = 16, max_col = -1, min_row = 16, max_row = -1;
	const int rows = std::max(layout.sector_rows, 1), cols = std::max(layout.sector_count, 1);
	for (int r = 0; r < rows; ++r)
		for (int c = 0; c < cols; ++c)
			if (layout.sector_grid && layout.sector_grid[r * terrain::COORDS_SECTOR_GRID_DIM + c] > 0) {
				min_col = std::min(min_col, c);
				max_col = std::max(max_col, c);
				min_row = std::min(min_row, r);
				max_row = std::max(max_row, r);
			}
	if (max_col < 0) {
		min_col = min_row = 0;
		max_col = cols - 1;
		max_row = rows - 1;
	}
	const double size = double(terrain::COORDS_SECTOR_SIZE);
	const double x0 = (layout.origin_x + min_col) * size, x1 = (layout.origin_x + max_col + 1) * size;
	const double z0 = (layout.origin_y + min_row) * size, z1 = (layout.origin_y + max_row + 1) * size;
	const double cx = (x0 + x1) * 0.5, cz = (z0 + z1) * 0.5;
	double ground = 0.0;
	const float found = terrain::height_field_height_world_bilinear(*field, float(cx), float(cz));
	if (std::isfinite(found)) ground = found;
	const double point[3] = { cx, -cz, ground };
	const float radius = float(std::max(x1 - x0, z1 - z0) * 0.5 * std::sqrt(2.0));
	camera.yaw = 0.0f;
	camera.pitch = kFramePitch;
	camera.frame(mission_to_preview(point), radius, std::max(width, 1), std::max(height, 1));
	// As the game draws it, nothing past the fog shows: the framing stands within it (the mission view's rule).
	if (fog_reach_ > 0.0f && camera.distance > fog_reach_) camera.distance = fog_reach_;
	return camera;
}

ViewportAction TerrainViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) return stop_(TerrainViewStatus::NoProject, std::string());
	const auto *terrain = dynamic_cast<const TerrainDocument *>(input.document);
	if (!terrain || !terrain->config()) return stop_(TerrainViewStatus::NoTerrain, std::string());
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path()) : nullptr;
	terrain_name_ = stem_of(entry ? entry->logical_name : path());
	// The game reads the file Save would write: a document that cannot be written leaves none to read, and one
	// its gate refuses loads no ground (the mission's load aborts).
	const SerializeResult written = terrain->serialize();
	if (!written.ok()) {
		const ViewportAction action = stop_(TerrainViewStatus::Unwritable, written.issues.front().message);
		shown(*terrain);
		return action;
	}
	if (const std::string refused = terrain->refused(); !refused.empty()) {
		const ViewportAction action = stop_(TerrainViewStatus::Refused, refused);
		shown(*terrain);
		return action;
	}
	reason_ = TerrainViewStatus::Ready;
	detail_.clear();
	const bool placed = place_(view);
	const float reach = fog_reach_;
	if (placed || fog_reach_ <= 0.0f) {
		fog_reach_ = mission_fog_reach(*view.findings.assets, header_);
		// With no mission's .env, the engine's own environment's fog [orig: Environment_InitDefaults @ 0x57c010]
		// under the terrain's own lines (env::load_mission_env).
		if (fog_reach_ <= 0.0f && header_.environment.empty()) {
			env::MissionEnv loaded;
			env::load_mission_env_config(*view.findings.assets, header_.terrain, header_.environment,
			                             env::BmsEnvOverrides(), loaded);
			const env::Config &defaults = loaded.config;
			const env::FogParams fog =
					env::compute_fog_params(defaults.fog_type, env::EnvScalarChannels::settled_fog_level(defaults.fog_level), 0.0f);
			if (fog.enabled && fog.end > 0.0f) fog_reach_ = fog.end * 0.5f;
		}
	}
	if (layout_moved_) {
		layout_moved_ = false;
		++layout_serial_;
	}
	// An edit of the document builds the terrain again (its file moved through the project's files the device
	// reads it from).
	const bool edited = terrain->identity() != terrain_identity_ || terrain->load_generation() != terrain_load_ ||
	                    terrain->revision() != terrain_revision_;
	terrain_identity_ = terrain->identity();
	terrain_load_ = terrain->load_generation();
	terrain_revision_ = terrain->revision();
	follow_ground_(view);
	const bool overlaid = follow_overlay_(view);
	// Framed once the ground is read; framed again where the fog it was framed in moved (the project's references
	// read after the first follow name the mission and its environment) while no one moved the camera since.
	const bool unmoved = framed_ && camera_.target.x == framing_.target.x && camera_.target.y == framing_.target.y &&
	                     camera_.target.z == framing_.target.z && camera_.yaw == framing_.yaw &&
	                     camera_.pitch == framing_.pitch && camera_.distance == framing_.distance;
	if ((!framed_ || (unmoved && reach != fog_reach_)) && (ground_.height_field() || !ground_.error().empty())) {
		framed_ = true;
		const ViewportState picture = size();
		camera_ = framed(picture.width, picture.height);
		framing_ = camera_;
		state_moved();
	}
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const PreviewFollow::Key key{ 0, layout_serial_ };
	const bool moved = input.change != ChangeClass::None || edited;
	switch (picture_.follow(key, moved, files, generation)) {
	case PreviewFollow::Found::Same:
		shown(*terrain);
		if (options_moved_ || overlaid) {
			options_moved_ = false;
			return ViewportAction::Update;
		}
		return ViewportAction::Keep;
	case PreviewFollow::Found::Files:
		options_moved_ = false;
		missing_.clear();
		return picture_.built(FileStamps());
	case PreviewFollow::Found::Anew: break;
	}
	picture_.show(key, generation);
	shown(*terrain);
	options_moved_ = false;
	missing_.clear();
	return picture_.built(FileStamps());
}

bool TerrainViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool TerrainViewport::check_(const io::JsonValue &json, std::string &error) const {
	TerrainViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, uses_, options, error)) return false;
	OrbitCamera camera = camera_;
	if (const JsonValue *member = json.get("camera"); member && !mission_camera_from_json(*member, camera, error))
		return false;
	return true;
}

void TerrainViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) {
		TerrainViewportOptions options = options_;
		if (read_options(*member, uses_, options, error)) {
			if (options.mission != options_.mission) layout_moved_ = true;
			if (options.foliage != options_.foliage || options.water != options_.water || options.overlay != options_.overlay)
				options_moved_ = true;
			options_ = options;
		}
	}
	if (const JsonValue *member = json.get("camera")) mission_camera_from_json(*member, camera_, error);
}

bool TerrainViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	bool moved = surface_ != report.surface;
	surface_ = report.surface;
	drawn_ = report.drawn;
	for (const std::string &name : report.missing)
		if (std::find(missing_.begin(), missing_.end(), name) == missing_.end()) {
			missing_.push_back(name);
			moved = true;
		}
	return moved;
}

std::unique_ptr<CanvasHalf> TerrainViewport::make_canvas() const {
	OrbitCanvasHooks hooks;
	hooks.camera = [](const ViewportModel &viewport) -> const OrbitCamera & {
		return static_cast<const TerrainViewport &>(viewport).camera();
	};
	hooks.framed = [](const ViewportModel &viewport, int width, int height) {
		return static_cast<const TerrainViewport &>(viewport).framed(width, height);
	};
	hooks.change = terrain_camera_change;
	return std::make_unique<OrbitCanvas>(hooks);
}

MissionGroundFacts TerrainViewport::ground_under(const ViewportContext &context, float x, float y) const {
	MissionGroundFacts none;
	if (reason_ != TerrainViewStatus::Ready || !context.device) return none;
	PreviewVec3 from, along;
	if (!camera_.ray(x, y, context.width, context.height, from, along)) return none;
	// The ray as a segment, from the eye as far as a pick reaches (the mission view's).
	const double length = std::sqrt(double(along.x) * along.x + double(along.y) * along.y + double(along.z) * along.z);
	if (!(length > 0.0)) return none;
	const double reach = kMissionPickReach / length;
	const PreviewVec3 far{ float(double(from.x) + double(along.x) * reach), float(double(from.y) + double(along.y) * reach),
		float(double(from.z) + double(along.z) * reach) };
	double start[3], end[3];
	preview_to_mission(from, start);
	preview_to_mission(far, end);
	double at[3] = { 0.0, 0.0, 0.0 };
	if (!context.device->surface_between(start, end, at)) return none;
	follow_ground_(context.input.view);
	// On the ground: its height there as the game's collision reads it.
	double height = 0.0;
	if (context.device->ground_at(at[0], at[1], height)) at[2] = height;
	return ground_.terrain_at(at[0], at[1], at[2]);
}

ViewportHit TerrainViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	if (reason_ != TerrainViewStatus::Ready) return out;
	out.ground = mission_ground_to_json(ground_under(context, x, y));
	return out;
}

bool TerrainViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "A terrain's picture has no handles: its keys are edited in its records.";
	return false;
}

bool TerrainViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in a terrain's picture: its camera is set with set_viewport.";
	return false;
}

bool TerrainViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &out, std::string &error) const {
	if (name == "frame") {
		out.request(request::set_viewport(path(), terrain_camera_change(framed(context.width, context.height))));
		return true;
	}
	if (name == "top") {
		OrbitCamera camera = camera_;
		mission_camera_top(camera);
		out.request(request::set_viewport(path(), terrain_camera_change(camera)));
		return true;
	}
	error = "A terrain's picture has no command \"" + name + "\" (frame, top).";
	return false;
}

io::JsonValue TerrainViewport::options_json() const {
	return terrain_options_to_json(options_);
}

io::JsonValue TerrainViewport::camera_json() const {
	return mission_camera_to_json(camera_);
}

io::JsonValue TerrainViewport::body_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_object();
	out.set("terrain", json_string(terrain_name_));
	// The mission whose environment, tile set and tiles it is drawn with (null: the engine's own environment).
	if (const TerrainMissionUse *use = mission()) {
		JsonValue drawn = JsonValue::make_object();
		drawn.set("path", json_string(use->mission));
		drawn.set("title", json_string(use->title));
		drawn.set("environment", json_string(use->environment));
		drawn.set("environment_file", json_string(use->environment_file));
		drawn.set("tile_set", json_string(use->tile_set));
		drawn.set("start_time", json_number(double(use->start_time)));
		out.set("mission", std::move(drawn));
	} else {
		out.set("mission", JsonValue::make_null());
	}
	out.set("missions", json_number(double(uses_.missions.size())));
	// The ground as the game reads it: the char map, the tiles, the foliage map, the water.
	JsonValue ground = JsonValue::make_object();
	ground.set("read", JsonValue::make_bool(ground_.terrain()));
	if (!ground_.error().empty()) ground.set("error", json_string(ground_.error()));
	ground.set("surface_map", json_string(ground_.surface_map()));
	ground.set("surface_side", json_number(double(ground_.surface_side())));
	ground.set("tiles", json_number(double(ground_.tiles())));
	ground.set("tiles_file", json_string(ground_.tiles_file()));
	ground.set("foliage_map", json_string(ground_.foliage_map()));
	ground.set("foliage_definitions", json_number(double(ground_.foliage_defs().size())));
	ground.set("water", JsonValue::make_bool(ground_.water()));
	ground.set("water_height", json_number(ground_.water_height()));
	if (const terrain::TerrainHeightField *field = ground_.height_field()) {
		JsonValue grid = JsonValue::make_object();
		grid.set("width", json_number(double(field->layout.sector_count)));
		grid.set("rows", json_number(double(field->layout.sector_rows)));
		JsonValue origin = JsonValue::make_array();
		origin.push(json_number(double(field->layout.origin_x)));
		origin.push(json_number(double(field->layout.origin_y)));
		grid.set("origin", std::move(origin));
		ground.set("grid", std::move(grid));
	}
	out.set("ground", std::move(ground));
	out.set("surface", JsonValue::make_bool(surface_));
	out.set("drawn", drawn_);
	out.set("fog_reach", json_number(double(fog_reach_)));
	// The ground overlay the options ask (DI-29): its legend and extent, null with none asked.
	out.set("overlay", options_.overlay == MissionGroundOverlay::None ? JsonValue::make_null() : mission_overlay_to_json(overlay_));
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : missing_) missing.push(json_string(name));
	out.set("missing", std::move(missing));
	return out;
}

io::JsonValue TerrainViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	for (size_t i = 0; i < uses_.missions.size(); ++i) {
		const TerrainMissionUse &use = uses_.missions[i];
		JsonValue item = JsonValue::make_object();
		item.set("index", json_number(double(i)));
		item.set("name", json_string(use.mission));
		item.set("kind", json_string("mission"));
		item.set("title", json_string(use.title));
		item.set("current", JsonValue::make_bool(use.mission == mission_path_));
		out.push(std::move(item));
	}
	return out;
}

io::JsonValue TerrainViewport::notes_json(const ViewportInput &) const {
	JsonValue notes = JsonValue::make_array();
	if (reason_ != TerrainViewStatus::Ready) return notes;
	const auto note = [&](const char *code, const std::string &message) {
		JsonValue row = JsonValue::make_object();
		row.set("code", json_string(code));
		row.set("message", json_string(message));
		notes.push(std::move(row));
	};
	if (!mission()) {
		if (options_.mission == kTerrainNeutral)
			note("terrain.neutral", "Under the engine's own environment, as a mission with no .env starts on it.");
		else
			note("terrain.no_mission", uses_.reading ? "Reading the project's references..."
			                                         : "No mission runs on it: drawn under the engine's own environment "
			                                           "(a mission's header names its terrain).");
	} else if (const TerrainMissionUse *use = mission(); use->environment_file.empty() && !use->environment.empty()) {
		note("terrain.no_environment", base_of(use->mission) + " names the environment " + use->environment +
		                                       ", which the project does not have: the engine's own environment.");
	}
	for (const std::string &name : missing_) {
		JsonValue row = JsonValue::make_object();
		row.set("code", json_string("file.missing"));
		row.set("name", json_string(name));
		row.set("message", json_string("The project has no " + name + ": import it to see it."));
		notes.push(std::move(row));
	}
	return notes;
}

} // namespace opennova::editor
