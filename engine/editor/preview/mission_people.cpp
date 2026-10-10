#include <editor/preview/mission_people.h>

#include <algorithm>

#include <editor/preview/mission_poses.h>
#include <editor/preview/model_preview_rig.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/world/entity_spawn.h>

namespace opennova::editor {

void MissionPeople::clear() {
	people_.clear();
	rows_.clear();
	rings_ = world::AnimVariantRings();
	events_.clear();
	started_ = false;
	++serial_;
}

void MissionPeople::reset(const MissionPoses &poses, int32_t tick) {
	people_.clear();
	rows_.clear();
	events_.clear();
	for (const MissionPose &pose : poses.poses()) {
		if (pose.status != "posed" || pose.adm_id < 0) continue;
		Person person;
		person.row = pose.row;
		person.channels = pose.channels;
		person.pose = pose.pose;
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
	if (!started_ || poses.serial() != poses_serial_ || poses.generation() != poses_generation_ || tick < tick_ ||
			tick - tick_ > kCatchUpTicks) {
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
		for (Person &person : people_) {
			world::RootMotionFrame frame;
			const uint32_t word = world::organic_body_tick(person.channels, motion, rings_, frame);
			if (word != 0)
				events_.push_back(Event{ person.row, tick_, word, person.channels.clip_phase, frame.capsule_bottom });
		}
	}
	for (Person &person : people_) person.pose = world::infantry_body_pose(person.channels);
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
		due.bottom = float(double(event.bottom) / 65536.0);
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
