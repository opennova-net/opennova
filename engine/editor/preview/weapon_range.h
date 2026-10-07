#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/preview/viewport_follow.h>
#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/geom.h>
#include <runtime/world/player_weapon.h>

namespace opennova::world {
class World;
class LocalPlayer;
class CollisionWorld;
} // namespace opennova::world

namespace opennova::editor {

// A weapon fired in the editor as the game fires it (ADR 0046 DI-22): a range of its own, a world::World holding
// the game's local player with the weapon in hand, before a target of the surface the editor picks. Nothing here
// decides how a weapon behaves; every tick runs the game's own legs in the order its frame runs them:
//
// - the pending fire sounds' countdown [orig: Sound_TickPendingSlots @ 0x529310, the frame's head], then the rounds
//   in flight along the game's ballistics, each stop on the target playing the ammo's row for the face it struck
//   (world::RoundSim::tick: the CFAC face walk, material + 4 [orig: Projectile_HandleEntityImpact @ 0x4E9390];
//   the row's sound through the distance gate, its scar on the struck entity's ring [orig:
//   AmmoDef_ProcessImpactEffect @ 0x40A170 -> Impact_SpawnGlassEffectsOrScar @ 0x5CF1B0]) [orig:
//   Weapon_UpdateAllProjectiles @ 0x4EC020, from the entity update];
// - the local player's view tick (the scope's ease and its promotion) and the weapon pump: the input the
//   gestures give (the fire key's press and hold, the reload key, the scope toggle) through the game's gates,
//   the weapon's action state machine stepped (world::weapon_fsm_tick), the FP clip channel stepped as the game
//   steps it, each round spawned where the game spawns it (the shooter's eye along its aim, the weapon's
//   dispersion, its zero) [orig: WeaponAction_ProcessAllEntities @ 0x542690 -> WeaponAction_ProcessFrame
//   @ 0x540E60; WeaponAction_Fire @ 0x542BB0 -> RoundData_SpawnRound @ 0x4EC0D0] (world::LocalPlayer's
//   run_local_view_tick and pump_local_weapon);
// - what the game hands its presenter, drained: the weapon's presentation events (a clip started; an action's
//   begin leg, its soundset and its particle at its user point; the recoil row's direct effect, the casing; an
//   action's end leg, its soundsetend), the round's fire record, the impacts mapped through the ammo's effects
//   table, the sounds the fire-sound queue readies.
//
// The range is the shooter's mission frame: the eye facing +x (heading 0, level), z up, the target a wall `range`
// metres down +x, its face the material whose row the editor shows. Every place it hands out is the eye's (the eye
// at the origin), though its world stands the eye above the world's zero, which the game reads as the water plane
// where none is authored. The run is a function of the
// gestures alone: stepped on as the clock runs, run again from tick 0 when the clock steps back or what it runs
// with moves.
//
// The editor's own choices beside the game's: the target (the game's world holds the walls), the gestures (the
// keys), the view the shots are presented in (WeaponShotView), and the shooter standing still at the origin.

// What a gesture asks of the weapon: the keys the game's input dispatcher reads.
enum class WeaponGesture : uint8_t {
	Fire,    // the fire key pressed and let go: one press edge [orig: Player_RequestPrimaryFire @ 0x5414C0]
	Hold,    // the fire key pressed and held until a Release (an auto weapon's re-queue chain)
	Release, // the fire key let go
	Reload,  // the reload key [orig: input case 0xD3 @ 0x4E0420]
	Scope,   // the scope toggle [orig: input case 6 -> Player_ToggleWeaponScope @ 0x4DF0C0]
	Switch,  // the weapon put away and drawn again (SWITCHFROM, then SWITCHTO)
};
const char *weapon_gesture_token(WeaponGesture gesture); // "fire", "hold", "release", "reload", "scope", "switch"
bool weapon_gesture_of(const std::string &token, WeaponGesture &out);

// A gesture on a tick of the clock.
struct WeaponGestureAt {
	int32_t tick = 0;
	WeaponGesture gesture = WeaponGesture::Fire;
	bool operator==(const WeaponGestureAt &other) const { return tick == other.tick && gesture == other.gesture; }
	bool operator!=(const WeaponGestureAt &other) const { return !(*this == other); }
};

// How the shots show: in the shooter's own view (the local player presenting its own weapon: each action's legs
// at the first-person gun), or in another's (the gun in a soldier's hands): as a soldier's shot shows to the
// others (the round's fire record presented through the ammo, its ai_launch and ai_launcheffect, the round
// event's ammo arm), or as another player's (the weapon's FIRE and RECOIL rows played at the gun, the adm arm)
// [orig: NetPacket_DeserializeRoundEvent @ 0x42F270, the ammo arm @ 0x42F521, the adm arm @ 0x42F6CE;
// WeaponSlot_FireAndSpawnEffects @ 0x53F440].
enum class WeaponShotView : uint8_t { Own, Soldier, Player };
const char *weapon_shot_view_token(WeaponShotView view); // "own", "soldier", "player"

// The range's target: a wall `range` metres down the line of fire whose face is the surface an ammo's impact row
// is chosen by (`tag`, an effects-table tag, world::kImpactEffectTagNames: a struck face's material byte b plays
// row b + 4, so the face's byte is the tag less 4), or none (the rounds fly on until they age out).
struct WeaponRangeTarget {
	bool shown = true;
	int tag = 5; // dirt
	float range = 25.0f;
	bool operator==(const WeaponRangeTarget &other) const {
		return shown == other.shown && tag == other.tag && range == other.range;
	}
	bool operator!=(const WeaponRangeTarget &other) const { return !(*this == other); }
};
// The tags a target's face can play: an entity face's byte reaches every row from obj (4) past the null, move,
// player and zip rows.
inline constexpr int kWeaponRangeFirstTag = 4;
inline constexpr float kWeaponRangeNearest = 2.0f;
inline constexpr float kWeaponRangeFarthest = 500.0f;
// The wall's half-size, metres: big enough to take a burst's dispersion at the range's distances.
inline constexpr float kWeaponRangeTargetHalf = 2.0f;
// How long a run is followed: ten minutes of the game's ticks.
inline constexpr int32_t kWeaponRangeMostTicks = 62 * 600;

// What the range fires with: the project's files (the open documents standing in for theirs), the weapon table's
// file and the record fired, the view the shots show in, whether another's view is the other side's (the round's
// tracer style is the enemy's then, the spawn's select against the presenting client's team [orig:
// RoundData_SpawnRound @ 0x4EC740]), the target.
struct WeaponRangeSetup {
	std::shared_ptr<const FileSource> files;
	std::string catalog;
	std::string weapon;
	WeaponShotView view = WeaponShotView::Own;
	bool enemy = false;
	WeaponRangeTarget target;
};

// One thing the run did, on the clock's tick it did it. Positions and directions are the range's (the shooter's
// mission frame); the FP channel's clip, variant and ticks are the gun's pose on that tick (the begin and effect
// legs' user points ride it).
struct WeaponRangeEvent {
	enum class Kind : uint8_t {
		Clip,    // an action's clip started on the FP channel
		Begin,   // an action's active phase began: its soundset, its particle at its user point
		Effect,  // the recoil row's direct effect: its particle at its user point (the casing)
		End,     // an action's active phase finished: its soundsetend
		Fired,   // a round left the gun
		Dry,     // the empty click: a fire with the clip spent
		Reload,  // the clip refilled
		Launch,  // a shot as another sees it: its effect at the gun (the soldier's ai_launcheffect, the player's FIRE row)
		Impact,  // a round stopped: its row's effect at the stop
		Sound,   // a sound the fire-sound queue readied (an impact's, a shot heard by another), at its place
		Refused, // a gesture the game's gates refused (the reason in words)
	};
	int32_t tick = 0;
	Kind kind = Kind::Begin;
	std::string action;    // the action's suffix ("fire", "recoil", ...)
	std::string set;       // the sound set it plays ("" none)
	std::string effect;    // the particle it spawns ("" none)
	std::string point;     // the user point it spawns at ("" the place `at`)
	bool admitted = true;  // the particle shows (the begin leg's admission: world::local_fire_effect_admitted)
	world::Vec3 at;        // where (Launch, Impact, Sound, Fired)
	world::Vec3 direction; // the spawn's orientation (zero: none, the descriptor's +Y)
	int tag = -1;          // an impact's row
	int round = 0;         // the shot's number (from 1)
	bool tracer = false;   // a Fired round draws its tracer
	std::string clip;      // the FP channel at the tick
	int32_t clip_variant = 0;
	int32_t clip_ticks = 0;
	std::string words;
};
const char *weapon_range_event_token(WeaponRangeEvent::Kind kind);

// A tracer's trail on the tick the run stands at: its style (the ammo's tracer_type) and its points, oldest
// first, each with its width multiplier (world::TracerTrailPool's channel).
struct WeaponRangeTrail {
	int32_t style = 0;
	int32_t age = 0;
	std::vector<world::Vec3> points;
	std::vector<float> widths;
};

// A round in flight.
struct WeaponRangeRound {
	world::Vec3 at;
	bool tracer = false;
	int32_t item = 0; // the item it draws as (its tracer item's type id; 0 none)
};

class WeaponRange {
public:
	WeaponRange();
	~WeaponRange();
	WeaponRange(const WeaponRange &) = delete;
	WeaponRange &operator=(const WeaponRange &) = delete;

