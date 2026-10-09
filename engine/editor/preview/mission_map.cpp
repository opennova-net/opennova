#include <editor/preview/mission_map.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <variant>

#include <base/io/fixed.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/preview/mission_map_canvas.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_source.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <runtime/hud/hud_map_view.h>
#include <runtime/hud/hud_math.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The record type of the commander map grid's origin marker (Map Centerpoint, items.def 102043).
constexpr int64_t kMapCenterpointType = 2043;

const Document *document_of(const ViewportInput &input) {
	return input.document ? records_of(*input.document) : nullptr;
}

const Selection *selection_of(const ViewportInput &input, const Document &document) {
	const DocumentsView &documents = input.view.documents;
	return documents.active == document.path() ? &documents.selection : nullptr;
}

std::string title_of(const SessionView &view, const Document &document, const NodeAddress &record) {
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return record_display(document, record, nullptr);
	const GraphNameSource names(*graph);
	return record_display(document, record, &names);
}

bool read_bool(const JsonValue &json, const char *member, bool &out, std::string &error) {
	const JsonValue *value = json.get(member);
	if (!value) return true;
	if (!value->is_bool()) {
		error = std::string(member) + " is true or false.";
		return false;
	}
	out = value->boolean;
	return true;
}

// The marks' pixel slack past the picture's edge: a pin half off it still shows.
constexpr float kOffPicture = 16.0f;
// The time a follow gives the models' outlines: the first picture's, then a frame's (ADR 0046 S14's budgets for the
// mission's device: a first picture within 12 ms, 4 ms a frame after).
constexpr int64_t kOutlinesFirstUs = 12000;
constexpr int64_t kOutlinesFrameUs = 4000;

} // namespace

// --- the camera and the view ---------------------------------------------------------------------

io::JsonValue mission_map_camera_to_json(const MissionMapCamera &camera, int width) {
	JsonValue out = JsonValue::make_object();
	JsonValue center = JsonValue::make_array();
	center.push(json_number(camera.center[0]));
	center.push(json_number(camera.center[1]));
	out.set("center", std::move(center));
	out.set("zoom", json_number(camera.zoom));
	out.set("scale", json_number(mission_map_view(camera, std::max(width, 1), 1).scale));
	return out;
}

bool mission_map_camera_from_json(const io::JsonValue &json, MissionMapCamera &camera, std::string &error) {
	if (!json.is_object()) {
		error = "camera is an object: {center?: [x, y], zoom?}.";
		return false;
	}
	MissionMapCamera out = camera;
	for (const auto &member : json.object) {
		if (member.key == "center") {
			const JsonValue &value = member.value;
			if (!value.is_array() || value.array.size() != 2 || !value.array[0].is_number() || !value.array[1].is_number() ||
					!std::isfinite(value.array[0].number) || !std::isfinite(value.array[1].number)) {
				error = "camera.center is [x, y], mission metres.";
				return false;
			}
			out.center[0] = value.array[0].number;
			out.center[1] = value.array[1].number;
		} else if (member.key == "zoom") {
			const JsonValue &value = member.value;
			if (!value.is_number() || !(value.number >= kMissionMapZoomMin) || !(value.number <= kMissionMapZoomMax)) {
				error = "camera.zoom is " + std::to_string(kMissionMapZoomMin).substr(0, 3) + " to " +
				        std::to_string(int(kMissionMapZoomMax)) + " (the commander map's zoom: metres across = zoom x 327.68).";
				return false;
			}
			out.zoom = float(value.number);
		} else if (member.key != "scale") {
			error = "camera takes center and zoom, not " + member.key + ".";
			return false;
		}
	}
	camera = out;
	return true;
}

MissionMapView mission_map_view(const MissionMapCamera &camera, int width, int height) {
	MissionMapView view;
	view.width = width;
	view.height = height;
	view.center[0] = camera.center[0];
	view.center[1] = camera.center[1];
	view.zoom = camera.zoom;
	if (width <= 0 || height <= 0) return view;
	// The CMAP's render over a payload rect that is the whole picture: the rect into the 1024 x 768 design space,
	// the zoom's scale over the scaled width x 200 [orig: CMapWindow_HandleEvent @0x549861..0x5498b3 ->
	// HUD_BuildMapOverlayView mode 4 @0x5a7f29..0x5a803c; hud::CommandMapView::render].
	hud::CommandMapView cmap;
	cmap.view.zoom = camera.zoom;
	hud::HudMinimapInput base;
	base.surface_w = float(width);
	base.surface_h = float(height);
	const int32_t scaled_800 = hud::map_view_design_to_device(800, float(width) / 800.0f);
	cmap.render(hud::MapViewRect{ 0, 0, width, height }, scaled_800, bms::to_fixed_16_16(camera.center[0]),
			bms::to_fixed_16_16(camera.center[1]), 0, base);
	view.scale = base.window_scale > 0.0f ? base.window_scale : 1.0f;
	// The compile's pixel rect: the design rect back onto the surface, its middle truncated [orig: HUD_DrawMapOverlay
	// @0x5a63b5..0x5a642e; hud_minimap.cpp make_view].
	const int x1 = int(hud::scale_axis(base.rect_x1, base.surface_w, hud::kDesignWidth));
	const int x2 = int(hud::scale_axis(base.rect_x2, base.surface_w, hud::kDesignWidth));
	const int y1 = int(hud::scale_axis(base.rect_y1, base.surface_h, hud::kDesignHeight));
	const int y2 = int(hud::scale_axis(base.rect_y2, base.surface_h, hud::kDesignHeight));
	view.middle_x = float(int((float(x1) + float(x2)) * 0.5f));
	view.middle_y = float(int((float(y1) + float(y2)) * 0.5f));
	return view;
}

