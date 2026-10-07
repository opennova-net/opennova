#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/definition_effects.h>
#include <editor/preview/definition_weapon.h>
#include <editor/preview/effect_catalog.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/model_damage.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class DefCatalogDocument;
struct CatalogRow;

// What a definition viewport shows, and why not (ADR 0046 DI-21): the kind's reason.
enum class DefinitionViewStatus : uint8_t {
	NoProject,  // no project is open
	NoCatalog,  // no definition table is open at its path
	NoRecord,   // none of its records is selected (or the one shown went)
	NoModel,    // the record draws no model of its own (detail: why, in words)
	Missing,    // the project has no model of the name the record draws (detail: the name)
	Unreadable, // the project's model of the name does not read as a model (detail: its file)
	Ready,
};
// "no_project", "no_catalog", "no_record", "no_model", "missing", "unreadable", "ready".
const char *definition_view_status_token(DefinitionViewStatus status);

// The state an item is drawn in (the State menu): alive, as its mission starts; destroying, its death as the
// game runs it from the preview clock's tick 0 (preview/model_damage's plan: the husk swap, the pieces' sections
// gone, the destroy fade, the death's effects and sound); its husk, the wreck standing after the death (the
// swap done, the fade at its end, the Fire and Other banks burning on it [orig: Entity_UpdateDeadWreckEffects
// @ 0x493140]); its final husk, the model the death pieces are cut from, whole.
enum class DefinitionState : uint8_t { Alive, Destroying, Husk, HuskFinal };
// "alive", "destroying", "husk", "husk_final".
const char *definition_state_token(DefinitionState state);
// A weapon drawn as the game draws it in a soldier's hands (its gfx3), or in the player's own view (its gfx1).
enum class DefinitionWeaponView : uint8_t { Third, First };

// How a definition viewport draws its record: the item's state; the side seeing it (`enemy`: an item's
// graphic_enemy, an ammo's enemy tracer item); a weapon's view; whether a drivable item is occupied (its
// particle slot attaches only then [orig: Entity_UpdateHeloRotorSpin @ 0x48fa70]); the record SSN a person's
// spawn warms up by (world::organic_warmup_updates: a definition has no record, the preview picks one); the
// death's and a weapon's sounds muted; the ground grid (the editor's aid); how a weapon fires (DI-22).
struct DefinitionViewportOptions {
	DefinitionState state = DefinitionState::Alive;
	bool enemy = false;
	DefinitionWeaponView weapon = DefinitionWeaponView::Third;
	bool occupied = false;
	int ssn = 1;
	bool mute = false;
	bool grid = true;
	DefinitionFireOptions fire;
	bool operator==(const DefinitionViewportOptions &other) const {
		return state == other.state && enemy == other.enemy && weapon == other.weapon && occupied == other.occupied &&
		       ssn == other.ssn && mute == other.mute && grid == other.grid && fire == other.fire;
	}
	bool operator!=(const DefinitionViewportOptions &other) const { return !(*this == other); }
};
// On the wire (the envelope's `options`, a SetViewport's): {state, enemy, weapon ("third", "first"), occupied,
// ssn, mute, grid, fire (definition_fire_options_to_json)}.
io::JsonValue definition_options_to_json(const DefinitionViewportOptions &options);
// The change a SetViewport makes to set a definition viewport's options to `options`, and its camera.
std::string definition_options_change(const DefinitionViewportOptions &options);
std::string definition_camera_change(const OrbitCamera &camera);

// The record a definition viewport shows and what it draws of it: its catalog kind ("item", "weapon", "ammo",
// "powerup", "carry"), its name, the field naming the model drawn and the name it gives, the project's file of
// that model (project-relative; "" none), and, where another record gives the model (an ammo's round is the
// item its tracer id names), that record: its name, its file and its locator, which a Go to opens.
struct DefinitionDrawn {
	std::string kind;
	std::string record;
	std::string field;
	std::string name;
	std::string file;
	std::string via;
	std::string via_file;
	std::string via_locator;
};

// An item's particle slot as the mission's start attaches it [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails
// @ 0x522ee0]: its effect and point as authored, whether the item's pool admits it (world::item_effect_pool_allows
// by the pool its TYPE puts it in; a drivable item's while occupied, world::item_effect_controller_allows), the
// user points it attaches at (the drawn model's first 16 of the name; none: once at the origin), and why it
// shows nothing where it does not.
struct DefinitionParticleSlot {
	std::string effect;
	std::string point;
	bool admitted = false;
	bool controller = false; // it attaches while a driver controls the item (Occupied)
	std::vector<int> user_points;
	std::string words;
};

