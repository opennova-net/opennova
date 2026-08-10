#pragma once

#include "til.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

bool load_til(const uint8_t *data, size_t size, TilFile &out, std::string &error);
bool save_til(const TilFile &til, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova
