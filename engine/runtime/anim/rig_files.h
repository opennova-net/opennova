#pragma once

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>

#include <memory>
#include <string>

namespace opennova::anim {

// The files a rig loads by name (SkeletalClips): its animation table and its clips, each
// parsed, null when it does not resolve or read. The game's mounted store answers them
// (assets::AssetStore); an embedder answers its own set (the editor's project, the
// documents it has open standing in for their files).
class RigFiles {
public:
	virtual ~RigFiles() = default;
	virtual std::shared_ptr<const adm::AdmFile> animation_map(const std::string &name) const = 0;
	virtual std::shared_ptr<const bad::BadFile> bone_animation(const std::string &name) const = 0;
};

// The clip the game plays for a token of an animation table whose own .bad does not load.
inline constexpr const char *kFailsafeClip = "failsafe.bad";

// The clip a table's token registers: its own .bad, else failsafe.bad in its place when that
// loads, else none (the token registers nothing). `file`, when given, names the file that
// loaded. Only a table's tokens take the failsafe: the loader's one caller is the table row
// parser, and the game's other .bad loads (the player-info preview) take none.
// [orig: AnimMap_ParseConfigLine @0x40cb60 -> AnimMap_FindOrLoadBoneFile @0x40c030
//  (@0x40cbdd): the token's load @0x40c208, the failsafe entry in its place
//  @0x40c25b..0x40c2a1, none without it @0x40c260; AnimMap_Init @0x40be40 loads
//  failsafe.bad @0x40be96..0x40bead; BoneFile_Load's other callers
//  PlayerInfo_InitPreviewModel @0x56013d / @0x56014c]
inline std::shared_ptr<const bad::BadFile> adm_token_clip(const RigFiles &files, const std::string &token,
                                                          std::string *file = nullptr) {
	if (auto clip = files.bone_animation(token)) {
		if (file) *file = token;
		return clip;
	}
	auto failsafe = files.bone_animation(kFailsafeClip);
	if (failsafe && file) *file = kFailsafeClip;
	return failsafe;
}

} // namespace opennova::anim
