#pragma once

#include "cpt.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova {

bool load_cpt(const uint8_t *data, size_t size, CptFile &out, std::string &error);

} // namespace opennova