void MissionMapView::project(double x, double y, float &px, float &py) const {
	// Mission 16.16 to the map's pixels, north up: +Y negated [orig: Terrain_FixedPointToWorldFloat @0x607060].
	const float inv = 1.0f / std::max(scale, 1e-6f);
	const int32_t dx = int32_t(uint32_t(bms::to_fixed_16_16(x)) - uint32_t(bms::to_fixed_16_16(center[0])));
	const int32_t dy = int32_t(uint32_t(bms::to_fixed_16_16(y)) - uint32_t(bms::to_fixed_16_16(center[1])));
	px = middle_x + float(dx) * inv / io::kFp16One;
	py = middle_y + float(dy) * inv * -io::kInvFp16One;
}

void MissionMapView::unproject(float px, float py, double &x, double &y) const {
	x = center[0] + double(px - middle_x) * double(scale);
	y = center[1] - double(py - middle_y) * double(scale);
}

float mission_map_zoom_for(double metres, int width) {
	(void)width;
	// Across the picture the map shows zoom x 65536 / 200 metres, whatever its width (the scale's law).
	return std::clamp(float(metres * 200.0 / 65536.0), kMissionMapZoomMin, kMissionMapZoomMax);
}

// --- the options -----------------------------------------------------------------------------------

bool MissionMapOptions::operator==(const MissionMapOptions &o) const {
	return items == o.items && buildings == o.buildings && markers == o.markers && organics == o.organics &&
	       areas == o.areas && paths == o.paths && labels == o.labels && grid == o.grid && text == o.text &&
	       stick == o.stick && snap == o.snap;
}

io::JsonValue mission_map_options_to_json(const MissionMapOptions &options) {
	JsonValue marks = JsonValue::make_object();
	marks.set("items", JsonValue::make_bool(options.items));
	marks.set("buildings", JsonValue::make_bool(options.buildings));
	marks.set("markers", JsonValue::make_bool(options.markers));
	marks.set("organics", JsonValue::make_bool(options.organics));
	marks.set("areas", JsonValue::make_bool(options.areas));
	marks.set("paths", JsonValue::make_bool(options.paths));
	marks.set("labels", JsonValue::make_bool(options.labels));
	JsonValue out = JsonValue::make_object();
	out.set("marks", std::move(marks));
	out.set("grid", JsonValue::make_bool(options.grid));
	out.set("text", JsonValue::make_bool(options.text));
	out.set("stick", JsonValue::make_bool(options.stick));
	out.set("snap", json_number(options.snap));
	return out;
}

bool mission_map_options_from_json(const io::JsonValue &json, MissionMapOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object: {marks?, grid?, text?, stick?, snap?}.";
		return false;
	}
	MissionMapOptions out = held;
	for (const auto &member : json.object) {
		const std::string &name = member.key;
		if (name == "marks") {
			if (!member.value.is_object()) {
				error = "options.marks is an object of the kinds shown.";
				return false;
			}
			for (const auto &mark : member.value.object) {
				bool *into = mark.key == "items" ? &out.items : mark.key == "buildings" ? &out.buildings
						: mark.key == "markers" ? &out.markers : mark.key == "organics" ? &out.organics
						: mark.key == "areas" ? &out.areas : mark.key == "paths" ? &out.paths
						: mark.key == "labels" ? &out.labels : nullptr;
				if (!into) {
					error = "options.marks takes items, buildings, markers, organics, areas, paths and labels, not " +
					        mark.key + ".";
					return false;
				}
				if (!mark.value.is_bool()) {
					error = "options.marks." + mark.key + " is true or false.";
					return false;
				}
				*into = mark.value.boolean;
			}
		} else if (name == "grid" || name == "text" || name == "stick") {
			bool &into = name == "grid" ? out.grid : name == "text" ? out.text : out.stick;
			if (!read_bool(json, name.c_str(), into, error)) return false;
		} else if (name == "snap") {
			if (!member.value.is_number() || !(member.value.number >= 0.0) || !(member.value.number <= 1000.0)) {
				error = "options.snap is metres, 0 (free) to 1000.";
				return false;
			}
			out.snap = float(member.value.number);
		} else {
			error = "options takes marks, grid, text, stick and snap, not " + name + ".";
			return false;
		}
	}
	held = out;
	return true;
}

