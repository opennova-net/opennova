#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/node.h>
#include <editor/preview/model_preview_camera.h>
#include <base/resource_index/resource_index.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/rig_files.h>
#include <runtime/assets/asset_store.h>
#include <runtime/anim/skeletal_clips.h>

namespace opennova {
class FileSource;
}

namespace opennova {
class StampedFiles;
}

namespace opennova::editor {

class AssetGraph;
class Document;

// The files a previewed rig reads (ADR 0046 S10p6): the game's own store (assets::AssetStore,
// the anim::RigFiles the mission loads through) mounted on the project's files (the open
// documents standing in for theirs), as the weapon range mounts them: a table and a clip are
// opened by the names the game's loads open, every read remembered with its stamp.
struct PreviewRigFiles {
	explicit PreviewRigFiles(std::shared_ptr<const StampedFiles> files);
	PreviewRigFiles(const PreviewRigFiles &) = delete;
	PreviewRigFiles &operator=(const PreviewRigFiles &) = delete;
	ResourceIndex index;
	assets::AssetStore store{&index};
};

// What an animation document plays on: a model and the table whose rig it binds (the
// table's reset head is the bind), or a lone clip (no table names it: it is its own bind).
struct PreviewRig {
	std::string model;  // the model's file name ("" none)
	std::string table;  // the table's file name ("" a lone clip)
	std::string clip;   // the clip's file name when the document is a clip
	std::string source; // where the model came from: "chosen", else the record that pairs them
	// The record that pairs them, where Go to on "paired by" takes (DI-05: its file opened at the record,
	// its map field shown); an empty file for a chosen model or none.
	ReferenceTarget pairing;
	// The same record as the graph names it, where one pairs them: its file (project-relative), its name,
	// and the field that names the table (an item's anim_def, a weapon's animadm); "" for a model chosen.
	std::string record_file;
	std::string record;
	std::string record_field;
};

// The model fields of a record that pair with its map field (S17 review): an item's anim_def plays
// on its graphic, and on its graphic_enemy (the model the other side sees it as, itemdef +0x90); a
// weapon's animadm on its gfx1, the first-person view model [orig: Player_RenderFirstPersonViewModel
// @ 0x4DED60]. Empty for any other field.
std::vector<const char *> preview_model_fields(const std::string &map_field);

// The rig of the animation document `file` (a table or a clip, its file name): a table
// plays on the model a record pairs with it (preview_model_fields, the first that names a model of
// the project: an item's graphic, else its graphic_enemy; a weapon's gfx1); a clip on the first
// table naming it that a record pairs with a model, and that table's model (a clip two tables share,
// one of them no item's, plays on the other's), else on the first table naming it. `chosen` (a model's
// file name) wins.
PreviewRig resolve_preview_rig(const AssetGraph &graph, const AssetScan &scan, const std::string &file, AssetKind kind,
                               const std::string &chosen);

// The rig itself: the table (else the lone clip) over the model's bone table, through the
// game's own loader (SkeletalClips); null when it does not load.
std::shared_ptr<const anim::SkeletalClips> load_preview_rig(const PreviewRig &rig, const threedi::Threedi3di3 &model,
                                                           const anim::RigFiles &files);

// What the selection in an animation document plays on the loaded rig, and what the game plays
// where the selection names a clip it does not load (ADR 0046 S17): the key and the variant of
// the clip, and a note in words where it is not the one selected.
struct PreviewClipChoice {
	std::string key; // "" nothing plays
	int variant = 0;
	std::string note;
};
// A table's selected clip (else its row's first clip the rig registered), as the key and variant
// the game's loader registered that row's token under (SkeletalClips::variant_of); a selected
// token that registered nothing (its file not in the project, no failsafe.bad) plays the row's
// first that did, and a row none of whose tokens registered plays what the game plays for an
// unauthored slot, the reset row's first clip [orig: AnimMap_RegisterEntity @ 0x40BB60, the
// backfill @ 0x40BC24, @ 0x40BD2E], each with its note; a key naming no slot plays nothing (the
// game skips the row [orig: AnimMap_ParseConfigLine @ 0x40CB60, the test @ 0x40CBA4]). A clip
// plays as the first token that registered its file; a lone clip plays under the key "clip".
PreviewClipChoice preview_clip_choice(const Document &document, const NodeAddress &selection, const PreviewRig &rig,
                                      const anim::SkeletalClips &clips);

// The key a lone clip plays under.
inline constexpr const char *kPreviewLoneClipKey = "clip";

// A clip's event on the preview's timeline: the frame, the tick the game first reads its
// trigger at (the clip's own clock, anim::ClipTimeline::first_ticks), and the
// event's trigger bits.
struct PreviewClipEvent {
	int frame = 0;
	int32_t tick = 0;
	uint32_t trigger = 0;
};
// The events of the clip `key`/`variant` plays that carry a trigger the game reads, in tick
// order, read from its file (`clip_file`); a frame the clock never runs on has none.
std::vector<PreviewClipEvent> preview_clip_events(const anim::SkeletalClips &rig, const std::string &key, int variant,
                                                  const bad::BadFile &clip_file);
// An event's letter on the timeline by what it does, a shot first ("F", an NPC's), then a footstep
// ("L", "R"), then a sound ("S"); "" for an event of bits the engine does not read alone, since
// nothing plays.
const char *preview_event_letter(uint32_t trigger);

// A joint's name as a modder reads it beside the model's parts (the models lane's BN## for a rig's
// bone, two digits and 1-based, docs/threedi/scene-naming-contract.md): "BN08" alone where the bind
// clip names the bone nothing of its own (its MDL<i>, SkeletalClips' made-up name), the clip's name
// where it begins with that word ("BN01 Hips"), else both ("BN08 RArm").
std::string preview_joint_name(size_t bone, const std::string &clip_name);

// A bone of the rig as the clip poses it at a tick (ADR 0046 S17): its name (preview_joint_name),
// its parent (-1: a root), where its joint stands in the preview's
// space, and the transform that carries a point of the model riding it from the rest pose to the
// pose (the skin's: the posed global over the rest global's inverse), in the rig's frame, which is
// the preview's (the X-negated model frame the device's skeleton stands in).
struct PreviewJoint {
	int bone = 0;
	int parent = -1;
	std::string name;
	PreviewVec3 at;
	anim::SkeletalClips::RestTransform deform;
};
// Every bone of the rig posed by `key`/`variant` at `ticks` of its clock (the sample the device
// draws: SkeletalClips::eval_pose at clip_seconds_at_tick); empty when the rig is not FK-safe or
// the key plays nothing.
std::vector<PreviewJoint> preview_posed_joints(const anim::SkeletalClips &rig, const std::string &key, int variant,
                                               int32_t ticks);
// A point of the rest pose, the preview's space, carried by a joint's deform.
PreviewVec3 preview_joint_carry(const PreviewJoint &joint, const PreviewVec3 &point, bool direction = false);

} // namespace opennova::editor
