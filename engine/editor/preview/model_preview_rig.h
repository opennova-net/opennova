#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/node.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_clips.h>

namespace opennova {
class FileSource;
}

namespace opennova::editor {

class AssetGraph;
class Document;
class StampedFiles;

// The files a previewed rig reads (ADR 0046 S10p6): anim::RigFiles over the project's files
// (the open documents standing in for theirs), looked up by the name the game's store looks
// them up by (assets::asset_file_name), every read remembered with its stamp.
class PreviewRigFiles : public anim::RigFiles {
public:
	explicit PreviewRigFiles(std::shared_ptr<const StampedFiles> files) : files_(std::move(files)) {}
	std::shared_ptr<const adm::AdmFile> animation_map(const std::string &name) const override;
	std::shared_ptr<const bad::BadFile> bone_animation(const std::string &name) const override;

private:
	std::shared_ptr<const StampedFiles> files_;
};

// What an animation document plays on: a model and the table whose rig it binds (the
// table's reset head is the bind), or a lone clip (no table names it: it is its own bind).
struct PreviewRig {
	std::string model;  // the model's file name ("" none)
	std::string table;  // the table's file name ("" a lone clip)
	std::string clip;   // the clip's file name when the document is a clip
	std::string source; // where the model came from: "chosen", else the record that pairs them
};

// The rig of the animation document `file` (a table or a clip, its file name): a table
// plays on the model an item pairs with it (its graphic beside its anim_def; a weapon's
// first-person animadm is not paired here: that rig is the viewmodel's); a clip on the
// first table that names it, and that table's model. `chosen` (a model's file name) wins.
PreviewRig resolve_preview_rig(const AssetGraph &graph, const AssetScan &scan, const std::string &file, AssetKind kind,
                               const std::string &chosen);

// The rig itself: the table (else the lone clip) over the model's bone table, through the
// game's own loader (SkeletalClips); null when it does not load.
std::shared_ptr<const anim::SkeletalClips> load_preview_rig(const PreviewRig &rig, const threedi::Threedi3di3 &model,
                                                           const anim::RigFiles &files);

// What the selection in an animation document plays on the loaded rig `clips`: a table's
// selected clip (else its row's first clip the rig registered), as the key and variant the
// game's loader registered that row's token under (SkeletalClips::variant_of); a clip as the
// first token that registered its file; a lone clip plays under the key "clip". False when
// nothing plays (the row names no slot, the clip's file does not load).
bool preview_clip_of(const Document &document, const NodeAddress &selection, const PreviewRig &rig,
                     const anim::SkeletalClips &clips, std::string &key, int &variant);

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

} // namespace opennova::editor
