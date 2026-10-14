#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <base/io/tick_rate.h>
#include <editor/model/node.h>
#include <editor/preview/preview_clip_sounds.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/world/infantry.h>

namespace opennova::editor {

class MissionPoses;

// A mission's people playing their clips (ADR 0046 S23 C): each person its spawn poses (MissionPoses, DI-38) whose
// item's move_function binds the org1 physics row ticked on from its spawn, one tick a tick of the preview clock, from
// the tick of the clock it was posed at, the first the game's tick 1; another row's body (org0, whose update is a
// nullsub) stands as its init left it. Each tick's event word is what the body's sound block and its fire pass read
// that tick (the mission view hears the people's footsteps and foley with Listen on).
// - Where the mission's floors' world stands (MissionPoses::floor_serial: a mission and its terrain read), the people
//   play in a world of their own, the same records loaded again over the same files and ground (S27): every posed
//   person's body there initialized by the game's own organic init in pool order over the poses' tables
//   (world::initialize_organic_ai), and each played body ticked in pool order by the game's tick of a still body
//   (world::AiSystem::tick_still_body: the motor head; on its think ticks the think of a still body, an alert_state's
//   person turning from its idle to idle_2 at its first, a guard holding guard, and the think's attention tail, which
//   looks at a neighbour within its scan (the look idles 125 / 126 / 141 where the .adm authors them, the head look),
//   turns an idle_2 person away from a teammate close by and alerts on an enemy or a corpse it sees; the heading
//   chase; the eye restamp) [orig: Entity_UpdateInfantryAI @0x4B9910]. The device draws each body's aim overlay
//   (aim_overlay) as the game's presenter does.
// - Else the motor head (world::infantry_org1_motor_head) and the still body's think (world::infantry_org1_still_think)
//   alone, over channels of the people's own, the think skipped for a person its spawn left in the air over a ground
//   read (MissionPose::stood, unsettled: airborne, the game's think exits [orig: @0x4BAA61..0x4BAA66]); a person the
//   init hides (another team's respawn zone [orig: @0x4B9A03]) is known only in the world, whose zones it reads.
// The editor's choice beside the game's: what the still body's tick does not run (the perception's targets and the
// combat that follows, an enemy within the scan alerting a person the game's perception would have engaged; the root
// integrate and the ground solve, a person standing where its spawn stood it; the picture has no simulation, DI-31),
// the clock's ticks standing in for the world's; a route's walker, whom the game walks along its route, stands frozen at
// its spawn pose rather than walking in place (the maintainer's S26 ruling), its body in the world only for the
// others to look at; a person on route 127 (follow the local player) is a still body only because the view has no
// local player to follow (AiSystem::infantry_command_think's 127 leg), as its think would walk it to the player.
class MissionPeople {
public:
	// A jump of the clock longer than this many ticks (a seek, the Shell held up, a clock gone back) starts the people
	// again from their spawn at the clock rather than playing the jump through.
	static constexpr int32_t kCatchUpTicks = io::kTicksPerSecondInt;

	MissionPeople();
	~MissionPeople();
	MissionPeople(const MissionPeople &) = delete;
	MissionPeople &operator=(const MissionPeople &) = delete;

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
	// Its channels as they play now (null: no posed person of the row), what the Shoot tool's world poses its rig by.
	const world::InfantryState *channels(NodeId row) const;
	// Whether the people play in a world of their own (the floors' world loaded again for them).
	bool in_world() const;
	// A posed person's body in that world as the game's presenter draws it: its aim overlay's nine classes, the body's
	// own the model's root (world::aim_overlay_inputs_for, anim::compute_aim_overlay_angles) [orig:
	// Entity_BuildBoneTransformMatrices @0x4B1290]. False: no world, or no body of the row.
	bool aim_overlay(NodeId row, anim::AimOverlayAngles out[anim::kOverlayClassCount]) const;
	// Where it faces and looks (in the world, false without): the body's yaw and the look's (the entity's), whole
	// mission degrees, and the row it looks at (0: none).
	struct Look {
		double body_yaw = 0.0;
		double look_yaw = 0.0;
		NodeId target = 0;
	};
	bool look(NodeId row, Look &out) const;
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
		world::InfantryState channels; // without a world
		world::InfantryBodyPose pose;
		// What its think reads (MissionPose's): its SSN, its AI slot's alert byte, whether it guards.
		int ssn = 0;
		uint8_t alert = 0;
		bool guarding = false;
		bool airborne = false;    // without a world: left in the air over a ground read (no think)
		world::EntityHandle body; // in the world
	};
	struct PeopleWorld; // the people's world, its load as it stood before the inits, its bodies by row
	bool start_world_(const MissionPoses &poses);
	std::unique_ptr<PeopleWorld> world_;
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