// --- the marks -------------------------------------------------------------------------------------

uint32_t mission_map_rgb(MissionPool pool, int team) {
	if (team == 1 || team == 2) return mission_team_rgb(team);
	switch (pool) {
	case MissionPool::Item: return kMissionItemRgb;
	case MissionPool::Building: return kMissionBuildingRgb;
	case MissionPool::Marker: return kMissionMarkerRgb;
	case MissionPool::Organic: return kMissionOrganicRgb;
	}
	return kMissionItemRgb;
}

std::vector<MissionMapMark> mission_map_marks(const MissionScene &scene, const MissionMapOptions &options,
		const MissionMapView &view, const std::vector<MissionMapFootprint> *footprints) {
	std::vector<MissionMapMark> out;
	out.reserve(scene.entities().size() + scene.areas().size());
	const auto on_picture = [&](float px, float py, float reach) {
		const float margin = kOffPicture + reach;
		return px >= -margin && py >= -margin && px <= float(view.width) + margin && py <= float(view.height) + margin;
	};
	for (size_t i = 0; i < scene.entities().size(); ++i) {
		const MissionEntityMark &entity = scene.entities()[i];
		MissionMapMark mark;
		mark.record = NodeAddress{ entity.row, entity.kind, 0 };
		mark.kind = mission_pool_token(entity.pool);
		mark.pool = entity.pool;
		mark.team = entity.team;
		mark.x = entity.x;
		mark.y = entity.y;
		mark.entity = int(i);
		view.project(mark.x, mark.y, mark.px, mark.py);
		float reach = 0.0f;
		if (footprints && i < footprints->size() && (*footprints)[i].outline && !(*footprints)[i].hull.empty()) {
			const MissionMapFootprint &footprint = (*footprints)[i];
			mark.footprint = &footprint;
			reach = float((footprint.reach + std::hypot(footprint.centre[0] - entity.x, footprint.centre[1] - entity.y)) /
					double(view.scale));
			mark.outlined = float(footprint.reach / double(view.scale)) >= kMissionMapOutlinePx * 0.5f;
		}
		const bool kind_on = entity.pool == MissionPool::Item ? options.items
				: entity.pool == MissionPool::Building ? options.buildings
				: entity.pool == MissionPool::Marker ? options.markers : options.organics;
		mark.shown = kind_on && on_picture(mark.px, mark.py, reach);
		out.push_back(mark);
	}
	for (size_t i = 0; i < scene.areas().size(); ++i) {
		const MissionAreaMark &area = scene.areas()[i];
		MissionMapMark mark;
		mark.record = NodeAddress{ area.row, area.kind, 0 };
		mark.kind = "area";
		mark.x = (area.min[0] + area.max[0]) * 0.5;
		mark.y = (area.min[1] + area.max[1]) * 0.5;
		mark.area = int(i);
		view.project(mark.x, mark.y, mark.px, mark.py);
		mark.shown = options.areas && on_picture(mark.px, mark.py, 0.0f);
		out.push_back(mark);
	}
	return out;
}

int pick_mission_map_mark(const std::vector<MissionMapMark> &marks, const MissionMapView &view, float x, float y) {
	// A pin first: it is drawn over the wireframes.
	int best = -1;
	float best_distance = kMissionPickSlop * kMissionPickSlop;
	for (size_t i = 0; i < marks.size(); ++i) {
		const MissionMapMark &mark = marks[i];
		if (!mark.shown || mark.outlined) continue;
		const float dx = mark.px - x, dy = mark.py - y, distance = dx * dx + dy * dy;
		// The later drawn (an area over the entities, a later entity over an earlier) takes a tie.
		if (distance <= best_distance) {
			best_distance = distance;
			best = int(i);
		}
	}
	if (best >= 0) return best;
	// Then a wireframe: one whose model the point is on, else one whose footprint is within the slop; the smaller
	// footprint first.
	double mx = 0.0, my = 0.0;
	view.unproject(x, y, mx, my);
	const double slop = double(kMissionPickSlop) * double(view.scale);
	double best_score = INFINITY, best_area = INFINITY;
	for (size_t i = 0; i < marks.size(); ++i) {
		const MissionMapMark &mark = marks[i];
		if (!mark.shown || !mark.outlined) continue;
		const double distance = mark.footprint->distance(mx, my);
		if (distance > slop) continue;
		const double score = mark.footprint->covers(mx, my) ? 0.0 : distance + slop;
		if (score < best_score || (score == best_score && mark.footprint->area <= best_area)) {
			best_score = score;
			best_area = mark.footprint->area;
			best = int(i);
		}
	}
	return best;
}

