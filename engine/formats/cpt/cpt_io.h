#pragma once

#include "cpt.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

bool load_cpt(const uint8_t *data, size_t size, CptFile &out, std::string &error);
// The file's bytes (the CDEP/DPTH depth section, then the POLY tiles); false, with `error` in words and
// `out` untouched, for a depth buffer or a tile the writer cannot encode. CptFile::write_bytes is the
// same writer for callers that take its refusal as a std::runtime_error.
bool save_cpt(const CptFile &cpt, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova
