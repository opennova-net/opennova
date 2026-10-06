#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/assets/asset_registry.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/viewport_follow.h>
#include <runtime/assets/asset_store.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/weapon_fsm.h>

namespace opennova::editor {

// A weapon's first-person view in the model preview (DI-13; ADR 0046, "The first-person view"): a map a
// weapon names as its animadm plays on the gun with the arms on it, as the game draws them, and a row an
// action of the weapon plays sounds the action's sets at the game's ticks. Every rule is the engine's own:
//
// - The models: the gun is the weapon's gfx1, the arms the arms graphic of the character the player is,
//   drawn with the gun's bone matrices; an emplaced weapon draws no arms, nor does a character with none
//   (renderer::fp_viewmodel_spec) [orig: Player_RenderFirstPersonViewModel @0x4ded60, the arms submit with
//   the gun's array @0x4df088]. The character is the one the editor chooses among Avatars.def's (the
//   game's registry, inmatch::CharacterRegistry), else the one a fresh profile seeds (its first of the
//   good side) [orig: EntitySlot_LookupAndPackEntry @0x57ad40].
// - The eye: the camera the game draws the view model before, the model at the weapon.def `pos` at the
//   hip, turned by its cant and the rig's axis map (renderer::fp_viewmodel_pose, the game presenter's own
//   placement), seen through the weapon's renderfov with the view model pass's near plane.
// - The actions: the weapon's ACTION rows bound and baked as the game's weapon table bakes them
//   (world::build_weapon_table over the project's files: each clip's length from its map, the delays an
//   `auto` row reads from the clips), then each run through the game's own pump (world::weapon_fsm_tick)
//   from the slot entering it: the tick its active phase begins plays its `soundset` and starts its clip
//   [orig: ActionSlot_ExecuteActionWithEffect @0x541860], the tick it finishes plays its `soundsetend`
//   [orig: ActionSlot_FinishActivePhase @0x53f7b0], and the clip steps one tick a tick while its counter
//   runs [orig: ActionSlot_ExecuteActionNoEffect @0x541a4d]. A first-person clip's own events are read by
//   nothing: the body's sound block reads its body channel alone.
//
// The seam DI-22 steps on: the baked actions (FirstPersonSources::fsm) and the pump's run of one
// (weapon_action_run), which a weapon firing in the editor drives tick by tick.

// The first-person view's options (the model viewport's options' `first_person`): the eye or the orbit
// camera, the character whose arms draw (a packed character id, -1 the one a fresh profile seeds), and the
// action whose sets a row plays ("" the first naming the row's slot, a suffix: "fire", "reload").
struct FirstPersonOptions {
	bool eye = false;
	int32_t character = -1;
	std::string action;
	bool operator==(const FirstPersonOptions &other) const {
		return eye == other.eye && character == other.character && action == other.action;
	}
	bool operator!=(const FirstPersonOptions &other) const { return !(*this == other); }
};
// On the wire: {view ("orbit", "eye"), character (an id, -1 the seeded one), action (a suffix, "" the first)}.
io::JsonValue first_person_options_to_json(const FirstPersonOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why.
bool read_first_person_options(const io::JsonValue &json, FirstPersonOptions &held, std::string &error);

// A character the player can be, as Avatars.def's registry flattens them: its packed id, its names in
// words, its side, and its arms (the arms part's graphic, "" a combo with none, and its camo triplet).
struct FirstPersonCharacter {
	uint16_t id = 0;
	int alignment = 0; // 0 good, 1 evil
	std::string words; // "AV_ON_NATION, AV_ON_DIVISION, combo 1"
	std::string arms;  // the arms graphic as authored
	int camo[3] = {0, 0, 0};
};

// A set an action plays: as its active phase begins (its row's soundset) or as it finishes (its
// soundsetend), and the tick of the action's clip it plays on (-1 never: the action never finishes).
struct WeaponActionLeg {
	bool end = false;
	std::string set;
	int32_t tick = -1;
	std::string words;
};

// An action of the weapon as the game's pump runs it, from the slot entering it until it finishes or
// leaves: its suffix, the clip its row plays and the handler it runs, its baked delays, the tick its
// active phase begins (its clip starts there) and the ticks after that it finishes (-1 never), how many
// ticks the channel steps its clip before another clip plays or the action ends, the action it hands to,
// and its legs, each at its tick of the clip.
struct WeaponActionRun {
	int32_t action = -1;
	std::string suffix;
	std::string anim_key;
	std::string handler;
	int32_t delay_start = 0;
	int32_t delay_end = 0;
	bool begins = false;   // its active phase begins (a handler whose entry plays nothing, the idle's, never)
	int32_t finish = -1;   // ticks from its begin to its finish (-1 never)
	int32_t stepped = 0;   // the clip's ticks the channel steps while it runs
	std::string next;      // the suffix it hands to ("" it never leaves)
	std::vector<WeaponActionLeg> legs;
	std::string words;
};
// The run of `action` (world::weapon_action) over `def`: the slot standing done on another action with
// this one queued, a full clip and rounds to spare, the owner the local player and the authority, then the
// game's pump ticked until the action finishes, leaves, or a minute passes.
WeaponActionRun weapon_action_run(const world::WeaponFsmDef &def, int32_t action);

// The project's files a weapon's first-person view reads, as the game reads them by name (an open
// document standing in for its file): the weapon from its catalog, Avatars.def's characters, the actions
// baked over the weapon's map, and the arms model; each read again only as its stamps move.
class FirstPersonSources {
public:
	// True when anything it holds moved (the arms model's serial moves when the arms were read again).
	// `gun_parts`: the parts of the gun the rig plays on, its bone array (0 unknown).
	bool refresh(const std::shared_ptr<const FileSource> &files, const AssetScan *scan, const PreviewRig &rig,
	             const FirstPersonOptions &options, size_t gun_parts);
	void clear();

	// The rig is a weapon's map (its animadm, paired with its gfx1) and the catalog holds the weapon.
	bool active() const { return active_; }
	const std::string &weapon() const { return weapon_; }
	// What the view is in words: the gun and the arms, or why a map shows no first person.
	const std::string &words() const { return words_; }
	const renderer::FpViewmodelSpec &spec() const { return spec_; }
	uint32_t flags() const { return flags_; }
	// The weapon.def view record: `pos` (raw units), its cant (degrees), `tpos`, renderfov.
	const float *pos_units() const { return pos_units_; }
	const float *rot_bias_deg() const { return rot_bias_deg_; }
	const float *tpos_units() const { return tpos_units_; }
	float renderfov() const { return renderfov_; }
	// The characters, and the one whose arms draw (null: Avatars.def holds none).
	const std::vector<FirstPersonCharacter> &characters() const { return characters_; }
	const FirstPersonCharacter *character() const;
	// The team the player is on, its character's side's: the game picks a player's character by the side
	// its team is (team 1 the good side, any other the evil) [orig: NapiNPClientMsg 0x0C
	// @0x42eae4..0x42eb03, side = team != 1 -> EntitySlot_LookupAndPackEntry @0x57ad40], so a good
	// character's player is team 1 and an evil one's team 2. The view model's TEX_TEAM is its byte
	// (renderer::viewmodel_team_byte).
	int team() const;
	// The arms model drawn (null: none draws), its file as the scan names it, a serial that moves when it is
	// read again, and why it draws as it does where it says anything (a reach past the gun's parts, a model
	// the project lacks).
	const assets::Model &arms_model() const { return arms_model_; }
	const std::string &arms_file() const { return arms_file_; }
	uint64_t arms_serial() const { return arms_serial_; }
	const std::string &arms_note() const { return arms_note_; }
	// The weapon's actions as the game bakes them, and each action's run (twelve, by world::weapon_action).
	const world::WeaponFsmDef &fsm() const { return fsm_; }
	bool baked() const { return baked_; }
	const std::vector<WeaponActionRun> &actions() const { return actions_; }
	// The actions whose row plays the map's slot `clip_key` names (an anim key, the slot past its first
	// five characters), in the actions' order.
	std::vector<const WeaponActionRun *> actions_playing(const std::string &clip_key) const;

private:
	void read_weapon_(const FileSource &files, const std::shared_ptr<const FileSource> &source, const PreviewRig &rig);
	void read_characters_(const FileSource &files);
	bool read_arms_(const FileSource &files, const AssetScan *scan);

	bool active_ = false;
	std::string weapon_;
	std::string words_;
	renderer::FpViewmodelSpec spec_;
	uint32_t flags_ = 0;
	float pos_units_[3] = {0.0f, 0.0f, 0.0f};
	float rot_bias_deg_[3] = {0.0f, 0.0f, 0.0f};
	float tpos_units_[3] = {0.0f, 0.0f, 0.0f};
	float renderfov_ = renderer::kWeaponRenderFovHDegDefault;
	std::string gfx1_;
	std::string animadm_;
	world::WeaponFsmDef fsm_;
	bool baked_ = false;
	std::vector<WeaponActionRun> actions_;
	// The weapon's reads: its catalog's stamp, and what the bake read (the map, the clips) as it read them.
	std::string catalog_;
	std::string record_;
	uint64_t catalog_stamp_ = 0;
	FileStamps bake_read_;
	bool weapon_read_ = false;
	// The characters: Avatars.def's stamp, and the one chosen.
	uint64_t avatars_stamp_ = 0;
	bool avatars_read_ = false;
	std::vector<FirstPersonCharacter> characters_;
	uint16_t seeded_ = 0;
	int32_t chosen_ = -1;
	size_t character_ = SIZE_MAX;
	// The arms model, by its file and stamp (one that does not read latched until its stamp moves).
	std::string arms_file_;
	uint64_t arms_stamp_ = 0;
	bool arms_read_ = false;
	assets::Model arms_model_;
	uint64_t arms_serial_ = 0;
	std::string arms_note_;
};

// The view-frame offset the eye places the view model at: the weapon's `pos` at the hip over the def's
// scale, with the 4:3 framing drop on a picture `width` x `height` no wider than 4:3, as the game's
// presenter takes it standing still [orig: Player_UpdateFirstPersonCamera @0x4dd571..0x4dd578].
void first_person_view_units(const FirstPersonSources &sources, int width, int height, float out[3]);
// The first-person eye looking at the view model where the game draws it, in the preview's space (the
// model at the origin): the camera the presenter stands the model before, inverted (the view model's root
// is renderer::fp_viewmodel_pose of the view units, the cant and the rig's yaw-180), seeing with the
// weapon's renderfov and the view model pass's near plane [orig: Render_SwapProjectionNearZ(0.05)
// @0x4dee29; fov = WeaponDef+0x148 @0x4dee71].
OrbitCamera first_person_eye(const FirstPersonSources &sources, const OrbitCamera &orbit, int width, int height);

// The envelope's `first_person` (null where the map is no weapon's): the weapon, the view, the gun and
// the arms, the characters and the chosen one, the view record, the actions and their runs, and the
// actions playing the clip `clip_key` with the one chosen.
io::JsonValue first_person_json(const FirstPersonSources &sources, const FirstPersonOptions &options,
                                const std::string &clip_key, const WeaponActionRun *chosen);

} // namespace opennova::editor