std::vector<NodeAddress> mission_map_box_records(const std::vector<MissionMapMark> &marks, const MissionMapView &view,
		CanvasPoint a, CanvasPoint b) {
	const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
	// The box in the mission (north up: the picture's top its north edge).
	double west = 0.0, north = 0.0, east = 0.0, south = 0.0;
	view.unproject(x0, y0, west, north);
	view.unproject(x1, y1, east, south);
	std::vector<NodeAddress> out;
	for (const MissionMapMark &mark : marks) {
		if (!mark.shown) continue;
		const bool taken = mark.outlined ? mark.footprint->meets(west, south, east, north)
		                                 : mark.px >= x0 && mark.px <= x1 && mark.py >= y0 && mark.py <= y1;
		if (taken) out.push_back(mark.record);
	}
	return out;
}

int mission_map_route(const MissionScene &scene) {
	for (size_t i = 0; i < scene.paths().size(); ++i)
		if (scene.paths()[i].flags & uint32_t(bms::WaypointFlags::PlayerRoute)) return int(i);
	return -1;
}

// --- MissionMapViewport ----------------------------------------------------------------------------

MissionMapViewport::MissionMapViewport(std::string path) :
		ViewportModel(ViewportKind::Map, std::move(path), ViewportState{ 512, 512 }) {}

std::unique_ptr<ViewportModel> MissionMapViewport::make(const std::string &path) {
	return std::make_unique<MissionMapViewport>(path);
}

ViewportStatus MissionMapViewport::status() const {
	return reason_ == Reason::Ready ? ViewportStatus::Ready : ViewportStatus::Empty;
}

const char *MissionMapViewport::reason() const {
	switch (reason_) {
	case Reason::NoProject: return "no_project";
	case Reason::NoMission: return "no_mission";
	case Reason::Ready: break;
	}
	return "ready";
}

std::string MissionMapViewport::message() const {
	switch (reason_) {
	case Reason::NoProject: return "Open a project to see its missions.";
	case Reason::NoMission: return "Open a mission to see its map.";
	case Reason::Ready: break;
	}
	return std::string();
}

std::unique_ptr<CanvasHalf> MissionMapViewport::make_canvas() const {
	return std::make_unique<MissionMapCanvas>();
}

std::vector<MissionMapMark> MissionMapViewport::marks(int width, int height) const {
	if (reason_ != Reason::Ready) return {};
	return mission_map_marks(scene_, options_, view(width, height), &footprints_);
}

bool MissionMapViewport::pressed(const NodeAddress &record, MissionPressed &out) const {
	out = MissionPressed();
	out.record = record;
	if (const MissionEntityMark *entity = scene_.entity(record.row)) {
		out.x = entity->x;
		out.y = entity->y;
		out.z = entity->z;
		out.yaw = entity->yaw;
		return true;
	}
	if (const MissionAreaMark *area = scene_.area(record.row)) {
		out.area = true;
		for (int axis = 0; axis < 3; ++axis) {
			out.min[axis] = area->min[axis];
			out.max[axis] = area->max[axis];
		}
		out.x = (area->min[0] + area->max[0]) * 0.5;
		out.y = (area->min[1] + area->max[1]) * 0.5;
		out.z = area->min[2];
		return true;
	}
	return false;
}

std::vector<MissionPressed> MissionMapViewport::taken(const ViewportContext &context, const Document &document,
		const NodeAddress &record, size_t &grabbed) const {
	std::vector<MissionPressed> out;
	grabbed = 0;
	MissionPressed own;
	if (!pressed(record, own)) return out;
	const Selection *selection = selection_of(context.input, document);
	// A record not selected moves alone; a selected one takes the selection's entities and areas (the 3D view's move).
	if (!selection || !selection->holds(record)) {
		out.push_back(own);
		return out;
	}
	for (const NodeAddress &each : selection->records) {
		MissionPressed held;
		if (!pressed(each, held)) continue;
		if (each == record) grabbed = out.size();
		out.push_back(held);
	}
	return out;
}

const Document *MissionMapViewport::planned(const ViewportContext &context, std::string &error) const {
	const Document *document = document_of(context.input);
	if (reason_ != Reason::Ready || !document || !current(context.input)) {
		const std::string why = message();
		error = "The map shows no picture of the mission as it is now" + (why.empty() ? std::string(".") : ": " + why);
		return nullptr;
	}
	return document;
}

std::string MissionMapViewport::title(const ViewportContext &context, const NodeAddress &record) const {
	const Document *document = document_of(context.input);
	return document ? title_of(context.input.view, *document, record) : std::string();
}

