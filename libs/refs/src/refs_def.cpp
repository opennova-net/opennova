// items.def -> 3di graphics, anim defs, sound profiles.
#include <string>

#include "def/def.h"
#include "extractors.h"

namespace opennova::refs::detail {

bool extract_items_def(const std::string& source_path, const uint8_t* data, size_t size,
                       std::vector<Reference>& out, std::string& error) {
    DefItemsFile items;
    if (def_parse_items_memory(data, size, &items) != 0) {
        error = "items.def parse failed";
        return false;
    }
    EdgeSink sink(source_path, "item_defs", out);
    for (size_t i = 0; i < items.count; ++i) {
        const DefItemDef& item = items.entries[i];
        const std::string prefix = "item " + std::to_string(item.id) + " ";
        // graphic/husk are .3di basenames; anim_def/sound_profile/soundloops are
        // names the engine resolves through their own tables. The *shot screenshot
        // fields stay out until their target semantics are witnessed.
        sink.add(item.graphic, "object_model", prefix + "graphic");
        sink.add(item.husk, "object_model", prefix + "husk");
        sink.add(item.anim_def, "anim_def", prefix + "anim_def");
        sink.add(item.sound_profile, "sound_profile", prefix + "sound_profile");
        sink.add(item.sound_profile_female, "sound_profile", prefix + "sound_profileFemale");
        for (size_t j = 0; j < 7; ++j) {
            sink.add(item.soundloops[j], "sound_profile", prefix + "soundloop[" + std::to_string(j) + "]");
        }
    }
    def_free_items(&items);
    return true;
}

}  // namespace opennova::refs::detail