	// The range set up to fire `setup.weapon`: its tables read from the project's files as the game's mission
	// load reads them (weapon.def through the game's parser and its table build, the clips its actions play read
	// from its map; ammo.def; the round types linked), the weapon mounted as the game mounts a weapon by name.
	// True when anything it runs with moved (the run starts again from tick 0). False in `ready()` with
	// `why` when the weapon does not mount.
	bool configure(const WeaponRangeSetup &setup);
	// The gestures, in tick order: a change starts the run again from tick 0 where it reached a tick at or past
	// the first that changed.
	void set_gestures(std::vector<WeaponGestureAt> gestures);
	const std::vector<WeaponGestureAt> &gestures() const { return gestures_; }
	// The run to the clock's tick `tick` (bounded by kWeaponRangeMostTicks): stepped on from where it stands, or
	// run again from tick 0 when the clock is behind it.
	void run_to(int32_t tick);

	bool ready() const { return ready_; }
	const std::string &why() const { return why_; }
	int32_t tick() const { return tick_; }
	// Moves whenever the run moves (a step, a run again).
	uint64_t serial() const { return serial_; }
	// How many times the run started again from tick 0 (a test's measure).
	uint64_t runs() const { return runs_; }
	// The files the tables were read from, each with its stamp.
	const FileStamps &reads() const { return reads_; }

