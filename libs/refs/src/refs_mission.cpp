// .bms mission -> terrain, environment, items.def.
#include <cstring>
#include <string>

#include "extractors.h"
#include "mission/bms.h"

namespace opennova::refs::detail {

namespace {

// A fixed-width header field may legally fill its array with no terminator.
std::string fixed_field(const char* field, size_t cap) {
    return std::string(field, strnlen(field, cap));
}

}  // namespace

bool extract_mission(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error) {
    opennova::bms::File file;
    if (!opennova::bms::parse(data, size, file, error)) {
        return false;
    }
    EdgeSink sink(source_path, "mission", out);
    // Header refs are extensionless basenames; the engine appends .trn/.env at
    // load. Names stay verbatim — extension conventions are resolution policy.
    sink.add(file.get_terrain(), "terrain", "header.terrain");
    sink.add(fixed_field(file.header.environment, sizeof(file.header.environment)),
             "environment", "header.environment");
    // Entities reference item ids INSIDE items.def (bms type_id + 100000), so at
    // file granularity any placed entity is one edge to the item table. Per-item
    // usage stays an entry-level query for later (key-suffix convention), per the
    // graph-granularity decision. The briefing field is deliberately NOT an edge:
    // whether the engine treats it as an rtxt key is grill-gated (TODO.md).
    const size_t entity_count =
        file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size();
    if (entity_count > 0) {
        sink.add("items.def", "item_defs", "entity item table");
    }
    return true;
}

}  // namespace opennova::refs::detail