// A definition table's picture (ADR 0046 DI-21; ViewportKind::Definition, the Preview role of the definition
// tables; CONTEXT.md "Definition preview"): the record the selection lands in, drawn as the game draws the
// thing it defines. An item: its graphic (its graphic_enemy seen as the enemy), in the State the options pick,
// its death as preview/model_damage plans it (DI-10's: the husk the game swaps in, the sections the pieces
// leave, the destroy fade's six registers, the death's legs in order), its effects spawned by the engine's
// effect scene where the game spawns them (its particle slot at its user points at the mission's start; the
// death's banks at the piece model's Dead, Fire and Other points [orig: Entity_InitDeathSounds @ 0x4939B0;
// Entity_SpawnMaskedEffectBank @ 0x5F7620], world::death_bank_spawns; a class's one effect at the item) on the
// preview clock (preview/definition_effects), its death sound played through DI-04's player as the clock runs
// (the session's clip sounds), a person posed as its spawn poses it (DI-38's pose_person, its .adm on its
// graphic's rig). A weapon: its third-person model (gfx3), or its first-person (gfx1). An ammo: the round as the
// item its tracer id names draws it (frndly_trcr_type_id, foe_trcr_type_id seen as the enemy, falling back to
// the friendly one as the game does). A powerup row and a carry limit draw nothing of their own, which it says.
// A weapon fires (DI-22, preview/definition_weapon): in first person its gfx1 with the character's arms on the
// gun's rig, posed by the first-person channel the game's weapon pump steps, seen from the eye; in third person
// its gfx3; the gestures (Fire, Hold, Release, Reload, Scope, Switch) run through the game's local player in a
// range of its own, its effects spawned where the game's presenter spawns them, its sounds heard as the clock
// runs, its tracers, its target and the scars on it drawn by the device.
// The record is read as it stands (the catalog's row is the record the game's parser makes); the models, the
// .adm and the particle files are the project's, as the game would read them were they saved now (the open
// documents standing in). Its camera orbits the drawn model, framed as another model first shows; a point of
// its picture names nothing. Its device: godot/src/authoring/definition_viewport_applier.
//
// The seams the slices after it build on: the record's kind and its subject (the record, its drawn model and
// its rig), the effects' playback (spawn at a pose on a tick), the clip sounds' firing, and the options and
// envelope extended per kind (DI-22: a weapon fires; DI-23: an ammo's impact rows; DI-24: a clip's fire events).
class DefinitionViewport final : public ViewportModel {
public:
	explicit DefinitionViewport(std::string path);
	~DefinitionViewport() override;
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	DefinitionViewStatus view_status() const { return reason_; }
	const DefinitionViewportOptions &options() const { return options_; }
	// The camera the picture is seen through: the first-person eye where a weapon shows it (DI-22), else the orbit.
	const OrbitCamera &camera() const { return eye_ ? weapon_.eye_camera() : camera_; }
	const OrbitCamera &orbit() const { return camera_; }
	// The record shown (its row; 0 none) and what it draws.
	NodeId record_row() const { return row_; }
	const DefinitionDrawn &drawn() const { return drawn_; }
	// The model the device draws now (null unless ready), a serial that moves when another is drawn, and the
	// level the game draws at the camera's distance.
	const assets::Model &model() const { return model_; }
	uint64_t model_serial() const { return model_serial_; }
	int lod() const;
	// A person's rig (its .adm over the drawn model's bones; null: none) and a serial that moves when it is
	// loaded again, and its pose as the spawn makes it (status "" for a record that is no person).
	const std::shared_ptr<const anim::SkeletalClips> &skeleton() const { return skeleton_; }
	uint64_t skeleton_serial() const { return skeleton_serial_; }
	const MissionPose &person() const { return person_; }
	// An item's facts the death reads, its death as the game runs it and the state the game draws at `clock`
	// (Destroying: from tick 0; the husk: the fade's end; none otherwise).
	const DamageItem &item() const { return item_; }
	const DamagePlan &plan() const { return plan_; }
	DamageFrame frame_at(const PreviewClock &clock) const;
	// The CTRL registers the drawn model reads at `clock` (the destroy fade's six while husked) and the sections
	// the death pieces left hidden on it.
	std::map<std::string, int64_t> ctrl_at(const PreviewClock &clock) const;
	uint32_t hidden_sections_at(const PreviewClock &clock) const;
	// The item's particle slot, the effects playing (the scene the device draws) and what the picture says beside
	// itself (an enemy model the item does not author, a state the record has none of, a slot the game attaches
	// only while occupied).
	const DefinitionParticleSlot &particle_slot() const { return slot_; }
	const DefinitionEffects &effects() const { return effects_; }
	// A weapon record's firing (DI-22): the gun, its rig and arms, its range (inactive for another kind).
	const DefinitionWeapon &weapon() const { return weapon_; }
	bool weapon_record() const { return drawn_.kind == "weapon" && weapon_.active(); }
	const std::vector<std::string> &notes() const { return notes_; }
	// The death's sounds over the ticks the clock ran through since the last call (Destroying alone: each leg's
	// set played at the item as the camera hears it, never over a seek; a leg on the tick a seek lands on fires
	// as the clock runs from it, as the model preview's damage state fires them), or a weapon's (DI-22: its
	// actions' legs and what its rounds and shots sound, DefinitionWeapon::sounds_between), kept as the clip
	// sounds are (sounds_fired, the last kSoundsFiredKept) and returned.
	static constexpr size_t kSoundsFiredKept = 16;
	const std::vector<ClipSoundFired> &sounds_fired() const { return fired_; }
	std::vector<ClipSoundFired> fire_sounds(const PreviewClock &clock, const AssetScan *scan, audio::SoundSelector &selector,
			uint64_t &next_seq);
	// The camera looking at the drawn model on a picture `width` x `height` (its angles kept).
	OrbitCamera framed(int width, int height) const;