MissionMapCamera MissionMapViewport::framed(const std::vector<NodeId> &of, int width, int height) const {
	MissionMapCamera out = camera_;
	double lo[2] = { 0.0, 0.0 }, hi[2] = { 0.0, 0.0 };
	bool any = false;
	const auto take = [&](double x, double y) {
		if (!any) {
			lo[0] = hi[0] = x;
			lo[1] = hi[1] = y;
			any = true;
			return;
		}
		lo[0] = std::min(lo[0], x);
		lo[1] = std::min(lo[1], y);
		hi[0] = std::max(hi[0], x);
		hi[1] = std::max(hi[1], y);
	};
	const auto named = [&](NodeId row) { return of.empty() || std::find(of.begin(), of.end(), row) != of.end(); };
	for (const MissionEntityMark &entity : scene_.entities())
		if (named(entity.row)) take(entity.x, entity.y);
	for (const MissionAreaMark &area : scene_.areas())
		if (named(area.row)) {
			take(area.min[0], area.min[1]);
			take(area.max[0], area.max[1]);
		}
	if (!any) return out;
	out.center[0] = (lo[0] + hi[0]) * 0.5;
	out.center[1] = (lo[1] + hi[1]) * 0.5;
	// The box with a margin, across the picture's narrower side (zoom is metres across its width).
	const double across = std::max(hi[0] - lo[0], (hi[1] - lo[1]) * (height > 0 ? double(width) / double(height) : 1.0));
	out.zoom = mission_map_zoom_for(std::max(across * 1.2, 60.0), width);
	return out;
}

std::string MissionMapViewport::camera_change(const MissionMapCamera &camera) const {
	JsonValue taken = JsonValue::make_object();
	JsonValue center = JsonValue::make_array();
	center.push(json_number(camera.center[0]));
	center.push(json_number(camera.center[1]));
	taken.set("center", std::move(center));
	taken.set("zoom", json_number(camera.zoom));
	return viewport_change(ViewportKind::Map, "camera", std::move(taken));
}

void MissionMapViewport::follow_ground_(const SessionView &view) {
	reader_.follow(view.findings.assets, view.findings.assets ? view.findings.assets->generation() : 0, scene_.header(),
			mission::mission_base_name(path()));
	MissionMapGround ground;
	ground.terrain = scene_.header().terrain;
	ground.water = reader_.water();
	ground.water_height = ground.water ? reader_.water_height() : 0.0;
	// The grid's origin: the first marker whose record's type is 2043 (the Map Centerpoint, items.def 102043), as
	// the game's HUD init scans its pool-3 entities [orig: HUD_InitOverlaySystem @0x5a4999, entity+80 == 2043; the
	// runtime's promote, mission::kItemIdOffset]. Every record stands in the editor: the game's scan is over the ones
	// its mode admits, which the map does not know.
	for (const MissionEntityMark &entity : scene_.entities()) {
		if (entity.pool != MissionPool::Marker || entity.item != mission::kItemIdOffset + kMapCenterpointType) continue;
		ground.grid_origin = true;
		ground.grid_x = entity.x;
		ground.grid_y = entity.y;
		break;
	}
	if (ground.terrain != ground_.terrain || ground.water != ground_.water || ground.water_height != ground_.water_height ||
			ground.grid_origin != ground_.grid_origin || ground.grid_x != ground_.grid_x || ground.grid_y != ground_.grid_y)
		moved_ = true;
	ground_ = std::move(ground);
}

bool MissionMapViewport::follow_outlines_(const SessionView &view, int64_t budget_us) {
	// The items the entities name, each once, in the scene's order.
	std::vector<int64_t> items;
	items.reserve(scene_.entities().size());
	{
		std::unordered_set<int64_t> named;
		for (const MissionEntityMark &entity : scene_.entities())
			if (named.insert(entity.item).second) items.push_back(entity.item);
	}
	const bool read = outlines_.step(view, items, budget_us);
	const bool kinds = options_.items != placed_options_.items || options_.buildings != placed_options_.buildings ||
	                   options_.markers != placed_options_.markers || options_.organics != placed_options_.organics;
	if (!read && !kinds && scene_.serial() == placed_scene_) return false;
	placed_scene_ = scene_.serial();
	placed_options_ = options_;
	// Each entity's model placed as it stands; the shown kinds' wireframes, every edge of each placed.
	footprints_.clear();
	footprints_.reserve(scene_.entities().size());
	lines_.clear();
	line_rgb_.clear();
	for (const MissionEntityMark &entity : scene_.entities()) {
		const MissionOutlineCache::Item *item = outlines_.item(entity.item);
		if (!item || !item->outline) {
			footprints_.emplace_back();
			continue;
		}
		footprints_.push_back(mission_map_footprint(item->outline, entity.x, entity.y, double(entity.pitch),
				double(entity.yaw), double(entity.roll), item->scale_q16));
		const bool kind_on = entity.pool == MissionPool::Item ? options_.items
				: entity.pool == MissionPool::Building ? options_.buildings
				: entity.pool == MissionPool::Marker ? options_.markers : options_.organics;
		if (!kind_on) continue;
		const MissionMapFootprint &footprint = footprints_.back();
		const uint32_t rgb = mission_map_rgb(entity.pool, entity.team);
		const std::vector<float> &edges = item->outline->edges;
		for (size_t i = 0; i + 5 < edges.size(); i += 6) {
			double ax, ay, bx, by;
			footprint.place(&edges[i], ax, ay);
			footprint.place(&edges[i + 3], bx, by);
			lines_.insert(lines_.end(), { float(ax), float(ay), float(bx), float(by) });
			line_rgb_.push_back(rgb);
		}
	}
	++outline_serial_;
	return true;
}

