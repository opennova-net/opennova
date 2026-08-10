#include "extractors.h"

#include <avatars/avatars.h>

namespace opennova::refs::detail {

bool extract_avatars_def(const std::string& source_path, const uint8_t* data, size_t size,
                         std::vector<Reference>& out, std::string& error) {
    error.clear();
    const char empty[] = "";
    const void* bytes = data != nullptr ? static_cast<const void*>(data) : static_cast<const void*>(empty);

    AvatarsFile file = {};
    if (avatars_parse_memory(bytes, size, &file) != 0) {
        error = "avatars parse failed";
        return false;
    }

    EdgeSink sink(source_path, "avatar", out);
    for (size_t i = 0; i < file.parts_count; ++i) {
        const AvatarPart& p = file.parts[i];
        const std::string base = std::string("part[") + p.name + "]";
        sink.add(p.display_name, "string_id", base + ".name");
        sink.add(p.graphic, "object_model", base + ".graphic");
        sink.add(p.graphic_j, "object_model", base + ".graphic_j");
        sink.add(p.graphic_s, "object_model", base + ".graphic_s");
    }

    for (size_t i = 0; i < file.nationalities_count; ++i) {
        const AvatarNationality& n = file.nationalities[i];
        const std::string nat_base = std::string("nationality[") + n.raw_id + "]";
        sink.add(n.name_key, "string_id", nat_base + ".name");
        for (size_t j = 0; j < n.divisions_count; ++j) {
            const AvatarDivision& d = n.divisions[j];
            sink.add(d.name_key, "string_id",
                     nat_base + ".division[" + d.raw_id + "].name");
        }
    }

    avatars_free(&file);
    return true;
}

}  // namespace opennova::refs::detail
