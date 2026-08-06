#pragma once

#include "pcx.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

bool decode_pcx_rgb(const uint8_t *data, size_t size, RgbImage &out, std::string &error);
bool decode_pcx_indexed(const uint8_t *data, size_t size, IndexedImage8 &out, std::string &error);
bool encode_pcx_indexed(const IndexedImage8 &image, std::vector<uint8_t> &out, std::string &error);

bool load_pcx_rgb_file(const std::string &path, RgbImage &out, std::string &error);
bool load_pcx_indexed_file(const std::string &path, IndexedImage8 &out, std::string &error);

} // namespace opennova
