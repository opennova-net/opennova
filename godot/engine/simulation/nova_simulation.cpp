#include "simulation/nova_simulation.h"

#include <cmath>
#include <string>

#include <mission/bms.h>

using namespace godot;
using opennova::world::AiBrain;
using opennova::world::AiEntity;
using opennova::world::AiSystem;
using opennova::world::TickContext;
using opennova::world::World;

// mission yaw degrees <-> 32-bit binary angle. [orig: AI_HandleCommand cmd 0x16 @0x4659fa.]
static constexpr double kBamPerDegree = 11930464.0;

namespace {

// BAM (32-bit binary angle) -> radians.
constexpr double kRadPerBam = 6.283185307179586 / 4294967296.0;
constexpr double kFixed16 = 65536.0;

// Build the same synthetic patrol mission the C++ promote_test uses: 3 markers forming a
// path, one looping waypoint record (channel 1), 2 organics on that route, 1 building.
opennova::bms::File make_demo_mission() {
	opennova::bms::File m{};

	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		e.x = x; e.y = y; e.z = z;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x; e.y = y; e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};

	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));

	opennova::bms::WaypointRecord wr{};
	wr.flags = opennova::bms::WaypointFlags::None; // loops
	wr.marker_count = 3;
	wr.waypoint_numbers = {0, 1, 2};
	m.waypoint_records.push_back(wr);

	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));

	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);
	return m;
}

} // namespace

NovaSimulation::NovaSimulation() {
	world_ = std::make_unique<World>();
	ai_ = std::make_unique<AiSystem>();
	set_process(true);
}

void NovaSimulation::reset_world() {
	world_ = std::make_unique<World>();
	ai_ = std::make_unique<AiSystem>();
	promo_ = opennova::mission::PromoteResult{};
	loaded_ = false;
}

void NovaSimulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_mission_data", "mission"), &NovaSimulation::load_from_mission_data);
	ClassDB::bind_method(D_METHOD("load_mission_file", "path"), &NovaSimulation::load_mission_file);
	ClassDB::bind_method(D_METHOD("build_demo_mission"), &NovaSimulation::build_demo_mission);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSimulation::is_loaded);
	ClassDB::bind_method(D_METHOD("set_playing", "playing"), &NovaSimulation::set_playing);
	ClassDB::bind_method(D_METHOD("is_playing"), &NovaSimulation::is_playing);
	ClassDB::bind_method(D_METHOD("step"), &NovaSimulation::step);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &NovaSimulation::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entity_kind", "index"), &NovaSimulation::get_entity_kind);
	ClassDB::bind_method(D_METHOD("get_entity_index", "index"), &NovaSimulation::get_entity_index);
	ClassDB::bind_method(D_METHOD("get_entity_position", "index"), &NovaSimulation::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_yaw", "index"), &NovaSimulation::get_entity_yaw);
	ClassDB::bind_method(D_METHOD("get_entity_yaw_deg", "index"), &NovaSimulation::get_entity_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_entity_state", "index"), &NovaSimulation::get_entity_state);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &NovaSimulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &NovaSimulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &NovaSimulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &NovaSimulation::get_brain_count);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "playing"), "set_playing", "is_playing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}

void NovaSimulation::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		if (playing_ && loaded_) step();
	}
}

bool NovaSimulation::load_from_mission_data(const Ref<NovaMissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	reset_world();
	// The editor's live, in-memory mission (unsaved edits included).
	const opennova::bms::File &file = p_mission->native_document().bms_file();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_);
	loaded_ = true;
	return true;
}

bool NovaSimulation::load_mission_file(const String &path) {
	reset_world();
	opennova::bms::File file;
	std::string err;
	if (!opennova::bms::parse_file(std::string(path.utf8().get_data()), file, err)) {
		return false;
	}
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_);
	loaded_ = true;
	return true;
}

void NovaSimulation::build_demo_mission() {
	reset_world();
	opennova::bms::File file = make_demo_mission();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_);
	loaded_ = true;
}

void NovaSimulation::step() {
	if (!loaded_) return;
	TickContext ctx;
	ctx.world = world_.get();
	ctx.is_authority = true;
	ai_->tick(*world_, ctx); // decision + locomotion (apply_locomotion runs inside tick)
}

int NovaSimulation::get_entity_count() const {
	return ai_ ? ai_->count() : 0;
}

int NovaSimulation::get_entity_kind(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin >> 24); // [orig promote: (kind<<24)|index]
}

int NovaSimulation::get_entity_index(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin & 0xFFFFFF);
}

Vector3 NovaSimulation::get_entity_position(int p_index) const {
	if (!ai_) return Vector3();
	AiEntity *e = ai_->at(p_index);
	if (!e) return Vector3();
	// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
	return Vector3(static_cast<float>(e->pos[0] / kFixed16),
	               static_cast<float>(e->pos[2] / kFixed16),
	               static_cast<float>(-e->pos[1] / kFixed16));
}

float NovaSimulation::get_entity_yaw(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(static_cast<double>(e->heading) * kRadPerBam);
}

float NovaSimulation::get_entity_yaw_deg(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(static_cast<double>(e->heading) / kBamPerDegree);
}

int NovaSimulation::get_entity_state(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

void NovaSimulation::set_loco_scale(int p_scale) {
	if (ai_) ai_->loco_scale = p_scale;
}

int NovaSimulation::get_loco_scale() const {
	return ai_ ? ai_->loco_scale : 0;
}