ViewportAction MissionMapViewport::stop_(Reason reason) {
	reason_ = reason;
	detail_.clear();
	scene_.clear();
	ground_ = MissionMapGround();
	surface_ = false;
	drawn_ = JsonValue();
	footprints_.clear();
	lines_.clear();
	line_rgb_.clear();
	placed_scene_ = UINT64_MAX;
	++outline_serial_;
	shown_none();
	return picture_.stop();
}

ViewportAction MissionMapViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) return stop_(Reason::NoProject);
	const Document *document = document_of(input);
	const std::unique_ptr<MissionSceneSource> source = document ? mission_scene_source(*document) : nullptr;
	if (!source) return stop_(Reason::NoMission);
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const PreviewFollow::Key key;
	reason_ = Reason::Ready;
	const auto *rows = input.change == ChangeClass::Changed && input.changes ? std::get_if<RowChanges>(input.changes)
	                                                                       : nullptr;
	const bool anew = !picture_.shows() || (input.change != ChangeClass::None && !rows);
	if (anew) {
		scene_.read(*source);
		follow_ground_(view);
		follow_outlines_(view, kOutlinesFirstUs);
		picture_.show(key, generation);
		shown(*document);
		if (!framed_) {
			// The one change its follow derives: the map over every entity and area.
			framed_ = true;
			const ViewportState picture = size();
			camera_ = framed({}, picture.width, picture.height);
			state_moved();
		}
		moved_ = false;
		return picture_.built(FileStamps());
	}
	MissionSceneDelta delta;
	if (rows) {
		delta = scene_.patch(*rows, *source);
		shown(*document);
	}
	const MissionSceneHeader before = scene_.header();
	follow_ground_(view);
	if (follow_outlines_(view, kOutlinesFrameUs)) moved_ = true;
	// A file the device read moved (the terrain, the HUD's layout and art): the picture made again from the files.
	if (picture_.follow(key, false, files, generation) == PreviewFollow::Found::Files) {
		moved_ = false;
		return picture_.built(FileStamps());
	}
	if (delta.header && scene_.header().terrain != ground_.terrain) {
		moved_ = false;
		return ViewportAction::Rebuild;
	}
	(void)before;
	// The pins are the canvas's: an entity moved, added or removed redraws them with no word to the device. What the
	// device draws (the camera, the CMAP toggles, the water, the grid's origin) moved: an Update.
	if (!moved_) return ViewportAction::Keep;
	moved_ = false;
	return ViewportAction::Update;
}

bool MissionMapViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool MissionMapViewport::check_(const io::JsonValue &json, std::string &error) const {
	MissionMapOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !mission_map_options_from_json(*member, options, error))
		return false;
	MissionMapCamera camera = camera_;
	if (const JsonValue *member = json.get("camera"); member && !mission_map_camera_from_json(*member, camera, error))
		return false;
	return true;
}

void MissionMapViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) {
		MissionMapOptions options = options_;
		if (mission_map_options_from_json(*member, options, error)) {
			// The CMAP's toggles are the device's; the marks, stick and the snap the canvas's alone.
			if (options.grid != options_.grid || options.text != options_.text) moved_ = true;
			options_ = options;
		}
	}
	if (const JsonValue *member = json.get("camera")) {
		MissionMapCamera camera = camera_;
		if (mission_map_camera_from_json(*member, camera, error) && camera != camera_) {
			camera_ = camera;
			framed_ = true;
			moved_ = true;
		}
	}
}

bool MissionMapViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	const bool moved = surface_ != report.surface;
	surface_ = report.surface;
	drawn_ = report.drawn;
	return moved;
}

ViewportHit MissionMapViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	const Document *document = document_of(context.input);
	if (reason_ != Reason::Ready || !document) return out;
	const std::vector<MissionMapMark> shown = marks(context.width, context.height);
	out.index = pick_mission_map_mark(shown, view(context.width, context.height), x, y);
	if (out.index < 0) return out;
	const MissionMapMark &mark = shown[size_t(out.index)];
	out.id = out.current ? mark.record.row : 0;
	out.name = title_of(context.input.view, *document, mark.record);
	out.kind = mark.kind;
	return out;
}

