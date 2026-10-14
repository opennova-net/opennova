#include <editor/preview/mission_people.h>

#include <algorithm>
#include <map>

#include <base/io/fixed.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/mission_world.h>
#include <editor/preview/model_preview_rig.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/physics_class_table.h>
#include <runtime/world/pose_inputs.h>

namespace opennova::editor {

struct MissionPeople::PeopleWorld {
	std::unique_ptr<MissionWorld> mission;
	world::World::Snapshot inited; // after the inits, the bodies' AI with it (World::restore rewinds both)
	// What it was made from: the poses' people world's serial, their generation and their root motion.
	uint64_t serial = 0;
	size_t generation = 0;
	const void *motion = nullptr;
	std::map<NodeId, world::EntityHandle> bodies; // every person's, by row
	std::unordered_map<uint16_t, NodeId> rows;    // a body's row, by its handle
};

MissionPeople::MissionPeople() = default;
MissionPeople::~MissionPeople() = default;

void MissionPeople::clear() {
	people_.clear();
	rows_.clear();
	rings_ = world::AnimVariantRings();
	events_.clear();
	world_.reset();
	started_ = false;
	++serial_;
}

bool MissionPeople::in_world() const {
	return world_ != nullptr;
}

bool MissionPeople::start_world_(const MissionPoses &poses) {
	const uint64_t serial = poses.people_serial();
	if (serial == 0 || poses.motion() == nullptr) {
		world_.reset();
		return false;
	}
	if (world_ && world_->serial == serial && world_->generation == poses.generation() &&
			world_->motion == static_cast<const void *>(poses.motion())) {
		// The same world: back to its bodies as the inits left them.
		world_->mission->world.restore(world_->inited);
		world_->mission->world.logic_tick = 0;
		return true;
	}
	auto fresh = std::make_unique<PeopleWorld>();
	fresh->mission = poses.load_people_world(fresh->bodies);
	if (!fresh->mission) {
		world_.reset();
		return false;
	}
	fresh->serial = serial;
	fresh->generation = poses.generation();
	fresh->motion = poses.motion();
	world::World &w = fresh->mission->world;
	w.ai.root_motion = poses.motion();
	w.ai.anim_rings = world::AnimVariantRings();
	w.logic_tick = 0;
	// Each posed person's table as the poses registered it; then every pool-0 row's organic init in pool order, the
	// ring heads served as the game's load serves them [orig: Entity_SpawnFromBMSRecord @0x40E9F0 ->
	// Entity_InitOrganicAI @0x4BFCC0, in pool order; mission::MissionKernel::boot's organic_init].
	for (const MissionPose &pose : poses.poses()) {
		const auto found = fresh->bodies.find(pose.row);
		if (found == fresh->bodies.end()) continue;
		fresh->rows[found->second.packed] = pose.row;
		world::AiEntity *body = w.ai.for_handle(found->second);
		if (body != nullptr && pose.status == "posed" && pose.adm_id >= 0) body->inf.adm_id = pose.adm_id;
	}
	w.registry.for_each_in_pool(0, [&](const world::Entity &row) {
		world::initialize_organic_ai(w, *w.registry.get(row.handle));
	});
	w.ai.capture_spawn_baseline();
	fresh->inited = w.snapshot();
	world_ = std::move(fresh);
	return true;
}

void MissionPeople::reset(const MissionPoses &poses, int32_t tick) {
	people_.clear();
	rows_.clear();
	events_.clear();
	const bool in_world = start_world_(poses);
	for (const MissionPose &pose : poses.poses()) {
		if (pose.status != "posed" || pose.adm_id < 0) continue;
		// Only a body on the org1 physics row runs the head: an org0 body's update is a nullsub, its channels
		// standing as the warmup left them (world::infantry_org1_motor_head).
		if (world::physics_class_from_move_function(pose.definition.move_function) != world::PhysicsClass::Org1)
			continue;
		// A route's walker (a route or an order to walk, 123..125 included) stands at its spawn pose: the game walks it
		// along its route, which no think runs here; the reserved 126 and 127 stand (infantry_command_think).
		if (pose.route != 0 && pose.route != 126 && pose.route != 127) continue;
		Person person;
		person.row = pose.row;
		person.channels = pose.channels;
		person.pose = pose.pose;
		person.ssn = pose.ssn;
		person.alert = pose.alert;
		person.guarding = pose.guarding;
		person.airborne = pose.stood && !pose.settled;
		if (in_world) {
			const auto body = world_->bodies.find(pose.row);
			const world::AiEntity *ai = body != world_->bodies.end() ? world_->mission->world.ai.for_handle(body->second)
			                                                         : nullptr;
			const world::Entity *entity = ai ? world_->mission->world.registry.get(body->second) : nullptr;
			// A body the init hid (in another team's respawn zone) exits its update before the head [orig:
			// Entity_UpdateInfantryAI @0x4B9A03].
			if (ai == nullptr || entity == nullptr || ((entity->flags | entity->engine_flags) & 1u) != 0) continue;
			person.body = body->second;
			person.pose = world::infantry_body_pose(ai->inf);
		}
		rows_[pose.row] = people_.size();
		people_.push_back(std::move(person));
	}
	// The ring heads as the spawns left them, served on by the people's wraps as the game's one table is.
	rings_ = poses.rings();
	started_ = true;
	poses_serial_ = poses.serial();
	poses_generation_ = poses.generation();
	tick_ = tick;
	started_at_ = tick;
	++serial_;
}

bool MissionPeople::run_to(const MissionPoses &poses, int32_t tick) {
	const uint64_t people_world = world_ ? world_->serial : 0;
	if (!started_ || poses.serial() != poses_serial_ || poses.generation() != poses_generation_ ||
			poses.people_serial() != people_world || tick < tick_ || tick - tick_ > kCatchUpTicks) {
		reset(poses, tick);
		return true;
	}
	if (tick == tick_ || people_.empty()) {
		tick_ = tick;
		return false;
	}
	world::IRootMotionSource *motion = poses.motion();
	while (tick_ < tick) {
		++tick_;
		// The game's tick: its first frame runs at 1 (MissionKernel::boot), the clock's first tick past the start.
		const uint32_t game_tick = uint32_t(tick_ - started_at_);
		if (world_) {
			// Each played body's update in pool order, as the game's entity update runs pool 0 [orig:
			// Entity_UpdateAllEntities @0x4C2460..0x4C2472].
			world::World &w = world_->mission->world;
			w.logic_tick = game_tick;
			for (Person &person : people_) {
				world::AiEntity *body = w.ai.for_handle(person.body);
				if (body == nullptr) continue;
				const world::RootMotionFrame frame = w.ai.tick_still_body(*body, w, game_tick);
				const uint32_t word = body->inf.last_events;
				if (word != 0)
					events_.push_back(Event{ person.row, tick_, word, body->inf.clip_phase, frame.capsule_bottom });
			}
			continue;
		}
		for (Person &person : people_) {
			world::RootMotionFrame frame;
			world::infantry_org1_motor_head(person.channels, motion, rings_, frame);
			const uint32_t word = person.channels.last_events;
			if (word != 0)
				events_.push_back(Event{ person.row, tick_, word, person.channels.clip_phase, frame.capsule_bottom });
			// The think after the head, on its own 16-tick stagger, unless the spawn left the body in the air.
			if (!person.airborne)
				world::infantry_org1_still_think(
						person.channels, motion, game_tick, person.ssn, person.guarding, person.alert);
		}
	}
	for (Person &person : people_) {
		const world::InfantryState *played = channels(person.row);
		if (played) person.pose = world::infantry_body_pose(*played);
	}
	// The last kCatchUpTicks ticks' words kept.
	const int32_t oldest = tick_ - kCatchUpTicks;
	events_.erase(events_.begin(),
			std::find_if(events_.begin(), events_.end(), [oldest](const Event &event) { return event.tick > oldest; }));
	++serial_;
	return true;
}

const world::InfantryBodyPose *MissionPeople::pose(NodeId row) const {
	const auto found = rows_.find(row);
	return found == rows_.end() ? nullptr : &people_[found->second].pose;
}

const world::InfantryState *MissionPeople::channels(NodeId row) const {
	const auto found = rows_.find(row);
	if (found == rows_.end()) return nullptr;
	const Person &person = people_[found->second];
	if (!world_) return &person.channels;
	const world::AiEntity *body = world_->mission->world.ai.for_handle(person.body);
	return body ? &body->inf : nullptr;
}

bool MissionPeople::aim_overlay(NodeId row, anim::AimOverlayAngles out[anim::kOverlayClassCount]) const {
	if (!world_) return false;
	const auto found = world_->bodies.find(row);
	if (found == world_->bodies.end()) return false;
	const world::World &w = world_->mission->world;
	const world::AiEntity *body = w.ai.for_handle(found->second);
	const world::Entity *entity = w.registry.get(found->second);
	// A body the init hid draws nothing in the game [orig: Entity_UpdateInfantryAI @0x4B9A03].
	if (body == nullptr || entity == nullptr || ((entity->flags | entity->engine_flags) & 1u) != 0) return false;
	anim::compute_aim_overlay_angles(world::aim_overlay_inputs_for(*body, *entity), out);
	return true;
}

bool MissionPeople::look(NodeId row, Look &out) const {
	if (!world_) return false;
	const auto found = world_->bodies.find(row);
	if (found == world_->bodies.end()) return false;
	const world::AiEntity *body = world_->mission->world.ai.for_handle(found->second);
	if (body == nullptr) return false;
	out.body_yaw = world::mission_yaw_deg_from_bam_heading(body->inf.body_heading);
	out.look_yaw = world::mission_yaw_deg_from_bam_heading(body->heading);
	const auto target = world_->rows.find(body->inf.head_look_target.packed);
	out.target = body->inf.head_look_target.valid() && target != world_->rows.end() ? target->second : 0;
	return true;
}

io::JsonValue mission_body_json(const world::InfantryBodyPose &body, int32_t tick) {
	using io::JsonValue;
	using io::json_number;
	JsonValue out = JsonValue::make_object();
	out.set("tick", json_number(tick));
	JsonValue playing = JsonValue::make_object();
	playing.set("state", json_number(body.state));
	playing.set("row", io::json_string(world::infantry_anim_key(body.state)));
	playing.set("phase", json_number(body.phase));
	playing.set("variant", json_number(body.variant));
	playing.set("parked", JsonValue::make_bool(body.parked));
	out.set("playing", std::move(playing));
	if (body.blending) {
		JsonValue from = JsonValue::make_object();
		from.set("state", json_number(body.source_state));
		from.set("row", io::json_string(world::infantry_anim_key(body.source_state)));
		from.set("phase", json_number(body.source_phase));
		from.set("variant", json_number(body.source_variant));
		out.set("from", std::move(from));
		out.set("weight", json_number(body.weight));
	}
	return out;
}

std::vector<ClipSoundFired> mission_people_sounds(const MissionPeople &people, int32_t from, int32_t to,
		const std::function<bool(NodeId, MissionPersonHeard &)> &where, const ClipSoundSources &sources,
		const PreviewVec3 &listener, audio::SoundSelector &selector) {
	std::vector<ClipSoundFired> out;
	std::unordered_map<NodeId, MissionPersonHeard> heard;
	std::unordered_map<NodeId, bool> known;
	for (const MissionPeople::Event &event : people.events()) {
		if (event.tick <= from || event.tick > to) continue;
		auto seen = known.find(event.row);
		if (seen == known.end()) {
			MissionPersonHeard person;
			const bool found = where(event.row, person);
			if (found) heard[event.row] = std::move(person);
			seen = known.emplace(event.row, found).first;
		}
		if (!seen->second) continue;
		const MissionPersonHeard &person = heard[event.row];
		ClipSoundOptions options;
		options.surface = person.surface;
		const ClipSoundBinding binding = clip_sound_binding(sources.profiles(), options, person.item, PreviewRig());
		// The body's sound block reads its word on its own tick half.
		if (!world::anim_sound_tick(uint32_t(event.tick), binding.player)) continue;
		ClipEventDue due;
		due.tick = event.tick;
		due.clip_tick = event.phase;
		due.frame = event.phase;
		due.word = event.word;
		due.bottom = float(double(event.bottom) / io::kFp16OneD);
		// The body at the preview's origin as plan_clip_event hears it: the listener where it stands from the person.
		const PreviewVec3 from_person{ listener.x - person.origin.x, listener.y - person.origin.y,
			listener.z - person.origin.z };
		for (ClipSoundFired &fired : plan_clip_event(due, options, binding, sources, from_person, selector)) {
			// Its words name the person and the clip's tick rather than a frame of a previewed clip.
			const size_t colon = fired.words.find(": ");
			fired.words = person.title + ", tick " + std::to_string(event.phase) + " of its clip" +
			              (colon == std::string::npos ? std::string(": ") + fired.words : fired.words.substr(colon));
			out.push_back(std::move(fired));
		}
	}
	return out;
}

} // namespace opennova::editor