	ViewportStatus status() const override;
	const char *reason() const override { return definition_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const FileStamps *picture_reads() const override { return &picture_.files(); }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// "frame" (the camera on the drawn model), "replay" (the clock sought to tick 0: the state played anew); a
	// weapon's gestures "fire", "hold", "release", "reload", "scope", "switch" (one on the clock's tick, the clock
	// run) and "clear" (none).
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The effects it spawns, each an item: its index, its effect as its name, its source as its kind.
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	struct HeldModel {
		std::string file; // the scan's file name
		uint64_t stamp = 0;
		assets::Model model; // null: it does not read
	};
	ViewportAction stop_(DefinitionViewStatus reason, const std::string &detail);
	// What the record draws (drawn_ and the item's facts) from its row; false, with why, for a record that draws
	// no model of its own.
	bool subject_(const ViewportInput &input, const DefCatalogDocument &document, const CatalogRow &row,
			const PreviewClock &clock, std::string &why);
	// The project's model of `name` (a scan's model file, read and parsed once while its stamp stands; null when
	// it does not read); `file` its file name ("" the project has none).
	assets::Model held_(const SessionView &view, const std::string &name, std::string &file);
	// The person's pose and rig over the drawn model (again where the item, the SSN, the model or a file they
	// read moved).
	void pose_(const SessionView &view, const std::string &model_file);
	// The item's particle slot over its intact model (`intact`: its graphic, or its enemy model): admitted or
	// not, and its emitters' spawns.
	void particle_slot_(const assets::Model &intact);
	// The effects the picture spawns in its state.
	std::vector<DefinitionSpawn> spawns_() const;
	void frame_();
	// A weapon's gesture (a token) and gestures ({tick, gesture} rows), read off a SetViewport.
	static bool read_gesture_(const io::JsonValue &json, std::vector<WeaponGestureAt> &out, std::string &error);
	static bool read_gestures_(const io::JsonValue &json, std::vector<WeaponGestureAt> &out, std::string &error);

	DefinitionViewStatus reason_ = DefinitionViewStatus::NoProject;
	std::string detail_;
	DefinitionViewportOptions options_;
	OrbitCamera camera_;
	NodeId row_ = 0;
	DefinitionDrawn drawn_;
	PreviewFollow picture_;
	std::vector<HeldModel> models_;
	assets::Model model_;
	std::string model_file_;
	uint64_t model_serial_ = 0;
	std::string framed_; // the model the camera last framed
	// An item's.
	bool item_record_ = false;
	DamageItem item_;
	DamageModels damage_models_;
	DamagePlan plan_;
	std::string husk_file_, piece_file_;
	assets::Model piece_model_;
	std::string anim_def_;
	uint32_t attrib_ = 0;
	int type_ = 0;
	DefinitionParticleSlot slot_;
	std::vector<DefinitionSpawn> slot_spawns_;
	std::string intact_; // the intact model's name (its graphic, or its enemy model)
	std::vector<std::string> notes_;
	// A person's.
	MissionPose person_;
	std::string person_key_;
	std::string rig_key_;  // the model and the .adm the rig was loaded of
	std::string rig_note_; // why a person's rig does not load ("" it does)
	std::shared_ptr<const anim::SkeletalClips> skeleton_;
	uint64_t skeleton_serial_ = 0;
	FileStamps rig_read_;
	// Its effects, over the catalog the game would load.
	PreviewEffectCatalog catalog_;
	DefinitionEffects effects_;
	std::vector<std::string> missing_; // the graphics the device found no file for (each once)
	// A weapon's firing (DI-22), and whether its eye is the camera.
	DefinitionWeapon weapon_;
	bool eye_ = false;
	// The death's sounds: their sources, what fired, and the clock's tick they were last fired to (-1 none).
	ClipSoundSources sound_sources_;
	std::vector<ClipSoundFired> fired_;
	int32_t sound_cursor_ = -1;
	uint64_t sound_seeks_ = 0;
	bool options_moved_ = false;
};

} // namespace opennova::editor
