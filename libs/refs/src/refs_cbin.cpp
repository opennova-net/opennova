// .kda credits -> fonts + inline images.
#include "cbin/cbin.h"
#include "extractors.h"

namespace opennova::refs::detail {

bool extract_credits(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error) {
    cbin::Credits credits;
    if (!cbin::decode_credits(data, size, credits, error)) {
        return false;
    }
    EdgeSink sink(source_path, "credits", out);
    for (size_t i = 0; i < credits.entries.size(); ++i) {
        const cbin::Entry& entry = credits.entries[i];
        const std::string site = "entry[" + std::to_string(i) + "]";
        if (entry.type == cbin::EntryType::Text) {
            sink.add(entry.font, "font", site + ".font");
        } else if (entry.type == cbin::EntryType::Image) {
            sink.add(entry.image_path, "image", site + ".image");
        }
    }
    return true;
}

}  // namespace opennova::refs::detail