	// Everything the run did up to its tick, in order.
	const std::vector<WeaponRangeEvent> &events() const { return events_; }
	// The local player's weapon as the game holds it now: the action, the clip and the reserve, the FP channel.
	const world::LocalPlayerWeaponView &weapon_view() const { return view_; }
	// Whether the scope is up (the promoted byte every consumer keys on), and whether the frame shows the SIGHTS
	// card in the first-person view model's place (the game's view frame decides it: ADS settled on a weapon whose
	// flags select the card; the frame draws the card or the view model, never both [orig:
	// Render_ProcessMainSceneFrame @0x5CA299..0x5CA304]).
	bool scoped() const;
	bool card() const { return card_; }
	// The weapon's ammo ("" none) and its tracer rate.
	const std::string &ammo() const { return ammo_; }
	int shots() const { return shots_; }
	// The tracers' trails, the rounds in flight and the scars as they stand.
	std::vector<WeaponRangeTrail> trails() const;
	std::vector<WeaponRangeRound> rounds() const;
	// The scars as the game's ring cache holds them, compiled as its scar renderer compiles them (every ring,
	// no fog cull), in the range's frame.
	renderer::ScarDrawList scars() const;
	int scar_count() const;
	// The target's face, its corners in the range's frame (none when it is not shown), its material byte.
	bool target_corners(world::Vec3 out[4]) const;
	const WeaponRangeTarget &target() const { return setup_.target; }
	WeaponShotView shot_view() const { return setup_.view; }

private:
	void reset_();
	void step_();
	void drain_();
	void apply_gestures_(int32_t tick);

	WeaponRangeSetup setup_;
	bool ready_ = false;
	bool read_ = false; // the setup's files were read (whether the weapon mounts or not)
	std::string why_;
	FileStamps reads_;
	// The tables as the load built them, copied into each run's world (a run moves the clip rings' heads).
	struct Tables;
	std::unique_ptr<Tables> tables_;
	int weapon_index_ = -1;
	world::WeaponInstallData install_;
	std::string ammo_;

	std::vector<WeaponGestureAt> gestures_;
	std::unique_ptr<world::World> world_;
	std::unique_ptr<world::LocalPlayer> local_;
	std::unique_ptr<world::CollisionWorld> collision_;
	int32_t tick_ = 0;
	bool holding_ = false;
	int32_t switch_timer_ = 0; // the slot's holster timer before the pump (the completion's edge)
	bool draw_again_ = false;  // the holster finished: the draw is queued on the next tick
	bool card_ = false;
	int shots_ = 0;
	std::vector<WeaponRangeEvent> events_;
	world::LocalPlayerWeaponView view_;
	uint64_t serial_ = 0;
	uint64_t runs_ = 0;
};

} // namespace opennova::editor
