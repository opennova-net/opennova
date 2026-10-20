#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <base/vfs/file_stamps.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class AssetGraph;
struct ClipSoundItem;

// A person's held weapon in the model preview (ADR 0046 DI-13's follow-up, "A person's held
// weapon"): a row of a person's map plays with the weapon the body would hold in it, drawn where
// the game draws it. Every rule is the engine's own:
//
// - Which weapon: the weapon table as the mission load builds it from weapon.def
//   (mission::read_weapon_defs, world::build_weapon_table: index 1 the file's first weapon, its
//   gfx3 loaded through the game's model store [orig: WeaponDefs_ParseLineCallback @0x544FCE]).
//   A weapon reaches the row the way the body's weapon channel picks its hold state: its
//   special_hold through the hold ladder (world::infantry_weapon_hold_state: the knife's row 50,
//   the grenade's 52, a rifle mirroring the body's own row) [orig:
//   Entity_UpdateInfantryPlayerBody @0x4b5dad..0x4b5e6f], its attack_anim through the fire stamp
//   (world::infantry_weapon_attack_stamp: 62, 63) [orig: WeaponAction_Fire @0x542bbc..0x542bea],
//   any weapon with a clip through the reload window (65, a pistol's 66) [orig:
//   WeaponSlot_ReloadAmmo @0x54173c], any weapon through the binoculars (64). Auto draws the
//   first weapon of the file that reaches the row (a body row: the first whose hold mirrors it);
//   a weapon the options name draws whatever row plays.
// - Whether it draws: the present row's gate (inmatch::write_present_held_weapon: alive and on
//   foot, an ADM index [orig: BoneCallback_org0_World @0x4e3c97]), so a death row draws none
//   [orig: Entity_CanFireWeapon @0x4dcb10, the Flags & 2 return]; and only the player class sets
//   a body's held weapon [orig: PlayerClass_InitEntity @0x4b1060], so Auto draws none for an
//   NPC's map (org0/org1 never write +0x2B0; world-wac-ai-re.md 13.5).
// - Where: the present row's attach (the same writer: the attach triple of a body standing in the
//   preview's frame, its aim level, and the hand frame where the row's anim flags carry 0x80),
//   hung from bone 16 as the game's presenter hangs it (godot/src/simulation
//   EntityPresenter::held_weapon_attach_transform over world::kHeldWeaponBoneIndex and its
//   nudge) [orig: Entity_BuildBoneTransformMatrices @0x4b2180..0x4b22f8; the draw
//   BoneCallback_org0_World @0x4e3c87..0x4e3d99].