std::vector<ViewportHit> MissionMapViewport::box(const ViewportContext &context, float x0, float y0, float x1,
		float y1) const {
	std::vector<ViewportHit> out;
	const Document *document = document_of(context.input);
	if (reason_ != Reason::Ready || !document || !current(context.input)) return out;
	const std::vector<MissionMapMark> shown = marks(context.width, context.height);
	for (const NodeAddress &record :
			mission_map_box_records(shown, view(context.width, context.height), CanvasPoint{ x0, y0 }, CanvasPoint{ x1, y1 })) {
		const int index = scene_.mark_index(record.row);
		if (index < 0 || size_t(index) >= shown.size()) continue;
		ViewportHit hit;
		hit.current = true;
		hit.index = index;
		hit.id = record.row;
		hit.name = title_of(context.input.view, *document, record);
		hit.kind = shown[size_t(index)].kind;
		out.push_back(std::move(hit));
	}
	return out;
}

bool MissionMapViewport::click_frame(const ViewportContext &context, SelectMode, int &width, int &height,
		std::string &error) const {
	if (!planned(context, error)) return false;
	width = context.width;
	height = context.height;
	return true;
}

bool MissionMapViewport::handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x,
		float &y, std::string &error) const {
	if (handle != "move") {
		error = "The map moves a record (handle move); its height, heading and an area's edges are the 3D view's.";
		return false;
	}
	if (!planned(context, error)) return false;
	MissionPressed held;
	const Document *document = document_of(context.input);
	if (!pressed(document->address_of(id), held)) {
		error = "Record " + std::to_string(id) + " is no entity or area of the mission.";
		return false;
	}
	view(context.width, context.height).project(held.x, held.y, x, y);
	return true;
}

bool MissionMapViewport::drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
		std::string &error) const {
	float hx = 0.0f, hy = 0.0f;
	if (!handle_point(context, drag.id, drag.handle, hx, hy, error)) return false;
	const Document *document = planned(context, error);
	if (!document) return false;
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	if (!(drag.snap >= 0.0f)) {
		error = "The snap is 0 or more.";
		return false;
	}
	if (drag.by && drag.x == 0.0f && drag.y == 0.0f) {
		if (drag.end && drag.gesture) out.request(request::end_edit(document->path()));
		return true;
	}
	size_t grabbed = 0;
	const std::vector<MissionPressed> held = taken(context, *document, document->address_of(drag.id), grabbed);
	if (held.empty()) {
		error = "Record " + std::to_string(drag.id) + " is no entity or area of the mission.";
		return false;
	}
	// The record goes as far as the point under the pointer went from the one under its pin: the 3D view's move
	// (mission_move_edits), the selection with it, its height over the device's ground kept with stick.
	const MissionMapView shown = view(context.width, context.height);
	const float x = drag.by ? hx + drag.x : drag.x, y = drag.by ? hy + drag.y : drag.y;
	double to[2], from[2];
	shown.unproject(x, y, to[0], to[1]);
	shown.unproject(hx, hy, from[0], from[1]);
	const double target[2] = { held[grabbed].x + (to[0] - from[0]), held[grabbed].y + (to[1] - from[1]) };
	const uint64_t gesture = drag.gesture ? drag.gesture : next_edit_gesture();
	std::vector<Edit> edits;
	if (!mission_move_edits(*document, held, grabbed, target, drag.snap, options_.stick, context.device, gesture, edits)) {
		error = "The record cannot be moved.";
		return false;
	}
	const bool wrote = !edits.empty();
	if (wrote) out.request(request::edit_record(document->path(), std::move(edits)));
	if (drag.end && (wrote || drag.gesture)) out.request(request::end_edit(document->path()));
	return true;
}

bool MissionMapViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
		CanvasRequests &out, std::string &error) const {
	if (reason_ != Reason::Ready) {
		error = message();
		return false;
	}
	MissionMapCamera camera = camera_;
	if (name == "frame") {
		// The named records, else the selection, else everything.
		std::vector<NodeId> of = ids;
		if (of.empty())
			if (const Document *document = document_of(context.input))
				if (const Selection *selection = selection_of(context.input, *document))
					for (const NodeAddress &record : selection->records) of.push_back(record.row);
		camera = framed(of, context.width, context.height);
	} else if (name == "zoom_in" || name == "zoom_out") {
		// The CMAP's ZOOMIN and ZOOMOUT steps [orig: loc_548230 @0x548230; hud::MapViewPan::zoom_step], over the map's
		// own range.
		camera.zoom = std::clamp(camera.zoom * (name == "zoom_in" ? hud::kMapViewZoomInStep : hud::kMapViewZoomOutStep),
				kMissionMapZoomMin, kMissionMapZoomMax);
	} else {
		error = "The map's commands are frame, zoom_in and zoom_out.";
		return false;
	}
	out.request(request::set_viewport(path(), camera_change(camera)));
	return true;
}

