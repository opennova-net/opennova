#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/definition_effects.h>
#include <editor/preview/definition_weapon.h>
#include <editor/preview/effect_catalog.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/weapon_range.h>
#include <runtime/anim/clip_timeline.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/assets/asset_store.h>
#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/organic_fire.h>

namespace opennova::editor {

struct SessionView;

// A clip's fire events fire the item's ammo (ADR 0046 DI-24; CONTEXT.md "Clip fire"): as the model preview's clock
// runs a clip on a body that fires, each tick the body's fire block reads a fire bit fires what the game's NPC body
// fires there, through the engine's own rules, and the shot plays out in DI-22's weapon range as another sees a
// soldier's shot:
//
// - which body: an NPC's (the item's move_function org1, or the body chosen) has the fire block; a player's body
//   (org2) has none, its twin of the sound block reads no fire bit [orig: Entity_UpdateInfantryAI @0x4BF15C..
//   0x4BF4AD; Entity_UpdateInfantryPlayerBody @0x4B76E6..0x4B78A8]; the body is the clip sounds' (DI-04's binding);
// - when: the body's odd ticks, the gate its sound block shares [orig: @0x4BF144..0x4BF156], each reading the
//   word of the frame its channel stands on (anim::clip_trigger_at, DI-04's schedule);
// - what: the shots of one pass of the block (world::organic_fire_pass: 0x4 the closeattack ammo from its point,
//   0x10 the marker3 ammo from its point, 0x8 the easyrocket ammo and then a different advancedrocket from the
//   rocket point), each byte the item's as the organic init resolves it (world::resolve_organic_weapons over the
//   project's ammo.def and the rig's model: an ai_function of the person classes alone runs the init
//   [orig: g_EntityClassEventCallbackTable @0x813018 / @0x813030 -> Entity_InitOrganicAI @0x4BFCC0]); nothing
//   else gates it: no magazine, no weapon state [orig: @0x4BF42A..0x4BF45A];
// - where: the launch point on the person's own model as the clip poses it on that tick (the point's bone carried
//   from the rest to the pose), or the body's origin where the byte is zero, along the body's own heading and pitch
//   (the entity's triple, never the bone's) [orig: Entity_GetAttachmentWorldPosition @0x4B2670, the raw fallback
//   @0x4b2767..0x4b278e];
// - how it shows: the game's NPC fire entry in the range (WeaponRange's soldier shots [orig:
//   WeaponSlot_FireAndSpawnEffects @0x53F440]): the ammo's ai_launch heard through the distance gate, its
//   ai_launcheffect at the launch point along the aim (the presenter's ammo arm), the round's flight and tracer,
//   its stop on the range's target playing the ammo's row for the face (effect, sound, scar).
//
// The editor's choices beside the game's: the body stands level at the preview's origin facing the model's forward
// (the mission frame's +x, the preview's +z), the range's frame stood at the height of the item's first launch
// point so its target faces the gun; the target (DI-22's); the other side's view (the enemy's tracer style).
// The run is a function of the clock: a shot is fired on its tick as the clock runs, the range run again from
// tick 0 when the clock steps back or what the shots are made of moves.

// How a clip's fire events show (the model viewport's options' `fire`): the range's target and whether the side
// seeing the shots is the other one.
struct ClipFireOptions {
	WeaponRangeTarget target;
	bool enemy = false;
	bool operator==(const ClipFireOptions &other) const { return target == other.target && enemy == other.enemy; }
	bool operator!=(const ClipFireOptions &other) const { return !(*this == other); }
};
// On the wire: {target (shown), surface (an effects-table row's name), range (metres), enemy}.
io::JsonValue clip_fire_options_to_json(const ClipFireOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why.
bool read_clip_fire_options(const io::JsonValue &json, ClipFireOptions &held, std::string &error);

// One shot of a fire event as the block takes it: the clock's tick and the clip's own tick and frame there, the bit
// that asked for it, its ammo slot and byte (zero: the item names none, or a name ammo.def lacks: nothing fires)
// and the ammo's name, its launch slot and point ("" the body's origin), where it leaves and along what (the
// preview's space), its number among the run's shots (from 1; 0 for a shot of a pressed mark), and what it is
// in words.
struct ClipFireShot {
	int32_t tick = 0;
	int32_t clip_tick = 0;
	int frame = 0;
	uint32_t bit = 0;
	int ammo_slot = 0;
	uint8_t ammo = 0;
	std::string ammo_name;
	int launch_slot = 0;
	std::string point;
	PreviewVec3 at;
	PreviewVec3 direction{0.0f, 0.0f, 1.0f};
	int number = 0;
	std::string words;
};

class ClipFire {
public:
	ClipFire();
	~ClipFire();

	// What the clip fires with: the project (its files, ammo.def and weapon.def as the load reads them, its effect
	// catalog), the item pairing the clip (DI-04's sources), the body the clip sounds bind (a player's fires
	// nothing), the rig's model and rig, the clip playing (its key, variant, timeline and event words) and the
	// clock's period a repeated one-shot plays again at (0 none). Nothing fires while no clip plays.
	void refresh(const SessionView &view, const ClipSoundItem &item, const ClipSoundBinding &binding,
			const assets::Model &model, const std::shared_ptr<const anim::SkeletalClips> &rig, const std::string &key,
			int variant, const anim::ClipTimeline *timeline, const ClipSoundTrack &track, int32_t period,
			const ClipFireOptions &options);
	void clear();
	// The run to the clock's tick `tick`: every shot on the ticks up to it fired (the range stands after that tick,
	// as the game's frame shows what its tick left), the effects it spawned played to it.
	void run_to(int32_t tick);

	// Whether anything fires: the body reads fire bits, the clip carries one, and the item's bytes name an ammo.
	bool armed() const { return armed_; }
	// Why it fires or not, in words; the item's facts as read; its bytes as the init resolves them.
	const std::string &words() const { return words_; }
	const world::OrganicWeapons &weapons() const { return weapons_; }
	const WeaponRange &range() const { return range_; }
	// The shots planned so far, each on its tick (to the furthest tick the clock reached since what they are made of
	// last moved), the effects the run spawned and their scene.
	const std::vector<ClipFireShot> &shots() const { return shots_; }
	const DefinitionEffects &effects() const { return effects_; }
	// The tracers, the scars and the target's corners in the preview's space.
	std::vector<DefinitionTrail> trails() const;
	renderer::ScarDrawList scars() const;
	bool target_corners(PreviewVec3 out[4]) const;
	PreviewVec3 to_preview(const world::Vec3 &point) const;

	// What an event word fires, a line a shot, nothing fired (a timeline mark's hover): "an NPC fires its first
	// ammo: AMMO_X (ammo_closeattack) from MFlash01 (launchups_closeattack), GS_X heard, Effect_X at the point",
	// or why it fires nothing. Empty for a word with no fire bit.
	std::vector<std::string> event_words(uint32_t word) const;
	// The shots the word at `frame` fires as the body reads it on the clock's `tick` (a pressed mark: the clip's own
	// tick `clip_tick`), each its ammo's ai_launch planned once as the camera at `listener` hears it from the launch
	// point (a press fires it once); none for a word with no fire bit or a body that fires nothing.
	std::vector<ClipFireShot> shots_of(uint32_t word, int32_t tick, int32_t clip_tick, int frame) const;
	std::vector<ClipSoundFired> press(uint32_t word, int32_t clip_tick, int frame, const ClipSoundSources &sources,
			const PreviewVec3 &listener, audio::SoundSelector &selector) const;
	// The sounds the run made on the ticks (from, to] of the clock, each where it plays heard at `listener` (a
	// 3D one-shot at its distance [orig: Sound_Play3DPositional @0x527CB0]).
	std::vector<ClipSoundFired> sounds_between(int32_t from, int32_t to, const ClipSoundSources &sources,
			const PreviewVec3 &listener, audio::SoundSelector &selector) const;

	// The envelope's `fire`: the body and whether it reads, the item's slots (each ammo's name, byte and launch
	// point), the words, the range and its target, the last shots and events, the rounds, tracers and scars.
	io::JsonValue to_json(const ClipFireOptions &options) const;

private:
	void schedule_to_(int32_t tick);
	void plan_spawns_();
	// The posed launch point of slot `launch_slot` at the clip's `clip_tick` (the body's origin where it is zero).
	PreviewVec3 launch_point_(int launch_slot, int32_t clip_tick, std::string &name) const;
	world::Vec3 to_range_(const PreviewVec3 &point) const;
	std::string ammo_name_(uint8_t byte) const;

	bool active_ = false;
	bool armed_ = false;
	bool reads_ = false; // the body reads fire bits (an NPC's)
	std::string words_;
	ClipSoundItem item_;
	world::OrganicWeapons weapons_;
	assets::Model model_;
	std::shared_ptr<const anim::SkeletalClips> rig_;
	std::string key_;
	int variant_ = 0;
	anim::ClipTimeline timeline_;
	bool has_clip_ = false;
	ClipSoundTrack track_;
	int32_t period_ = 0;
	float height_ = 0.0f; // the range's frame over the preview's origin (metres, the preview's +y)
	uint64_t inputs_ = 0; // what the shots are made of, hashed: another starts the schedule again
	std::vector<ClipFireShot> shots_;
	int32_t scheduled_to_ = 0; // the clock's last tick the schedule covers
	WeaponRange range_;
	PreviewEffectCatalog catalog_;
	DefinitionEffects effects_;
	uint64_t planned_runs_ = UINT64_MAX;
	size_t planned_events_ = 0;
	std::vector<DefinitionSpawn> spawns_;
};

} // namespace opennova::editor
