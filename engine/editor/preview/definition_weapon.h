#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/definition_effects.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/preview_first_person.h>
#include <editor/preview/weapon_range.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/assets/asset_store.h>
#include <runtime/renderer/scar_draw_list.h>

namespace opennova::def {
struct DefWeaponDef;
}

namespace opennova::editor {

struct SessionView;

// How a weapon record fires in the definition preview (the definition options' `fire`): the first person's eye or
// the orbit camera, the character whose arms draw (DI-13's: -1 the one a fresh profile seeds), how another sees
// the third person's shots (WeaponShotView Soldier or Player), and the range's target.
struct DefinitionFireOptions {
	bool eye = true;
	int32_t character = -1;
	WeaponShotView shooter = WeaponShotView::Soldier;
	WeaponRangeTarget target;
	bool operator==(const DefinitionFireOptions &other) const {
		return eye == other.eye && character == other.character && shooter == other.shooter && target == other.target;
	}
	bool operator!=(const DefinitionFireOptions &other) const { return !(*this == other); }
};
// On the wire: {view ("eye", "orbit"), character, shooter ("soldier", "player"), surface (an effects-table tag's
// name, "obj" to "uwatersurface"), range (metres), target (shown)}.
io::JsonValue definition_fire_options_to_json(const DefinitionFireOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why.
bool read_definition_fire_options(const io::JsonValue &json, DefinitionFireOptions &held, std::string &error);

// The gun's first-person channel as the device poses the gun and the arms by it: the clip at its gated ticks, or
// the primary slerped toward the next ring entry by the weight while a loop wrap fades it in (world/player_weapon.h
// carries the witnesses).
struct DefinitionWeaponClip {
	std::string key;
	int32_t variant = 0;
	int32_t ticks = 0;
	bool blending = false;
	std::string blend_key;
	int32_t blend_variant = 0;
	int32_t blend_ticks = 0;
	float blend_weight = 0.0f;
};

// A tracer drawn: its style and its points, oldest first, in the preview's space, each with its width multiplier.
struct DefinitionTrail {
	int32_t style = 0;
	int32_t age = 0;
	std::vector<PreviewVec3> points;
	std::vector<float> widths;
};

// A weapon record's picture firing (ADR 0046 DI-22; preview/weapon_range): the gun the record names drawn as the
// game draws it, in the player's own view (its gfx1 with the arms of the character the player is, posed by the
// first-person channel the weapon's state machine steps, seen from the eye the game stands the view model
// before: DI-13's FirstPersonSources and first_person_eye) or in a soldier's hands (its gfx3), and fired in a range
// of its own as the game fires it (WeaponRange). Every gesture runs through the game's local player; what it
// hands its presenter is drawn here where the presenter draws it:
//
// - first person: an action's begin leg plays its soundset as the player hears their own weapon, and FIRE's
//   particle shows at its user point on the gun as the channel poses it (or the arms', the game's search over the
//   view model's parts; an unresolved point at the entity's origin) [orig: ActionSlot_SpawnEffect @ 0x401F20 ->
//   Entity_ComputeActionTransform @ 0x401310]; the recoil row's direct effect (the casing) at its point; an
//   action's end leg its soundsetend;
// - third person: each shot as another sees it, the soldier's ammo arm (its ai_launch, its ai_launcheffect at the
//   gun's launch point along its aim) or another player's adm arm (its FIRE and RECOIL rows' sets, FIRE's particle
//   at the gun's user point);
// - both: the round from the shooter along its aim, its tracer, its stop on the range's target playing the ammo's
//   row for the face (the effect at the stop, the sound heard from there, the scar on the face).
//
// The range's frame in the preview: in first person the eye (the shooter's eye: the round leaves it), its forward
// and up; in third person the gun's launch point (weapon.def `launchuserpoint` on the gfx3, else its origin)
// along its direction made level (else the gun's forward), up the preview's: drawn alone, the gun stands for the
// soldier, whose round leaves its eye.
class DefinitionWeapon {
public:
	DefinitionWeapon();
	~DefinitionWeapon();

	// The record `row` of the table at `catalog` (its project path), drawn by `gun` (`gun_file` the scan's file of
	// it; the gfx1 in first person, the gfx3 in third), `first` its view, `enemy` a third person's shots seen by the
	// other side: the first person's sources and rig, the eye on a picture `width` x `height` from `orbit`, the
	// range set up. True when what the device draws moved (the rig or the arms read again).
	bool refresh(const SessionView &view, const std::string &catalog, const def::DefWeaponDef &row, bool first,
			bool enemy, const DefinitionFireOptions &options, const assets::Model &gun, const std::string &gun_file,
			const OrbitCamera &orbit, int width, int height);
	// An ammo.def record fired alone (DI-23, WeaponRangeSetup's ammo): a soldier's shot of it from the picture's
	// origin along +Z at the range's target, its impact row played on the face. True when what the device draws
	// moved.
	bool refresh_ammo(const SessionView &view, const std::string &ammo, bool enemy,
			const DefinitionFireOptions &options);
	void clear();
	// Whether the range fires an ammo alone (no gun).
	bool ammo_alone() const { return active_ && range_.ammo_alone(); }

	// The gestures (a change runs the range again from where they differ) and the run to the clock's tick.
	void set_gestures(std::vector<WeaponGestureAt> gestures) { range_.set_gestures(std::move(gestures)); }
	void run_to(int32_t tick);
	const WeaponRange &range() const { return range_; }

	bool active() const { return active_; }
	bool first() const { return first_; }
	// The first person: DI-13's sources (the gun and the arms, the character, the team), the gun's rig (its map over
	// it; null: none loads), a serial that moves when either is read again, and the eye.
	const FirstPersonSources &first_person() const { return first_person_; }
	const std::shared_ptr<const anim::SkeletalClips> &rig() const { return rig_; }
	uint64_t rig_serial() const { return rig_serial_; }
	const FileStamps &rig_reads() const { return rig_read_; }
	bool eye() const { return eye_; }
	const OrbitCamera &eye_camera() const { return eye_camera_; }
	// The first person's SIGHTS card is up: the game draws the card, not the gun and the arms (WeaponRange::card).
	bool card_up() const { return first_ && range_.card(); }

	// What the picture shows now: the gun's channel, the effects the run spawned (each on its tick, in the preview's
	// space), the tracers, the scars and the target's corners.
	DefinitionWeaponClip clip() const;
	const std::vector<DefinitionSpawn> &spawns() const { return spawns_; }
	std::vector<DefinitionTrail> trails() const;
	renderer::ScarDrawList scars() const;
	bool target_corners(PreviewVec3 out[4]) const;
	// A point and a direction of the range in the preview's space.
	PreviewVec3 to_preview(const world::Vec3 &point) const;
	PreviewVec3 direction_to_preview(const world::Vec3 &direction) const;
	const std::vector<std::string> &notes() const { return notes_; }

	// The sounds the run made on the ticks [from, to) of the clock: an action's legs heard as the player hears
	// their own weapon, every other one where it plays, heard at `listener` (the preview camera); each set found
	// in the game's bank order through `sources` and its member picked through `selector`.
	std::vector<ClipSoundFired> sounds_between(int32_t from, int32_t to, const ClipSoundSources &sources,
			const PreviewVec3 &listener, audio::SoundSelector &selector) const;

	// The envelope's `weapon`: the view, the first person (DI-13's envelope), the range and its target, the weapon's
	// state, the gestures, the last events, the rounds, tracers and scars.
	io::JsonValue to_json(const DefinitionFireOptions &options) const;

private:
	void frame_(const def::DefWeaponDef &row, const OrbitCamera &orbit, int width, int height);
	void plan_spawns_();
	// The posed point of the user point `name` on the first-person gun (else the arms) at the channel `clip` /
	// `variant` / `ticks`, and its direction; false when no part carries it.
	bool first_person_point_(const std::string &name, const std::string &clip, int32_t variant, int32_t ticks,
			PreviewVec3 &at, PreviewVec3 &direction) const;
	// The unposed point of `name` on the drawn model (the third person's gun).
	bool model_point_(const std::string &name, PreviewVec3 &at, PreviewVec3 &direction) const;

	bool active_ = false;
	bool first_ = false;
	std::string record_;
	assets::Model gun_;
	std::string gun_file_;
	FirstPersonSources first_person_;
	PreviewRig rig_files_;
	std::shared_ptr<const anim::SkeletalClips> rig_;
	uint64_t rig_serial_ = 0;
	FileStamps rig_read_;
	bool eye_ = false;
	OrbitCamera eye_camera_;
	// The range's frame in the preview: its origin, its forward (+x), left (+y) and up (+z).
	PreviewVec3 origin_;
	PreviewVec3 forward_{0.0f, 0.0f, 1.0f};
	PreviewVec3 left_{1.0f, 0.0f, 0.0f};
	PreviewVec3 up_{0.0f, 1.0f, 0.0f};
	std::string frame_words_;
	WeaponRange range_;
	// The spawns placed so far: the run they were placed for (its runs()) and the events they cover.
	uint64_t planned_runs_ = UINT64_MAX;
	size_t planned_events_ = 0;
	std::vector<DefinitionSpawn> spawns_;
	std::vector<std::string> notes_;
};

} // namespace opennova::editor