io::JsonValue MissionMapViewport::options_json() const {
	return mission_map_options_to_json(options_);
}

io::JsonValue MissionMapViewport::camera_json() const {
	return mission_map_camera_to_json(camera_, size().width);
}

io::JsonValue MissionMapViewport::body_json(const ViewportInput &input) const {
	JsonValue out = JsonValue::make_object();
	if (reason_ != Reason::Ready) return out;
	const ViewportState picture = size();
	const MissionMapView shown = view(picture.width, picture.height);
	out.set("scale", json_number(shown.scale));
	out.set("across", json_number(double(shown.scale) * double(picture.width)));
	JsonValue ground = JsonValue::make_object();
	ground.set("terrain", json_string(ground_.terrain));
	ground.set("water", JsonValue::make_bool(ground_.water));
	if (ground_.water) ground.set("water_height", json_number(ground_.water_height));
	ground.set("grid_origin", ground_.grid_origin ? [&] {
		JsonValue at = JsonValue::make_array();
		at.push(json_number(ground_.grid_x));
		at.push(json_number(ground_.grid_y));
		return at;
	}() : JsonValue());
	out.set("ground", std::move(ground));
	const std::vector<MissionMapMark> all = marks(picture.width, picture.height);
	size_t count = 0;
	for (const MissionMapMark &mark : all) count += mark.shown ? 1 : 0;
	out.set("shown", json_number(double(count)));
	// The models seen from above: the wireframes drawn (entities, edges), the model files read, the items still to read.
	JsonValue outlines = JsonValue::make_object();
	size_t outlined = 0;
	for (const MissionMapMark &mark : all) outlined += mark.shown && mark.outlined ? 1 : 0;
	outlines.set("outlined", json_number(double(outlined)));
	outlines.set("edges", json_number(double(line_rgb_.size())));
	outlines.set("files_read", json_number(double(outlines_.files_read())));
	outlines.set("pending", json_number(double(outlines_.pending())));
	out.set("outlines", std::move(outlines));
	// What the device drew: the terrain read (the surface), the pass's terrain triangles, sprites and labels.
	out.set("surface", JsonValue::make_bool(surface_));
	out.set("drawn", drawn_);
	const int route = mission_map_route(scene_);
	out.set("route", route >= 0 ? json_number(scene_.paths()[size_t(route)].index) : JsonValue());
	// The selection the map rings (the document's, shared with the 3D view while the mission is active).
	JsonValue selected = JsonValue::make_array();
	if (const Document *document = document_of(input))
		if (const Selection *selection = selection_of(input, *document))
			for (const NodeAddress &record : selection->records)
				if (scene_.entity(record.row) || scene_.area(record.row)) selected.push(json_number(double(record.row)));
	out.set("selected", std::move(selected));
	return out;
}

io::JsonValue MissionMapViewport::items_json(const ViewportInput &input) const {
	JsonValue out = JsonValue::make_array();
	const Document *document = document_of(input);
	if (reason_ != Reason::Ready || !document) return out;
	const ViewportState picture = size();
	const Selection *selection = selection_of(input, *document);
	for (const MissionMapMark &mark : marks(picture.width, picture.height)) {
		if (!mark.shown) continue;
		JsonValue row = JsonValue::make_object();
		row.set("id", json_number(double(mark.record.row)));
		row.set("kind", json_string(mark.kind));
		row.set("selected", JsonValue::make_bool(selection && selection->holds(mark.record)));
		row.set("name", json_string(title_of(input.view, *document, mark.record)));
		JsonValue at = JsonValue::make_array();
		at.push(json_number(mark.x));
		at.push(json_number(mark.y));
		row.set("at", std::move(at));
		JsonValue screen = JsonValue::make_array();
		screen.push(json_number(mark.px));
		screen.push(json_number(mark.py));
		row.set("screen", std::move(screen));
		if (mark.entity >= 0) {
			row.set("team", json_number(mark.team));
			row.set("item", json_number(double(scene_.entities()[size_t(mark.entity)].item)));
			// Its model seen from above: drawn as its wireframe or as its pin, and its footprint's outline.
			if (mark.footprint) {
				row.set("outlined", JsonValue::make_bool(mark.outlined));
				JsonValue hull = JsonValue::make_array();
				for (size_t i = 0; i + 1 < mark.footprint->hull.size(); i += 2) {
					JsonValue point = JsonValue::make_array();
					point.push(json_number(mark.footprint->hull[i]));
					point.push(json_number(mark.footprint->hull[i + 1]));
					hull.push(std::move(point));
				}
				row.set("footprint", std::move(hull));
			}
		}
		out.push(std::move(row));
	}
	return out;
}

} // namespace opennova::editor
