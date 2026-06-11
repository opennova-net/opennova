// .3di (3DI3) -> material texture slots.
#include <cstring>
#include <string>

#include "extractors.h"
#include "threedi/threedi_3di3.h"

namespace opennova::refs::detail {

namespace {

const char* slot_label(uint8_t slot) {
    switch (slot) {
        case THREEDI_TEX_SLOT_DIFFUSE:
            return "diffuse";
        case THREEDI_TEX_SLOT_DETAIL:
            return "detail";
        case THREEDI_TEX_SLOT_NORMAL:
            return "normal";
        default:
            return "slot";
    }
}

}  // namespace

bool extract_threedi(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error) {
    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    if (threedi_3di3_read_memory(data, size, &model) != 0) {
        threedi_3di3_free(&model);
        error = "3di parse failed";
        return false;
    }
    EdgeSink sink(source_path, "object_model", out);
    for (uint32_t m = 0; m < model.material_count; ++m) {
        const ThreediMaterial& material = model.materials[m];
        for (size_t t = 0; t < sizeof(material.textures) / sizeof(material.textures[0]); ++t) {
            const ThreediMaterialTexture& texture = material.textures[t];
            if (texture.name[0] == '\0') {
                continue;
            }
            sink.add(texture.name, "texture",
                     "material[" + std::to_string(m) + "] " + slot_label(texture.slot));
        }
    }
    threedi_3di3_free(&model);
    return true;
}

}  // namespace opennova::refs::detail
