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

// The file a .bad load opens for a name: the name to its last '.' (all of it where it has none)
// with ".bad" appended, so "idle", "IDLE.BAD" and "idle.txt" all open "<stem>.bad". Empty for an
// empty name, which loads nothing. The game's other .bad loads (failsafe.bad, the player-info
// preview's two) pass names already ending in ".bad", which the rule leaves as they are.
// [orig: AnimMap_FindOrLoadBoneFile @0x40c030 -- an empty name returns @0x40c073, the cut at the
//  last '.' @0x40c08b..0x40c09d, ".bad" appended @0x40c0a6..0x40c0c6, the cache matched and the
//  file loaded by that name (stricmp @0x40c10c, BoneFile_Load @0x40c208)]
inline std::string bad_file_name(const std::string &name) {
	if (name.empty()) return std::string();
	const size_t dot = name.find_last_of('.');
	return (dot == std::string::npos ? name : name.substr(0, dot)) + ".bad";
}

// The clip a table's token registers: its own .bad (the store opens bad_file_name's file), else
// failsafe.bad in its place when that loads, else none (the token registers nothing). `file`, when given, names the file that
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
