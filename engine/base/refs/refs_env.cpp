// .env -> sky textures + celestial models.
#include <sstream>

#include "env/env.h"
#include "extractors.h"

namespace opennova::refs::detail {

bool extract_env(const std::string& source_path, const uint8_t* data, size_t size,
                 std::vector<Reference>& out, std::string& error) {
    env::Config cfg;
    std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
    if (!env::load_env(in, cfg, error)) {
        return false;
    }
    EdgeSink sink(source_path, "environment", out);
    // Fields the file does not set keep the engine defaults (cld_day1.pcx,
    // msun.3di, ... [orig: Environment_InitDefaults @ 0x57c010]) and the engine
    // loads those defaults, so the edges reflect the file's EFFECTIVE references.
    sink.add(cfg.sky_map1, "texture", "sky_map1");
    sink.add(cfg.sky_map2, "texture", "sky_map2");
    sink.add(cfg.sun_3di, "object_model", "sun_3di");
    sink.add(cfg.moon_3di, "object_model", "moon_3di");
    sink.add(cfg.glare_3di, "object_model", "glare_3di");
    sink.add(cfg.star_3di, "object_model", "star_3di");
    return true;
}

}  // namespace opennova::refs::detail