// The held weapon's options (the model viewport's options' `held`): the weapon drawn, "" the first
// of weapon.def that reaches the row, "none" none, else a weapon's name (without case).
struct HeldWeaponOptions {
	std::string weapon;
	bool operator==(const HeldWeaponOptions &other) const { return weapon == other.weapon; }
	bool operator!=(const HeldWeaponOptions &other) const { return !(*this == other); }
};
// The word that asks for no weapon.
inline constexpr const char *kHeldWeaponNone = "none";
// On the wire: {weapon ("" auto, "none", a weapon's name)}.
io::JsonValue held_weapon_options_to_json(const HeldWeaponOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why.
bool read_held_weapon_options(
		const io::JsonValue &json, HeldWeaponOptions &held, std::string &error);

// How a weapon reaches the row playing.
enum class HeldRoute : uint8_t {
	None, // it never plays this row (a weapon-channel row its hold, attack and clip do not reach)
	Hold, // its special_hold's own hold row (scoped or not)
	Attack, // its attack_anim's fire stamp
	Reload, // the reload window (a weapon with a clip)
	Binoculars, // the binoculars raised over it
	Mirror, // a body row its hold mirrors: the upper body plays the row itself
	Carried, // a body row it is carried through, its own hold row on the upper body
};
inline constexpr const char *kHeldRouteTokens[] = { "none", "hold", "attack", "reload",
	"binoculars", "mirror", "carried" };

// A weapon of weapon.def as the game's weapon table holds it, and how it reaches the row playing.
struct HeldWeaponChoice {
	std::string name;
	int adm = 0; // its ADM index (1 the file's first weapon)
	int32_t hold = 0; // special_hold
	int32_t attack = 0; // attack_anim
	bool clip = false; // a clip the reload refills (clipsize > 0)
	std::string gfx3; // as authored ("" none)
	HeldRoute route = HeldRoute::None;
	int state = -1; // the weapon channel's hold state with it while the row plays
};

// Where the weapon hangs at a pose of the rig, in the preview's space: its origin (bone 16's joint,
// the nudge carried by the hand) and its muzzle (+Z) and up (+Y) axes.
struct HeldWeaponPlace {
	PreviewVec3 at;
	PreviewVec3 muzzle{ 0.0f, 0.0f, 1.0f };
	PreviewVec3 up{ 0.0f, 1.0f, 0.0f };
	std::string point; // the bone's name ("BN17 R Hand")
};

// The project's files a held weapon reads, as the game's mission load reads them by name (an open
// document standing in for its file): weapon.def into the weapon table, each weapon's gfx3 through
// the game's model store; read again only when a file it read moves.
class HeldWeapon {
public:
	// True when what it draws, or what it says, moved. `item` is the record pairing the map (its
	// class); `clip_key` the row playing ("" none); `rig_bones` the rig's bones (one of no bone 16
	// is no person's: the game hangs the weapon there); `first_person` a weapon's map, which holds
	// none.
	bool refresh(const std::shared_ptr<const FileSource> &files, const AssetScan *scan,
			const AssetGraph *graph, const ClipSoundItem &item, const std::string &clip_key,
			size_t rig_bones, bool first_person, const HeldWeaponOptions &options);
	void clear();

	// A body's row plays (a person's map, not a weapon's): the held weapon has a say.
	bool active() const { return active_; }
	// The row playing (its anim slot) and whether the body is the player class's.
	int row() const { return row_; }
	bool player() const { return player_; }
	// weapon.def's weapons as the game's table holds them, each with how it reaches the row.
	const std::vector<HeldWeaponChoice> &choices() const { return choices_; }
	// The weapon drawn (null none) and whether the options named it.
	const HeldWeaponChoice *drawn() const {
		return drawn_ < choices_.size() ? &choices_[drawn_] : nullptr;
	}
	bool named() const { return named_; }
	// Its model as the game loads its gfx3 (null: none draws), the project's file of it ("" none)
	// and its path, and a serial that moves whenever the model drawn is another.
	const assets::Model &model() const { return model_; }
	const std::string &model_file() const { return model_file_; }
	const std::string &model_path() const { return model_path_; }
	uint64_t serial() const { return serial_; }
	// The present row's attach: the triple (mission degrees: pitch, yaw, roll) and the hand frame.
	const float *attach_deg() const { return attach_deg_; }
	bool hand_frame() const { return hand_frame_; }
	// The weapon's record in weapon.def, where its links go (an empty file: none).
	const ReferenceTarget &record() const { return record_; }
	// What draws and why, in words; why nothing draws or the one asked is not the game's ("" none).
	const std::string &words() const { return words_; }
	const std::string &note() const { return note_; }
	// What it read (a test's measure: the weapon table built again only as these move).
	uint64_t reads() const { return reads_count_; }

	// Where the drawn weapon hangs at the rig's pose `joints` (preview_posed_joints); false where
	// none draws or the rig has no bone 16.
	bool place(const std::vector<PreviewJoint> &joints, HeldWeaponPlace &out) const;

private:
	void read_table_(const std::shared_ptr<const FileSource> &files);

	bool active_ = false;
	int row_ = -1;
	bool player_ = false;
	// The table as read: its weapons, their models, and what the read read.
	struct Entry {
		HeldWeaponChoice choice;
		assets::Model model;
	};
	std::vector<Entry> table_;
	FileStamps table_read_;
	bool table_known_ = false;
	uint64_t reads_count_ = 0;
	// The row's.
	std::vector<HeldWeaponChoice> choices_;
	size_t drawn_ = SIZE_MAX;
	bool named_ = false;
	assets::Model model_;
	std::string model_file_;
	std::string model_path_;
	uint64_t serial_ = 0;
	float attach_deg_[3] = { 0.0f, 0.0f, 0.0f };
	bool hand_frame_ = false;
	ReferenceTarget record_;
	std::string words_;
	std::string note_;
};

// The envelope's `held` (null where no body row plays: a weapon's map, a lone clip, no row): the
// weapon drawn, its ADM index, record and gfx3 file, how it reaches the row, the frame and the
// point it hangs from with where at `joints` and its pixel on a picture `width` x `height` through
// `camera`, the words and the note, and the choices.
io::JsonValue held_weapon_json(const HeldWeapon &held, const HeldWeaponOptions &options,
		const std::vector<PreviewJoint> &joints, const OrbitCamera &camera, int width, int height);

} // namespace opennova::editor
