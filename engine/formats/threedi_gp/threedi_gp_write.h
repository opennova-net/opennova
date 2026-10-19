// The GP writer: a threedi_gp::File serialized in the order BHD's loader reads it, the inverse
// of parse() (threedi_gp_read.cpp) section by section. Every count and size is the struct's;
// a word the loader skips is written as BHD's own model tool writes it [orig: GP_WriteModel, the
// GP save, and its section writers (df4oed.exe)]. threedi_gp_lower.h makes the File from a 3DI3.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/threedi_gp/threedi_gp.h>

namespace opennova::threedi_gp {

// Serialize `file` into `out`. False, with `error` saying why, when it holds what the loader
// refuses (no GP kind, a revision past 0x103, a LOD count outside 1..4, a palette slot past 15)
// or what this writer does not carry (a render model's +0x60 or +0x68 records), or when its
// counts disagree (a primitive run, a part animation table, a name past its field).
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova::threedi_gp
