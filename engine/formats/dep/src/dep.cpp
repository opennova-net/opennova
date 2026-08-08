#include "dep/dep.h"

#include <cstdio>
#include <fstream>

namespace opennova::dep {

std::vector<uint16_t> read(const std::string &path) {
    std::vector<uint16_t> depth_buffer(kDepthSamples);
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.read(reinterpret_cast<char *>(depth_buffer.data()),
                  static_cast<std::streamsize>(depth_buffer.size() * sizeof(uint16_t)));
    }
    return depth_buffer;
}

bool write(const std::string &path, const uint16_t *samples, size_t count) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const size_t written = std::fwrite(samples, sizeof(uint16_t), count, f);
    std::fclose(f);
    return written == count;
}

}  // namespace opennova::dep
