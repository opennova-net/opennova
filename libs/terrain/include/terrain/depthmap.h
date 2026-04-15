#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

std::vector<uint8_t> load_depthmap_raw(const std::string &filepath);
std::vector<uint16_t> load_depthmap_raw16(const std::string &filepath);
std::vector<uint16_t> smooth_depthmap(const std::vector<uint8_t> &raw);

} // namespace opennova
