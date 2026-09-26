// opennova-3di anim: the animation commands' shared surface. A clip set is one
// rig's `.adm` table and every `.bad` clip it names; the `.o3a` scene text
// (docs/anim/o3a-scene-format.md) is what a DCC front end writes and reads.
//
//   anim build   <set.o3a> -o <out.adm|out.bad>
//   anim scene   <in.adm|in.bad> -o <set.o3a>
//   anim info    <in.adm|in.bad> [--verbose | --keys]
//   anim compare <expected.adm|.bad> <actual.adm|.bad>
//
// Every clip byte is read and written by the engine (formats/bad, formats/adm,
// the construction seam formats/bad/bad_build.h), so there is one encoder and
// one decoder; the text carries MISSION axes and the seam owns the conversion.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>

namespace threedi_cli {

// One clip of a loaded set: the bytes as read, and the parsed document.
struct AnimLoadedClip {
	std::string name; // the file stem, which is how a row names it
	std::string path;
	std::vector<uint8_t> bytes;
	opennova::bad::BadFile file{};
};

// A variant a table names whose clip did not load: absent beside the table,
// unreadable, or not a `.bad` the reader accepts.
struct AnimMissingClip {
	std::string variant;
	std::string reason;
};

struct AnimLoadedSet {
	std::string table_name;  // the `.adm` file name, empty for a lone clip
	std::string table_path;  // as given
	std::vector<opennova::bad::BadBuildRow> rows;
	std::vector<AnimLoadedClip> clips; // in the order the rows first name them
	std::vector<AnimMissingClip> missing; // in table order, each variant once
};

// Read a `.adm` table and every `.bad` beside it, or a lone `.bad`. The lookup
// is case-insensitive over the directory, because a retail table names
// `Dt1RunF.bad` where the file on disk is `DT1RUNF.BAD`. False with `error`
// set when the input itself cannot be read, or when a variant names a path; a
// clip that does not load is recorded in `missing`, with why, and does not
// fail the load.
bool anim_load(const std::string &path, AnimLoadedSet &out, std::string &error);
void anim_free(AnimLoadedSet &set);

// One table row as `info` prints it: the key, then the variants in the order
// the engine serves them (a ring from the last back; a reset row is no ring).
std::string anim_info_row(const opennova::bad::BadBuildRow &row);

int cmd_anim_build(const char *scene_path, const char *out_path);
int cmd_anim_scene(const char *in_path, const char *out_path);
int cmd_anim_info(const char *in_path, int verbose);
int cmd_anim_compare(const char *expected_path, const char *actual_path);

} // namespace threedi_cli
