// A 3DI3 model lowered to the GP model Black Hawk Down loads: the migration
// (threedi_gp_migrate.h) run backwards, field for field, so that migrating the result gives the
// model back on every word the two loaders keep. Where a 3DI3 holds what BHD has no form for (a
// JO-only technique, a translation track) the lowering refuses or says so in a note; a word
// BHD's loader skips takes what BHD's own model tool writes there [orig: sub_4488F0, the GP
// save (df4oed)]. Rigid models (GPM) only: a skinned 3DI3 is refused. The map is
// docs/threedi/3di-gp-format-re.md, "3DI3 to GP".
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>

namespace opennova::threedi_gp {

struct LowerOptions {
	// The model's name in the header; empty: the 3DI3's own. The tool names a model after the
	// file it writes, at most 15 characters [orig: GP_WriteModel @ 0x448A94..0x448AD1 (df4oed)].
	std::string name;
	// A texture file's size in texels, for its row's +0x28 words (the checkerboard a failed
	// load makes). Unset, or false: 256 x 256.
	std::function<bool(const std::string &name, uint16_t &width, uint16_t &height)> texture_size;
};

// Lower `model` to `out`. False, with `error` saying why, when the model holds what no GP model
// can (a skinned mesh, a technique BHD has no form for, a translation track, a name past its
// field). `notes` says what the lowering could not carry exactly, one line a kind.
bool lower(const threedi::Threedi3di3 &model, File &out, std::vector<MigrateNote> &notes, std::string &error,
		const LowerOptions &options = {});

} // namespace opennova::threedi_gp
