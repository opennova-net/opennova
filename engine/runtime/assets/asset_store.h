#pragma once

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/rig_files.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opennova { class ResourceIndex; }
namespace opennova::anim { class SkeletalClips; }

namespace opennova::assets {

using Model = std::shared_ptr<const threedi::Threedi3di3>;
using AnimationMap = std::shared_ptr<const adm::AdmFile>;
using BoneAnimation = std::shared_ptr<const bad::BadFile>;
using SkeletalRig = std::shared_ptr<const anim::SkeletalClips>;

// One store per mounted source, shared by simulation and presentation. The
// index outlives the store; returned immutable handles can outlive both,
// including remounts and invalidation. Access is serialized by the embedder.
class AssetStore : public anim::RigFiles {
public:
	explicit AssetStore(const ResourceIndex *index = nullptr);
	~AssetStore() override;
	AssetStore(const AssetStore &) = delete;
	AssetStore &operator=(const AssetStore &) = delete;

	const ResourceIndex *index() const;
	bool has_source() const;
	void invalidate() const;
	Model model(const std::string &graphic) const;
	AnimationMap animation_map(const std::string &name) const override;
	BoneAnimation bone_animation(const std::string &name) const override;
	SkeletalRig skeletal_rig(const std::string &adm_name,
			const std::vector<anim::Vec3> &origins = {},
			const std::vector<int> &parents = {}) const;
	SkeletalRig skeletal_rig_from_files(const std::string &skeleton_bad,
			const std::vector<std::pair<std::string, std::string>> &clips,
			const std::vector<anim::Vec3> &origins = {},
			const std::vector<int> &parents = {}) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	void sync_source() const;
};

// The name the store looks a file up by: the query's file name, lower-cased, with
// `extension` appended when it lacks it (with `replace`, the query's own extension is
// dropped first). Empty for an empty file name. Another file set that answers the same
// queries (anim::RigFiles over the editor's project) looks them up the same way.
std::string asset_file_name(const std::string &name, const char *extension, bool replace = false);

// Standalone documents use native ownership without creating a mount.
// Mounted consumers always use AssetStore::model.
Model read_model_file(const std::string &path);
// A model's, a table's or a clip's bytes read (the editor's documents hold one as their
// base, its preview reads the project's); null when they do not read.
Model parse_model(const uint8_t *data, size_t size);
AnimationMap parse_animation_map(const uint8_t *data, size_t size);
BoneAnimation parse_bone_animation(const uint8_t *data, size_t size);

} // namespace opennova::assets
