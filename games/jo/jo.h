#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::world {
struct TitleRules; // runtime/world/title_rules.h (opennova_games_rules)
}

namespace opennova::games {

// Joint Operations: Typhoon Rising, the JO:CA Jointops.exe (docs/engine-primer.md's IDB): the
// title (jo_title.cpp), the protocol it speaks (jo_protocol.cpp) and the rules it runs
// (jo_rules.cpp).
const Title &jo_title();
const Protocol &jo_protocol();
const world::TitleRules &jo_rules();

} // namespace opennova::games
