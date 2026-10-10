#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/model/node.h>
#include <editor/preview/preview_clip_sounds.h>
#include <runtime/world/infantry.h>

namespace opennova::editor {

class MissionPoses;

// A mission's people playing their clips (ADR 0046 S23 C): each person its spawn poses (MissionPoses, DI-38) ticked
// on from its spawn as the game's org1 motor head ticks a body before its think (world::organic_body_tick: the
// primary's state copied into the secondary's request, then the AnimMap dual update), one tick a tick of the preview
// clock, from the tick of the clock it was posed at. Each tick's event word is what the body's sound block and its
// fire pass read that tick (the mission view hears the people's footsteps and foley with Listen on). The editor's
// choice beside the game's: the think does not run, so a person plays the state its spawn requested on and on (a
// route's walk in place, an idle's variants as its ring serves them), the clock's ticks standing in for the world's.
class MissionPeople {
public:
	// A jump of the clock longer than this many ticks (a seek, the Shell held up, a clock gone back) starts the people
	// again from their spawn at the clock rather than playing the jump through.
	static constexpr int32_t kCatchUpTicks = 62;

	// An event word a body's channel read on a tick: whose, on which of the clock's ticks, the word, the body's playhead
	// (its clip's tick) and the capsule's bottom under its origin (16.16, its footsteps' height below it).
	struct Event {
		NodeId row = 0;
		int32_t tick = 0;
		uint32_t word = 0;
		int32_t phase = 0;
		int32_t bottom = 0;
	};

	void clear();
	// Every posed person of `poses` at its spawn, the clock at `tick`.
	void reset(const MissionPoses &poses, int32_t tick);
	// The people ticked on to the clock's `tick`: started again where the poses moved or the clock jumped (back, or
	// past kCatchUpTicks); true when a body's pose moved.
	bool run_to(const MissionPoses &poses, int32_t tick);

	// A person's body as it plays now (null: no posed person of the row).
	const world::InfantryBodyPose *pose(NodeId row) const;
	// The event words of the last kCatchUpTicks ticks played, in tick order.
	const std::vector<Event> &events() const { return events_; }
	// The clock's tick the people stand at, the tick they were started at, how many play; a serial that moves with
	// every tick played and every start.
	int32_t tick() const { return tick_; }
	int32_t started_at() const { return started_at_; }
	size_t playing() const { return people_.size(); }
	uint64_t serial() const { return serial_; }

private:
	struct Person {
		NodeId row = 0;
		world::InfantryState channels;
		world::InfantryBodyPose pose;
	};
	std::vector<Person> people_;
	std::unordered_map<NodeId, size_t> rows_;
	world::AnimVariantRings rings_;
	std::vector<Event> events_;
	bool started_ = false;
	uint32_t poses_serial_ = 0;
	size_t poses_generation_ = 0;
	int32_t tick_ = 0;
	int32_t started_at_ = 0;
	uint64_t serial_ = 0;
};

// A body as it plays now on the wire (an organic's pose's `now`): {tick, playing {state, row, phase, variant,
// parked}, and while it blends from {state, row, phase, variant} and weight}.
io::JsonValue mission_body_json(const world::InfantryBodyPose &body, int32_t tick);

// What a person's sounds read of where it stands: its origin in the preview's space (its record lifted where its
// spawn stands it), the surface under its feet as the game reads it there (the water's under the mission's water
// plane, the snow's on surface 3, else the ground's), the item its record names (its sound profiles and the body its
// move_function runs) and its title (what the words name it by). False: none to hear.
struct MissionPersonHeard {
	PreviewVec3 origin;
	audio::FootSurface surface = audio::FootSurface::Ground;
	ClipSoundItem item;
	std::string title;
};

// The sounds the people's event words make on the clock's ticks (`from`, `to`], each as the game's body sound block
// plays it on the tick it reads (an NPC body's on odd ticks, a player body's on even: world::anim_sound_tick): its
// foley slots at its origin, its footsteps at its feet (the frame's capsule bottom under it) through the slot the
// surface under them picks, each slot its item's bound profile's, its set found in the game's bank order and played
// as a 3D one-shot heard at `listener` (the preview's space) [orig: org1 @0x4bf144..0x4bf2b0; Sound_Play3DPositional
// @0x527CB0]. `where` answers a person (false: not heard).
std::vector<ClipSoundFired> mission_people_sounds(const MissionPeople &people, int32_t from, int32_t to,
		const std::function<bool(NodeId, MissionPersonHeard &)> &where, const ClipSoundSources &sources,
		const PreviewVec3 &listener, audio::SoundSelector &selector);

} // namespace opennova::editor
